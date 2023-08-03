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
#include "third_party/bpf/schedfair.h"
// clang-format on

/*
 * A task's fair share of a multiprocessor at an instant 't' is the nr_cpus /
 * runnable load. ('running' counts as runnable).
 *
 *      f(t) = nr_cpus / runnable_load
 *
 * But what if we want a weighted fair share?  Load is a function of the
 * specific runnables at time t:
 *
 *      f(t) = nr_cpus / wl(p0, p1, p2, ...)
 *
 * When we calculate Weighted Load, we weight each runnable task (p_i) according
 * to its weight w_i.
 *
 *      wl = w0 + w1 + ...
 *
 * wl() depends on t, since the runnable load varies over time, i.e. based on
 * which tasks have woken up.  Let's call it wl(t).
 *
 * A task's share of that wl is: w_x / wl.  Thus a runnable task X's share of
 * the multiprocessor (at time t) is:
 *
 *      f(t, p_x) = nr_cpus * w_x / wl(t)
 *
 * If all the weights = 1, that's: nr_cpus / nr_runnables
 *
 *
 * A task's fair share of the machine might be more than one cpu.  But a Linux
 * task can only run on one cpu at a time.
 *
 *      f(t, p_x) = min(1, nr_cpus * w_x / wl(t))
 *
 * A task's fair share over a window of time is the integral of f(t); consider
 * tiny time slices of 't': we calculate the integral by Sum(f(t_0) * delta_t)
 * over the window.  This F(t)'s units are cpu-seconds.
 *
 * What window(s) do we pick?  For a given task, the windows are when it is
 * runnable.  If a task is asleep, it doesn't count.  Over the length of time of
 * the entire trace, we can compute a task's fair share (from when it was
 * runnable) as well as how much it actually ran.
 *
 * The min() is important here.  For a delta_t, we don't want to accumulate more
 * share than a task could possibly use.  Say we have 100 cpus and only one task
 * is running.  If you ignore the min, it's share of the multiprocessor is 100
 * cpus for that time.  Yet it can't run on 100 cpus.  If we later compare how
 * much time the task ran to its share, we'd think it only ran 1% of its share.
 *
 * Ideally, every time t_i any task blocked or woke up, we'd change wl(t) (add
 * or remove the task's weight), and calculate f(t_i, p_x) * (t_i - t_prev) for
 * every task.  That's a O(nr_runnables) on every block/wake event...  Too much.
 *
 * Forget about specific tasks for a moment.  We can maintain a running
 * calculation of F(t) (the integral) on the fly for a given weight.  Then given
 * that weight's F(t), compute the integral of f(x) over a window as
 * F(t_1) - F(t_0).
 *
 * In essence, instead of computing F(t) for every task, we do it for every
 * weight.  Then for a particular task's runnable window, compute:
 *
 * 	p's fair share for the window = F(t_blocked) - F(t_woke)
 *
 * We'll need to do that for every weight, which the user can set up.  We can't
 * just keep a running integral of the total weighted load, since f(t) depends
 * on the min(1, weighted_share).  For now, let's just deal with a single
 * weight.
 *
 * How do we maintain the running calculation of F(t)?  Ideally, every time a
 * task wakes or blocks, that changes the load (wl).  We could (atomically):
 *
 * 	delta_t = now - last_update;
 * 	for each weight_i
 * 		F += f(wl, weight_i) * delta_t;
 * 	wl += delta_wl;
 * 	last_update = now;
 *
 * Thus every time the load changes, f changes, and we accumulate those changes
 * in F.  However, that's a minor pain to do on every wake/block.  BPF has
 * spinlocks, but doing that update and locking on every wakeup/block on every
 * cpu seems like a potential scalability problem.
 *
 * I considered only updating this on timer ticks from cpu0, which would be once
 * per ms.  However, that is too slow.  Tasks that wake and block very quickly,
 * O(10us) will not have their load measured, nor will they perceive any change
 * in F(t).
 *
 * To avoid major scalability problems or issues with spinlocks, we'll use a 
 * lockless "post/poke" style where if we have multiple cpus trying to update F
 * at the same time, only a single thread does the actual update: atomic_add on
 * delta_wl, bool to control if someone is updating F, atomic_swap(delta_wl, 0)
 * to snarf load changes and apply to F.  Threads that don't do the update may
 * end up using an older value of F for their specific task's F(now) - F(then)
 * calc, which is fine.
 *
 * A note on units:
 * 1) When we compute:
 *
 *      f(t, p_x) = min(1, nr_cpus * w_x / wl(t))
 *
 * ideally we want a float (0,1].  Since we're doing integer math, we'd only get
 * 0 or 1.  To fix that, we'll deal with "millicpus": 1000th of a cpu.
 *
 * 2) bpf_ktime_get_us() returns usec.
 *
 * 3) Put together, F(t) (accum_fair_share) is in *millicpu-usec*.  Same goes
 * for ti->total_cpu_runtime and ti->total_cpu_share.  We're not in danger of
 * overflowing the u64 (20 bits for us, 10 for milli, 8 for 256 cpus, etc.)  If
 * you're looking at these raw numbers, 10^9 millicpu-usec is 1 cpu-sec.
 */

