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

#include "third_party/bpf/common.bpf.h"
#include "third_party/bpf/klockstat.h"

const volatile pid_t targ_tgid = 0;
const volatile pid_t targ_pid = 0;
struct mutex *const volatile targ_lock = NULL;

/*
 * Make sure all programs are loaded before we start tracing so that we see
 * lock/lock_acquired/unlock for all locks.
 *
 * Otherwise, there is a period of time when we are loading the programs where
 * we are tracing mutex_lock but not mutex_unlock.  For every mutex we grab
 * during that period, we'll have a 'depth entry' for that task that does not
 * get removed when the task calls mutex unlock.
 *
 * Normally, that wouldn't be a problem, other than wasted space.  As far as our
 * tool is concerned, it'd look like a task grabbed a lock and never unlocked.
 * Since it never unlocks, we don't account() for it.  However, if we happen to
 * have a case where we traced an unlock that didn't have a matching lock, that
 * unlock will pair with one of those earlier unmatched locks, and give
 * erroneous results.  That shouldn't happen, but if we miss a lock acquisition
 * path that calls mutex_unlock (but not mutex_lock or trylock) internally, such
 * as the ww_mutex_lock, then we'll think we unlocked an ancient lock.
 *
 * For example, this backtrace from an earlier version of klockstat without the
 * 'enabled' flag and without tracing mutex_trylock incorrectly reports a mutex
 * was held for the duration of the test:
 *
 * ./klockstat -s5 -n1 -d4
 *                         Caller   Avg Hold    Count   Max Hold   Total Hold
 *     modify_ftrace_direct+0x18b 4002597813        1 4002597813   4002597813
 *    bpf_trampoline_update+0x427
 * bpf_trampoline_link_prog+0x133
 *  bpf_tracing_prog_attach+0x26f
 *                SYSC_bpf+0x2b3a
 *                        Max PID 2086, COMM klockstat
 */
bool enabled = false;

struct {
	__uint(type, BPF_MAP_TYPE_STACK_TRACE);
	__uint(max_entries, MAX_ENTRIES);
	__uint(key_size, sizeof(u32));
	__uint(value_size, PERF_MAX_STACK_DEPTH * sizeof(u64));
} stack_map SEC(".maps");

/* depth == nr of locks held, including the currently traced lock. */
struct task_info {
	u32 depth;
};

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, MAX_ENTRIES);
	__type(key, u64);
	__type(value, struct task_info);
} task_map SEC(".maps");

/* Represents a task at a given lock depth, used for lookups. */
struct depth_id {
	u64 task_id;
	u32 depth;
};

struct lockholder_info {
	s32 stack_id;
	u64 task_id;
	u64 try_at;
	u64 acq_at;
	u64 rel_at;
};

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, MAX_ENTRIES);
	__type(key, struct depth_id);
	__type(value, struct lockholder_info);
} lockholder_map SEC(".maps");

/*
 * Keyed by stack_id.
 *
 * Multiple call sites may have the same underlying lock, but we only know the
 * stats for a particular stack frame.  Multiple tasks may have the same
 * stackframe.
 */
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, MAX_ENTRIES);
	__type(key, s32);
	__type(value, struct lock_stat);
} stat_map SEC(".maps");

static bool tracing_task(u64 task_id)
{
	u32 tgid = task_id >> 32;
	u32 pid = task_id;

	if (targ_tgid && targ_tgid != tgid)
		return false;
	if (targ_pid && targ_pid != pid)
		return false;
	return true;
}

static void lock_contended(void *ctx, struct mutex *lock)
{
	u64 task_id;
	struct task_info *ti;
	struct lockholder_info li[1] = {0};
	struct depth_id di = {};

	if (!enabled)
		return;
	if (targ_lock && targ_lock != lock)
		return;
	task_id = bpf_get_current_pid_tgid();
	if (!tracing_task(task_id))
		return;
	ti = bpf_map_lookup_elem(&task_map, &task_id);
	if (!ti) {
		struct task_info fresh = {0};

		bpf_map_update_elem(&task_map, &task_id, &fresh, BPF_ANY);
		ti = bpf_map_lookup_elem(&task_map, &task_id);
		if (!ti)
			return;
	}

	ti->depth += 1;

	li->task_id = task_id;
	/*
	 * Skip 5 frames, e.g.:
	 *       __this_module+0x34ef
	 *       __this_module+0x34ef
	 *       __this_module+0x8c44
	 *      __this_module+0x16efc
	 *             mutex_lock+0x5
	 */
	li->stack_id = bpf_get_stackid(ctx, &stack_map,
				       5 | BPF_F_FAST_STACK_CMP);
	/* Legit failures include EEXIST */
	if (li->stack_id < 0)
		return;
	li->try_at = bpf_ktime_get_ns();

	di.task_id = task_id;
	di.depth = ti->depth;
	bpf_map_update_elem(&lockholder_map, &di, li, BPF_ANY);
}

