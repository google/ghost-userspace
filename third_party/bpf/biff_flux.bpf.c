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

/* Biff scheduler implementation for Flux. */

#define biff_cpu(s, cpu) (cpu)->__sched_cpu_union(s).biff

#define biff_arr_list_pop_first(head, field)				\
	arr_list_pop_first(__t_arr, FLUX_MAX_GTIDS, head, field)

#define biff_arr_list_remove(head, elem, field)				\
	arr_list_remove(__t_arr, FLUX_MAX_GTIDS, head, elem, field)

#define biff_arr_list_insert_head(head, elem, field)			\
	arr_list_insert_head(__t_arr, FLUX_MAX_GTIDS, head, elem, field)

#define biff_arr_list_insert_tail(head, elem, field)			\
	arr_list_insert_tail(__t_arr, FLUX_MAX_GTIDS, head, elem, field)

static void biff_request_for_cpus(struct flux_sched *b, int child_id,
				  int nr_cpus, int *ret)
{
	/* Never called, we have no children. */
}

static void biff_cpu_allocated(struct flux_sched *b, struct flux_cpu *cpu)
{
	/* Don't care.  We'll just pick a task in PNT */
}

static void biff_cpu_returned(struct flux_sched *b, int child_id,
			      struct flux_cpu *cpu)
{
	/* Never called, we have no children. */
}

static void biff_cpu_preempted(struct flux_sched *b, int child_id,
			       struct flux_cpu *cpu)
{
	/* Don't care.  We'll get a message for the task preemption. */
}

static void biff_cpu_preemption_completed(struct flux_sched *b, int child_id,
					  struct flux_cpu *cpu)
{
	/*
	 * We don't do remote cpu preemption, and the local cpu preemption is
	 * sorted out with times_up.
	 */
}

static void biff_cpu_ticked(struct flux_sched *b, int child_id,
			    struct flux_cpu *cpu)
{
	struct flux_thread *t;

	/*
	 * Accounting and sync are easy: this function runs on the cpu, so we can
	 * freely access the cpu and the current thread structs  If we wanted to
	 * remotely preempt, we'd need to be a little more careful, since the
	 * thread could be concurrently yielding/departing/etc.
	 */
	if (!biff_cpu(b, cpu).current)
		return;
	t = gtid_to_thread(biff_cpu(b, cpu).current);
	if (!t)
		return;
	/* Arbitrary policy: kick anyone off cpu after 50ms
	 *
	 * We'll clear times_up when we run a thread.  You could try and clear
	 * it in yield/block/preempt/etc, but there's no need.
	 *
	 * Also, you'd run into issues with switchto, in that the thread that
	 * started the ST chain won't actually yield/block later, and we might
	 * have already gotten the ST chain start message.
	 *
	 * Note that when there's an ST chain, we keep tracking the originating
	 * thread as current.  And if that thread departs before the chain
	 * resolves, we'll have a period of time where we have no current thread
	 * until the next yield/block/preempt.  We'll need more ST messages for
	 * that.  We could work around that by tracking ran_at in the cpu, but
	 * it's not worth it.
	 *
	 * Alternatively, in PNT, we could just recheck the policy: now - ran_at
	 * > 50ms.  If you had a more complicated policy, you might not even
	 * bother with times_up.  This function is essentially
	 * check_preempt_curr().
	 */
	if (bpf_ktime_get_us() - t->biff.ran_at > 50000) {
		t->biff.times_up = true;
		flux_preempt_cpu(b, cpu);
	}
}

/*
 * Biff POLICY: dumb global fifo.  No locality, etc.
 *
 * nr_cpus_wanted == nr tasks running + nr tasks runnable.  i.e. "on_rq" in
 * linux terms.  Though biff's runqueue is just the runnable-but-not-running.
 * i.e. the on_rq but not the on_cpu.
 *
 * Anytime a task becomes runnable (which happens before running on_cpu), we
 * want another cpu.  Whenever a task is no longer runnable or running, we want
 * one less cpu.
 *
 * Functions beginning with __ are biff-internal helpers.  Functions beginning
 * with biff_ are flux sched ops.
 */
static void __biff_enqueue_task(struct flux_sched *b, struct flux_thread *t)
{
	struct flux_thread *__t_arr = get_thread_array();

	if (!__t_arr)
		return;

	/* Catchall for any path a task took to get off cpu */
	t->biff.times_up = false;

	bpf_spin_lock(&b->lock);
	biff_arr_list_insert_tail(&b->biff.rq, t, biff.link);
	t->biff.enqueued = true;
	bpf_spin_unlock(&b->lock);
}

