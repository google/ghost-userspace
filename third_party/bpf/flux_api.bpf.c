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

/* Manually #include this C file in your BPF program after your sched ops. */

#ifndef pre_thread_new
#define pre_thread_new(...)
#endif
#ifndef post_thread_new
#define post_thread_new(...)
#endif

#ifndef pre_thread_on_cpu
#define pre_thread_on_cpu(...)
#endif
#ifndef post_thread_on_cpu
#define post_thread_on_cpu(...)
#endif

#ifndef pre_thread_blocked
#define pre_thread_blocked(...)
#endif
#ifndef post_thread_blocked
#define post_thread_blocked(...)
#endif

#ifndef pre_thread_runnable
#define pre_thread_runnable(...)
#endif
#ifndef post_thread_runnable
#define post_thread_runnable(...)
#endif

#ifndef pre_thread_wakeup
#define pre_thread_wakeup(...)
#endif
#ifndef post_thread_wakeup
#define post_thread_wakeup(...)
#endif

#ifndef pre_thread_yielded
#define pre_thread_yielded(...)
#endif
#ifndef post_thread_yielded
#define post_thread_yielded(...)
#endif

#ifndef pre_thread_preempted
#define pre_thread_preempted(...)
#endif
#ifndef post_thread_preempted
#define post_thread_preempted(...)
#endif

#ifndef pre_thread_switchto
#define pre_thread_switchto(...)
#endif
#ifndef post_thread_switchto
#define post_thread_switchto(...)
#endif

#ifndef pre_thread_affinity_changed
#define pre_thread_affinity_changed(...)
#endif
#ifndef post_thread_affinity_changed
#define post_thread_affinity_changed(...)
#endif

#ifndef pre_thread_prio_changed
#define pre_thread_prio_changed(...)
#endif
#ifndef post_thread_prio_changed
#define post_thread_prio_changed(...)
#endif

#ifndef pre_thread_departed
#define pre_thread_departed(...)
#endif
#ifndef post_thread_departed
#define post_thread_departed(...)
#endif

#ifndef pre_thread_dead
#define pre_thread_dead(...)
#endif
#ifndef post_thread_dead
#define post_thread_dead(...)
#endif


/********************* FLUX API *********************/

/*
 * In general:
 * - s is the caller's sched struct (s == self).
 * - The child_id is an int: the ID of a particular scheduler, e.g.
 *   FLUX_SCHED_BIFF.
 * - A scheduler's callbacks will take its self pointer (s), but typically refer
 *   to other schedulers by ID.
 */

static int flux_request_cpus(struct flux_sched *s, int nr_cpus)
{
	struct flux_sched *p = get_parent(s);

	if (!p)
		return -1;
	__sync_fetch_and_add(&s->f.nr_cpus_wanted, nr_cpus);
	return flux_request_for_cpus(p, s->f.id, nr_cpus);
}

static void flux_cpu_grant(struct flux_sched *s, int child_id,
			   struct flux_cpu *cpu)
{
	struct flux_sched *child = get_sched(child_id);

	if (!child)
		return;
	__sync_fetch_and_add(&child->f.nr_cpus, 1);
	cpu->f.current_sched = child_id;
	flux_cpu_allocated(child, cpu);
}

/*
 * Preempts the calling cpu from all schedulers in the hierarchy up to but not
 * including 'tier'.  Runs the preemption_completed callback at 'tier'.
 * Implicit in preemption_completed() is that you are the current_sched, and it
 * is back under your control.
 *
 * Recall tier 0 is a kernel-level preemption, which preempts all schedulers.
 * FLUX_SCHED_NONE (tier 0) has no preemption_completed callback, but we could
 * have dummy function for it, especially if we ever wanted to 'ack' a
 * kernel-level preemption or do some bookkeeping.  Though we have the BUSY
 * handler for that.
 */
