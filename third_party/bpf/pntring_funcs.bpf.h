// Copyright 2022 Google LLC
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// version 2 as published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

#ifndef GHOST_LIB_BPF_PNTRING_FUNCS_BPF_H_
#define GHOST_LIB_BPF_PNTRING_FUNCS_BPF_H_

// NOLINTBEGIN
// clang-format off

#include "third_party/bpf/pntring.bpf.h"

#include <asm-generic/errno.h>

/*
 * Functions for using PNT rings.  #include this in your bpf.c after your maps
 * and data structures.
 *
 * At a minimum, you need a bpf map of struct pnt_ring called "pnt_rings" and a
 * u64 called latch_error.
 *
 * For userspace, include this like a standard header.
 */

#define READ_ONCE(x) (*(volatile typeof(x) *)&(x))
#define WRITE_ONCE(x, val) ((*(volatile typeof(x) *)&(x)) = val)

#ifdef __BPF__

#include "third_party/bpf/common.bpf.h"

#define PNT_NR_RING_RETRY 5

/*
 * Attempts to latch a task from ring.  Returns true on success.  We pass cpu so
 * that we do not need to call bpf_get_smp_processor_id() for every ring
 * attempt.
 */
static inline bool pnt_latch_task_from_ring(int which_ring, int cpu)
{
	struct pnt_ring *ring;
	u64 cons_idx, prod_idx;
	struct pnt_ring_slot *slot;
	int ret;
	s32 state;	/* must be s32, see comment below */

	if (which_ring < 0)
		return false;

	ring = bpf_map_lookup_elem(&pnt_rings, &which_ring);
	if (!ring) {
		bpf_printk("Couldn't find which_ring %d for cpu %d!",
			   which_ring, cpu);
		return false;
	}

	for (int i = 0; i < PNT_NR_RING_RETRY; i++) {
		cons_idx = READ_ONCE(ring->cons_idx);
		prod_idx = READ_ONCE(ring->prod_idx);

		if (prod_idx == cons_idx)
			break;
		slot = pnt_ring_get_slot(ring, cons_idx);

		state = READ_ONCE(slot->txn_state);
		/* See if the producer has finished or revoked it. */
		if (!(state == GHOST_TXN_READY || state == GHOST_TXN_ABORTED))
			break;

		if (!__sync_bool_compare_and_swap(&ring->cons_idx, cons_idx,
						  cons_idx + 1)) {
			/* Another consumer grabbed this slot */
			continue;
		}
		/*
		 * We claimed the slot, we know we are the only consumer.
		 *
		 * txn_state is either READY or ABORTED, and if it was READY
		 * before, it may be changed concurrently to ABORTED.
		 */
		if (!__sync_bool_compare_and_swap(&slot->txn_state,
						  GHOST_TXN_READY, cpu)) {
			/* Must be ABORTED */
			state = READ_ONCE(slot->txn_state);
			if (state != GHOST_TXN_ABORTED)
				bpf_printk("Weird state 0x%x!", state);
			slot->txn_state = (__s32)GHOST_TXN_REAPED;
			continue;
		}

		/* We claimed both the slot and the txn within it. */
		slot->cpu = cpu;
		ret = bpf_ghost_run_gtid(slot->gtid, slot->task_barrier,
					 SEND_TASK_ON_CPU);

		if (!ret) {
			/*
			 * This is nasty.  The GHOST_TXN_ statuses, other than
			 * GHOST_TXN_READY, are negative numbers in an enum.
			 * BPF gets them from vmlinux.h, which has them as very
			 * large positive numbers.  e.g.
			 *
			 * 	GHOST_TXN_COMPLETE = 2147483648, aka 0x80000000
			 *
			 * When assigned (or cast) to an s32, they'll be
			 * negative.  So to make our 64 bit txn_state a negative
			 * number, we need the intermediate cast to an s32.
			 */
			slot->txn_state = (__s32)GHOST_TXN_COMPLETE;
			return true;
		}

		switch (-ret) {
		case ESTALE:
			slot->txn_state = (__s32)GHOST_TXN_TARGET_STALE;
			break;
		case EBUSY:
			slot->txn_state = (__s32)GHOST_TXN_TARGET_NOT_RUNNABLE;
			break;
		case ENOENT:
			slot->txn_state = (__s32)GHOST_TXN_TARGET_NOT_FOUND;
			break;
		case ERANGE:
			slot->txn_state = (__s32)GHOST_TXN_CPU_OFFLINE;
			break;
		case ENOSPC:
			slot->txn_state = (__s32)GHOST_TXN_CPU_UNAVAIL;
			break;
		case EXDEV:
		case EINVAL:
		default:
			slot->txn_state = (__s32)GHOST_TXN_NOT_PERMITTED;
			break;
		}

		/*
		 * Ensure the txn_state write happens before writing
		 * latch_error.
		 *
		 * Ideally we'd use smp_store_release(), which isn't
		 * readily available in BPF.  The atomic suffices, and
		 * this is rare enough to not worry about overhead.
		 */
		__sync_fetch_and_or(&latch_error, true);
	}

	return false;
}

