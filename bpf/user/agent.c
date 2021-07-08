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
#include "third_party/iovisor_bcc/trace_helpers.h"
#include "libbpf/bpf.h"
#include "libbpf/libbpf.h"

static struct ghost_bpf *obj;
static struct ghost_per_cpu_data *cpu_data;
static struct {
	bool inserted;
	int fd;
} bpf_info [GHOST_BPF_MAX_NR_PROGS];

// mmaps a bpf map.  Returns MAP_FAILED on error; unmap it with munmap().
static void *bpf_map__mmap(struct bpf_map *map)
{
	const struct bpf_map_def *def = bpf_map__def(map);
	size_t mmap_sz;

	mmap_sz = (size_t)roundup(def->value_size, 8) * def->max_entries;
	mmap_sz = roundup(mmap_sz, sysconf(_SC_PAGE_SIZE));
	return mmap(NULL, mmap_sz, PROT_READ | PROT_WRITE, MAP_SHARED,
		    bpf_map__fd(map), 0);
}

static void bpf_program__set_types(struct bpf_program *prog, int prog_type,
				   int expected_attach_type)
{
	bpf_program__set_type(prog, prog_type);
	bpf_program__set_expected_attach_type(prog, expected_attach_type);
}

// libbpf doesn't know about ghost-specific BPF programs, so you have to set the
// type and attach type for every bpf program before they are loaded.
static void set_all_program_types(struct ghost_bpf *obj)
{
	bpf_program__set_types(obj->progs.ghost_sched_skip_tick,
			       BPF_PROG_TYPE_GHOST_SCHED,
			       BPF_GHOST_SCHED_SKIP_TICK);
}

// Inserts the program in the the kernel.  The program must already be loaded.
// Internally, this does an ATTACH or LINK.  libbpf doesn't know about ghost's
// BPF programs, so you have to call this instead of something like
// SKELETON_bpf__attach(obj).
//
// Returns the inserted program's FD on success, or -1 with errno set on
// failure.
static int insert_prog(int ctl_fd, struct bpf_program *prog)
{
	int prog_fd = bpf_program__fd(prog);
	int eat = bpf_program__get_expected_attach_type(prog);
	int ret;

	switch (eat) {
	case BPF_GHOST_SCHED_SKIP_TICK:
		ret = bpf_link_create(prog_fd, ctl_fd, eat, NULL);
		break;
	default:
		/* no attach types yet, but here's how we'd do it */
		ret = bpf_prog_attach(prog_fd, ctl_fd, eat, 0);
		break;
	}

	// libbpf's functions typically return -error_code, but some *usually*
	// just return -1 and have errno set, but *might* return -EINVAL.  It
	// depends if they are calling sys_bpf() directly without handling
	// errors.  bpf_link_create() and prog_attach are like the latter.
	if (ret == -1) {
		// Make sure errno has some non-zero value
		errno = errno ?: EINVAL;
		return -1;
	}
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return ret;
}

// Returns 0 on success, or -1 with errno set on failure.
// You can attempt to insert the same program multiple times; it will only be
// inserted once.
static int insert_prog_by_id(struct ghost_bpf *obj, int ctl_fd, int id)
{
	int ret;

	if (bpf_info[id].inserted)
		return 0;

	switch (id) {
	case GHOST_BPF_NONE:
		return 0;
	case GHOST_BPF_TICK_ON_REQUEST:
		ret = insert_prog(ctl_fd, obj->progs.ghost_sched_skip_tick);
		if (ret < 0)
			return ret;
		break;
	default:
		errno = ENOENT;
		return -1;
	}

	bpf_info[id].inserted = true;
	bpf_info[id].fd = ret;

	return 0;
}

// Returns 0 on success, -1 with errno set on failure.
int bpf_init(void)
{
	int err;

	if (obj)
		return 0;

	if (bump_memlock_rlimit())
		return -1;

	obj = ghost_bpf__open();
	if (!obj) {
		// ghost_bpf__open() clobbered errno.
		errno = EINVAL;
		return -1;
	}

	bpf_map__resize(obj->maps.cpu_data, libbpf_num_possible_cpus());

	set_all_program_types(obj);

	err = ghost_bpf__load(obj);
	if (err) {
		ghost_bpf__destroy(obj);
		errno = -err;
		return -1;
	}
	return 0;
}

// Returns 0 on success, -1 with errno set on failure.  Any programs inserted
// are not removed; call bpf_destroy() or just exit your process.
int bpf_insert(int ctl_fd, int *progs, size_t nr_progs)
{
	int ret;

	if (!obj) {
		errno = ENXIO;
		return -1;
	}

	for (int i = 0; i < nr_progs; i++) {
		ret = insert_prog_by_id(obj, ctl_fd, progs[i]);
		if (ret)
			return ret;
	}

	cpu_data = bpf_map__mmap(obj->maps.cpu_data);

	if (cpu_data == MAP_FAILED)
		return -1;

	return 0;
}

// Gracefully unlinks and unloads the BPF programs.  When agents call this, they
// explicitly close (and thus unlink/detach) BPF programs from the enclave,
// which will speed up agent upgrade/handoff.
void bpf_destroy(void)
{
	for (int i = 0; i < GHOST_BPF_MAX_NR_PROGS; ++i) {
		if (bpf_info[i].inserted) {
			close(bpf_info[i].fd);
			bpf_info[i].inserted = false;
		}
	}
	ghost_bpf__destroy(obj);
	obj = NULL;
}

// Returns 0 on success, -1 with errno set on failure.  Must have selected
// GHOST_BPF_TICK_ON_REQUEST.
int request_tick_on_cpu(int cpu)
{
	unsigned int nr_cpus = libbpf_num_possible_cpus();

	if (!bpf_info[GHOST_BPF_TICK_ON_REQUEST].inserted) {
		errno = ENOENT;
		return -1;
	}
	if (cpu >= nr_cpus) {
		errno = ERANGE;
		return -1;
	}
	if (!cpu_data) {
		errno = EINVAL;
		return -1;
	}
	cpu_data[cpu].want_tick = true;
	return 0;
}
