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

#include "lib/ghost_uapi.h"
#include "third_party/bpf/common.bpf.h"
#include "third_party/iovisor_bcc/bits.bpf.h"

#define MAX_CPUS 512
/* Keep this in sync with schedghostidle.c and bpf/user/agent.c */
#define NR_SLOTS 25

uint64_t nr_latches = 0;
uint64_t nr_bpf_latches = 0;
uint64_t nr_idle_to_bpf_latches = 0;

/*
 * This array maps is racy, but it's fine.  Both the latcher and sched_switch
 * tracepoints hold the RQ lock.  We want to access a cpu's data from another
 * cpu, since the latcher may not be on a particular cpu.
 */
struct cpu_info {
	bool is_idle;
	u64 idle_start;
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, MAX_CPUS);
	__type(key, u32);
	__type(value, struct cpu_info);
} cpu_info SEC(".maps");

/* key: hist slot idx.  value: count */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, NR_SLOTS);
	__type(key, u32);
	__type(value, u64);
} hist SEC(".maps");

static bool task_is_idle(struct task_struct *p)
{
	#define PF_IDLE 0x2	/* linux/sched.h */
	u32 flags = BPF_CORE_READ(p, flags);

	return flags & PF_IDLE;
}

SEC("tp_btf/sched_switch")
int BPF_PROG(sched_switch, bool preempt, struct task_struct *prev,
	     struct task_struct *next)
{
	u32 cpu = bpf_get_smp_processor_id();
	struct cpu_info *ci = bpf_map_lookup_elem(&cpu_info, &cpu);

	if (!ci)
		return 0;

	if (task_is_idle(next)) {
		ci->is_idle = true;
		ci->idle_start = bpf_ktime_get_ns();
	} else {
		ci->is_idle = false;
	}

	return 0;
}

static int task_cpu(struct task_struct *p)
{
	return BPF_CORE_READ(p, cpu);
}

static void update_hist(u64 nsec)
{
	u64 slot, *count;

	slot = log2l(nsec / 1000);
	if (slot >= NR_SLOTS)
		slot = NR_SLOTS - 1;
	count = bpf_map_lookup_elem(&hist, &slot);
	if (!count)
		return;
	*count += 1;
}

SEC("tp_btf/sched_ghost_latched")
int BPF_PROG(sched_ghost_latched, struct task_struct *old,
	     struct task_struct *new, int run_flags)
{
	u32 cpu = task_cpu(new);
	struct cpu_info *ci = bpf_map_lookup_elem(&cpu_info, &cpu);

	__sync_fetch_and_add(&nr_latches, 1);
	/* BPF-PNT is the only one who uses SEND_TASK_ON_CPU. */
	if (run_flags & SEND_TASK_ON_CPU)
		__sync_fetch_and_add(&nr_bpf_latches, 1);

	if (!ci || !ci->is_idle) {
		/*
		 * When BPF-PNT latches a task, the cpu might not go idle.
		 * However, we'd like to measure those events.
		 */
		if (run_flags & SEND_TASK_ON_CPU)
			update_hist(0);
		return 0;
	}
	__sync_fetch_and_add(&nr_idle_to_bpf_latches, 1);

	update_hist(bpf_ktime_get_ns() - ci->idle_start);
	/*
	 * Technically, the cpu is still idle, and our latch may get aborted or
	 * otherwise fail.  But the agent has noticed the previous idling (as
	 * shown by it trying to latch), so we do not want to count as idle for
	 * any other latchings that happen before the next sched_switch.
	 */
	ci->is_idle = false;

	return 0;
}

char LICENSE[] SEC("license") = "GPL";
