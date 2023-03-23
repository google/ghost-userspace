/*
 * Copyright 2023 Google LLC
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifndef GHOST_LIB_BPF_BPF_FLUX_BPF_H_
#define GHOST_LIB_BPF_BPF_FLUX_BPF_H_

#include "third_party/bpf/biff_flux_bpf.h"
#include "third_party/bpf/flux_header_bpf.h"
#include "third_party/bpf/idle_flux_bpf.h"
#include "third_party/bpf/roci_flux_bpf.h"
#include "lib/queue.bpf.h"

struct flux_sched {
	struct __flux_sched f;

#ifdef __BPF__
	/*
	 * bpf_spin_lock is not available in userspace.
	 * The sizeof == 32 is UAPI and statically asserted in flux_pnt.
	 */
	struct bpf_spin_lock lock;
#else
	uint32_t lock;
#endif
	union {
		struct roci_flux_sched roci;
		struct biff_flux_sched biff;
		struct idle_flux_sched idle;
	};
} __attribute__((aligned(8)));
/* aligned(8) since this is a bpf map value. */

enum {
	FLUX_SCHED_NONE,
	FLUX_SCHED_ROCI,
	FLUX_SCHED_BIFF,
	FLUX_SCHED_IDLE,
	FLUX_NR_SCHEDS,
};

enum {
	FLUX_SCHED_TYPE_NONE,
	FLUX_SCHED_TYPE_ROCI,
	FLUX_SCHED_TYPE_BIFF,
	FLUX_SCHED_TYPE_IDLE,
	FLUX_NR_SCHED_TYPES,
};

struct flux_cpu {
	struct __flux_cpu f;

	/*
	 * A cpu can be used by many schedulers concurrently, i.e. roci and biff
	 * can both use cpu fields, since roci allocs the cpu to biff.  Thus we
	 * don't use a union.
	 */
	struct roci_flux_cpu roci;
	struct biff_flux_cpu biff;
	struct idle_flux_cpu idle;
} __attribute__((aligned(64)));
/* aligned(64) for per-cpu caching */

struct flux_thread {
	struct __flux_thread f;

	/* A thread belongs to a single scheduler at a time. */
	union {
		struct biff_flux_thread biff;
	};
} __attribute__((aligned(8)));
/* aligned(8) since this is a bpf map value. */

#endif  // GHOST_LIB_BPF_BPF_FLUX_BPF_H_