static void flux_preempt_up_to(struct flux_cpu *cpu, int tier)
{
	struct flux_sched *s;
	int prev_child_id = FLUX_SCHED_NONE;

	for (int i = 0; i < FLUX_MAX_NR_TIERS; i++) {
		/*
		 * Lower value tiers are higher in the hierarchy.  Don't preempt
		 * anyone higher than (or equal to) tier.
		 *
		 * 'break', so that we run preemption_completed on 'tier'.
		 */
		if (sched_id_to_tier(cpu->f.current_sched) <= tier)
			break;
		s = get_sched(cpu->f.current_sched);
		if (!s)
			return;

		__sync_fetch_and_add(&s->f.nr_cpus, -1);
		flux_cpu_preempted(s, prev_child_id, cpu);

		prev_child_id = cpu->f.current_sched;
		cpu->f.current_sched = get_parent_id(s);
	}
	if (sched_id_to_tier(cpu->f.current_sched) != tier)
		bpf_printd("cpu failed to preempt_to tier %d, current_sched %d",
			   tier, cpu->f.current_sched);
	s = get_sched(cpu->f.current_sched);
	if (!s)
		return;
	flux_cpu_preemption_completed(s, prev_child_id, cpu);
}

/*
 * Trigger a preemption on the (possibly remote) cpu up to sched s's tier.  The
 * sched uses this to gain control of its cpu from a thread or child scheduler.
 * It's possible the sched's child will yield to us before the premption handler
 * runs.  It's the caller's responsibility to handle this.  See e.g.
 * roci_request_for_cpus()
 */
static void flux_preempt_cpu(struct flux_sched *s, struct flux_cpu *cpu)
{
	int64_t my_tier = sched_id_to_tier(s->f.id);
	int64_t preempt_to;

	/*
	 * Bounded loop to convince the verifier we complete.
	 *
	 * Plus, there shouldn't be more than FLUX_MAX_NR_TIERS concurrent,
	 * failing, preempters.  The only way for the CAS to fail is when
	 * preempt_to was changed.  Since preempt_to can only be changed to
	 * lower-tier-values (higher tiers), you can only have MAX_NR_TIERS
	 * changes total.
	 *
	 * If we happen to be preempting during PNT, where preempt_to got
	 * changed, we could have a scenario where you'd repeat this loop more
	 * than FLUX_MAX_NR_TIERS.  Doing it twice (hence the * 2) would require
	 * PNT to pick a task, that task rescheds, and we reenter BPF-PNT before
	 * we made it through the loop another set of MAX_NR_TIERS failures.
	 */
	for (int i = 0; i < FLUX_MAX_NR_TIERS * 2; i++) {
		preempt_to = READ_ONCE(cpu->f.preempt_to);
		/*
		 * Lower value tiers are higher in the hierarchy.  The cpu is
		 * already set to preempt us.
		 */
		if (preempt_to <= my_tier)
			break;
		if (__sync_bool_compare_and_swap(&cpu->f.preempt_to, preempt_to,
						 my_tier)) {
			/*
			 * RESCHED_ANY: in the off chance the cpu is in CFS,
			 * this might be an excessive IPI.  But if/when we have
			 * ghost above CFS, I might model yielding to CFS as a
			 * scheduler that we could preempt.
			 */
			bpf_ghost_resched_cpu2(cpu->f.id, RESCHED_ANY_CLASS);
			break;
		}
	}
}

static void flux_cpu_yield(struct flux_sched *s, struct flux_cpu *cpu)
{
	struct flux_sched *p = get_parent(s);

	if (!p)
		return;
	cpu->f.current_sched = p->f.id;
	__sync_fetch_and_add(&s->f.nr_cpus, -1);
	flux_cpu_returned(p, s->f.id, cpu);
}

/*
 * Call this while holding your scheduler lock when you select the thread to
 * run.  After unlocking, you must pass it to flux_run_thread().
 */
