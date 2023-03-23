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
 * Biff: the bpf-only ghost scheduler.
 * brho@google.com
 *
 * The world's dumbest scheduler just needs to handle the messages in bpf-msg
 * for new, wakeup, preempt, and yield, and then enqueue those in a global
 * queue.  Pop tasks from the queue in bpf-pnt.  You only need a single map for
 * the global queue.  You can do all of this in probably 100 lines of code.
 * I've commented the policy bits with "POLICY" for easy grepping.
 *
 * But any real scheduler will want more, so Biff has a few extras:
 * - want to know when tasks block, run, etc, so handle the other message types
 * - want data structures to track that info, such as "which task is on which
 *   cpu", or "how much runtime has this task had".  generically, that's a
 *   per-cpu data structure and a per-task data structure, with lookup helpers.
 * - and example of how to preempt tasks based on some basic policy.
 *
 * FAQ:
 * - Do we need to request SEND_TASK_ON_CPU?  Yes.  (This has the kernel
 *   generate a TASK_ON_CPU message when we got on_cpu).  Arguably, if
 *   bpf_ghost_run_gtid() returns successfully, our task was latched, and we
 *   could run the contents of handle_on_cpu() in bpf-pnt.  However, we gain
 *   two things from the message handler: the cpu_seqnum (used in resched) and
 *   synchronization.  bpf-msg runs with the RQ lock for the task
 *   held.  bpf-pnt does not.  By deferring the work to handle_on_cpu(), we
 *   don't have to worry about concurrent changes to the task's data structures.
 *
 * - Can we do something smarter with dont_idle?  Yes!  Right now, we just tell
 *   the cpu to not idle.  The kernel will schedule the idle task, which will
 *   instantly yield back to the kernel, which will call bpf-pnt again.  A
 *   smarter scheduler could use the length of the runqueue to determine how
 *   many cpus to wake up.  (If you do this, make sure to handle the EBUSY case
 *   below by rescheding your cpu).
 *
 * - What happens if any of the bpf operations fail?  You're out of luck.  If
 *   the global_rq overflows (65k tasks) or bpf_ghost_run_gtid() fails with an
 *   esoteric error code, we might lose track of a task.  As far as the kernel
 *   is concerned, the task is sitting on the runqueue, but bpf will never run
 *   it.  There are a few ways out:
 *   - if we detect an error, add infrastructure to pass the task to userspace,
 *   which can try to handle it in a more forgiving environment than bpf.
 *   - userspace can periodically poll the status word table for runnable tasks
 *   that aren't getting cpu time.
 *   - make sure userspace sets an enclave runnable_timeout.  If bpf fails to
 *   schedule a runnable task, the kernel will destroy the enclave and this
 *   scheduler, and all of our tasks will fall back to CFS.
 *   - another option for userspace's recovery is to tell the kernel to generate
 *   a task new for every task.  We already do this for Discovery, during agent
 *   live update.
 *
 * - When do we receive messages without having had a task_new first?  During
 *   agent live update, which is when there are tasks already in the enclave
 *   when we start up.  Thanks to the existence of the 'agent task' on every
 *   enclave cpu that runs a ghost task, we know that no ghost tasks are running
 *   during the handoff.  This means we can only receive a few messages:
 *   new (for tasks that join while we are initializing), wakeup and departed.
 *   The handlers for all of these can deal with receiving an unexpected
 *   message.  e.g. handle_wakeup() will fail gtid_to_swd().  Note that we just
 *   ignore the wakeup message, knowing that we'll eventually receive a task_new
 *   for it (due to how Discovery works).
 *
 * - Why can't we receive a task_dead during an agent handoff?  I keep having to
 *   look this up.  The exit path is do_exit() -> __schedule() ->
 *   finish_task_switch() -> task_dead_ghost().  A task needs to be running to
 *   go do_exit().  (Incidentally, in __schedule(), the task blocks, so we'll
 *   get a task_blocked before task_dead).  Until we run a task, it can't exit.
 *   Additionally, we can't be in the middle of any context switches either.
 *   The last (ghost-class) task to run on a cpu from the old agent-process was
 *   that cpu's agent-task.  The next task to run is the agent-task from the new
 *   agent-process.  After all of that, we insert the bpf programs.  So we
 *   couldn't be context-switching from the dying task that ran from the old
 *   agent to the new agent or something like that.  There's a context switch
 *   from dying-task to old-agent-task in between.
 */


