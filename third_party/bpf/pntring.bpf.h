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

#ifndef GHOST_LIB_BPF_PNTRING_BPF_H_
#define GHOST_LIB_BPF_PNTRING_BPF_H_

// NOLINTBEGIN
// clang-format off

#ifndef __BPF__
#include <stdint.h>
#include <atomic>
#endif

/*
 * PNT rings are multi-producer, multi-consumer, power-of-two ring buffers.
 * Although we have a single global agent (producer), we may end up producing
 * into the ring from a task wakeup BPF program.
 *
 * The producers claim free slots and fill them in with a txn.  Consumers claim
 * ready slots and attempt to complete the txn.  Consumers mark when they are
 * done with a txn (completed or failed).
 *
 * One thing that may be unusual is that consumers do not mark slots empty
 * (fully consumed).  e.g. in Xen's ring buffer, the consumer's public index
 * means that a slot has been fully consumed and can be reused by the producer.
 * Since the agent needs to get an answer from the txn, the slot is not free
 * until it has been 'reaped' by the agent in userspace.  This is something the
 * producer will manage: specifically, we use task_ptr to mark if a slot is
 * reaped.
 *
 * prod_idx: the next slot for the producer to claim and produce into.
 * cons_idx: the next slot for the consumer to claim and consume
 * nr_produced: prod_idx - cons_idx
 * empty: prod_idx == cons_idx
 * full: NR_PNT_RING_SLOTS - nr_produced == 0
 *
 * Note that once a slot is 'produced', it may not be ready for immediate
 * consumption.  A producer claimed the slot, but it may take time to fill in
 * the txn.  When it has, it will set txn_state = GHOST_TXN_READY.
 * Additionally, the agent may unschedule a task that was previously in a slot.
 * If so, it will set the state to GHOST_TXN_ABORTED.  Either way, the consumer
 * consumes the slot.
 *
 * From the perspective of both the producers and consumers, we can only tell if
 * a slot is available (to be produced or consumed) if the ring indexes tell us
 * it is available and the txn_state is "done".  Put another way, we're using
 * txn_state as part of the ring protocol: it is the "ready for consumption" bit
 * from the producer as well as the "it's been consumed" bit from the consumer.
 *
 * And lastly, before the producer reuses the slot, it needs to reap it: get the
 * error code (if any) and remove the task -> slot mapping.  Reaping normally
 * happens in TaskLatched() (for success).  On error, it happens in
 * BpfReapPntErrors().  There's a third case for reaping that complicates
 * things: unscheduling/Aborting, discussed below.
 *
 * So not only do we need to have the consumer change the state to mark it has
 * consumed the txn, but we also need the producer to change the state to mark
 * it has reaped the result.  This state is GHOST_TXN_REAPED.
 *
 * The normal txn_state transition is:
 *  1 GHOST_TXN_REAPED (default/unused)
 *  2   producer claims slot with CAS(prod_idx).
 *  3   producer fills in txn, including slot->task_ptr, and task->slot
 *  4 GHOST_TXN_READY (producer write)
 *  5   consumer claims slot with CAS(cons_idx)
 *  6 state = cpu (consumer CAS write, claiming the txn)
 *  7   consumer attempts txn, gets result
 *  8 GHOST_TXN_COMPLETE / error (consumer write)
 *  9   producer runs TaskLatched() or BpfReapPntErrors()
 * 10   producer clears mapping: task->slot = NULL;
 * 11 GHOST_TXN_REAPED (producer write)
 *
 * During Unschedule, the agent sets txn_state = ABORTED.  The issue is that the
 * agent needs to reap still.  But we have to be careful: we don't know if/when
 * BPF will get around to consuming the slot.  The agent certainly doesn't want
 * to spinwait around for it.  We could treat Abort as an error case, and have
 * the agent reap during BpfReapPntErrors().  However, if we unscheduled, we may
 * want to reuse task->slot (or free the task).  So we don't want to delay until
 * Reap.
 *
 * The fix is to have bpf set REAPED.  The unscheduler knows that it
 * successfully aborted (the ball is in its court), so it can clear task->slot,
 * breaking the connection from the task to the slot.  The unscheduler cannot
 * set reaped, since the consumer needs to have seen ABORTED to consume the
 * item.  In essence, the consumer (bpf) will see ABORTED, then notionally write
 * COMPLETED, and then REAPED.  Though it can just write REAPED directly.
 *
 * The txn_state transition for a successful abort is:
 *  1 GHOST_TXN_REAPED (default/unused)
 *  2   producer claims slot with CAS(prod_idx).
 *  3   producer fills in txn, including slot->task_ptr, and task->slot
 *  4 GHOST_TXN_READY (producer write)
 *  5   consumer claims slot with CAS(cons_idx)
 *  6 GHOST_TXN_ABORT (producer CAS write)
 *  7   producer clears task->slot.  It has the ball.
 *  8   consumer failed to claim txn with CAS; sees ABORT
 *  9 GHOST_TXN_REAPED (consumer write)
 *
 * Steps 5 and 6 can happen in any order.  Once READY|ABORT, the consumer will
 * consume the slot.  For a failed abort, it looks like the normal txn_state
 * transition.  The producer merely lost the CAS race with the consumer in step
 * 6.
 *
 * Q: Why do we need REAPED?  If we don't care about the fields in slot after
 * it's been COMPLETED, who cares if we reuse a slot?
 * A: Because of Unschedule and having multiple producers.  The userspace Task
 * has a pointer to slot, and the agent may attempt to unschedule, using that
 * pointer, at any moment before it learns the disposition of the txn, i.e.
 * before reaping.  Consider if we simply set TXN_COMPLETED and let a producer
 * reuse a slot.  What if we haven't received TaskLatched() yet?  If we attempt
 * to unschedule, we'll point at a slot and a txn for another task.  Another way
 * to look at it: after step 4, when the producer writes READY, in the next
 * instant, bpf can consume and write COMPLETE.  Another producer could come
 * along, fill in a new txn and task, and write READY.  Then Unschedule's
 * CAS(READY->ABORT) succeeds, aborting the *new* transaction, not the intended
 * txn pointed to by task->slot.
 *
 * Q: Do we need to clear slot->task_ptr?
 * A: No, though in older code, I was using task_ptr as a way to determine if we
 * had been Reaped, back before GHOST_TXN_REAPED.  Once a slot's state is
 * REAPED, all of its fields are "don't care".  A new producer will reset them,
 * just like we do with slot->gtid or slot->task_barrier.
 *
 * Q: Why do we even have slot->task_ptr?
 * A: It's a convenience.  When we reap errors, we know which task it was that
 * had the error.  We could also look up based on gtid.  Or we could maintain a
 * list of all tasks in BPF and scan each task->slot for an error.
 *
 * Q: Why do we have task->slot again?
 * A: Two reasons: so we can unschedule and so we can reap from TaskLatched().
 * Though those are really the same reason, deep down.  We wouldn't need to reap
 * successes if we didn't want to support unschedule.
 *
 * Q: OK, why are we supporting Unschedule?
 * A: If the agent has an available cpu and a high priority task is sitting in
 * BPF too long, it may want to change its mind - especially if we have a ring
 * serviced by only a few cpus.  It also helps TaskDeparted() a little, though
 * if we used gtid instead of task_ptr, it'd be less of a concern.  (Bpf will
 * fail to run a departed task.)
 *
 * Q: What stops us from having Unschedule seeing GHOST_TXN_READY, but that's
 * the state for a new producer's transaction?
 * A: The reaper is single threaded: global agent only.  Unschedule checks
 * task->slot, which is cleared in reaping.  If you had Unschedule run
 * *concurrently* with TaskLatched(), then you could have that race.  This is an
 * extension of the genereal philosophy of the global agent: only one thread can
 * muck with a userspace Task's state.
 *
 * Q: Wait, you said we didn't want the agent to wait on BPF when Unscheduling,
 * but you spin!
 * A: We don't want to wait an arbitrary amount of time for BPF to *claim* a
 * transaction.  But once it has claimed it, we know it will complete quickly.
 * (O(us), actually, due to ghost_move_task(), but that's relatively quickly.)
 */