static void lock_acquired(struct mutex *lock)
{
	u64 task_id;
	struct task_info *ti;
	struct lockholder_info *li;
	struct depth_id di = {};

	if (targ_lock && targ_lock != lock)
		return;
	task_id = bpf_get_current_pid_tgid();
	if (!tracing_task(task_id))
		return;
	ti = bpf_map_lookup_elem(&task_map, &task_id);
	if (!ti)
		return;
	if (!ti->depth)
		return;
	di.task_id = task_id;
	di.depth = ti->depth;
	li = bpf_map_lookup_elem(&lockholder_map, &di);
	if (!li)
		return;

	li->acq_at = bpf_ktime_get_ns();
}

static void account(struct lockholder_info *li)
{
	struct lock_stat *ls;
	u64 delta;

	ls = bpf_map_lookup_elem(&stat_map, &li->stack_id);
	if (!ls) {
		struct lock_stat fresh = {0};

		bpf_map_update_elem(&stat_map, &li->stack_id, &fresh, BPF_ANY);
		ls = bpf_map_lookup_elem(&stat_map, &li->stack_id);
		if (!ls)
			return;
	}

	delta = li->acq_at - li->try_at;
	ls->acq_count++;
	ls->acq_total_time += delta;
	if (delta > ls->acq_max_time) {
		ls->acq_max_time = delta;
		ls->acq_max_id = li->task_id;
		bpf_get_current_comm(ls->acq_max_comm, TASK_COMM_LEN);
	}

	delta = li->rel_at - li->acq_at;
	ls->hld_count++;
	ls->hld_total_time += delta;
	if (delta > ls->hld_max_time) {
		ls->hld_max_time = delta;
		ls->hld_max_id = li->task_id;
		bpf_get_current_comm(ls->hld_max_comm, TASK_COMM_LEN);
	}
}

static void lock_released(struct mutex *lock)
{
	u64 task_id;
	struct task_info *ti;
	struct lockholder_info *li;
	struct depth_id di = {};

	if (targ_lock && targ_lock != lock)
		return;
	task_id = bpf_get_current_pid_tgid();
	if (!tracing_task(task_id))
		return;
	ti = bpf_map_lookup_elem(&task_map, &task_id);
	if (!ti)
		return;
	if (!ti->depth)
		return;
	di.task_id = task_id;
	di.depth = ti->depth;
	ti->depth--;
	li = bpf_map_lookup_elem(&lockholder_map, &di);
	if (!li)
		return;

	li->rel_at = bpf_ktime_get_ns();
	account(li);

	bpf_map_delete_elem(&lockholder_map, &di);
}

static void task_exiting(u64 task_id)
{
	bpf_map_delete_elem(&task_map, &task_id);
}

SEC("fentry/mutex_lock")
int BPF_PROG(mutex_lock, struct mutex *lock)
{
	lock_contended(ctx, lock);
	return 0;
}

SEC("fexit/mutex_lock")
int BPF_PROG(mutex_lock_exit, struct mutex *lock, long ret)
{
	lock_acquired(lock);
	return 0;
}

SEC("fexit/mutex_trylock")
int BPF_PROG(mutex_trylock_exit, struct mutex *lock, long ret)
{
	if (ret) {
		lock_contended(ctx, lock);
		lock_acquired(lock);
	}
	return 0;
}

SEC("fentry/mutex_unlock")
int BPF_PROG(mutex_unlock, struct mutex *lock)
{
	lock_released(lock);
	return 0;
}

SEC("tp_btf/sched_switch")
int BPF_PROG(sched_switch, bool preempt, struct task_struct *prev,
	     struct task_struct *next)
{
	#define TASK_DEAD 0x0080
	if (BPF_CORE_READ(prev, state) != TASK_DEAD)
		return 0;
	task_exiting((u64)prev->tgid << 32 | prev->pid);
	return 0;
}

char LICENSE[] SEC("license") = "GPL";