/* Returns a refcounted thread.  Free with flux_thread_decref(). */
static struct flux_thread *__biff_pick_first_to_run(struct flux_sched *b,
						    struct flux_cpu *cpu)
{
	struct flux_thread *__t_arr = get_thread_array();
	struct flux_thread *t;

	if (!__t_arr)
		return NULL;
	bpf_spin_lock(&b->lock);
	t = biff_arr_list_pop_first(&b->biff.rq, biff.link);
	if (t) {
		t->biff.enqueued = false;
		flux_prepare_to_run(t, cpu);
	}
	bpf_spin_unlock(&b->lock);
	return t;
}

static void __biff_dequeue_task(struct flux_sched *b, struct flux_thread *t)
{
	struct flux_thread *__t_arr = get_thread_array();

	if (!__t_arr)
		return;
	bpf_spin_lock(&b->lock);
	if (t->biff.enqueued) {
		biff_arr_list_remove(&b->biff.rq, t, biff.link);
		t->biff.enqueued = false;
	}
	bpf_spin_unlock(&b->lock);
}

static void __biff_task_started(struct flux_sched *b, struct flux_thread *t,
				int cpu_id, u64 at_us)
{
	struct flux_cpu *cpu;

	cpu = cpuid_to_cpu(cpu_id);
	if (!cpu)
		return;
	biff_cpu(b, cpu).current = t->f.gtid;

	t->biff.ran_at = at_us;
}

static void __biff_task_stopped(struct flux_sched *b, struct flux_thread *t,
				int cpu_id, u64 at_us)
{
	struct flux_cpu *cpu;

	/*
	 * Q: Hey, didn't we record t->biff.cpu?  Why do we need the cpu from
	 * the message?
	 * A: switchto!  We only hear about switchto chain starts.  We know when
	 * a task that we scheduled switchtos another task, but when a task
	 * that was switchto_waiting runs and then blocks/yields/preempts, we
	 * only find out about it when it stops.
	 */
	cpu = cpuid_to_cpu(cpu_id);
	if (!cpu)
		return;
	biff_cpu(b, cpu).current = 0;

	t->biff.ran_until = at_us;
}

static void __biff_task_runnable(struct flux_sched *b, struct flux_thread *t,
				 u64 at_us)
{
	/*
	 * Arguably setting runnable_at isn't quite right for a latch_failure.
	 * It's not quite "time we got on_rq" either, since we use it for
	 * preempts.  But a latch_failure is just as hokey as using this for
	 * preempt for scheduling policy.  (Consider a prio change the moment
	 * after you latch (a preempt with was_latched, then prio change) versus
	 * the moment before you latch (a prio change, then a latch failure).
	 */
	t->biff.runnable_at = at_us;
	__biff_enqueue_task(b, t);
}

static void biff_pick_next_task(struct flux_sched *b, struct flux_cpu *cpu,
				struct bpf_ghost_sched *ctx)
{
	struct flux_thread *t, *current;

	current = gtid_to_thread(biff_cpu(b, cpu).current);
	if (current && !current->biff.times_up) {
		flux_run_current(cpu, ctx);
		return;
	}
	t = __biff_pick_first_to_run(b, cpu);
	if (!t) {
		if (current) {
			current->biff.times_up = false;
			flux_run_current(cpu, ctx);
		} else {
			flux_cpu_yield(b, cpu);
		}
		return;
	}
	if (flux_run_thread(t, cpu, ctx) == 0) {
		/*
		 * This is how you do per-cpu tracking for migration.  See the
		 * Treatise for details.
		 */
		t->biff.cpu = cpu->f.id;
	}
}

static void biff_thread_new(struct flux_sched *b, struct flux_thread *t,
			    struct flux_args_new *new)
{
	u64 now = bpf_ktime_get_us();

	t->biff.ran_at = 0;
	t->biff.ran_until = now;
	t->biff.runnable_at = 0;
	t->biff.enqueued = false;
	t->biff.times_up = false;
	t->biff.cpu = 0;

	if (t->f.on_rq)
		flux_request_cpus(b, 1);
}

static void biff_thread_on_cpu(struct flux_sched *b, struct flux_thread *t,
			       struct flux_args_on_cpu *on_cpu)
{
	__biff_task_started(b, t, on_cpu->cpu, bpf_ktime_get_us());
	t->biff.cpu = on_cpu->cpu;
}

