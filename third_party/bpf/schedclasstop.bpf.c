// Copyright 2021 Google LLC
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// version 2 as published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

#include <linux/types.h>

// clang-format off
#include <linux/bpf.h>
#include "libbpf/bpf_core_read.h"
#include "libbpf/bpf_helpers.h"
#include "libbpf/bpf_tracing.h"
// clang-format on

#include "third_party/bpf/common.bpf.h"

#define SCHED_GHOST 18
#define SCHED_AGENT 19  /* Not a real sched class */
#define MAX_SCHED_CLASS (SCHED_AGENT + 1)

/* Using this map as a per-cpu u64 */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, u64);
} start_times SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, MAX_SCHED_CLASS);
	__type(key, u32);
	__type(value, u64);
} class_times SEC(".maps");

static int task_sched_policy(struct task_struct *p)
{
	#define PF_IDLE 0x2	/* linux/sched.h */
	u32 flags = BPF_CORE_READ(p, flags);

	/*
	 * SCHED_IDLE isn't the idle thread, but we do want to track idle
	 * separately.  We reuse SCHED_ISO (4), which is probably the least
	 * likely value to be used.
	 */
	if (flags & PF_IDLE)
		return 4;
	if (task_has_ghost_policy(p)) {
		if (is_agent(p))
			return SCHED_AGENT;
		else
			return SCHED_GHOST;

	}
	return BPF_CORE_READ(p, policy);
}

SEC("tp_btf/sched_switch")
int BPF_PROG(sched_switch, bool preempt, struct task_struct *prev,
	     struct task_struct *next)
{
	u64 *start_time, *class_time;
	u32 prev_policy;
	u32 zero = 0;
	u64 now;

	prev_policy = task_sched_policy(prev);

	start_time = bpf_map_lookup_elem(&start_times, &zero);
	/* This lookup always succeeds, but the verifier needs proof. */
	if (!start_time)
		return 0;

	now = bpf_ktime_get_ns();
	if (*start_time) {
		class_time = bpf_map_lookup_elem(&class_times, &prev_policy);
		if (class_time)
			*class_time += now - *start_time;
	}
	*start_time = now;

	return 0;
}

char LICENSE[] SEC("license") = "GPL";