static void flux_prepare_to_run(struct flux_thread *t, struct flux_cpu *cpu)
{
	/*
	 * We can't store latched_seqnum in t, since we could receive a message
	 * in parallel (another cpu) that sees pending_latch and could put the
	 * task back on a runqueue, which could get picked up by another PNT.
	 *
	 * But this cpu's pcpu data is a safe place to store it.  We could
	 * return it to our caller, but then our thread schedulers' lives are a
	 * little worse.
	 */
	cpu->f.latched_seqnum = READ_ONCE(t->f.seqnum);
	t->f.pending_latch = true;
	/*
	 * The instant we unlock the biff lock, we could have a pending departed
	 * message that sees pending_latch, has the ball, and tries to delete
	 * the thread.  Protect the lifetime of t.  Decreffed by
	 * flux_run_thread().
	 */
	flux_thread_incref(t);
}

/*
 * Pairs with flux_prepare_to_run().  Call this to attempt to latch a task.  If
 * it fails, you'll get the thread back via either a message handler that saw
 * pending_latch or LATCH_FAILURE.
 *
 * Returns 0 on success.  On success, you can touch t still, but otherwise you
 * cannot.
 */
static int flux_run_thread(struct flux_thread *t, struct flux_cpu *cpu,
			   struct bpf_ghost_sched *ctx)
{
	int err;

	err = bpf_ghost_run_gtid(t->f.gtid, cpu->f.latched_seqnum,
				 SEND_TASK_ON_CPU);
	if (!err) {
		/* We have the ball */
		cpu->f.pnt_success = true;
		t->f.pending_latch = false;
		flux_thread_decref(t);

		return 0;
	}
	/*
	 * We failed to latch.  We still hold a ref on the thread, so the memory
	 * isn't going away.  However, the thread might already be re-enqueued
	 * elsewhere.  LATCH_FAILURE or a message handler that saw pending_latch
	 * could have taken the ball and reenqueued it.  Another cpu's PNT could
	 * even have set pending_latch again.  So don't touch the thread
	 * further.
	 *
	 * Note that this all implies that on error, we never have the ball.
	 * Even if we got ENOENT and see pending_latch, that could be from
	 * another PNT attempt.  If we tried to run a gtid that was never in
	 * ghost or a very old one, we'll also get ENOENT and see pending_latch.
	 * But in that scenario, we have bigger problems: e.g. where did you
	 * even get that thread struct from.
	 */
	flux_thread_decref(t);

	/*
	 * Remember that we don't have the ball, and in all likelihood, the task
	 * was already handed back to our scheduler - we're just finding out
	 * about it now.
	 *
	 * It's always safe to restart PNT.  This cpu still belongs to some
	 * scheduler, so we'll call its PNT again.
	 *
	 * If we simply return to our scheduler directly, they'll return to PNT
	 * and try again from the loop in flux_pnt().  However, for some error
	 * cases, such as EBUSY, we want to get the task off cpu quickly.  The
	 * fastest way to do that is by forcing a restart of PNT.
	 */

	switch (-err) {
	case ENOENT:
		/*
		 * task departed and exited, ignore.  This can still happen even
		 * though we yank the task in the departed callback.  Departed
		 * could arrive after we dequeued from the sched's list.
		 */
		break;
	case ESTALE:
		/*
		 * task_barrier is old.  Could be a prio_change, departed, etc.
		 */
		break;
	case EBUSY:
		/*
		 * task is still on_cpu.  this happens when it was preempted (we
		 * got the message early in PNT), and we are trying to pick what
		 * runs next, but the task hasn't actually gotten off cpu yet.
		 *
		 * TODO: one option would be to change the kernel to allow
		 * some classes of EBUSY to relatch the task.  (Probably not
		 * when we resolve switchto chains though).
		 */
		flux_restart_pnt(cpu, ctx);
		break;
	case EXDEV:
	case ERANGE:
	case EINVAL:
	case ENOSPC:
	default:
		/*
		 * Various issues, none of which should happen from PNT,
		 * since we are called from an online cpu in the
		 * enclave with an agent.  Though as we change the
		 * kernel, some of these may occur.
		 *   - ERANGE: cpu is offline
		 *   - EXDEV: cpu is not in the enclave
		 *   - EINVAL: Catchall that shouldn't happen.  Other than stuff
		 *   like "bad run flags", another scenario is "no agent task".
		 *   That shouldn't happen, since we run bpf-pnt only if there
		 *   is an agent task (currently!).
		 */
		bpf_printd("failed to run %p, err %d\n", t->f.gtid, err);
		flux_restart_pnt(cpu, ctx);
		break;
	}
	return err;
}

