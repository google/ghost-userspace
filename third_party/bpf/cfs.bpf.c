/*
 * Copyright 2022 Google LLC
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 or later as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * CFS: the bpf-only CFS ghost scheduler.
 * shauryapatel@google.com
 *
 * This is the first attempt at implementing CFS in ghost, in the current
 * version we just attempt to model tasks without priority and weights
 * just using vruntime and linkedlist.
 */

#include <stdbool.h>

// clang-format off
#include <linux/bpf.h>
#include "libbpf/bpf_helpers.h"
#include "libbpf/bpf_tracing.h"
// clang-format on

#include "lib/ghost_uapi.h"
#include "third_party/bpf/cfs_bpf.h"
#include "third_party/bpf/common.bpf.h"
#include "lib/queue.bpf.h"

#include <asm-generic/errno.h>

#define NICE_0_LOAD 1024

// Assume SMP.
#define ENQUEUE_WAKEUP		0x01
#define ENQUEUE_MIGRATED	0x40
/*
 * Targeted preemption latency for CPU-bound tasks:
 *
 * NOTE: this latency value is not the same as the concept of
 * 'timeslice length' - timeslices in CFS are of variable length
 * and have no persistent notion like in traditional, time-slice
 * based scheduling concepts.
 *
 * (to see the precise effective timeslice length of your workload,
 *  run vmstat and monitor the context-switches (cs) field)
 *
 * (default: 6ms * (1 + ilog(ncpus)), units: nanoseconds)
 */
unsigned int sched_latency			 = 6000ULL;

/*
 * Minimal preemption granularity for CPU-bound tasks:
 *
 * (default: 0.75 msec * (1 + ilog(ncpus)), units: nanoseconds)
 */
unsigned int sched_min_granularity			 = 750ULL;
static unsigned int sched_nr_latency = 8;

bool initialized;

struct __cpu_arr {
	struct cfs_bpf_cpu_data e[CFS_MAX_CPUS];
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct __cpu_arr);
	__uint(map_flags, BPF_F_MMAPABLE);
} cpu_data SEC(".maps");

struct __thread_arr {
	struct cfs_bpf_thread e[CFS_MAX_GTIDS];
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct __thread_arr);
	__uint(map_flags, BPF_F_MMAPABLE);
} thread_data SEC(".maps");


struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, CFS_MAX_CPUS);
	__type(key, u32);
	__type(value, struct cfs_bpf_rq);
} cfs_rqs SEC(".maps");



/*
 * Hash map of task_sw_info, indexed by gtid, used for getting the SW info to
 * lookup the *real* per-task data: the sw_data.
 * aligned(8) since this is a bpf map value.
 */
struct task_sw_info {
	uint32_t id;
	uint32_t index;
} __attribute__((aligned(8)));

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, CFS_MAX_GTIDS);
	__type(key, u64);
	__type(value, struct task_sw_info);
} sw_lookup SEC(".maps");

/* Helper to get cpus array. */
static struct __cpu_arr *get_cpus()
{
	struct __cpu_arr *__ca;
	u32 zero = 0;

	__ca = bpf_map_lookup_elem(&cpu_data, &zero);
	if (!__ca)
		return NULL;

	return __ca;
}

/* Helper, from cpu id to per-cpu data blob */
static struct cfs_bpf_cpu_data *cpu_to_pcpu(u32 cpu)
{
	struct __cpu_arr *__ca = get_cpus();
	
	if (!__ca)
		return NULL;
	
	u32 __cpu = BPF_MUST_CHECK(cpu);
	if (__cpu >= CFS_MAX_CPUS)
		return NULL;
	return &__ca->e[__cpu];
}

/* Helper to get threads array */
static struct __thread_arr * get_threads()
{
	struct __thread_arr *threads;
	u32 zero = 0;
	threads = bpf_map_lookup_elem(&thread_data, &zero);

	return threads;
}

/* Helper, from gtid to per-task thread blob */
static struct cfs_bpf_thread *gtid_to_thread(u64 gtid)
{
	struct task_sw_info *swi;
	struct __thread_arr *__threads;
	u32 idx;

