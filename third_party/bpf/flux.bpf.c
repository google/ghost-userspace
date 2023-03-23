/*
 * Copyright 2023 Google LLC
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 or later as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

// vmlinux.h must be included before bpf_helpers.h
// clang-format off
#include "kernel/vmlinux_ghost_5_11.h"
#include "libbpf/bpf_helpers.h"
#include "libbpf/bpf_tracing.h"
// clang-format on

#include "third_party/bpf/common.bpf.h"
#include "third_party/bpf/flux_bpf.h"

#include <asm-generic/errno.h>

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, FLUX_NR_SCHEDS);
	__type(key, u32);
	__type(value, struct flux_sched);
} schedulers SEC(".maps");

static inline struct flux_sched *get_sched(int id)
{
	return bpf_map_lookup_elem(&schedulers, &id);
}

static inline int get_parent_id(struct flux_sched *s)
{
	if (s->f.id == FLUX_SCHED_ROCI)
		return FLUX_SCHED_NONE;
	return FLUX_SCHED_ROCI;
}

/*
 * The 'tier' is where a scheduler is in the hierarchy of schedulers.  Since
 * we're in BPF, this is hardcoded: Roci is at top, with biff and idle below.
 *
 * Schedulers can preempt their cpus, and you can have preemptions at every tier
 * concurrently.  e.g.
 * - biff can preempt its own cpu to kick a thread off cpu (tier = 2)
 * - roci can preempt that cpu to kick biff off (tier = 1)
 * - the kernel can preempt the cpu completely (availability change, tier = 0)
 *
 * When preempt_to is 3 (FLUX_MAX_NR_TIERS, aka FLUX_TIER_NO_PREEMPT), there are
 * no preemption requests.
 *
 * Keep in mind that it's always OK for us to preempt a cpu.  If there's some
 * corner case where we accidentally preempt a cpu unintentionally, that's fine.
 * The schedulers will just reallocate it.
 *
 * Quick example: roci on cpu A wants to preempt cpu B.  It does its
 * bookkeeping, plans to preempt, then calls flux_preempt_cpu.  At that point,
 * the kernel preempts the cpu, then reallocates it, and the cpu is roci's
 * again.  Then cpu A writes preempt_to and sends the IPI.  Next time we run
 * PNT, we'll preempt that cpu up to roci, which can then hand it back to
 * biff/idle/whoever.
 */

#define FLUX_MAX_NR_TIERS 3

static inline int sched_id_to_tier(int id)
{
	switch (id) {
	case FLUX_SCHED_NONE:
		return 0;
	case FLUX_SCHED_ROCI:
		return 1;
	case FLUX_SCHED_BIFF:
	case FLUX_SCHED_IDLE:
		return 2;
	};
	return 0;
}

static int new_thread_sched_id(struct ghost_msg_payload_task_new *new)
{
	return FLUX_SCHED_BIFF;
}

static int top_tier_sched_id(void)
{
	return FLUX_SCHED_ROCI;
}

#define __gen_thread_op_cases(op_type, op, sched, ...)			\
	case FLUX_SCHED_TYPE_BIFF:					\
		op_type(biff, op)(sched, __VA_ARGS__);			\
		break;							\

#define __gen_cpu_op_cases(op_type, op, sched, ...)			\
	case FLUX_SCHED_TYPE_ROCI:					\
		op_type(roci, op)(sched, __VA_ARGS__);			\
		break;							\
	case FLUX_SCHED_TYPE_BIFF:					\
		op_type(biff, op)(sched, __VA_ARGS__);			\
		break;							\
	case FLUX_SCHED_TYPE_IDLE:					\
		op_type(idle, op)(sched, __VA_ARGS__);			\
		break;							\


#include "third_party/bpf/flux_dispatch.bpf.c"

/********************* SCHED OPS *********************/

/*
 * Roci wants a couple helpers, so that it knows the IDs of its "one child" /
 * primary scheduler (biff) and idle.
 */

static int __roci_primary_sched_id(void)
{
	return FLUX_SCHED_BIFF;
}

static int __roci_idle_sched_id(void)
{
	return FLUX_SCHED_IDLE;
}

#include "third_party/bpf/roci_flux.bpf.c"
#include "third_party/bpf/biff_flux.bpf.c"
#include "third_party/bpf/idle_flux.bpf.c"

#include "third_party/bpf/flux_api.bpf.c"