/*
 * In PNT, each cpu will pull tasks from a ring buffer and attempt to latch them
 * on its cpu.
 */

#define __PNT_RING_SLOT_ORDER 6
#define NR_PNT_RING_SLOTS (1 << __PNT_RING_SLOT_ORDER)

/*
 * Each slot is roughly equivalent to a ghost_txn.  We use the same protocol for
 * claiming txns and returning results.
 *
 * Even though the producer and consumer use the PNT ring to claim slots, we
 * still use CAS on txn_state, like with a normal ghost_txn, since userspace may
 * want to "unschedule" a task it put into a slot.
 */
struct pnt_ring_slot {
	int64_t txn_state;	/* need 64 bits for the atomic bpf op */
	uint64_t gtid;
	uint32_t task_barrier;
	uint32_t cpu;
	void *task_ptr;
} __attribute__((aligned(64)));

struct pnt_ring {
	uint64_t prod_idx;
	uint64_t cons_idx;
	struct pnt_ring_slot slots[NR_PNT_RING_SLOTS];
};

inline uint64_t pnt_ring_nr_used(uint64_t prod_idx, uint64_t cons_idx)
{
	return prod_idx - cons_idx;
}

inline uint64_t pnt_ring_nr_empty(uint64_t prod_idx, uint64_t cons_idx)
{
	return NR_PNT_RING_SLOTS - pnt_ring_nr_used(prod_idx, cons_idx);
}

