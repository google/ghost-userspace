// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "bpf/user/agent.h"
#include "kernel/ghost_uapi.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <unistd.h>

#include "bpf/user/schedghostidle_bpf.skel.h"
#include "third_party/iovisor_bcc/trace_helpers.h"

#define BUILD_BUG_ON(condition) ((void)sizeof(char[1 - 2*!!(condition)]))

// This contains all registered BPF programs, both from struct ghost_bpf as well
// as any scheduler-specific programs.  The kernel lets you insert up to one
// program at each attach point.
static struct bpf_registration {
	struct bpf_program *prog;
	bool inserted;
	int fd;
} bpf_registry [__MAX_BPF_GHOST_ATTACH_TYPE];

size_t bpf_map__mmap_sz(struct bpf_map *map)
{
	size_t mmap_sz;

	mmap_sz = (size_t)roundup(bpf_map__value_size(map), 8) *
		bpf_map__max_entries(map);
	mmap_sz = roundup(mmap_sz, sysconf(_SC_PAGE_SIZE));

	return mmap_sz;
}

// mmaps a bpf map.  Returns MAP_FAILED on error.
// munmap it with bpf_map__munmap().
void *bpf_map__mmap(struct bpf_map *map)
{
	return mmap(NULL, bpf_map__mmap_sz(map),
		    PROT_READ | PROT_WRITE, MAP_SHARED,
		    bpf_map__fd(map), 0);
}

int bpf_map__munmap(struct bpf_map *map, void *addr)
{
	return munmap(addr, bpf_map__mmap_sz(map));
}

void bpf_program__set_types(struct bpf_program *prog, int prog_type,
			    int expected_attach_type)
{
	BUILD_BUG_ON(__MAX_BPF_GHOST_ATTACH_TYPE > 0xFFFF);
	BUILD_BUG_ON(GHOST_VERSION > 0xFFFF);

	expected_attach_type |= GHOST_VERSION << 16;

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

	// Mask the ABI value encoded in the upper 16 bits.
	switch (eat & 0xFFFF) {
	case BPF_GHOST_SCHED_PNT:
	case BPF_GHOST_MSG_SEND:
	case BPF_GHOST_SELECT_RQ:
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

// Common BPF initialization
//
// Returns 0 on success, -1 with errno set on failure.
int agent_bpf_init(void)
{
	if (bump_memlock_rlimit())
		return -1;
	return 0;
}

// We'd like to use `enum bpf_attach_type eat` here, but for now we need an int
// because the ghost bpf values are not in the uapi available to userspace yet.
// They are in the kernel repo.  We keep them in sync in bpf/user/agent.h.
int agent_bpf_register(struct bpf_program *prog, int eat)
{
	if (eat >= __MAX_BPF_GHOST_ATTACH_TYPE) {
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

	for (int i = 0; i < __MAX_BPF_GHOST_ATTACH_TYPE; i++) {
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
//
// Idempotent.
void agent_bpf_destroy(void)
{
	struct bpf_registration *r;

	for (int i = 0; i < __MAX_BPF_GHOST_ATTACH_TYPE; ++i) {
		r = &bpf_registry[i];
		if (r->inserted) {
			close(r->fd);
			r->inserted = false;
		}
		r->prog = NULL;
	}
}

/* schedghostidle tracer */

static void *sgi_make_skel_obj(void)
{
	struct schedghostidle_bpf *obj;

	obj = schedghostidle_bpf__open_and_load();
	if (!obj) {
		fprintf(stderr, "failed to open/load schedghostidle\n");
		return NULL;
	}

	if (schedghostidle_bpf__attach(obj)) {
		fprintf(stderr, "failed to attach schedghostidle\n");
		schedghostidle_bpf__destroy(obj);
		return NULL;
	}
	return obj;
}

/* Keep this in sync with schedghostidle.bpf.c. */
#define SGI_NR_SLOTS 25

static void sgi_output(void *obj, FILE *to)
{
	struct schedghostidle_bpf *sgi_obj = obj;
	unsigned int nr_cpus = libbpf_num_possible_cpus();
	unsigned int hist[SGI_NR_SLOTS] = {0};
	uint64_t *count;
	int fd = bpf_map__fd(sgi_obj->maps.hist);

	count = calloc(nr_cpus, sizeof(*count));
	if (!count) {
		fprintf(to, "sgi calloc failed!\n");
		return;
	}

	for (int i = 0; i < SGI_NR_SLOTS; i++) {
		if (bpf_map_lookup_elem(fd, &i, count)) {
			fprintf(to, "sgi lookup failed!");
			return;
		}
		hist[i] = 0;
		for (int c = 0; c < nr_cpus; c++)
			hist[i] += count[c];
	}
	free(count);

	fprintf(to, "\n");
	fprintf(to, "Latency of a CPU going Idle until a task is Latched:\n");
	fprintf(to, "----------------------------------------------------\n");
	print_log2_hist_to(to, hist, SGI_NR_SLOTS, "usec");
}

static void sgi_reset(void *obj)
{
	struct schedghostidle_bpf *sgi_obj = obj;
	unsigned int nr_cpus = libbpf_num_possible_cpus();
	uint64_t *zeros;
	int fd = bpf_map__fd(sgi_obj->maps.hist);

	zeros = calloc(nr_cpus, sizeof(uint64_t));
	if (!zeros) {
		fprintf(stderr, "sgi calloc failed!\n");
		return;
	}

	/*
	 * As a reminder, the way you update per-cpu arrays is one array index
	 * at a time, for all cpus at once.
	 */
	for (int i = 0; i < SGI_NR_SLOTS; i++) {
		if (bpf_map_update_elem(fd, &i, zeros, BPF_ANY)) {
			fprintf(stderr, "sgi zeroing failed!");
			return;
		}
	}
	free(zeros);
}

static struct agent_bpf_trace {
	void *skel_obj;
	void *(*make_skel_obj)(void);
	void (*output)(void *skel_obj, FILE *to);
	void (*reset)(void *skel_obj);
} tracers[] = {
	[AGENT_BPF_TRACE_SCHEDGHOSTIDLE] = {
		.make_skel_obj = sgi_make_skel_obj,
		.output = sgi_output,
		.reset = sgi_reset,
	},
};

int agent_bpf_trace_init(unsigned int type)
{
	struct agent_bpf_trace *abt;

	if (type >= MAX_AGENT_BPF_TRACE) {
		fprintf(stderr, "cannot init unknown trace type %d\n", type);
		return -1;
	}
	abt = &tracers[type];
	abt->skel_obj = abt->make_skel_obj();
	if (!abt->skel_obj)
		return -1;
	return 0;
}

void agent_bpf_trace_output(FILE *to, unsigned int type)
{
	struct agent_bpf_trace *abt;

	if (type >= MAX_AGENT_BPF_TRACE) {
		fprintf(stderr, "cannot output unknown trace type %d\n", type);
		return;
	}
	abt = &tracers[type];
	if (!abt->skel_obj) {
		fprintf(stderr, "cannot output uninit trace type %d\n", type);
		return;
	}
	abt->output(abt->skel_obj, to);
}

void agent_bpf_trace_reset(unsigned int type)
{
	struct agent_bpf_trace *abt;

	if (type >= MAX_AGENT_BPF_TRACE) {
		fprintf(stderr, "cannot reset unknown trace type %d\n", type);
		return;
	}
	abt = &tracers[type];
	if (!abt->skel_obj) {
		fprintf(stderr, "cannot output uninit trace type %d\n", type);
		return;
	}
	abt->reset(abt->skel_obj);
}