/* Set by userspace before loading. */
const volatile uint32_t nr_milli_cpus;
uint64_t accum_fair_share;	/* a.k.a. F(last_update_time), weight=1. */

uint64_t last_update_time;
uint64_t current_load;	/* wl(last_update_time) */
int64_t delta_load;		/* change in wl since last update */

/* 2-D matrix, indexed by 40 user_prio * 20 'percentile' bins. */
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 40 * 20);
	__type(key, u32);
	__type(value, u64);
	__uint(map_flags, BPF_F_MMAPABLE);
} wtb_fair_percentiles SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, MAX_PIDS);
	__type(key, u32);
	__type(value, struct task_info);
} tasks SEC(".maps");

/*
 * Single threaded, called via 'poke' when tasks block or wake.  Specifically
 * when there is a change to wl, where the change is delta_load.
 */
static void __update_accum(void)
{
	u64 now, delta_t;
	s64 snarfed_load = 0;
	int weight = 1;
	u64 f_t;

	now = bpf_ktime_get_us();
	snarfed_load = __atomic_exchange_n(&delta_load, 0, __ATOMIC_RELAXED);

	/* Only happens the first time we update */
	if (!last_update_time)
		goto out;
	/*
	 * Having no load is valid.  There would have been no tasks with
	 * load_tracked, nor anyone saving accum_fair_share in
	 * ti->share_at_wake, so we do not need to update accum_fair_share.
	 * i.e. if any task was accumulating share, then it should have been in
	 * current_load.
	 *
	 * Since our update is racy, there may be times where threads are
	 * reading accum_fair_share, and delta_load != 0, but current_load is 0.
	 * This is fine.  The only thing going on here is that if current_load =
	 * 0, that's for the time interval covered by delta_t.  For that time
	 * interval, no load => no tasks runnable => don't bother updating F(t).
	 */
	if (!current_load)
		goto out;

	delta_t = now - last_update_time;

	/*
	 * TODO: support multiple weights, set from userspace.  We could
	 * have them provide a mapping from priority to weight.  For each
	 * weight, update F_w(t).
	 */
	f_t = min(1000, (nr_milli_cpus * weight) / current_load);
	WRITE_ONCE(accum_fair_share, accum_fair_share + f_t * delta_t);

out:
	last_update_time = now;
	current_load += snarfed_load;

	/*
	 * snarfed_load can be negative (and often will be), but we should never
	 * have current_load < 0.
	 */
	if ((s64)current_load < 0)
		current_load = 0;
}

static uint64_t in_progress = false;

/*
 * When we change wl (by adding/subbing delta_load, aka "posting" a change),
 * poke to make sure someone updates F(t).  Only a single thread will do the
 * update; it may be us.
 */
