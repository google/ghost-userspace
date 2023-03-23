/*
 * Copyright 2023 Google LLC
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifndef GHOST_LIB_BPF_BPF_FLUX_HEADER_BPF_H_
#define GHOST_LIB_BPF_BPF_FLUX_HEADER_BPF_H_

#ifndef __BPF__
#include <stdint.h>
#endif

/*
 * Flux is a framework for cooperative, preemptive hierarchical scheduling.
 * With Flux, you can compose an overall scheduler from "sub schedulers".  For
 * example, agent_flux (flux.bpf.c) is an overall scheduler made up of
 * subschedulers: biff (biff_flux.bpf.c) and idle (idle_flux.bpf.c).  Those two
 * schedulers are scheduled by agent_flux's "root" scheduler (flux.bpf.c).
 *
 * In an attempt to reduce copy-pasting between Flux "clients", Flux has a few
 * headers, C files, and macros.  If you play by the Rules, it'll work out -
 * Flux is closely coupled to the overall scheduler, perhaps too much.  Since
 * we're building this in BPF, we have limitations that led to this coupling.
 *
 * The Rules for Flux clients:
 * - #include flux_header_bpf.h (this file) in your header file.
 *
 * - make your own structs named flux_cpu, flux_sched, and flux_thread, and
 *   embed in them the __flux structs below with the name 'f'.  e.g.  inside
 *   struct flux_sched, embed "struct __flux_sched f;".
 *
 * - set up the hierarchy and define your schedulers:
 *   - declare an integer id for FLUX_SCHED_NONE.
 *   - define FLUX_MAX_NR_TIERS
 *   - implement sched_id_to_tier()
 *   - implement top_tier_sched_id()
 *   - implement get_sched() and define a map for the flux_sched structs
 *   - implement get_parent_id()
 *   - implement new_thread_sched_id()
 *   - define macros to generate cases: __gen_thread_op_cases and
 *   __gen_cpu_op_cases
 *
 * - then #include flux_dispatch.bpf.c
 *
 * - then add your sched ops
 *
 * - then #include flux_api.bpf.c
 */

#define FLUX_MAX_CPUS	1024
#define FLUX_MAX_GTIDS 65536

/* Userspace initializes the id and type fields. */
struct __flux_sched {
	int64_t nr_cpus;
	int64_t nr_cpus_wanted;
	int id;		/* like a pointer to the struct itself */
	int type;	/* like a pointer to a struct ops */
};

struct __flux_cpu {
	uint64_t preempt_to;	/* need 64 bits for CAS */
	int id;
	int current_sched;
	bool available;
	bool pnt_success;
	uint64_t latched_seqnum;	/* for use in PNT */
};

struct __flux_thread {
	uint64_t gtid;
	uint64_t seqnum;
	uint64_t refcnt;	/* need 64 bits for fetch-add */
	int sched;
	int nice;
	bool on_rq;		/* runnable and/or running. */
	bool on_cpu;		/* running */
	bool pending_latch;	/* in PNT limbo, trying to latch */
};