	swi = bpf_map_lookup_elem(&sw_lookup, &gtid);
	if (!swi)
		return NULL;
	__threads = get_threads();

	if (!__threads)
		return NULL;

	idx = swi->index;
	if (idx >= CFS_MAX_GTIDS)
		return NULL;
	return &__threads->e[idx];
}

/* Helper, from cpu id to cfs rq */
static struct cfs_bpf_rq *cpu_to_rq(u32 cpu)
{
	struct cfs_bpf_rq *rq;

	u32 __cpu = BPF_MUST_CHECK(cpu);
	if (__cpu > CFS_MAX_CPUS)
		return NULL;

	rq = bpf_map_lookup_elem(&cfs_rqs, &cpu);

	if (!rq)
		return NULL;

	return rq;
}
/*
 * Approximate CFS policy with a minimum feature set to run a vruntime
 * based scheduler. Instead of using a RB-tree we use a Linkedlist for
 * the runqueue
 */
// TODO: CFS has a lot of magic here for faster implementation, we
// ignore that.
static u64 __calc_delta(u64 delta_exec, unsigned long weight,
			unsigned long ld_weight)
{
	// XXX: Not a faithful reimplementation of CFS.
	// TODO: This implementation is potentially buggy revisit it.
	return delta_exec * (weight*1000/ld_weight);
}


static u64 calc_delta_fair(u64 delta, struct cfs_bpf_thread *proc)
{
	if (proc->weight != NICE_0_LOAD)
		return __calc_delta(delta, NICE_0_LOAD, proc->weight);
	return delta;
}


/* static u64 wakeup_gran(struct cfs_bpf_sw_data *curr_proc, struct cfs_bpf_sw_data *new_proc) {
  // TODO: This gran is setup by a sysctl value, replace with that eventually.
  unsigned long gran = 1000000UL;

  return calc_delta_fair(gran, new_proc);

} */

/* Helper to check if curr should be preempted */
// XXX: In the current version of ghost CFS we don't preempt current until tick
// preempt happens.
// TODO: When a task wakes up CFS checks if the current task on the CPU
// should be rescheduled. This function is used for that. The current version of
// the scheduler doesn't implement that functionality.
/* static int wakeup_preempt_entity(u64 curr, u64 gtid_new) {
  struct cfs_bpf_sw_data *curr_proc = gtid_to_swd(curr);
  // TODO: Return the correct error.
  if(!curr_proc)
    return -1;
  struct cfs_bpf_sw_data *new_proc = gtid_to_swd(gtid_new);

  if(!new_proc)
    return -1;


  u64 gran, vdiff = curr_proc->vruntime - new_proc->vruntime;

  if(vdiff <= 0)
    return -1;

  gran = wakeup_gran(curr_proc, new_proc);
  if(vdiff > gran)
    return 1;

  return 0;
} */

static void task_started(u64 gtid, int cpu, u64 cpu_seqnum)
{
	struct cfs_bpf_cpu_data *pcpu;

	pcpu = cpu_to_pcpu(cpu);
	if (!pcpu)
		return;
	
	struct cfs_bpf_rq *rq = cpu_to_rq(cpu);

	if (!rq)
		return;

	bpf_spin_lock(&rq->lock);
	pcpu->current = gtid;
	pcpu->cpu_seqnum = cpu_seqnum;
	rq->current = gtid;
	bpf_spin_unlock(&rq->lock);
}

static void task_stopped(int cpu)
{
	struct cfs_bpf_cpu_data *pcpu;

	pcpu = cpu_to_pcpu(cpu);
	if (!pcpu)
		return;
	struct cfs_bpf_rq *rq = cpu_to_rq(cpu);
	if (!rq)
		return;

	bpf_spin_lock(&rq->lock);
	pcpu->current = 0;
	rq->current = 0;
	bpf_spin_unlock(&rq->lock);
}

static struct cfs_bpf_thread *get_current(u32 cpu)
{
	struct cfs_bpf_cpu_data *pcpu;

	pcpu = cpu_to_pcpu(cpu);
	if (!pcpu)
		return NULL;
	if (!pcpu->current)
		return NULL;
	return gtid_to_thread(pcpu->current);
}

