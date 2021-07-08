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

// vmlinux.h must be included before bpf_helpers.h
// clang-format off
#include "kernel/vmlinux_ghost_5_11.h"
#include "libbpf/bpf_core_read.h"
#include "libbpf/bpf_helpers.h"
#include "libbpf/bpf_tracing.h"
// clang-format on

#include "third_party/iovisor_bcc/bits.bpf.h"
#include "third_party/bpf/common.bpf.h"

// Keep this in sync with schedrun.c.
#define NUM_BUCKETS 25

const volatile pid_t targ_tgid = 0;
const volatile bool ghost_only = false;

// Map each task's pid to the timestamp it started running.
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, MAX_PIDS);
	__type(key, u32);
	__type(value, u64);
} task_start_times SEC(".maps");

// Map histogram bucket index to counts.
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, NUM_BUCKETS);
	__type(key, u32);
	__type(value, u64);
} hist SEC(".maps");

static void update_hist(u64 value)
{
	u64 bucket_idx, *count;

	bucket_idx = log2l(value);
	if (bucket_idx >= NUM_BUCKETS)
		bucket_idx = NUM_BUCKETS - 1;

	count = bpf_map_lookup_elem(&hist, &bucket_idx);
	if (!count)
		return;

	*count += 1;
}

static void task_stop(struct task_struct *p)
{
	u32 pid = BPF_CORE_READ(p, pid);
	u64 stop = bpf_ktime_get_us();
	u64 *start = bpf_map_lookup_elem(&task_start_times, &pid);

	if (start) {
		u64 diff = stop - *start;
		update_hist(diff);

		long state = BPF_CORE_READ(p, state);
		if (state == TASK_DEAD)
			bpf_map_delete_elem(&task_start_times, &pid);
	}
}

static void task_run(struct task_struct *p)
{
	u32 pid = BPF_CORE_READ(p, pid);
	u64 start = bpf_ktime_get_us();

	bpf_map_update_elem(&task_start_times, &pid, &start, BPF_ANY);
}

static bool is_traced(struct task_struct *p)
{
	if (targ_tgid)
		return BPF_CORE_READ(p, tgid) == targ_tgid;

	if (ghost_only)
		return is_traced_ghost(p);

	return true;
}

SEC("tp_btf/sched_switch")
int BPF_PROG(sched_switch, bool preempt, struct task_struct *prev,
	     struct task_struct *next)
{
	if (is_traced(prev))
		task_stop(prev);

	if (is_traced(next))
		task_run(next);

	return 0;
}

char LICENSE[] SEC("license") = "GPL";