static void biff_thread_blocked(struct flux_sched *b, struct flux_thread *t,
				struct flux_args_blocked *block)
{
	__biff_task_stopped(b, t, block->cpu, bpf_ktime_get_us());
	flux_request_cpus(b, -1);
}

static void biff_thread_runnable(struct flux_sched *b, struct flux_thread *t,
				 struct flux_args_runnable *runnable)
{
	__biff_task_runnable(b, t, bpf_ktime_get_us());
}

static void biff_thread_wakeup(struct flux_sched *b, struct flux_thread *t,
			       struct flux_args_wakeup *wakeup)
{
	__biff_task_runnable(b, t, bpf_ktime_get_us());
	flux_request_cpus(b, 1);
}

static void biff_thread_yielded(struct flux_sched *b, struct flux_thread *t,
				struct flux_args_yield *yield)
{
	u64 now = bpf_ktime_get_us();

	__biff_task_stopped(b, t, yield->cpu, now);
	/*
	 * If you wanted to, you could switch schedulers here.  Instead of
	 * marking the task runnable, you could hand it off to another
	 * scheduler.  But since we just lost a runnable, don't forget to change
	 * nr_cpus_wanted!  e.g.
		flux_request_cpus(b, -1);
		flux_join_scheduler(t, FLUX_SCHED_SOME_OTHER, true);
	 */
	__biff_task_runnable(b, t, now);
}

static void biff_thread_preempted(struct flux_sched *b, struct flux_thread *t,
				  struct flux_args_preempt *preempt)
{
	u64 now = bpf_ktime_get_us();

	if (!preempt->was_latched)
		__biff_task_stopped(b, t, preempt->cpu, now);
	__biff_task_runnable(b, t, now);
}

static void biff_thread_switchto(struct flux_sched *b, struct flux_thread *t,
				 struct flux_args_switchto *switchto)
{
	/*
	 * TODO Leave biff.current set to the old task.  We have no idea
	 * who is running now, but can blame the switchto chain starter.  See
	 * notes in flux_api.c handle_switchto().
	 */
	t->biff.ran_until = bpf_ktime_get_us();
}

static void biff_thread_affinity_changed(struct flux_sched *b,
					 struct flux_thread *t,
					 struct flux_args_affinity_changed *a)
{
	/* Nothing we can do from BPF yet - we don't know the affinity! */
}

static void biff_thread_prio_changed(struct flux_sched *b,
				     struct flux_thread *t,
				     struct flux_args_prio_changed *prio)
{
	/*
	 * Note the thread could be pending_latch.  If you want to muck with
	 * anything related to the sched structure, or if you care about
	 * pending_latch, you need to lock the sched before looking.
	 *
	 * You could change schedulers here (if you know the sched id, but
	 * that's a different story), but don't tell the new sched the
	 * thread is runnable unless you actually know that.  i.e. grab the
	 * sched lock to verify the thread state.
	 */
}

static void biff_thread_departed(struct flux_sched *b, struct flux_thread *t,
				 struct flux_args_departed *departed)
{
	/*
	 * Several options for t.  First off, it could be not on_rq at all (e.g.
	 * blocked).  on_rq breaks down into the following:
	 * - on_cpu.  Flux tracks this, but the kernel also tells us via
	 *   was_current.
	 * - latched: actually, we should get a latched_preemption first, so
	 *   the task would have been reenqueued.
	 * - enqueued on the biff RQ
	 * - pending_latch: not on the RQ.  as far as the kernel is concerned,
	 *   the ball is still ours.  PNT is trying to run it on another cpu.
	 */

	if (departed->was_current)
		__biff_task_stopped(b, t, departed->cpu, bpf_ktime_get_us());

	/*
	 * Will dequeue the task it if was enqueued.  If it was pending_latch,
	 * it won't be enqueued.  Note that if you care about pending_latch, you
	 * can only check for it while holding the biff lock.
	 */
	__biff_dequeue_task(b, t);

	/* Running or runnable means the task contributed to load. */
	if (t->f.on_rq)
		flux_request_cpus(b, -1);
}

static void biff_thread_dead(struct flux_sched *b, struct flux_thread *t,
			     struct flux_args_dead *dead)
{
	/* Dead tasks already blocked, so they aren't contributing to load. */
}