/* Call from PNT.  Allows the cpu to idle. */
static void flux_run_idle(struct flux_cpu *cpu, struct bpf_ghost_sched *ctx)
{
	cpu->f.pnt_success = true;
	clr_dont_idle(ctx);
	set_must_resched(ctx);
}

/* Call from PNT.  Runs the thread currently on the cpu, a.k.a. "prev". */
static void flux_run_current(struct flux_cpu *cpu, struct bpf_ghost_sched *ctx)
{
	cpu->f.pnt_success = true;
	/*
	 * If the kernel set must_resched, pick_next_task_ghost() will not pick
	 * the current task (prev).  All we can do is make sure the cpu doesn't
	 * idle.  We'll get a preempt for current, it'll get off cpu, and we can
	 * pick it again the next time around through PNT.
	 */
	set_dont_idle(ctx);
}

/*
 * Call from PNT.  Will trigger a 'restart', such that BPF-PNT restarts.
 * Use this if you want to run an EBUSY task.
 */
static void flux_restart_pnt(struct flux_cpu *cpu, struct bpf_ghost_sched *ctx)
{
	cpu->f.pnt_success = true;	/* short circuit the pnt loop */
	set_dont_idle(ctx);
	set_must_resched(ctx);
}

/*
 * Add a thread to a scheduler.  A thread can belong to one scheduler at a time.
 *
 * If you're switching from one scheduler to another, the caller needs to make
 * sure it is done with the thread, e.g. removed from runqueues, accounted for
 * the runnable load (biff's cpu policy), etc.
 *
 * Caller is responsible for synchronization.  i.e. don't call this if 't' is
 * running on another cpu concurrently.  Act as if the thread had departed and
 * returned to ghost.
 */
static void flux_join_scheduler(struct flux_thread *t, int new_sched_id,
				bool runnable)
{
	struct flux_args_new new_f;
	struct flux_args_runnable runnable_f;

	t->f.sched = new_sched_id;
	flux_thread_new_thr(t, t->f.seqnum, &new_f);
	if (runnable)
		flux_thread_runnable_thr(t, t->f.seqnum, &runnable_f);
}

bool user_initialized;

#define FLUX_TIER_NO_PREEMPT FLUX_MAX_NR_TIERS