// vmlinux.h must be included before bpf_helpers.h
// clang-format off
#include "kernel/vmlinux_ghost_5_11.h"
#include "libbpf/bpf_helpers.h"
#include "libbpf/bpf_tracing.h"
// clang-format on

#include "third_party/bpf/biff_bpf.h"
#include "third_party/bpf/common.bpf.h"
#include "third_party/bpf/topology.bpf.h"

#include <asm-generic/errno.h>

bool initialized;

/*
 * You can't hold bpf spinlocks and make helper calls, which include looking up
 * map elements.  To use 'intrusive' list struct embedded cpu_data and sw_data
 * (e.g.  a 'next' index/pointer for a singly-linked list), and to manipulate
 * those structs while holding a lock, we need to safely access fields by index
 * without calling bpf_map_lookup_elem().
 *
 * We can do so with...  drumroll... another layer of indirection.  (Sort of).
 * The Array map is a map of a single struct, which contains the entire array
 * that we want to access.  So once we do a single lookup, we have access to the
 * entire array.  e.g. lookup(cpu_data, 0), and now we can access cpu[x],
 * cpu[y], cpu[z], etc.
 *
 * The data layout of the Array map is still just an array of structs; we just
 * have an intermediate struct (e.g. __cpu_arr) to convince the verifier this is
 * safe.  Userspace still can cast the array map to an array of
 * biff_bpf_cpu_data[BIFF_MAX_CPUS].
 */
struct __cpu_arr {
	struct biff_bpf_cpu_data e[BIFF_MAX_CPUS];
};
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct __cpu_arr);
	__uint(map_flags, BPF_F_MMAPABLE);
} cpu_data SEC(".maps");

struct __sw_arr {
	struct biff_bpf_sw_data e[BIFF_MAX_GTIDS];
};
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct __sw_arr);
	__uint(map_flags, BPF_F_MMAPABLE);
} sw_data SEC(".maps");

/*
 * Hash map of task_sw_info, indexed by gtid, used for getting the SW info to
 * lookup the *real* per-task data: the sw_data.
 *
 * Also, we can't use BPF_MAP_TYPE_TASK_STORAGE since we don't have the
 * task_struct pointer.  Ghost BPF doesn't really have access to kernel
 * internals - it's more an extension of userspace.
 *
 * aligned(8) since this is a bpf map value.
 */
struct task_sw_info {
	uint32_t id;
	uint32_t index;
} __attribute__((aligned(8)));

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, BIFF_MAX_GTIDS);
	__type(key, u64);
	__type(value, struct task_sw_info);
} sw_lookup SEC(".maps");

/* Helper, from cpu id to per-cpu data blob */
static struct biff_bpf_cpu_data *cpu_to_pcpu(u32 cpu)
{
	struct __cpu_arr *__ca;
	u32 zero = 0;

	__ca = bpf_map_lookup_elem(&cpu_data, &zero);
	if (!__ca)
		return NULL;
	BPF_MUST_CHECK(cpu);
	if (cpu >= BIFF_MAX_CPUS)
		return NULL;
	return &__ca->e[cpu];
}

/* Helper, from gtid to per-task sw_data blob */
static struct biff_bpf_sw_data *gtid_to_swd(u64 gtid)
{
	struct task_sw_info *swi;
	struct __sw_arr *__swa;
	u32 zero = 0;
	u32 idx;

