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

// The ghost_bpf object and its cpu_data mmap is our set of
// "scheduler-independent" BPF programs.  Right now, we have a generic one that
// controls whether the kernel generates a timer tick message.
static struct ghost_bpf *obj;
static struct ghost_per_cpu_data *cpu_data;
static bool gbl_tick_on_request;

// This contains all registered BPF programs, both from struct ghost_bpf as well
// as any scheduler-specific programs.  The kernel lets you insert up to one
// program at each attach point.
static struct bpf_registration {
	struct bpf_program *prog;
	bool inserted;
	int fd;
} bpf_registry [BPF_GHOST_SCHED_MAX_ATTACH_TYPE];

static size_t map_mmap_sz(struct bpf_map *map)
{
	const struct bpf_map_def *def = bpf_map__def(map);
	size_t mmap_sz;

	mmap_sz = (size_t)roundup(def->value_size, 8) * def->max_entries;
	mmap_sz = roundup(mmap_sz, sysconf(_SC_PAGE_SIZE));

	return mmap_sz;
}

// mmaps a bpf map.  Returns MAP_FAILED on error.
// munmap it with bpf_map__munmap().
void *bpf_map__mmap(struct bpf_map *map)
{
	return mmap(NULL, map_mmap_sz(map), PROT_READ | PROT_WRITE, MAP_SHARED,
		    bpf_map__fd(map), 0);
}

int bpf_map__munmap(struct bpf_map *map, void *addr)
{
	return munmap(addr, map_mmap_sz(map));
}

void bpf_program__set_types(struct bpf_program *prog, int prog_type,
			    int expected_attach_type)
{
	bpf_program__set_type(prog, prog_type);
	bpf_program__set_expected_attach_type(prog, expected_attach_type);
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

// Initializes the BPF infrastructure.
//
// Additionally, this loads and registers programs for scheduler-independent
// policies, such as how to handle the timer tick.  If a scheduler plans to
// insert its own timer tick program, then unset tick_on_request.
//
// Returns 0 on success, -1 with errno set on failure.
int agent_bpf_init(bool tick_on_request)
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

	bpf_program__set_types(obj->progs.ghost_sched_skip_tick,
			       BPF_PROG_TYPE_GHOST_SCHED,
			       BPF_GHOST_SCHED_SKIP_TICK);

	err = ghost_bpf__load(obj);

	if (err) {
		// ghost_bpf__load() returns a *negative* error code.
		err = -err;
		goto out_error;
	}

	if (tick_on_request) {
		err = agent_bpf_register(obj->progs.ghost_sched_skip_tick,
					 BPF_GHOST_SCHED_SKIP_TICK);
		if (err) {
			err = errno;
			goto out_error;
		}
		gbl_tick_on_request = true;
	}

	cpu_data = bpf_map__mmap(obj->maps.cpu_data);
	if (cpu_data == MAP_FAILED) {
		err = errno;
		goto out_error;
	}


	return 0;

out_error:
	ghost_bpf__destroy(obj);
	errno = err;
	return -1;
}

// We'd like to use `enum bpf_attach_type eat` here, but for now we need an int
// because the ghost bpf values are not in the uapi available to userspace yet.
// They are in the kernel repo.  We keep them in sync in bpf/user/agent.h.
int agent_bpf_register(struct bpf_program *prog, int eat)
{
	if (eat >= BPF_GHOST_SCHED_MAX_ATTACH_TYPE) {
		errno = ERANGE;
		return -1;
	}
	if (bpf_registry[eat].prog) {
		errno = EBUSY;
		return -1;
	}

	bpf_registry[eat].prog = prog;

	return 0;
}

// Returns 0 on success, -1 with errno set on failure.  Any programs inserted
// are not removed on error; call bpf_destroy() or just exit your process.
int agent_bpf_insert_registered(int ctl_fd)
{
	int fd;
	struct bpf_registration *r;

	for (int i = 0; i < BPF_GHOST_SCHED_MAX_ATTACH_TYPE; i++) {
		r = &bpf_registry[i];
		if (!r->prog)
			continue;
		if (r->inserted)
			continue;
		fd = insert_prog(ctl_fd, r->prog);
		if (fd < 0)
			return fd;
		r->inserted = true;
		r->fd = fd;
	}

	return 0;
}

// Gracefully unlinks and unloads the BPF programs.  When agents call this, they
// explicitly close (and thus unlink/detach) BPF programs from the enclave,
// which will speed up agent upgrade/handoff.
void agent_bpf_destroy(void)
{
	struct bpf_registration *r;

	for (int i = 0; i < BPF_GHOST_SCHED_MAX_ATTACH_TYPE; ++i) {
		r = &bpf_registry[i];
		if (r->inserted) {
			close(r->fd);
			r->inserted = false;
		}
	}

	ghost_bpf__destroy(obj);
	obj = NULL;
	gbl_tick_on_request = false;
}

// Returns 0 on success, -1 with errno set on failure.  Must have called
// agent_bpf_init() with tick_on_request.
int agent_bpf_request_tick_on_cpu(int cpu)
{
	unsigned int nr_cpus = libbpf_num_possible_cpus();

	if (!gbl_tick_on_request) {
		errno = EINVAL;
		return -1;
	}
	if (!bpf_registry[BPF_GHOST_SCHED_SKIP_TICK].inserted) {
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
