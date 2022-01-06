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
#include "third_party/bpf/schedlat.h"

/*
 * vmlinux.h does not include #defines.  We can't include the kernel headers,
 * even if we have easy access to them, since we'll conflict with the structs
 * and enums that vmlinux.h *does* include.
 */
#define SCHED_GHOST 18
#define TASK_RUNNING 0

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, MAX_PIDS);
	__type(key, u32);
	__type(value, struct task_stat);
} task_stats SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, NR_HISTS);
	__type(key, u32);
	__type(value, struct hist);
} hists SEC(".maps");

static void task_runnable(struct task_struct *p)
{
	struct task_stat stat[1] = {0};
	pid_t pid;

	if (is_agent(p))
		return;

	pid = BPF_CORE_READ(p, pid);
	stat->runnable_at = bpf_ktime_get_us();

	bpf_map_update_elem(&task_stats, &pid, stat, BPF_ANY);
}

static void task_latched(struct task_struct *p)
{
	struct task_stat *stat;
	pid_t pid = BPF_CORE_READ(p, pid);

	stat = bpf_map_lookup_elem(&task_stats, &pid);
	if (!stat)
		return;
	stat->latched_at = bpf_ktime_get_us();
}

static void increment_hist(u32 hist_id, u64 value)
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

static void task_ran(struct task_struct *p)
{
	struct task_stat *stat;
	pid_t pid = BPF_CORE_READ(p, pid);

	stat = bpf_map_lookup_elem(&task_stats, &pid);
	if (!stat)
		return;
	stat->ran_at = bpf_ktime_get_us();

	/*
	 * Not all tasks are latched/committed.  The agent can yield and
	 * context_switch to a task, bypassing pick_next_task.
	 */
	if (stat->latched_at) {
		increment_hist(RUNNABLE_TO_LATCHED,
			       stat->latched_at - stat->runnable_at);
		increment_hist(LATCHED_TO_RUN,
			       stat->ran_at - stat->latched_at);
	}
	increment_hist(RUNNABLE_TO_RUN, stat->ran_at - stat->runnable_at);

	bpf_map_delete_elem(&task_stats, &pid);
}

SEC("tp_btf/sched_wakeup")
int BPF_PROG(sched_wakeup, struct task_struct *p)
{
	if (task_has_ghost_policy(p))
		task_runnable(p);
	return 0;
}

SEC("tp_btf/sched_wakeup_new")
int BPF_PROG(sched_wakeup_new, struct task_struct *p)
{
	if (task_has_ghost_policy(p))
		task_runnable(p);
	return 0;
}

SEC("tp_btf/sched_switch")
int BPF_PROG(sched_switch, bool preempt, struct task_struct *prev,
	     struct task_struct *next)
{
	if (task_has_ghost_policy(prev) &&
	    (preempt || BPF_CORE_READ(prev, state) == TASK_RUNNING))
		task_runnable(prev);

	if (task_has_ghost_policy(next))
		task_ran(next);

	return 0;
}

SEC("tp_btf/sched_ghost_latched")
int BPF_PROG(sched_ghost_latched, struct task_struct *old,
	     struct task_struct *new)
{
	if (new)
		task_latched(new);
	return 0;
}

char LICENSE[] SEC("license") = "GPL";