/* Forces the cpu to reschedule and eventually call bpf-pnt. */
static int resched_cpu(int cpu)
{
	const int flags = RESCHED_GHOST_CLASS | RESCHED_IDLE_CLASS | SET_MUST_RESCHED;

	return bpf_ghost_resched_cpu2(cpu, flags);
}


static inline u64 max_vruntime(u64 max_vruntime, u64 vruntime)
{
	s64 delta = (s64)(vruntime - max_vruntime);
	if (delta > 0)
		max_vruntime = vruntime;

	return max_vruntime;
}

static inline u64 min_vruntime(u64 min_vruntime, u64 vruntime)
{
	s64 delta = (s64)(vruntime - min_vruntime);
	if (delta < 0)
		min_vruntime = vruntime;

	return min_vruntime;
}

static u64 __sched_period(unsigned long nr_running)
{
	if (nr_running > sched_nr_latency)
		return nr_running * sched_min_granularity;
	else
		return sched_latency;
}
/* Helper to update rq statistics when a task dies. */
static void update_rq_on_task_dead(struct cfs_bpf_rq *rq,
				   struct cfs_bpf_thread *thread)
{
	// Update required RQ stats.
	rq->nr_running--;
	rq->weight -= thread->weight;
}

/* Helper to remove departed tasks from rq. */
static void remove_task_from_rq(struct cfs_bpf_rq *rq,
				struct cfs_bpf_thread *thread,
				struct __thread_arr *thread_list)
{
	struct cfs_bpf_thread *t;
	if (!(rq->nr_running > 0 && rq->nr_running < 1000))
		return;

	int _max = rq->nr_running;
	int i = 0;
	if (!thread_list) {
		// bpf_printd("Thread list was null\n");
		return;
	}
	
	arr_list_foreach(t, thread_list->e, CFS_MAX_GTIDS, &rq->rq_root,
			 next_task, i, _max) {
		if(t == thread)
			break;
	}

	if(t != NULL) {
		arr_list_remove(thread_list->e, CFS_MAX_GTIDS, &rq->rq_root,
				t, next_task);
		update_rq_on_task_dead(rq, thread);
	}
}


/* Function to update the min runtime for a rq. Vruntime cannot go
 * back i.e be smaller than what it is right now.
 */
static void update_min_vruntime(struct cfs_bpf_rq *rq,
				struct cfs_bpf_thread *curr,
				struct __thread_arr *thread_list)
{
	u64 vruntime = rq->min_vruntime;
	if (curr) {
		vruntime = curr->vruntime;
	}

	// Assume since we are here LL must be atleast non-empty.
	// TODO: This part is a big problem of using an LL because
	// updating the min_vruntime is a o(n) operation and cannot be
	// avoided, with the tree its O(lg n), this would be easier to do
	// in eBPF with the tree but that requires the tree to be
	// implemented.
	struct cfs_bpf_thread *thread;
	if (!(rq->nr_running > 0 && rq->nr_running < 1000))
		return;

	int _max = rq->nr_running;
	int i = 0;
	if (!thread_list) {
		// bpf_printd("Thread list was null\n");
		return;
	}

	arr_list_foreach(thread, thread_list->e, CFS_MAX_GTIDS, &rq->rq_root,
			 next_task, i, _max) {
		vruntime = min_vruntime(vruntime, thread->vruntime);
	}
	// vruntime should never go backwards.
	rq->min_vruntime = max_vruntime(rq->min_vruntime, vruntime);
}

/*
 * Select task rq tries to mimic select task rq from kernel cfs as much as
 * possible.
 * Tradeoffs made were -
 * 1. wake_flags are not available in ghost so we can't check wake_flags
 * for current cpu but, we just need it to distonguish between a new
 * task and a task that woke up.
 * CPUs allowed is decided before the task is sent to ghost, maybe
 * we need that.
 * TODO: CFS will select an idle rq or an idle sibling based on wake flags,
 * wake flags aren't really needed from kernel, we can choose based on if a
 * process has woken up or if its new what the wake flags are. This will offer
 * much better load balancing than the current version of our scheduler.
 */
/* static void select_task_rq() {

}*/