static void poke_update_accum(void)
{
	/*
	 * Technically these atomics protect other memory accesses ordered by
	 * in_progress, but not arbitrary other accesses, such as delta_load.
	 * The caller's change to delta_load would be visible, but we may miss
	 * another task's update to delta_load.  Since we're okay with
	 * occasionally missing a "post" to delta_load, it's not worth worrying
	 * about.  Similarly, since we're OK with "best effort poking", if we
	 * see in_progress, we can skip the atomic.
	 */
	if (READ_ONCE(in_progress) ||
	    __atomic_exchange_n(&in_progress, true, __ATOMIC_ACQUIRE))
		return;
	/*
	 * In a classic "post/poke" scenario, the updater would keep updating so
	 * long as someone is posting more work.  Since this is BPF, we have to
	 * bound our loops, and any handoff we could do to another poker is
	 * complicated.
	 *
	 * Simpler to just try a few times.  If we miss a delta_load, the next
	 * poker will get it.  We're already in the approximation business since
	 * we let posters read F(t) without waiting for the poke to complete.
	 */
	for (int i = 0; i < 4; i++) {
		/*
		 * Update at least once, even if delta_load == 0.  It's important
		 * to keep F(t) up to date.  We could have a case where one task
		 * blocks and another wakes at the same time, so delta_load ==
		 * 0, and we still need to set F(t) += current_load * delta_t.
		 * Especically since the tasks that block/wake are going to
		 * read F(t) and want an up-to-date value.
		 */
		__update_accum();
		if (!READ_ONCE(delta_load))
			break;
	}
	/*
	 * smp_store_release.  clang (-target bpf) barfs on any variant of
	 * __atomic_store_n(&in_progress, false, __ATOMIC_RELEASE);
	 */
	asm volatile ("" ::: "memory");
	WRITE_ONCE(in_progress, false);
}

static int ti_weight(struct task_info *ti)
{
	/* TODO: use user_prio + some mapping from the user */
	return 1;
}

/* Equivalent to the kernel's TASK_USER_PRIO().  Returns a value in [0,39]. */
static int task_user_prio(struct task_struct *p)
{
	return BPF_CORE_READ(p, static_prio) - MAX_RT_PRIO;
}

static struct task_info *get_task_info(struct task_struct *p)
{
	pid_t pid = BPF_CORE_READ(p, pid);
	struct task_info fresh = {0};
	struct task_info *ti = bpf_map_lookup_elem(&tasks, &pid);

	if (ti)
		return ti;
	bpf_map_update_elem(&tasks, &pid, &fresh, BPF_ANY);
	return bpf_map_lookup_elem(&tasks, &pid);
}

/*
 * Idempotent: we now know the task is runnable.
 *
 * We could have a case where we wakeup and block, but the block isn't traced
 * yet.  Then we'll wake up again.  We'll see load_tracked still and ignore the
 * wakeup.  The only effect is that we may think a task was runnable longer than
 * it was, but only when we first start tracing.
 */
static void task_wakeup(struct task_struct *p)
{
	struct task_info *ti = get_task_info(p);

	if (!ti)
		return;

	/* Wakeups may happen while a task is still on_rq, e.g. ttwu_remote() */
	if (ti->load_tracked)
		return;
	ti->load_tracked = true;
	__atomic_fetch_add(&delta_load, ti_weight(ti), __ATOMIC_RELAXED);
	poke_update_accum();
	ti->share_at_wake = READ_ONCE(accum_fair_share);
	ti->cpu_runtime_since_wake = 0;

	/* Lazily keep this up-to-date. */
	ti->user_prio = task_user_prio(p);
}

static void task_run(struct task_struct *p)
{
	struct task_info *ti = get_task_info(p);

	if (!ti)
		return;
	ti->ran_at = bpf_ktime_get_us();
}