	swi = bpf_map_lookup_elem(&sw_lookup, &gtid);
	if (!swi)
		return NULL;
	idx = swi->index;
	if (idx >= BIFF_MAX_GTIDS)
		return NULL;
	__swa = bpf_map_lookup_elem(&sw_data, &zero);
	if (!__swa)
		return NULL;
	return &__swa->e[idx];
}

static void task_started(u64 gtid, int cpu, u64 cpu_seqnum)
{
	struct biff_bpf_cpu_data *pcpu;

	pcpu = cpu_to_pcpu(cpu);
	if (!pcpu)
		return;
	pcpu->current = gtid;
	pcpu->cpu_seqnum = cpu_seqnum;
}

static void task_stopped(int cpu)
{
	struct biff_bpf_cpu_data *pcpu;

	pcpu = cpu_to_pcpu(cpu);
	if (!pcpu)
		return;
	pcpu->current = 0;
}

static struct biff_bpf_sw_data *get_current(int cpu)
{
	struct biff_bpf_cpu_data *pcpu;

	pcpu = cpu_to_pcpu(cpu);
	if (!pcpu)
		return NULL;
	if (!pcpu->current)
		return NULL;
	return gtid_to_swd(pcpu->current);
}

/* Forces the cpu to reschedule and eventually call bpf-pnt. */
static int resched_cpu(int cpu)
{
	const int flags = RESCHED_GHOST_CLASS | RESCHED_IDLE_CLASS | SET_MUST_RESCHED;

	return bpf_ghost_resched_cpu2(cpu, flags);
}

/* Biff POLICY: dumb global fifo.  No locality, etc. */

struct rq_item {
	u64 gtid;
	u32 task_barrier;
};

struct {
	__uint(type, BPF_MAP_TYPE_QUEUE);
	__uint(max_entries, BIFF_MAX_GTIDS);
	__type(value, struct rq_item);
} global_rq SEC(".maps");

/* POLICY */
static void enqueue_task(u64 gtid, u32 task_barrier)
{
	/*
	 * Need to explicitly zero the entire struct, otherwise you get
	 * "invalid indirect read from stack".  Explicitly setting .gtid and
	 * .task_barrier in the definition (whether p is an array or just a
	 * struct) isn't enough.  I guess the verifier can't tell we aren't
	 * passing uninitialized stack data to some function.
	 */
	struct rq_item p[1] = {0};
	int err;

	p->gtid = gtid;
	p->task_barrier = task_barrier;
        err = bpf_map_push_elem(&global_rq, p, 0);
	if (err) {
		/*
		 * If we fail, we'll lose the task permanently.  This is where
		 * it's helpful to have userspace involved, even if just epolled
		 * on a bpf ring_buffer map to handle it by trying to shove the
		 * task into the queue again.
		 */
		bpf_printd("failed to enqueue %p, err %d\n", gtid, err);
	}
}