static u64 sched_slice(struct cfs_bpf_rq *rq, struct cfs_bpf_thread *thread)
{
	unsigned int nr_running = rq->nr_running;
	u64 slice = __sched_period(nr_running + !thread->on_rq);
	// TODO: CFS checks for all ancestors here.We ignore that
	// and focus only on the current task as we don't have
	// ancestor data.

	slice = __calc_delta(slice, thread->weight, rq->weight);

	// TODO: CFS will check for Base slice feature here and
	// assign a min granularity to the slice, we haven't implemented
	// this part yet.
	return slice;
}

/* We calculate the vruntime slice of a to-be-inserted task.
 *
 * vs = s/w
 */
static u64 sched_vslice(struct cfs_bpf_rq *rq, struct cfs_bpf_thread *thread)
{
	return calc_delta_fair(sched_slice(rq, thread), thread);
}

static void update_curr(uint64_t now, struct cfs_bpf_rq *rq,
			struct cfs_bpf_thread *curr,
			struct __thread_arr *thread_list)
{
	if (!curr)
		return;

	uint64_t delta_exec = now - curr->ran_at;
	if (delta_exec <= 0)
		return;

	curr->ran_at = now;

	curr->sum_exec_runtime += delta_exec;
	curr->vruntime += calc_delta_fair(delta_exec, curr);

	update_min_vruntime(rq, curr, thread_list);

	return;
}

static void place_entity(struct cfs_bpf_rq *rq, struct cfs_bpf_thread *thread,
			 int initial)
{
	uint64_t vruntime = rq->min_vruntime;

	// TODO: CFS checks for start debit here we ignore that.
	// We always give initial tasks the last slice.
	if (initial)
		  vruntime += sched_vslice(rq, thread);

	// Sleeps upto a single latency don't count.
	if (!initial) {
		  uint64_t thresh = sched_latency;

		  // TODO: CFS checks if gentle fair sleepers is set
		  // as a feature here we ignore that and assume its set
		  // to true.
		  thresh >>= 1;

		  vruntime -= thresh;
	}

	thread->vruntime = max_vruntime(thread->vruntime, vruntime);

}
static void enqueue_task(u64 gtid, u32 task_barrier, int flags)
{
	/*
	 * Need to explicitly zero the entire struct, otherwise you get
	 * "invalid indirect read from stack". Explicitly setting 
	 * gtid and
	 * task_barrier in the definition (whether p is an array or just a
	 * struct) isn't enough.  I guess the verifier can't tell we aren't
	 * passing uninitialized stack data to some function.
	 */
	u64 cpu = bpf_get_smp_processor_id();
	struct cfs_bpf_thread *current_task = get_current(cpu); 

	struct __thread_arr *thread_list = get_threads();

	if (!thread_list) {
		return;
	}
	
	
	struct task_sw_info *swi = bpf_map_lookup_elem(&sw_lookup,&gtid);
	if (!swi)
		return;

	struct cfs_bpf_thread *thread = gtid_to_thread(gtid);
	// We are in trouble if we lose tasks.
	if (!thread)
		return;

	thread->gtid = gtid;
	thread->task_barrier = task_barrier;
	uint64_t now = bpf_ktime_get_us();
	struct cfs_bpf_rq *rq = cpu_to_rq(cpu);

	if (!rq)
		return;

	bpf_spin_lock(&rq->lock);

	if (swi->index < 0 || swi->index >= CFS_MAX_GTIDS) {
		// bpf_printd("swi id value was %lu\n", swi->id);
		bpf_spin_unlock(&rq->lock);
		return;
	}

	
	if (!thread->weight)
		thread->weight = NICE_0_LOAD;
	// enqueue_entity.
	bool renorm = !(flags & ENQUEUE_WAKEUP) || (flags & ENQUEUE_MIGRATED);
	bool task_new = !(flags & ENQUEUE_WAKEUP);
	bool curr = rq->current == gtid;
	/*
	 * If we're the current task, we must renormalise before calling
	 * update_curr().
	 */
	if (renorm && curr)
		thread->vruntime += rq->min_vruntime;

	update_curr(now, rq, current_task, thread_list);

	/*
	 * Otherwise, renormalise after, such that we're placed at the
	 * current
	 * moment in time, instead of some random moment in the past. Being
	 * placed in the past could significantly boost this task to the
	 * fairness detriment of existing tasks.
	 */
	if (renorm && !curr)
		thread->vruntime += rq->min_vruntime;


	rq->weight += thread->weight;
	rq->nr_running++;
     
	if (flags & ENQUEUE_WAKEUP)
		place_entity(rq, thread, 0);
	// TODO: CFS excludes the curr task here, but we don't do that
	// because we dequeue the current task from the rq.
	// We add new tasks to head and old tasks to tail.
	// Once the tree is implemented this will not be needed but we do this
	// for now.
	if (task_new)
		arr_list_insert_head(thread_list->e, CFS_MAX_GTIDS, &rq->rq_root, thread, next_task);
	else
		arr_list_insert_tail(thread_list->e, CFS_MAX_GTIDS, &rq->rq_root, thread, next_task);

	bpf_spin_unlock(&rq->lock);

}

