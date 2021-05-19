/*
 * Copyright 2021 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "bpf/user/agent.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <unistd.h>

#include "bpf/user/ghost_bpf.skel.h"
#include "bpf/user/ghost_shared.h"
#include "bpf/iovisor_bcc/trace_helpers.h"
#include "linux_tools/bpf.h"
#include "linux_tools/libbpf.h"

static struct per_cpu_data *cpu_data;

static void *bpf_map__mmap(struct bpf_map *map)
{
	const struct bpf_map_def *def = bpf_map__def(map);
	size_t mmap_sz;

	mmap_sz = (size_t)roundup(def->value_size, 8) * def->max_entries;
	mmap_sz = roundup(mmap_sz, sysconf(_SC_PAGE_SIZE));
	return mmap(NULL, mmap_sz, PROT_READ | PROT_WRITE, MAP_SHARED,
		    bpf_map__fd(map), 0);
}

static void prep_prog(struct bpf_program *prog, int prog_type,
		      int expected_attach_type)
{
	bpf_program__set_type(prog, prog_type);
	bpf_program__set_expected_attach_type(prog, expected_attach_type);
}

/* attach or link, as applicable */
static int hook_prog(struct bpf_program *prog)
{
	int prog_fd = bpf_program__fd(prog);
	int eat = bpf_program__get_expected_attach_type(prog);

	switch (eat) {
	case BPF_SCHEDULER_TICK:
		if (bpf_link_create(prog_fd, -1, eat, NULL) < 0)
			return -1;
		return 0;
	default:
		/* no attach types yet, but here's how we'd do it */
		if (bpf_prog_attach(prog_fd, -1, eat, 0))
			return -1;
		return 0;
	}
}

int bpf_init(void)
{
	struct ghost_bpf *obj;
	int err;

	err = bump_memlock_rlimit();

	if (err) {
		fprintf(stderr, "failed to increase rlimit: %d\n", err);
		return -1;
	}

	obj = ghost_bpf__open();
	if (!obj) {
		fprintf(stderr, "failed to open BPF object\n");
		return -1;
	}

	bpf_map__resize(obj->maps.cpu_data, libbpf_num_possible_cpus());

	/* libbpf doesn't know about our sched type.  you have to do this for
	 * each program.
	 */
	prep_prog(obj->progs.sched_tick, BPF_PROG_TYPE_SCHEDULER,
		  BPF_SCHEDULER_TICK);

	if (ghost_bpf__load(obj)) {
		ghost_bpf__destroy(obj);
		fprintf(stderr, "failed to load BPF object\n");
		return -1;
	}

	if (hook_prog(obj->progs.sched_tick) < 0) {
		fprintf(stderr, "failed to attach/link BPF program\n");
		goto cleanup;
	}

	cpu_data = bpf_map__mmap(obj->maps.cpu_data);

	if (cpu_data == MAP_FAILED) {
		cpu_data = NULL;
		fprintf(stderr, "failed to mmap cpu_data\n");
		goto cleanup;
	}

cleanup:
	ghost_bpf__destroy(obj);

	return 0;
}

void bpf_request_tick_on(int cpu)
{
	unsigned int nr_cpus = libbpf_num_possible_cpus();

	if (cpu >= nr_cpus) {
		fprintf(stderr, "cpu %d out of range %d\n", cpu, nr_cpus);
		return;
	}
	if (!cpu_data) {
		fprintf(stderr, "BPF cpu_data not mmapped!\n");
		return;
	}
	cpu_data[cpu].want_tick = true;
}