/*
 * A more lengthy note on structures:
 *
 * Each scheduler/cpu/thread is a separate BPF map element.  The flux 'client'
 * needs to make e.g. flux_sched, and embed a "struct __flux_sched f" in it
 * somewhere.
 *
 * For scheds and threads, the struct contains a union of all sched-specific
 * structs, each used by its own scheduler.  A thread can only belong to one
 * scheduler at a time, so a union is sufficient.  For cpus, the struct flux_cpu
 * contains all structs, but they are not in a union: every scheduler can
 * maintain data for the same a cpu at the same time.
 *
 * We can only have a single lock per map element, kept outside the union for
 * BPF reasons.
 *
 * You can think of that union as the "void *private" blob in the Linux kernel
 * object-oriented "private blob" programming style (a la the VFS).
 *
 * Why a union?  Why are the sched-specific structs inside the flux_ structs?
 * Because every element must be of the same overall type, since there is a big
 * bpf map of all elements.  (This applies to cpus, threads, and sched structs).
 *
 * It's tempting to try the 'embedded' style of OO C programming, where e.g. the
 * __flux_thread is embedded in the biff_thread.  Instead, the sched-specific
 * thread blob is *adjacent* to __flux_thread, versus *embedded*.  And these
 * sched-specific structs are embedded in the main flux_ structs (e.g.  struct
 * flux_thread, which is different than __flux_thread), which is actually the
 * *reverse* of the usual "embedded pattern" (in our case, the 'specific' is
 * embedded in the 'generic', though the analogy is quite right, since the
 * 'generic' is actually struct __flux_cpu, not struct flux_cpu.
 *
 * The main reason that the embedded style doesn't work is that all the objects
 * are from the same map and thus are the same size and type.
 * i.e. biff, root, idle threads all come from the same 'allocator'.  We could
 * make a top-level union of all biff/root/idle, and then have the flux struct
 * embedded in each of those sched... in the same spot?  Good luck.
 *
 * Can we make flux API ops take the __flux struct instead of the overall
 * struct?  Maybe, but I didn't do that.  Mainly because we need to refer to the
 * top-level struct frequently:
 * - all the objects are in a giant map, so stuff like arr_list_pop_first() will
 *   return whatever the top-level struct is.  This is because we're using
 *   indexes instead of pointers.  So schedulers are dealing with the top-level
 *   struct.
 * - The bpf_spin_lock cannot be in a nested struct: it must be in the top-level
 *   struct (BPF verifier reasons).  So again schedulers refer to the top-level
 *   struct, and even refererence fields outside the union.
 * - flux_dispatch.bpf.c calls the sched's thread ops, but what object can it
 *   pass?  It has to pass the top-level struct.  If we did the embedding style,
 *   you could cast it, but we're adjacent, not embedded.  Doing a
 *   container_of() or something in the already-convoluted macros involved in
 *   dispatch seems like a mess.
 */