/*
 * Get next task from the CFS runqueue that is for the cpu requested.
 * TODO: Return the correct errors instead of enoent.
 * XXX: Currently we don't do any current checking here,
 * in case of resched it is assumed that either handle_preempt or
 * handle_blocked will handle putting current back onto the rq.
 * CFS manually checks for current and puts it back
 * instead.
 */
static int cfs_pick_next_task(uint64_t cpu, struct cfs_bpf_thread *next)
{
	uint64_t vruntime;
	// Max determines how many tasks we will check to select the one with the
	// lowest vruntime.
	uint64_t _max = 100, i;
	struct __thread_arr *thread_list = get_threads();

	if (!thread_list) {
	      bpf_printd("Thread list was null\n");
	      return -ENOENT;
	}

	struct cfs_bpf_rq *rq = cpu_to_rq(cpu);
	if (!rq)
	      return -ENOENT;

	bpf_spin_lock(&rq->lock);


	struct cfs_bpf_thread *thread_to_schedule;
	struct cfs_bpf_thread *thread = arr_list_first(thread_list->e,
						       CFS_MAX_GTIDS,
						       &rq->rq_root);

	if (!thread) {
	      bpf_spin_unlock(&rq->lock);
	      return -ENOENT;
	}
	thread_to_schedule = thread;

	vruntime = thread->vruntime;
	// XXX: We check for 100 tasks here but CFS would pick the one
	// with the lowest vruntime which would be leftmost in the tree.
	arr_list_foreach(thread, thread_list->e, CFS_MAX_GTIDS,
			 &rq->rq_root, next_task, i, _max) {

	       if (thread->vruntime < vruntime) {
			thread_to_schedule = thread;
			vruntime = thread->vruntime;
		}
	}

	if (!thread_to_schedule) {
		bpf_spin_unlock(&rq->lock);
		return -ENOENT;
	}

	if (!rq) {
		bpf_spin_unlock(&rq->lock);
		return -ENOENT;
	}

	next->gtid = thread_to_schedule->gtid;
	next->task_barrier = thread_to_schedule->task_barrier;
	next->prev_sum_exec_runtime = next->sum_exec_runtime;
	arr_list_remove(thread_list->e, CFS_MAX_GTIDS, &rq->rq_root,
			thread_to_schedule, next_task);
	bpf_spin_unlock(&rq->lock);


	bpf_printd("Task gtid is %d and %d, cpu is %d\n", thread_to_schedule->gtid, next->gtid, cpu);

	return 0;
}