SEC("ghost_sched/pnt")
int flux_pnt(struct bpf_ghost_sched *ctx)
{
	struct flux_cpu *cpu;
	uint64_t preempt_to;

	_Static_assert(sizeof(struct bpf_spin_lock) == sizeof(uint32_t),
		       "struct flux_sched lock field size mismatch");

	/*
	 * Lots of ways to fail.  The only time we clear dont_idle is when the
	 * idle_sched owns the cpu.
	 */
	set_dont_idle(ctx);

	if (!user_initialized) {
		/*
		 * Until the agent completes Discovery, don't schedule anything.
		 * Keeping the system quiescent makes it easier to handle corner
		 * cases.  Specifically, since tasks are not running, we don't
		 * need to deal with unexpected preempt/blocked/yield/switchtos.
		 */
		return 0;
	}

	/*
	 * Don't bother picking a task to run if any of these are true.  If the
	 * agent runs or CFS preempts us, we'll just get the latched task
	 * preempted.
	 *
	 * Note it is might_yield, not "will_yield", so there's a chance the CFS
	 * tasks gets migrated away while the RQ lock is unlocked.
	 *
	 * Note we no longer check next_gtid - letting the current task stay on
	 * cpu is up to the owning scheduler.
	 */
	if (ctx->agent_runnable || ctx->might_yield)
		return 0;

	cpu = get_this_cpu();
	if (!cpu)
		return 0;

	if (!cpu->f.available)
		return 0;
	/*
	 * We need an atomic exchange here.  If a scheduler expects a
	 * preemption, we need to do it (or they yield).
	 * e.g.
	 * - biff is preempting.  preempt_to = 2 (biff).
	 * - we're in PNT.  we see 2.  we decide to preempt up to 2.  we run the
	 *   handlers for biff's (non-existent) children we run biff's
	 *   preemption_complete
	 * - some other cpu sets preempt_to = 1 (roci is higher)
	 * - we set preempt_to = FLUX_TIER_NO_PREEMPT.  Whoops, we clobbered
	 *   roci's preemption.
	 * The same race applies regardless of whether we clobber preempt_to
	 * before or after running the preemptions.
	 */
	preempt_to = READ_ONCE(cpu->f.preempt_to);
	if (preempt_to != FLUX_TIER_NO_PREEMPT) {
		preempt_to = __atomic_exchange_n(&cpu->f.preempt_to,
						 FLUX_TIER_NO_PREEMPT,
						 __ATOMIC_ACQUIRE);
		flux_preempt_up_to(cpu, preempt_to);
	}

	/*
	 * This is an "availability edge", where the cpu recently became
	 * available.  current_sched should only be set to FLUX_SCHED_NONE due
	 * to a kernel-level preemption in the BUSY handler.
	 *
	 * We do all grants from PNT.
	 */
	if (cpu->f.current_sched == FLUX_SCHED_NONE)
		flux_cpu_grant(NULL, top_tier_sched_id(), cpu);

	cpu->f.pnt_success = false;
	/*
	 * This loop can fail if there are a lot of yields and grants up and
	 * down the hierarchy.  We must bound the loop for BPF, and if we fail,
	 * we'll go back to the kernel and try again.  Check the FAQ for
	 * RESTART_PNT.
	 *
	 * In the common case, 2x seems fine.  Consider the case where Biff ran
	 * out of tasks: biff (cpu_yield) -> roci (cpu_grant) -> idle.
	 *
	 * Or if CFS gives it to us, and roci gives it to biff, but biff no
	 * longer needs it: roci -> biff -> roci -> idle.  "2x" is essentially
	 * "try allocating twice down the hierarchy", where you get one "free"
	 * cpu_yield() before we have to hop back to the kernel.
	 *
	 * You could actually drop this loop and it'll still be correct: we'd
	 * just go back to the kernel, select idle, and restart PNT a lot (which
	 * involves extra context switches).
	 */
	for (int i = 0; i < FLUX_MAX_NR_TIERS * 2; i++) {
		struct flux_sched *s = get_sched(cpu->f.current_sched);

		if (!s) {
			bpf_printd("Couldn't find current_sched %d!",
				   cpu->f.current_sched);
			break;
		}
		flux_pick_next_task(s, cpu, ctx);
		/* Note we return here. */
		if (cpu->f.pnt_success)
			return 0;
	}

	/*
	 * The loop failed to pick a task.  We need both dont_idle and
	 * must_resched to tell the kernel to call PNT again: must_resched
	 * forces the prev ghost task (if any) off cpu, and dont_idle prevents
	 * the cpu from halting.  This is the "RESTART_PNT" scenario.
	 */
	ctx->must_resched = true;

	return 0;
}

/*
 * You have to play games to get the compiler to not modify the context pointer
 * (msg).  You can load X bytes off a ctx, but if you add to ctx, then load,
 * you'll get the dreaded: "dereference of modified ctx ptr" error.
 *
 * You can also sprinkle asm volatile ("" ::: "memory") to help reduce compiler
 * optimizations on the context.
 */