inline bool pnt_ring_full(uint64_t prod_idx, uint64_t cons_idx)
{
	return pnt_ring_nr_empty(prod_idx, cons_idx) == 0;
}

inline struct pnt_ring_slot *pnt_ring_get_slot(struct pnt_ring *ring,
                                               uint64_t idx)
{
	return &ring->slots[idx & (NR_PNT_RING_SLOTS - 1)];
}

/*
 * agent_data layout:
 *
 * +---- 1 -----+---- (64 - 1 - RING_SLOT_ORDER) ----+--- RING_SLOT_ORDER ---+
 * | entry used |              ring id               |      ring index       |
 * +------------+------------------------------------+-----------------------+
 */
inline uint64_t pnt_ring_to_agent_data(uint32_t ring_id,
                                       uint32_t ring_index)
{
	return (1ULL << 63) | (ring_id << __PNT_RING_SLOT_ORDER) |
		(ring_index & (NR_PNT_RING_SLOTS - 1));
}

inline uint64_t pnt_agent_data_to_ring_id(uint64_t agent_data)
{
	return (agent_data & ~(1ULL << 63)) >> __PNT_RING_SLOT_ORDER;
}

inline uint64_t pnt_agent_data_to_ring_index(uint64_t agent_data)
{
	return agent_data & (NR_PNT_RING_SLOTS - 1);
}

inline struct pnt_ring_slot *pnt_agent_data_to_ring_slot(struct pnt_ring *rings,
                                                         uint64_t agent_data)
{
	uint64_t ring_id = pnt_agent_data_to_ring_id(agent_data);
	uint64_t ring_index = pnt_agent_data_to_ring_index(agent_data);

	return pnt_ring_get_slot(&rings[ring_id], ring_index);
}

/* GHOST_TXN_REAPED is not part of the normal txn protocol */
#define GHOST_TXN_REAPED (GHOST_TXN_READY - 1)

// clang-format on
// NOLINTEND

#endif  // GHOST_LIB_BPF_PNTRING_BPF_H_