SEC("ghost_sched/pnt")
int cfs_pnt(struct bpf_ghost_sched *ctx)
{
	struct cfs_bpf_thread next[1];
	int err;

	if (!initialized) {
		/*
		 * Until the agent completes Discovery, don't schedule
		 * anything.Keeping the system quiescent makes it easier
		 * to handle corner cases.  Specifically, since tasks are
		 * not running, we don't
		 * need to deal with unexpected preempt/blocked/yield
		 * /switchtos.
		 */
		set_dont_idle(ctx);
		return 0;
	}

	/*
	 * Don't bother picking a task to run if any of these are true.
	 * If the agent runs or CFS preempts us, we'll just get the
	 * latched task preempted.  next_gtid is a task we already
	 * scheduled (via txn or was previously running), but did noti
	 * request a resched for. Note it is might_yield, not "will_yield"
	 * , so there's a chance the CFS
	 * tasks gets migrated away while the RQ lock is unlocked.
	 * It's always safer to set dont_idle.
	 */
	if (ctx->agent_runnable || ctx->might_yield || ctx->next_gtid) {
		set_dont_idle(ctx);
		return 0;
	}
	
	u32 cpu = bpf_get_smp_processor_id();
	err = cfs_pick_next_task(cpu, &next[0]);
	if (err) {
		switch (-err) {
		case ENOENT:
			break;
		default:
			bpf_printd("failed to dequeue, err %d\n", err);
		}
		goto done;
	}
	err = bpf_ghost_run_gtid(next->gtid, next->task_barrier,
				 SEND_TASK_ON_CPU);
	if (err) {
		/* Three broad classes of error:
		 * - ignore it
		 * - ok, enqueue and try again
		 * - bad, enqueue and hope for the best.
		 *
		 * We could consider retrying, but we'd need to be
		 * careful for something like EBUSY, where we do not want
		 * to try again until we've returned to the kernel and
		 * context switched to the idle
		 * task.  Simplest to just return with dont_idle set.
		 */
		switch (-err) {
		case ENOENT:
			/* task departed, ignore */
			break;
		case ESTALE:
			/*
			 * task_barrier is old.  since we "had the ball",
			 * the task should be departed or dying.  it's
			 * possible for it to depart and be readded
			 * (which will generate a new message),
			 * so just ignore it.
			 */
			break;
		case EBUSY:
			/*
			 * task is still on_cpu.  this happens when it was
			 * preempted (we got the message early in PNT),
			 * and we are trying to pick what runs next,
			 * but the task hasn't actually gotten off cpu
			 * yet. if we reenqueue,select the idle task,
			 * and then either set dont_idle
			 * or resched ourselves, we'll rerun bpf-pnt
			 * after the task got off cpu.
			 */
			enqueue_task(next->gtid, next->task_barrier, 0);
			break;
		case ERANGE:
		case EXDEV:
		case EINVAL:
		case ENOSPC:
		default:
			/*
			 * Various issues, none of which should happen
			 * from PNT,
			 * since we are called from an online cpu in the
			 * enclave with an agent.  Though as we change the
			 * kernel, some of these may occur.
			 * Reenqueue and hope for the best.
			 *   - ERANGE: cpu is offline
			 *   - EXDEV: cpu is not in the enclave.
			 *   - EINVAL: Catchall, shouldn't happen
			 *   Other than stuff like "bad run flags",
			 *   another scenario is "no
			 *   agent task".  That shouldn't happen, since
			 *   we run
			 *   bpf-pnt only if there is an agent task
			 *   (currently!).
			 *   - ENOSPC: corner case in __ghost_run_gtid_on()
			 *   where CFS is present, though right now it
			 *   shouldn't be reachable from bpf-pnt.
			 */
			bpf_printd("failed to run %p, err %d\n", next->gtid, err);
			enqueue_task(next->gtid, next->task_barrier, 0);
			break;
		}
	}
done:
	/*
	 * Alternatively, we could use bpf_ghost_resched_cpu()
	 * for fine-grained control of cpus idling or not.
	 */
	ctx->dont_idle = true;

	return 0;
}

/*
 * You have to play games to get the compiler to not modify the context
 * pointer (msg).  You can load X bytes off a ctx, but if you add to ctx,
 * then load, you'll get the dreaded: "dereference of modified ctx ptr"
 * error. You can also sprinkle asm volatile ("" ::: "memory") to help
 * reduce compiler optimizations on the context.
 *
 */