/* For each run-to-block, track how well this task did. */
static void account_wake_to_block(struct task_info *ti, u64 cpu_runtime,
				  u64 cpu_share)
{
	/*
	 * There are 20 'fairness' bins, 0 through 19.  "Task Fairness"
	 * (runtime divided by share) ratios from 0 to 2x are divided into these
	 * 20 bins, with each bin covering 10%.  Anything over 2x gets clamped
	 * to 2x (actually 1.9x).  In our output, we round down each bin in the
	 * column header.  Column headers greater than 1 elide the '.', e.g.
	 * 1.5x is called '15'
	 *
	 * e.g. cpu_runtime/cpu_share is mapped to a bin and column header:
	 * [  0, 0.1) -> bin 0,  header ".0"
	 * [0.1, 0.2) -> bin 1,  header ".1"
	 * ...
	 * [0.9, 1.0) -> bin 9,  header ".9"
	 * [1.0, 1.1) -> bin 10, header "1x"
	 * [1.1, 1.2) -> bin 11, header "11"
	 * ...
	 * [1.9, inf) -> bin 19, header "19"
	 *
	 * Note we don't bother scaling runtime by weight.  Userspace can do
	 * that if we want.
	 */
	int fairness = min(19, (10 * cpu_runtime) / cpu_share);
	u32 idx = ti->user_prio * 20 + fairness;
	u64 *bin;

	bin = bpf_map_lookup_elem(&wtb_fair_percentiles, &idx);
	if (!bin)
		return;
	__atomic_fetch_add(bin, 1, __ATOMIC_RELAXED);
}

/*
 * Pairs with task_wakeup: this task is no longer accumulating fair share.  Grab
 * the F(now) - F(wake_time) to collect our share from this runnability window.
 */
static void task_block(struct task_struct *p)
{
	struct task_info *ti = get_task_info(p);
	u64 cpu_share = 0;

	if (!ti)
		return;
	if (!ti->load_tracked) {
		/*
		 * You might get this if a task was running when we started
		 * tracing.
		 */
		return;
	}
	ti->load_tracked = false;
	__atomic_fetch_sub(&delta_load, ti_weight(ti), __ATOMIC_RELAXED);
	poke_update_accum();

	/*
	 * It's legit for share_at_wake to be 0, but only when the tracing first
	 * starts and the updater hasn't run yet.
	 */
	cpu_share = READ_ONCE(accum_fair_share) - ti->share_at_wake;
	ti->total_cpu_share += cpu_share;

	if (cpu_share && ti->cpu_runtime_since_wake)
		account_wake_to_block(ti, ti->cpu_runtime_since_wake,
				      cpu_share);
}

static void task_stop(struct task_struct *p)
{
	struct task_info *ti = get_task_info(p);
	u64 cpu_runtime = 0;

	if (!ti)
		return;
	if (!ti->ran_at) {
		/*
		 * You might get this if a task was running when we started
		 * tracing.
		 */
		return;
	}
	cpu_runtime = 1000 * (bpf_ktime_get_us() - ti->ran_at);
	ti->cpu_runtime_since_wake += cpu_runtime;
	ti->total_cpu_runtime += cpu_runtime;
}

static bool task_is_idle(struct task_struct *p)
{
	#define PF_IDLE 0x2	/* linux/sched.h */
	u32 flags = BPF_CORE_READ(p, flags);

	return flags & PF_IDLE;
}

SEC("tp_btf/sched_wakeup")
int BPF_PROG(sched_wakeup, struct task_struct *p)
{
	if (!task_is_idle(p))
		task_wakeup(p);
	return 0;
}

SEC("tp_btf/sched_wakeup_new")
int BPF_PROG(sched_wakeup_new, struct task_struct *p)
{
	if (!task_is_idle(p))
		task_wakeup(p);
	return 0;
}

SEC("tp_btf/sched_switch")
int BPF_PROG(sched_switch, bool preempt, struct task_struct *prev,
	     struct task_struct *next)
{
	if (!task_is_idle(prev)) {
		task_stop(prev);
		if (!preempt && BPF_CORE_READ(prev, state) != TASK_RUNNING)
			task_block(prev);
	}
	if (!task_is_idle(next)) {
		/*
		 * There are at least two ways where task_wakeup is necessary
		 * here:
		 * 1) switchto: tasks block, but the only tracepoint for when
		 * they wake is when we sched_switch to them.
		 * 2) the task was running/runnable when we started tracing.
		 * e.g. a spinning task that does not block.
		 */
		task_wakeup(next);
		task_run(next);
	}

	return 0;
}

char LICENSE[] SEC("license") = "GPL";