/*
 * A Treatise on Locking and Synchronization in Ghost-BPF:
 *
 * Locking and synchronization is complicated.  We have "asynchronous messages",
 * and lockless operations: namely trying to run a thread.  When we call
 * bpf_ghost_run_gtid(), we're "passing the ball" back to the kernel, but we
 * hold no locks (can't hold a bpf_spin_lock while calling a helper).  This
 * operation can fail, and the failure can be detected in several places.
 *
 * I'll try to walk through several of the concepts and ways around them:
 *
 *
 * "asynchronous messages": meaning the message can arrive regardless of the
 * state of the task.  It could be on_rq (runnable / the ball was in BPFs
 * court), it could be blocked, etc.  The main danger is that on_rq tasks are
 * usually on some scheduling structure (an RQ) and discoverable by PNT.
 * Another way to phrase it is that "async" messages have numerous edges in a
 * state diagram.  Contrast to the Wakeup msg: that only happens when your task
 * was blocked.  Yet Departed or PrioChange can happen regardless of whether you
 * are blocked or not.
 *
 *
 * "discoverable": your sched op's PNT, (which cannot be guaranteed to hold the
 * task lock), needs to find tasks to run.  Perhaps it does so by consulting
 * some scheduler structure, e.g. a per-cpu, RB tree, global fifo, etc.  This is
 * one way to "discovering" a task.  That list/structure is a source of pointers
 * (and refcounts) to tasks, and it races with async messages (e.g. departed)
 * that try to remove the task from those lists.  A task's membership on those
 * structures is protected by some scheduler lock (e.g. biff's global lock, a
 * per-rq lock, etc.).
 *
 * Discoverability also applies when doing migration between per-cpu RQs, e.g.
 * load balancing in cfs-bpf, and even for lookups like pcpu->current.  Any time
 * you publish a pointer (actually a thread index) somewhere, you've made it
 * discoverable and need to worry about concurrent access.
 *
 * Discoverability isn't a new concept or anything - it's just one of those
 * parallel programming things: your agent is a parallel program with shared
 * data structures, and you need to worry about object lifetime, concurrent
 * access, etc.  It just so happens that ghost has a lot of "edges" where we
 * commonly don't have pointers to threads in the main scheduling structure yet,
 * so having a term for when we can do lockless accesses (if ever) helps a
 * little.  Luckily, we have synchronization mechanisms to help:
 *
 *
 * "task lock": thread message handlers run with the kernel task's rq lock
 * held.  Additionally, when ghost_run_gtid returns *successfully*, we are
 * holding the task RQ lock since we know the kernel migrated the task to this
 * cpu.  Note this does not mean we always hold the kernel task's pi_lock.
 *
 * Think of the task's rq lock as a per-task lock, from the perspective of the
 * BPF agent, even though a single kernel lock may be the same for many tasks.
 * For us, we can only rely on that task's lock being held, since we have no
 * guarantee about which tasks are on a given kernel RQ.  It's more like the
 * kernel's pi_lock (per-task).
 *
 * For those familiar with userspace ghost, the task lock provides the same
 * benefits as associating a channel, which ultimately ensures that all messages
 * for a given task are handled by a single agent task at a time: a tasks
 * messages are handled sequentially: no races, in order, etc.  One difference
 * is that userspace agents route the messages to a single agent, on a single
 * cpu, and the message handlers run on that cpu.  It uses "per-cpu discipline"
 * to get mutual exclusion.  In contrast, BPF msg handlers for a given task can
 * run on any cpu, e.g. a wakeup triggered from a cpu that's not even in the
 * enclave, and the mutual exclusion is provided by the task rq lock.
 *
 * In either case, userspace or bpf, you get in-order messages, but you can't
 * rely on that for all of your synchronization.  Agents are parallel programs
 * (other than the single-cpu, centralized/global agents), and the usual rules
 * of parallel programming apply, and we'll need other forms of synchronization.
 *
 *
 * "sched lock": scheduler-specific lock(s), protecting some structure, e.g. a
 * linked list, RB tree, etc.  Think of these like the kernel's RQ locks.  Yes,
 * it's a little confusing that the "task lock" is provided by the kernel's RQ
 * lock.
 *
 * The sched lock(s) protects the sched's decision-making structure, e.g. the
 * next pointers in the list, and any other scheduling invariants you come up
 * with.  This applies to some thread struct fields, such as an int we use to
 * sort a structure.  This is scheduler specific.  Conceptually, you could lock
 * the scheduler, remove the task, unlock, change the sort field, relock the
 * list, then insert the task.  Don't do that - just hold the lock.  But it's
 * important to know you could do that, since the lock isn't required to access
 * the thread's sort field if your task is not on the sched structure.
 *
 * To be more precise, if a thread is discoverable, then you have to worry about
 * concurrent accesses.  The task lock will not protect you.  Other cpus that
 * are not holding your task lock can find and access your thread's struct -
 * specifically PNT, but also potentially stuff like cpu ticks.  Or even a
 * wakeup handler that checks the previous cpu: e.g. cpu[t->prev].current.
 * You'll need to synchronize with the 'sources of discovery'.  Typically this
 * means you need to grab a sched lock.
 *
 *
 * "current": for checking current, if you are running on that cpu (e.g. PNT),
 * then you hold current's task lock.  If not, you're on your own.  The instant
 * after you read current, that task could exit and a new one is current.  That
 * old thread struct could even be freed and reused.  I can add a helper to
 * incref-not-zero that task, which helps with the lifetime of the thread
 * struct.  But ultimately if you're doing something other than opportunistic
 * peeking, you'll probably need to grab a sched lock and roll your own sync, or
 * maybe use the seqnum as a seqctr.  e.g. read t->f.seqnum, read t->f.nice,
 * check that t->f.seqnum hasn't changed.
 *
 * Switchto complicates things.  When we switchto away from a task, ghost
 * doesn't tell us who is actually current now.  Even though the task is
 * technically off_cpu, I've been leaving current set to the old task for lack
 * of a better one to pick.  Presumably you can use the blocked task's
 * PID/priority to decide if you want to use that cpu or not.  If you just think
 * of tasks that called switchto as still being 'current', you'll probably be
 * fine.  Though note that that task can depart, so this is still a bit hokey.
 * The fix is probably to have ghost know about all switchtos, not just the
 * beginning and ending of the switchto chain.
 *
 *
 * If a task is not discoverable and you've got the task lock, e.g. in a wakeup
 * msg handler, then you can do whatever you want - no other part of the agent
 * can access your task, and since you hold the task lock, you know there are no
 * other msg handlers for your thread running concurrently.  You can think of
 * the msg stream as a source of discovery / pointers too, one that is
 * single-threaded.
 *
 * How do you know if a task is discoverable?  For async messages, if your task
 * is on_rq, then it's *likely* discoverable.  It depends on your scheduler.
 * Biff only keeps runnables on its list, so if you're on_cpu in biff, you're
 * not discoverable *by the list* (but maybe by 'current').  However, you could
 * construct a scheduler where all tasks are always in a tree/list/structure.
 *
 * If your task is not on_rq, then it's probably not discoverable.  e.g. a task
 * just woke up and you have yet to insert it in any sched structures.  So if
 * you're in a PRIO_CHANGE handler and your task isn't on_rq, you can just
 * change the prio, or do whatever (like change between flux schedulers).  And
 * you don't have to worry about a concurrent WAKEUP, since messages for a task
 * are handled in order.
 *
 * OK, say I have a thread message, and thus the task lock, but I think it might
 * be discoverable.  It's on_rq, and not on_cpu.  For biff, there's a single
 * list and single lock.  For a per-cpu rq scheduler, you'll need to track the
 * cpu/rq it belongs to, e.g. t->cfs.cpu.  (More on this below).  But say I look
 * at the sched structure, but it's not there!?  Enter pending_latch.
 *
 *
 * "pending_latch":  This is the limbo period between when PNT pulls a task off
 * the sched structure (e.g. the biff list) and before it latches.  The rule is
 * that PNT must call flux_prepare_to_run() before unlocking the sched (or at
 * least before removing the task if you manage to concoct a lockless
 * structure), which sets pending_latch and saves a copy of the seqnum.  So if
 * you're on_rq, you're either latched, on_cpu, on the struct, or in between:
 * pending_latch.  As far as the agent is concerned, latched and on_cpu are
 * largely the same: the ball is in the kernel's court.
 *
 * pending_latch gets set by PNT, without the task lock (but with the sched
 * lock), and cleared under the task lock.  Whoever sees pending_latch "has the
 * ball": they are the first task-locked function to see the task after it
 * entered limbo, *and* we know the result of the latch.  There are three places
 * that could see pending_latch set:
 * - 1: PNT, run_gtid succeeded: we hold the task lock since the task migrated
 *   to this cpu.  We know the latch succeeded.  This is sort of an
 *   optimized/in-line MSG_LATCH_SUCCESS (which isn't a msg type)
 * - 2: MSG_LATCH_FAILURE: triggered from a failed run_gtid, called back into
 *   BPF with the task lock held (not necessarily the RQ lock of the cpu we're
 *   on).  This is essentially in PNT, with the task lock held.  It just happens
 *   to be a callback from within run_gtid.
 * - 3: An async message, e.g. Departed.  We know the latch *will* fail, since
 *   our message is increasing the seqnum, and PNT will use an old seqnum.  (The
 *   message handler and PNT will synchronize with the scheduler lock.)
 *
 * We only clear pending_latch in Case 1 or Case 2.  Case 3 can look at
 * pending_latch if it wants to, but leave the "ball passing" to Case 2.
 * Specifically, an ESTALE Case 2 happens after Case 3, i.e. the message had to
 * arrive already (case 3) for run_gtid to fail and trigger Case 2.  So in your
 * message handler, you can look for pending_latch if it affects your message
 * handler's actions.  But don't clear pending_latch (unless you know what
 * you're doing) - just let Case 2 do that, which ultimately just calls your
 * sched's runnable op.
 *
 * Case 3 is tricky too.  Cases 1 and 2 run after PNT/run_gtid.  Case 3 runs
 * concurrently with PNT, and although Case 3 has the task lock, PNT does not.
 * So in the msg handler, when do we check pending_latch?  What if it *isn't*
 * set yet, but will be soon?  It's tempting to check pending_latch in the flux
 * core before running the sched-specific thread op for the message and handle
 * the error before running the op, as if PNT never happened.  e.g. stick it
 * back on the biff RQ.
 *
 * Consider this race:
 *
 * PNT 					DEPARTED
 * -----------------------------------------------------------
 *					check pending_latch
 *						if pending, handle the error
 *						but it's not set yet
 *
 * 	sched lock
 * 	copy old seqnum
 * 	set pending_latch
 * 	dequeue from list
 * 	sched unlock
 * 	ghost_run_gtid
 * 	    spins on task's rq lock
 *
 *					sched lock
 *					try to dequeue and fail?
 *						but the task was on_rq
 *						and pending_latch is set now...
 *					sched unlock
 *					update seqnum = msg->seqnum (increment)
 *					decref task
 *					return to kernel
 *						unlocks task's rq lock
 *
 * 	ghost_run_gtid returns ESTALE
 *
 * (Note that if Departed grabbed the sched lock first, there's no issue - PNT
 * won't even pick the task.)
 *
 * Who handles the pending_latch?
 *
 * If we only do the check before locking, then we'll see ESTALE and
 * pending_latch set in case 2 (the LATCH_FAILURE callback).  However, we've
 * lost the info about departed!  We need to remove it from ghost.  Same goes
 * for message like PRIO_CHANGE, particularly for schedulers that use prio to
 * pick between flux child schedulers: we had some work to do for the message
 * handler, but communicating that info to Case 2 sounds like a mess.
 *
 * The fix here is to check pending_latch while holding the sched lock.  Since
 * we have to do it here, checking pending_latch early is an optimization - and
 * one that doesn't really help much.
 *
 * An unfortunate consequence of checking pending_latch with the sched lock
 * means that flux thread schedulers (e.g. biff) might need to worry about
 * pending_latch.  However, it turns out that your sched might not care at all.
 * For instance, biff_thread_affinity_changed() does nothing.  So don't worry
 * about pending_latch; Case 2 will deal with it.  For departed, biff already
 * tracks whether a thread is enqueued in its structures or not,
 * (biff->enqueued), which it checks holding the sched lock, and pending_latch
 * implies not enqueued.
 *
 * Even for prio change, if you changed schedulers, it's still fine to let case
 * 2 handle the latch failure.  Thanks to flux's thread dispatching, it'll call
 * runnable for the new scheduler.
 *
 * Note that case 1 and 2 don't need the sched lock.  The sched lock
 * synchronizes with PNT (the source of pending_latch), but cases 1 and 2 happen
 * after PNT calls flux_prepare_to_run().
 *
 * There are many reason why latching can fail other than a missed message.  The
 * most common one is EBUSY: the task is still on a cpu.  EBUSY happens because
 * ghost will generate messages that "pass the ball" to the agent, but the task
 * hasn't quite gotten off cpu yet.  Ugly, but that's life.  There are other
 * weirder errors, like EINVAL (bad run flags - you're in serious trouble) and
 * EXDEV (the enclave is losing this cpu, which is OK and will resolve itself
 * soon).  In either case, we failed to latch, but its mostly a transient error,
 * so just stick the task back on the RQ, same as for any other latch failure,
 * e.g. call flux_thread_runnable().  (Don't worry: for EBUSY, we'll
 * eventually return to the kernel, select the idle task, and restart PNT with
 * the task off_cpu.)
 *
 * Note that for Departed, we won't get a latch_failure message, since the
 * kernel does not generate messages for task that are no longer in ghost.  Same
 * as with ENOENT: the task doesn't exist (left ghost and exited, perhaps).
 * These errors are handled in flux_run_thread(), and all we do is decref and
 * ultimately free the thread struct.  We're technically not holding the RQ lock
 * anymore, so it's not really case 2 (MSG_TASK_LATCH_FAILURE) or case 1
 * (LATCH_SUCCESS).
 *
 *
 * "latched_seqnum": A point here regarding seqnum: we need to make sure that
 * both PNT and the message agree on "who has the ball".  Specifically, if e.g.
 * Departed thinks the task will fail to latch, that it does fail to latch.  So
 * if Departed sees pending_latch, PNT must see the old seqnum and fail.
 *
 * The purpose of latched_seqnum (read of seqnum while holding the sched lock)
 * is to make sure run fails with ESTALE, or rather to say "when we make this
 * transaction, we believe the state is X".  This seqnum is temporarily saved in
 * pcpu data by flux_prepare_to_run().  The corresponding operation in the
 * message handler is to update seqnum *after* handling the message, where the
 * meaning is "I have incorporated the new state of this task in the scheduler."
 *
 * We don't even need to hold the sched lock when incrementing the seqnum.  A
 * store-release is sufficient, just like we do in userspace (in Task.Advance()
 * in lib/scheduler.h).
 *
 * Earlier, I claimed we needed the sched lock on departed, but in that example,
 * we ultimately had this sync pattern (with barriers as needed):
 *
 * PNT 					DEPARTED
 * -----------------------------------------------------------
 * 	read seqnum 			read pending_latch
 *
 * 	write pending_latch 		write new seqnum
 *
 * That guarantees that if Departed sees pending_latch (and thinks the latch
 * will fail), then PNT sees old seqnum and does fail.  Departed saw pending,
 * then we know PNT will fail!  And if PNT sees new seqnum (and won't fail),
 * departed doesn't see pending.  Wait, is that what we want!?  Yes, but we need
 * more.  That was necessary, but not sufficient.
 *
 * There are other potential outcomes of that scenario: PNT sees old seqnum, but
 * departed doesn't see pending_latch.  That's the scenario where the latch
 * fails, but departed doesn't know it.
 *
 * The shared-memory sync would cover the case where Departed thinks the latch
 * fails, and the latch does indeed fail. i.e. the "departed thinks it has the
 * ball, and it really does".  But it doesn't enforce the converse: that if the
 * latch fails (a.k.a. ESTALE) that Departed knows.  Another way to put it: for
 * ESTALE, we want the message hander to have the ball and handle the message,
 * and not for Case 2 to have the ball.
 *
 * So we write new seqnum after handling the message, but we still need to lock.
 * We could update seqnum while holding the lock, but since flux updates the
 * seqnum, and there might be other sched-specific state updated in the message
 * handlers, we update it at the end of the flux thread_op.
 *
 * Also, it's possible for PNT to see both the effects of a message handler and
 * the old seqnum.  For Departed, PNT wouldn't find the task, so it's moot, but
 * for something like PrioChange, we could have run the thread op, then PNT
 * picks the task, and then Flux increments the seqnum.  PNT will fail in Case 2
 * (LATCH_FAILURE) with ESTALE and pending_latch is still set.  Not a problem -
 * just something Flux will deal with.  Think of this as a spurious IRQ - you
 * got poked, but just ignore it.  LATCH_FAILURE has the ball and will make you
 * runnable again.
 *
 *
 * During pending_latch, how do I know which sched lock to grab?  The task isn't
 * on any list?!  For biff, this isn't a problem: there's only a single sched
 * structure.  But what about CFS, with per-cpu runqueues?  And what if we're
 * migrating a task?  This is a similar issue to how the kernel grabs a task's
 * RQ lock: the task could be migrating while you're trying to lock.  The kernel
 * will essentially retry until it has an RQ lock and confirms the task is on
 * that RQ.  We can't do that in BPF (no infinite loops).
 *
 * Luckily, it's not so dire here.  Track which cpu_rq a task is on in e.g.
 * t->cfs.cpu, but don't change this until the latch succeeds, and protect that
 * variable with the task lock.  In your message handler, which holds the task
 * lock, you can check cfs.cpu to see what RQ lock to sync with.  If it's on a
 * list, it would have been that one.
 *
 *
 * migration: As another aside, to migrate tasks in cfs-bpf, do it from PNT and
 * pull tasks from other cpus to yours: just like how biff pulls tasks from one
 * cpu to another.  Unlike in the kernel, you can't "double lock" multiple RQs
 * (only one bpf_spin_lock at a time).  And if you remove a task from a RQ
 * without setting pending_latch, you'll upset the delicate system we have so
 * far.
 *
 * Specifically, due to seqnums, you can abort a latch, but you can't abort a
 * migration as easily.  For the ESTALE case, Departed et al. know they can grab
 * the ball, and that the latch will fail - in part based on state that is saved
 * in the kernel (the seqnum mismatch).  You might be able to concoct a scheme
 * where Departed marks the task as "abort the migration", but don't forget
 * about PrioChange: we might want to move the task from CFS to Biff.  Unless
 * your migration is doing a latch, it's hard to "get the ball", and you don't
 * have the task lock (e.g. during PNT or a cpu timer tick).
 *
 * One option would be to have a BPF helper that migrates, but does not latch, a
 * task.  That way, if you're in PNT or a timer tick, if we migrate the task to
 * your cpu, your cpu's RQ lock is now the task's lock.  You still would only be
 * able to pull tasks to your cpu, so for now, I'd just use the latching
 * mechanism (note that the "migration helper" is a subset of the functionality
 * of latching, and it has the important property of "you have this task's RQ
 * lock, even if you aren't in that task's message handler").
 */

#endif // GHOST_LIB_BPF_BPF_FLUX_HEADER_BPF_H_