SEC("ghost_sched/pnt")
int biff_pnt(struct bpf_ghost_sched *ctx)
{
	struct rq_item next[1];
	int err;

	if (!initialized) {
		/*
		 * Until the agent completes Discovery, don't schedule anything.
		 * Keeping the system quiescent makes it easier to handle corner
		 * cases.  Specifically, since tasks are not running, we don't
		 * need to deal with unexpected preempt/blocked/yield/switchtos.
		 */
		set_dont_idle(ctx);
		return 0;
	}

	/*
	 * Don't bother picking a task to run if any of these are true.  If the
	 * agent runs or CFS preempts us, we'll just get the latched task
	 * preempted.  next_gtid is a task we already scheduled (via txn or was
	 * previously running), but did not request a resched for.
	 *
	 * Note it is might_yield, not "will_yield", so there's a chance the CFS
	 * tasks gets migrated away while the RQ lock is unlocked.  It's always
	 * safer to set dont_idle.
	 */
	if (ctx->agent_runnable || ctx->might_yield || ctx->next_gtid) {
		set_dont_idle(ctx);
		return 0;
	}

	/* POLICY */
	err = bpf_map_pop_elem(&global_rq, next);
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
		 * We could consider retrying, but we'd need to be careful for
		 * something like EBUSY, where we do not want to try again until
		 * we've returned to the kernel and context switched to the idle
		 * task.  Simplest to just return with dont_idle set.
		 */
		switch (-err) {
		case ENOENT:
			/* task departed, ignore */
			break;
		case ESTALE:
			/*
			 * task_barrier is old.  since we "had the ball", the
			 * task should be departed or dying.  it's possible for
			 * it to depart and be readded (which will generate a
			 * new message), so just ignore it.
			 */
			break;
		case EBUSY:
			/*
			 * task is still on_cpu.  this happens when it was
			 * preempted (we got the message early in PNT), and we
			 * are trying to pick what runs next, but the task
			 * hasn't actually gotten off cpu yet.  if we reenqueue,
			 * select the idle task, and then either set dont_idle
			 * or resched ourselves, we'll rerun bpf-pnt after the
			 * task got off cpu.
			 */
			enqueue_task(next->gtid, next->task_barrier);
			break;
		case ERANGE:
		case EXDEV:
		case EINVAL:
		case ENOSPC:
		default:
			/*
			 * Various issues, none of which should happen from PNT,
			 * since we are called from an online cpu in the
			 * enclave with an agent.  Though as we change the
			 * kernel, some of these may occur.  Reenqueue and hope
			 * for the best.
			 *   - ERANGE: cpu is offline
			 *   - EXDEV: cpu is not in the enclave
			 *   - EINVAL: Catchall, shouldn't happen.  Other than
			 *   stuff like "bad run flags", another scenario is "no
			 *   agent task".  That shouldn't happen, since we run
			 *   bpf-pnt only if there is an agent task
			 *   (currently!).
			 *   - ENOSPC: corner case in __ghost_run_gtid_on()
			 *   where CFS is present, though right now it shouldn't
			 *   be reachable from bpf-pnt.
			 */
			bpf_printd("failed to run %p, err %d\n", next->gtid, err);
			enqueue_task(next->gtid, next->task_barrier);
			break;
		}
	}

done:
	/*
	 * POLICY
	 *
	 * Alternatively, we could use bpf_ghost_resched_cpu() for fine-grained
	 * control of cpus idling or not.
	 */
	ctx->dont_idle = true;

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
static void __attribute__((noinline)) handle_new(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_new *new = &msg->newt;
	struct task_sw_info swi[1] = {0};
	struct biff_bpf_sw_data *swd;
	u64 gtid = new->gtid;
	u64 now = bpf_ktime_get_us();

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

	swd = gtid_to_swd(gtid);
	if (!swd)
		return;
	swd->parent = new->parent_gtid;
	swd->ran_until = now;
	if (new->runnable) {
		swd->runnable_at = now;
		enqueue_task(gtid, msg->seqnum);
	}
}

static void __attribute__((noinline)) handle_on_cpu(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_on_cpu *on_cpu = &msg->on_cpu;
	struct biff_bpf_sw_data *swd;
	u64 gtid = on_cpu->gtid;

	swd = gtid_to_swd(gtid);
	if (!swd)
		return;
	swd->ran_at = bpf_ktime_get_us();

	task_started(gtid, on_cpu->cpu, on_cpu->cpu_seqnum);
}

static void __attribute__((noinline)) handle_blocked(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_blocked *blocked = &msg->blocked;
	struct biff_bpf_sw_data *swd;
	u64 gtid = blocked->gtid;

	swd = gtid_to_swd(gtid);
	if (!swd)
		return;
	swd->ran_until = bpf_ktime_get_us();

	task_stopped(blocked->cpu);
}

static void __attribute__((noinline)) handle_wakeup(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_wakeup *wakeup = &msg->wakeup;
	struct biff_bpf_sw_data *swd;
	u64 gtid = wakeup->gtid;
	u64 now = bpf_ktime_get_us();

	swd = gtid_to_swd(gtid);
	if (!swd)
		return;
	swd->runnable_at = now;

	enqueue_task(gtid, msg->seqnum);
}