static inline u64 pnt_push_task_to_ring(int which_ring, u64 gtid,
                                        u32 task_barrier, int cpu)
{
	struct pnt_ring *ring;
	u64 cons_idx, prod_idx;
	struct pnt_ring_slot *slot;

	if (which_ring < 0)
		return 0;

	ring = bpf_map_lookup_elem(&pnt_rings, &which_ring);
	if (!ring) {
		bpf_printk("Couldn't find which_ring %d!", which_ring);
		return 0;
	}

	for (int i = 0; i < PNT_NR_RING_RETRY; i++) {
		prod_idx = READ_ONCE(ring->prod_idx);
		cons_idx = READ_ONCE(ring->cons_idx);

		if (pnt_ring_full(prod_idx, cons_idx))
			break;
		slot = pnt_ring_get_slot(ring, prod_idx);
		if (READ_ONCE(slot->txn_state) != (__s32)GHOST_TXN_REAPED)
			break;
		if (!__sync_bool_compare_and_swap(&ring->prod_idx, prod_idx,
						  prod_idx + 1)) {
			continue;
		}

		slot->gtid = gtid;
		slot->task_barrier = task_barrier;
		slot->task_ptr = NULL;

		smp_store_release(&slot->txn_state, GHOST_TXN_READY);

		return pnt_ring_to_agent_data(which_ring, prod_idx);
	}

	return 0;
}

#else  // ! __BPF__ (userspace functions)

#include "kernel/ghost_uapi.h"

/*
 * Schedules gtid onto the ring, returns the slot pointer or NULL on failure.
 * If successful, the 'ball' is in BPF's court (or the kernel's), until the task
 * is Unscheduled or Reaped.
 */
static inline struct pnt_ring_slot *pnt_schedule_onto_ring(
		struct pnt_ring *ring, uint64_t gtid, uint64_t task_barrier,
		void *task_ptr)
{
	uint64_t prod_idx;
	uint64_t cons_idx;
	struct pnt_ring_slot *slot;
	
	do {
		prod_idx = READ_ONCE(ring->prod_idx);
		cons_idx = READ_ONCE(ring->cons_idx);
		if (pnt_ring_full(prod_idx, cons_idx))
			return NULL;
		slot = pnt_ring_get_slot(ring, prod_idx);
		if (READ_ONCE(slot->txn_state) != GHOST_TXN_REAPED)
			return NULL;
	} while (
		!__sync_bool_compare_and_swap(&ring->prod_idx, prod_idx, prod_idx + 1));
	
	slot->gtid = gtid;
	slot->task_barrier = task_barrier;
	slot->task_ptr = task_ptr;
	__atomic_store_n(&slot->txn_state, GHOST_TXN_READY, __ATOMIC_RELEASE);

	return slot;
}

/* Reap a slot for a latched task, e.g. from a MSG_TASK_ON_CPU handler. */
static inline void pnt_ring_reap_slot(struct pnt_ring_slot *slot)
{
	__atomic_store_n(&slot->txn_state, GHOST_TXN_REAPED, __ATOMIC_RELEASE);
}

/*
 * Unschedules a task that was previously submitted to pnt_schedule_onto_ring().
 *
 * Returns true if we successfully unscheduled the task, meaning the agent has
 * *regained* control over the task's state and has "yanked the ball back".
 * This includes failed latches.
 */