static void handle_new(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_new *new = &msg->newt;
	struct task_sw_info swi[1] = {0};
	struct flux_thread *t;
	u64 gtid = new->gtid;

	/*
	 * TODO: there's a race where a task can depart, and if a new task
	 * uses the same SW (which the kernel controls) before we decref, we
	 * could clobber t.  You'll need to be close to 65k threads for the
	 * kernel to quickly reuse a SW.  The fix is to use our own BPF map for
	 * an integer allocator instead of using the SW index.
	 */
	swi->id = new->sw_info.id;
	swi->index = new->sw_info.index;

	if (bpf_map_update_elem(&sw_lookup, &gtid, swi, BPF_NOEXIST)) {
		/*
		 * We already knew about this task.  If a task joins the enclave
		 * during Discovery, we'll get a task_new message.  Then
		 * userspace asks for task_news for all tasks.  Use the bpf map
		 * as our synchronization point, similar to how userspace agents
		 * use the task's channel association.
		 *
		 * Note that if you use "send me task news" to handle failing to
		 * enqueue a task or something (which is essentially losing a
		 * wakeup), then you may need some other mechanism to track
		 * the actual runnability of the task.  i.e. make sure biff
		 * and new->runnable are in sync.
		 */
		return;
	}

	t = gtid_to_thread(gtid);
	if (!t)
		return;

	/* TODO: is there a clang builtin memset(0) for the whole struct? */
	t->f.gtid = gtid;
	t->f.seqnum = msg->seqnum;
	t->f.refcnt = 1;
	t->f.nice = new->nice;
	t->f.on_rq = new->runnable;
	t->f.on_cpu = false;
	t->f.pending_latch = false;

	flux_join_scheduler(t, new_thread_sched_id(new), new->runnable);
}

static void handle_on_cpu(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_on_cpu *on_cpu = &msg->on_cpu;
	struct flux_thread *t;

	t = flux_thread_on_cpu(on_cpu->gtid, msg->seqnum, on_cpu);
	t->f.on_cpu = true;
}

static void handle_blocked(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_blocked *blocked = &msg->blocked;
	struct flux_thread *t;

	t = flux_thread_blocked(blocked->gtid, msg->seqnum, blocked);

	/*
	 * t could have been from_switchto, in which case we didn't know it was
	 * on_rq.  but regardless, it's now off rq.
	 */
	t->f.on_rq = false;
	t->f.on_cpu = false;
}

static void handle_wakeup(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_wakeup *wakeup = &msg->wakeup;
	struct flux_thread *t;
	struct flux_args_wakeup wakeup_f;

	wakeup_f.deferrable = wakeup->deferrable;
	wakeup_f.waker_cpu = wakeup->waker_cpu;
	t = flux_thread_wakeup(wakeup->gtid, msg->seqnum, &wakeup_f);

	t->f.on_rq = true;
}

static void handle_preempt(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_preempt *preempt = &msg->preempt;
	struct flux_thread *t;

	t = flux_thread_preempted(preempt->gtid, msg->seqnum, preempt);
	if (preempt->from_switchto)
		t->f.on_rq = true;
	t->f.on_cpu = false;
}

static void handle_yield(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_yield *yield = &msg->yield;
	struct flux_thread *t;

	t = flux_thread_yielded(yield->gtid, msg->seqnum, yield);
	if (yield->from_switchto)
		t->f.on_rq = true;
	t->f.on_cpu = false;
}

static void handle_switchto(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_switchto *switchto = &msg->switchto;
	struct flux_thread *t;

	t = flux_thread_switchto(switchto->gtid, msg->seqnum, switchto);

	/*
	 * We don't know who is running, but we know t isn't.
	 *
	 * TODO: If we knew who we switched to and if we got these
	 * messages for every switchto (instead of just the first), we could
	 * update current new_t on_rq.  So either 1) add cpu->f.in_switchto, and
	 * track it in flux, and have biff et al. clear their current pointer,
	 * or 2) add a MSG_TASK_SWITCHTO_SWITCH and do the right bookkeeping.
	 *
	 * For now, when we get a from_switchto when the kernel resolves the
	 * switchto chain, we'll set on_rq.  We could potentially use on_rq as a
	 * way to detect from_switchto.  e.g. anytime we get a yield/preempt, we
	 * set on_rq, since it must be on_rq now, even if we thought it was
	 * off_rq before.
	 */
	t->f.on_rq = false;
	t->f.on_cpu = false;
}