static void __attribute__((noinline)) handle_new(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_new *new = &msg->newt;
	struct task_sw_info swi[1] = {0};
	struct cfs_bpf_thread *thread;
	u64 gtid = new->gtid;
	u64 now = bpf_ktime_get_us();

	swi->id = new->sw_info.id;
	swi->index = new->sw_info.index;

	if (bpf_map_update_elem(&sw_lookup, &gtid, swi, BPF_NOEXIST)) {
		/*
		 * We already knew about this task.  If a task joins the
		 * enclave during Discovery, we'll get a task_new
		 * message.  Then userspace asks for task_news for all
		 * tasks. Use the bpf map as our synchronization point,
		 * similar to how userspace agents use the task's channel
		 * association.
		 * Note that if you use "send me task news" to handle
		 * failing to enqueue a task or something (which is
		 * essentially losing a wakeup), then you may need some
		 * other mechanism to track the actual runnability of the
		 * task. i.e. make sure biff and new->runnable are in sync.
		 */
		return;
	}
	thread = gtid_to_thread(gtid);
	if (!thread)
		return;

	thread->gtid = gtid;
	thread->task_barrier = msg->seqnum;
	thread->ran_until = now;
	if (new->runnable) {
		enqueue_task(gtid, msg->seqnum, 0);
		thread->runnable_at = now;
	}
}

static void __attribute__((noinline)) handle_on_cpu(
    struct bpf_ghost_msg *msg)
{
	bpf_printd("Handle on_cpu\n");
	struct ghost_msg_payload_task_on_cpu *on_cpu = &msg->on_cpu;
	struct cfs_bpf_thread *thread;
	u64 gtid = on_cpu->gtid;

	thread = gtid_to_thread(gtid);
	if (!thread)
		return;
	thread->ran_at = bpf_ktime_get_us();

	task_started(gtid, on_cpu->cpu, on_cpu->cpu_seqnum);
}

static void __attribute__((noinline)) handle_blocked(
    struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_blocked *blocked = &msg->blocked;
	struct cfs_bpf_thread *thread;
	u64 gtid = blocked->gtid;

	thread = gtid_to_thread(gtid);
	if (!thread)
		return;
	thread->ran_until = bpf_ktime_get_us();

	task_stopped(blocked->cpu);
}

static void __attribute__((noinline)) handle_wakeup(
    struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_wakeup *wakeup = &msg->wakeup;
	struct cfs_bpf_thread *thread;
	u64 gtid = wakeup->gtid;
	u64 now = bpf_ktime_get_us();

	thread = gtid_to_thread(gtid);
	if (!thread)
		return;
	thread->runnable_at = now;
	enqueue_task(gtid, msg->seqnum, ENQUEUE_WAKEUP);
}

static void __attribute__((noinline)) handle_preempt(
    struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_preempt *preempt = &msg->preempt;
	struct cfs_bpf_thread *thread;
	u64 gtid = preempt->gtid;
	int cpu = preempt->cpu;
	u64 now = bpf_ktime_get_us();
	
	thread = gtid_to_thread(gtid);
	if (!thread)
		return;
	thread->ran_until = now;
	thread->runnable_at = now;
	task_stopped(cpu);

	enqueue_task(gtid, msg->seqnum, 0);
}

static void __attribute__((noinline)) handle_yield(
    struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_yield *yield = &msg->yield;
	struct cfs_bpf_thread *thread;
	u64 gtid = yield->gtid;
	int cpu = yield->cpu;
	u64 now = bpf_ktime_get_us();

	thread = gtid_to_thread(gtid);
	if (!thread)
		return;
	thread->ran_until = now;
	thread->runnable_at = now;

	task_stopped(cpu);

	enqueue_task(gtid, msg->seqnum, 0);
}

static void __attribute__((noinline)) handle_switchto(
    struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_switchto *switchto = &msg->switchto;
	struct cfs_bpf_thread *thread;
	u64 gtid = switchto->gtid;
	u64 now = bpf_ktime_get_us();

	thread = gtid_to_thread(gtid);
	if (!thread)
		return;
	thread->ran_until = now;

	/*
	 * If we knew who we switched to and if we got these messages for
	 * every switchto (instead of just the first), we could update
	 * pcpu->current.
	 */
}

