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

// common.bpf.h comes before bits.bpf.h for u32/s32/u64/s64 in OSS.
#include "third_party/bpf/common.bpf.h"
#include "third_party/iovisor_bcc/bits.bpf.h"
#include "third_party/bpf/schedrun.h"
// clang-format on

const volatile pid_t targ_tgid = 0;
const volatile bool ghost_only = false;

// Map each task's pid to the timestamp it started running.
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, MAX_PIDS);
	__type(key, u32);
	__type(value, u64);
} task_start_times SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, NR_HISTS);
	__type(key, u32);
	__type(value, struct hist);
} hists SEC(".maps");

// TODO: refactor (copied from schedlat.bpf.c).
static void update_hist(u32 hist_id, u64 value)
{
	u64 slot; /* Gotta love BPF.  slot needs to be a u64, not a u32. */
	struct hist *hist;

	hist = bpf_map_lookup_elem(&hists, &hist_id);
	if (!hist)
		return;
	slot = log2l(value);
	if (slot >= MAX_NR_HIST_SLOTS)
		slot = MAX_NR_HIST_SLOTS - 1;
	hist->slots[slot]++;
}

static void task_stop(struct task_struct *p)
{
	u32 pid = BPF_CORE_READ(p, pid);
	u64 stop = bpf_ktime_get_us();
	u64 *start = bpf_map_lookup_elem(&task_start_times, &pid);

	if (start) {
		u64 diff = stop - *start;
		update_hist(RUNTIMES_ALL, diff);

		long state = BPF_CORE_READ(p, state);
		if (state == TASK_RUNNING) // prev yielded or was preempted.
			update_hist(RUNTIMES_PREEMPTED_YIELDED, diff);
		else // prev blocked.
			update_hist(RUNTIMES_BLOCKED, diff);

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