static void handle_affinity_changed(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_affinity_changed *affin = &msg->affinity;

	flux_thread_affinity_changed(affin->gtid, msg->seqnum, affin);
}

static void handle_prio_changed(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_priority_changed *prio = &msg->priority;
	struct flux_thread *t;
	struct flux_args_prio_changed prio_f;

	t = gtid_to_thread(prio->gtid);
	if (!t)
		return;
	prio_f.old_nice = t->f.nice;
	t->f.nice = prio->nice;

	flux_thread_prio_changed_thr(t, msg->seqnum, &prio_f);
}

static void handle_latch_failure(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_latch_failure *lf =
		&msg->latch_failure;
	struct flux_thread *t;
	struct flux_args_runnable runnable_f;

	t = gtid_to_thread(lf->gtid);
	if (!t)
		return;

	if (!t->f.pending_latch) {
		bpf_printd("latch_failure didn't have the ball! p %d e %d",
			   gtid_to_pid(lf->gtid), lf->errno);
		smp_store_release(&t->f.seqnum, msg->seqnum);
		return;
	}

	/* We have the ball */
	t->f.pending_latch = false;
	flux_thread_runnable_thr(t, msg->seqnum, &runnable_f);
}

static void handle_dead(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_dead *dead = &msg->dead;
	struct flux_thread *t;

	t = flux_thread_dead(dead->gtid, msg->seqnum, dead);
	t->f.sched = FLUX_SCHED_NONE;
	/* Could not be from_switchto, since tasks block before dying. */
	flux_thread_decref(t);
}

static void handle_departed(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_departed *departed = &msg->departed;
	struct flux_thread *t;

	t = flux_thread_departed(departed->gtid, msg->seqnum, departed);
	/*
	 * Note that this thread could be pending_latch and will get a
	 * LATCH_FAILURE as soon as we return and unlock.  By clearing the
	 * sched, we make sure our old sched doesn't get the thread_runnable.
	 */
	t->f.sched = FLUX_SCHED_NONE;
	/* Could be from_switchto, but we don't care about on_rq anymore. */
	flux_thread_decref(t);
}

static void handle_cpu_tick(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_cpu_tick *cpu_tick = &msg->cpu_tick;
	struct flux_cpu *cpu;
	int sched_id;
	int prev_child_id = FLUX_SCHED_NONE;

	cpu = cpuid_to_cpu(cpu_tick->cpu);
	if (!cpu)
		return;

	/*
	 * Run the tick handlers from the bottom to the top of the hierarchy.
	 * Arguably, if the higher tiered scheduler wanted to take the cpu away,
	 * running the lower tier scheduler first is a waste.  But that seems
	 * rare, and it's simpler to walk up the hierarchy than to walk down.
	 */
	sched_id = cpu->f.current_sched;
	for (int i = 0; i < FLUX_MAX_NR_TIERS; i++) {
		struct flux_sched *s;

		if (sched_id == FLUX_SCHED_NONE)
			break;
		s = get_sched(sched_id);
		if (!s)
			break;
		flux_cpu_ticked(s, prev_child_id, cpu);
		prev_child_id = sched_id;
		sched_id = get_parent_id(s);
	}
}

static void handle_cpu_available(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_cpu_available *avail = &msg->cpu_available;
	struct flux_cpu *cpu;

	cpu = cpuid_to_cpu(avail->cpu);
	if (!cpu)
		return;
	cpu->f.available = true;

	/*
	 * Any lingering setters of preempt_to don't matter: this cpu has no
	 * schedulers yet.
	 */
	WRITE_ONCE(cpu->f.preempt_to, FLUX_TIER_NO_PREEMPT);
}