static void __attribute__((noinline)) handle_dead(
    struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_dead *dead = &msg->dead;
	u64 gtid = dead->gtid;

	struct __thread_arr *thread_list = get_threads();

	if (!thread_list) {
		bpf_printd("Thread list was null\n");
		return;
	}
	struct cfs_bpf_thread *thread = gtid_to_thread(gtid);

	if (!thread)
		return;
	u32 cpu = bpf_get_smp_processor_id();
	struct cfs_bpf_rq *rq = cpu_to_rq(cpu);
	if (!rq)
		return;
	bpf_spin_lock(&rq->lock);
	update_rq_on_task_dead(rq, thread);
	bpf_spin_unlock(&rq->lock);
	bpf_map_delete_elem(&sw_lookup, &gtid);
}

static void __attribute__((noinline)) handle_departed(
    struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_departed *departed = &msg->departed;
	u64 gtid = departed->gtid;

	if (departed->was_current)
		task_stopped(departed->cpu);

	struct __thread_arr *thread_list = get_threads();

	if (!thread_list) {
		bpf_printd("Thread list was null\n");
		return;
	}
	struct cfs_bpf_thread *thread = gtid_to_thread(gtid);

	if (!thread)
		return;
	u32 cpu = bpf_get_smp_processor_id();
	struct cfs_bpf_cpu_data *pcpu;
	pcpu = cpu_to_pcpu(cpu);

	if (!pcpu)
		return;

	struct cfs_bpf_rq *rq = cpu_to_rq(cpu);
	if (!rq)
		return;

	bpf_spin_lock(&rq->lock);
	remove_task_from_rq(rq, thread, thread_list);
	bpf_spin_unlock(&rq->lock);
	bpf_map_delete_elem(&sw_lookup, &gtid);
}

static void __attribute__((noinline)) handle_cpu_tick(
    struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_cpu_tick *cpu_tick = &msg->cpu_tick;
	struct cfs_bpf_thread *thread;
	int cpu = cpu_tick->cpu;

	thread = get_current(cpu);
	if (!thread)
		return;
	
	uint64_t ideal_runtime, delta_exec;
	
	struct cfs_bpf_cpu_data *pcpu;
	pcpu = cpu_to_pcpu(cpu);
	if (!pcpu)
		return;
	
	struct cfs_bpf_rq *rq = cpu_to_rq(cpu);
	if(!rq)
		return;

	struct __thread_arr *thread_list = get_threads();
	if(!thread_list)
		return;

	ideal_runtime = sched_slice(rq, thread);
	delta_exec = thread->sum_exec_runtime - thread->prev_sum_exec_runtime;
	
	/* XXX: The verifier doesn't like this.
	
	uint64_t now = bpf_ktime_get_us();
	bpf_spin_lock(&rq->lock);
	ideal_runtime = sched_slice(rq, thread);
	update_curr(now, rq, thread, thread_list);
	delta_exec = thread->sum_exec_runtime -
	thread->prev_sum_exec_runtime;
	bpf_spin_unlock(&rq->lock);
	*/
	// bpf_printd("Delta exec is %lu and ideal runtime is %lu\n",
	// delta_exec * 1000, ideal_runtime);
	/*
	 * Kick anyone off cpu after ideal runtime is reached,
	 * Multiply by 1000 for scaling for sched slice.
	 */
	if (delta_exec * 1000 > ideal_runtime) {
		bpf_printd("Resched cpu called\n");
		resched_cpu(cpu);
	}
}

static void __attribute__((noinline))
handle_cpu_available(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_cpu_available *avail = &msg->cpu_available;
	struct cfs_bpf_cpu_data *pcpu;
	int cpu = avail->cpu;


	pcpu = cpu_to_pcpu(cpu);
	if (!pcpu)
		return;
	pcpu->available = true;
}

static void __attribute__((noinline))
handle_cpu_busy(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_cpu_busy *busy = &msg->cpu_busy;
	struct cfs_bpf_cpu_data *pcpu;
	int cpu = busy->cpu;

	pcpu = cpu_to_pcpu(cpu);
	if (!pcpu)
		return;
	pcpu->available = false;
}

SEC("ghost_msg/msg_send")
int cfs_msg_send(struct bpf_ghost_msg *msg)
{
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
	default:
		// TODO: Handle the PRIO changed message to update
		// weight for a process.
		bpf_printd("Unhandled message\n");
		break;
	}

	/* Never send the message to userspace: no one is listening. */
	return 1;
}

char LICENSE[] SEC("license") = "GPL";