static inline bool pnt_unschedule_ring_slot(struct pnt_ring_slot *slot)
{
	int64_t state = __atomic_load_n(&slot->txn_state, __ATOMIC_ACQUIRE);

	if (state == GHOST_TXN_READY) {
		if (__atomic_compare_exchange_n(&slot->txn_state,
			&state, GHOST_TXN_ABORTED, /*weak=*/false,
			__ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
			/*
			 * Yanked it out before BPF looked at it.  We can't
			 * touch slot after this either.  From the moment we
			 * wrote ABORTED, BPF could have seen it and reaped it.
			 */
			return true;
		}
		/*
		 * Failed to yank it out.  We shouldn't touch slot's contents
		 * here.  It hasn't been reaped yet, but the consumer may still
		 * be using it.  Once it is COMPLETE, we can do other things.
		 */
	}
	
	while ((state = __atomic_load_n(&slot->txn_state, __ATOMIC_ACQUIRE))
		>= 0) {
		/*
		 * BPF claimed it but is not done yet.  We can safely spin; the
		 * kernel ensures BPF will finish.  (Presuming no bugs with
		 * BPF).
		 */
		;
	}
	if (state == GHOST_TXN_COMPLETE) {
		/*
		 * Latching completed.  We'll get a message for this and reap in
		 * TaskOnCpu().
		 */
		return false;
	}
	/* Latching failed: treat it like we aborted.  The ball is ours again.*/
	pnt_ring_reap_slot(slot);
	return true;
}

/*
 * Attempt to reap a slot that may have had an error when latching.  You need to
 * do this periodically, or whenever you were told there was an error, e.g. from
 * bpf setting latched_error = true.
 *
 * You need to reap all PNT ring slots to catch tasks that failed to latch as
 * well as to free the slot for future use.  Returns:
 * -1 : Try again, and you might have missed an error.  You should manually
 *  set latched_error = true to retrigger this.
 *  0 : No errors, but not reaped.  Likely not looked at yet by bpf.
 *  1 : We reaped an error.  Which means something went wrong with the latching,
 *  we freed up the slot for reuse, and you should probably reenqueue the task.
 */
static inline int pnt_ring_reap_errors(struct pnt_ring_slot *slot)
{
	int64_t state  = __atomic_load_n(&slot->txn_state, __ATOMIC_ACQUIRE);

	if (state == GHOST_TXN_COMPLETE) {
		/* Successful latches get reaped in TaskOnCpu(). */
		return 0;
	}
	if (state == GHOST_TXN_ABORTED) {
		/* Abort in progress, not an error */
		return 0;
	}
	if (state == GHOST_TXN_REAPED)
		return 0;
	if (state == GHOST_TXN_READY)
		return 0;
	if (state >= 0) {
		/*
		 * BPF claimed it, but it is still running.  We can reap it next
		 * time.  Reminder: All error values are negative.  A txn is
		 * "claimed" by writing the cpuid in for state (>= 0).
		 */
		return 0;
	}
	if (!slot->task_ptr) {
		/*
		 * BPF-MSG produced this task.  BPF-PNT already picked it up and
		 * failed it, however we have not yet handled the message that
		 * set slot->task_ptr.  Skip this one, and we'll have to rerun
		 * BpfReapPntErrors() next time we run the global loop.
		 */
		return -1;
	}
	
	/*
	 * We attempted to run the task via the BPF PNT program, but it failed
	 * for some reason.  Since the task was runnable when we passed it to
	 * BPF, the only message we could have receive about it is
	 * TASK_DEPARTED.  If the task departed, either we reap it here xor
	 * BpfUnscheduleViaPnt returns true: not both.  (This is controlled by
	 * task->slot.  Note that if BPF-MSG produced the task, we would have
	 * handled the corresponding message, e.g. TASK_WAKEUP, before
	 * TASK_DEPARTED, so task->slot will be valid.)
	 */
	pnt_ring_reap_slot(slot);
	return 1;
}

#endif  // !__BPF__

// clang-format on
// NOLINTEND

#endif  // GHOST_LIB_BPF_PNTRING_FUNCS_BPF_H_