static void handle_cpu_busy(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_cpu_busy *busy = &msg->cpu_busy;
	struct flux_cpu *cpu;

	cpu = cpuid_to_cpu(busy->cpu);
	if (!cpu)
		return;
	cpu->f.available = false;

	/*
	 * Not a big deal, but set preempt_to to the highest tier (0), in case
	 * another cpu wants to instigate a preemption.  We're going to preempt
	 * every scheduler on this cpu, so no need for an IPI.
	 *
	 * This might be rare, so half of the benefit here is *knowing* we can
	 * do this.  preempt_to will get reset when we get the cpu again.
	 */
	WRITE_ONCE(cpu->f.preempt_to, 0);
	flux_preempt_up_to(cpu, 0);
}

SEC("ghost_msg/msg_send")
int flux_msg_send(struct bpf_ghost_msg *__msg)
{
	/*
	 * This is crappy, but it seems to be the best way to avoid the dreaded
	 * "dereference of modified ctx ptr" error.  For whatever reason, you're
	 * allowed to do reads/writes from a context pointer at various offsets.
	 * e.g. r1 = *(u64 *)(r2 +40) would be a read from 40 into the context.
	 * But if you add 40 to r2, and then do the read, that's not OK.  It's
	 * hardcoded into the verifier like that, where certain pointer types
	 * are OK to modify (map items, stack objects, etc.) and some aren't.
	 *
	 * Previously, I tried to defeat this by no-inlining all of the handle
	 * functions, since that seemed to stop the compiler from modifying the
	 * pointer.  But as you write more complicated functions, you need to
	 * put the noinline on every sched op too.  And even then, you're just
	 * hoping the compiler doesn't decide to increment the pointer for its
	 * own reasons.  I ran into this with a simple on_cpu op, where all we
	 * did was read the cpu field (+24).
	 *
	 * The 'fix' is to just copy the msg to the stack, where we're allowed
	 * to modify the pointer.  Yeah, it's a copy of 48 bytes that we
	 * shouldn't have to do.
	 *
	 * bpf_ghost_msg is read-only for flux, so this is fine.  But if you're
	 * interested in changing the contents of the msg or setting the
	 * pref_cpu field from a handler, you'll need to copy that back to the
	 * "real" __msg.
	 *
	 * Hopefully we won't need do to this for BPF-PNT.  I've only run into
	 * this error with BPF-MSG, perhaps because of the union.
	 */
	struct bpf_ghost_msg msg[1] = {*__msg};

	switch (msg->type) {
	case MSG_TASK_NEW:
		handle_new(msg);
		break;
	case MSG_TASK_ON_CPU:
		handle_on_cpu(msg);
		break;
	case MSG_TASK_BLOCKED:
		handle_blocked(msg);
		break;
	case MSG_TASK_WAKEUP:
		handle_wakeup(msg);
		break;
	case MSG_TASK_PREEMPT:
		handle_preempt(msg);
		break;
	case MSG_TASK_YIELD:
		handle_yield(msg);
		break;
	case MSG_TASK_SWITCHTO:
		handle_switchto(msg);
		break;
	case MSG_TASK_AFFINITY_CHANGED:
		handle_affinity_changed(msg);
		break;
	case MSG_TASK_PRIORITY_CHANGED:
		handle_prio_changed(msg);
		break;
	case MSG_TASK_LATCH_FAILURE:
		handle_latch_failure(msg);
		break;
	case MSG_TASK_DEAD:
		handle_dead(msg);
		break;
	case MSG_TASK_DEPARTED:
		handle_departed(msg);
		break;
	case MSG_CPU_TICK:
		handle_cpu_tick(msg);
		break;
	case MSG_CPU_AVAILABLE:
		handle_cpu_available(msg);
		break;
	case MSG_CPU_BUSY:
		handle_cpu_busy(msg);
		break;
	}

	/* Never send the message to userspace: no one is listening. */
	return 1;
}

SEC("ghost_select_rq/select_rq")
int flux_select_rq(struct bpf_ghost_select_rq *ctx)
{
	return ctx->waker_cpu;
}

char LICENSE[] SEC("license") = "GPL";