static void __attribute__((noinline)) handle_preempt(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_preempt *preempt = &msg->preempt;
	struct biff_bpf_sw_data *swd;
	u64 gtid = preempt->gtid;
	int cpu = preempt->cpu;
	u64 now = bpf_ktime_get_us();

	swd = gtid_to_swd(gtid);
	if (!swd)
		return;
	swd->ran_until = now;
	swd->runnable_at = now;

	task_stopped(cpu);

	enqueue_task(gtid, msg->seqnum);
}

static void __attribute__((noinline)) handle_yield(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_yield *yield = &msg->yield;
	struct biff_bpf_sw_data *swd;
	u64 gtid = yield->gtid;
	int cpu = yield->cpu;
	u64 now = bpf_ktime_get_us();

	swd = gtid_to_swd(gtid);
	if (!swd)
		return;
	swd->ran_until = now;
	swd->runnable_at = now;

	task_stopped(cpu);

	enqueue_task(gtid, msg->seqnum);
}

static void __attribute__((noinline)) handle_switchto(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_switchto *switchto = &msg->switchto;
	struct biff_bpf_sw_data *swd;
	u64 gtid = switchto->gtid;
	u64 now = bpf_ktime_get_us();

	swd = gtid_to_swd(gtid);
	if (!swd)
		return;
	swd->ran_until = now;

	/*
	 * If we knew who we switched to and if we got these messages for every
	 * switchto (instead of just the first), we could update pcpu->current.
	 */
}

static void __attribute__((noinline)) handle_dead(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_dead *dead = &msg->dead;
	u64 gtid = dead->gtid;

	bpf_map_delete_elem(&sw_lookup, &gtid);
}

static void __attribute__((noinline)) handle_departed(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_departed *departed = &msg->departed;
	u64 gtid = departed->gtid;

	if (departed->was_current)
		task_stopped(departed->cpu);

	bpf_map_delete_elem(&sw_lookup, &gtid);
}

static void __attribute__((noinline)) handle_cpu_tick(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_cpu_tick *cpu_tick = &msg->cpu_tick;
	struct biff_bpf_sw_data *swd;
	int cpu = cpu_tick->cpu;

	swd = get_current(cpu);
	if (!swd)
		return;

	/*
	 * Arbitrary POLICY: kick anyone off cpu after 50ms, and kick the
	 * sibling too, just because we can.
	 */
	if (bpf_ktime_get_us() - swd->ran_at > 50000) {
		resched_cpu(cpu);
		resched_cpu(sibling_of(cpu));
	}
}

static void __attribute__((noinline))
handle_cpu_available(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_cpu_available *avail = &msg->cpu_available;
	struct biff_bpf_cpu_data *pcpu;
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
	struct biff_bpf_cpu_data *pcpu;
	int cpu = busy->cpu;

	pcpu = cpu_to_pcpu(cpu);
	if (!pcpu)
		return;
	pcpu->available = false;
}

SEC("ghost_msg/msg_send")
int biff_msg_send(struct bpf_ghost_msg *msg)
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
	}

	/* Never send the message to userspace: no one is listening. */
	return 1;
}

SEC("ghost_select_rq/select_rq")
int biff_select_rq(struct bpf_ghost_select_rq *ctx)
{
	u64 gtid = ctx->gtid;
	/* Can't pass ctx->gtid to gtid_to_thread (swd) directly.  (verifier) */
	struct biff_bpf_sw_data *t = gtid_to_swd(gtid);

	if (!t) {
		bpf_printd("Got select_rq without a task!");
		return -1;
	}

	/*
	 * POLICY
	 *
	 * Not necessarily a good policy.  The combo of skip + picking the
	 * task_cpu will grab remote cpus RQ locks for remote wakeups.  This is
	 * just an example of what you can do.
	 */
	ctx->skip_ttwu_queue = true;

	return ctx->task_cpu;
}

char LICENSE[] SEC("license") = "GPL";
