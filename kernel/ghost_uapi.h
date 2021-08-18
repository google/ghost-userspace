/*
 * Copyright 2021 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _SCHED_GHOST_H_
#define _SCHED_GHOST_H_

#include <linux/ioctl.h>

#ifndef __KERNEL__
#include <limits.h>
#include <stdint.h>
#endif

// NOLINTBEGIN
// clang-format off

/*
 * The version of ghOSt. It is important that the kernel and the userspace
 * process are the same version as each other. Each successive version changes
 * values in this header file, assumptions about operations in the kernel, etc.
 */
#define GHOST_VERSION	36

/*
 * Define SCHED_GHOST via the ghost uapi unless it has already been defined
 * via the proper channels (i.e. the official <sched.h> uapi header file).
 */
#ifndef SCHED_GHOST
#define SCHED_GHOST		18
#endif

/*
 * This is userspace API so the usual suspects like __cacheline_aligned
 * will not work.
 *
 * TODO: these are architecture-specific.
 */
#define __ghost_cacheline_aligned	__attribute__((__aligned__(64)))
#define __ghost_page_aligned		__attribute__((__aligned__(4096)))

enum ghost_type {
	GHOST_AGENT,
	GHOST_TASK,
};

enum {
	GHOST_SCHED_TASK_PRIO,
	GHOST_SCHED_AGENT_PRIO,
	GHOST_SCHED_MAX_PRIO,
};

/*
 * Collateral for GHOST_CONFIG_QUEUE_WAKEUP op:
 * - kernel supports up to 'GHOST_MAX_WINFO' wakeup targets for a queue.
 * - 'prio' must be zero (intended to be a tiebreaker but not defined yet).
 */
#define GHOST_MAX_WINFO	2
struct ghost_agent_wakeup {
	int cpu;
	int prio;
};

/*
 * <type,arg> tuple decoding:
 * 'type' is GHOST_TASK	 -> 'arg' is <gtid>
 * 'type' is GHOST_AGENT -> 'arg' is <cpu>
 */
struct ghost_msg_src {
	enum ghost_type type;
	uint64_t arg;
};
#define GHOST_THIS_CPU	-1

struct ghost_sw_info {
	uint32_t id;		/* status_word region id */
	uint32_t index;		/* index into the status_word array */
};

struct ghost_ioc_sw_get_info {
	struct ghost_msg_src request;
	struct ghost_sw_info response;
};

#define GHOST_IOC_NULL			_IO('g', 0)
#define GHOST_IOC_SW_GET_INFO		_IOWR('g', 1, struct ghost_ioc_sw_get_info *)
#define GHOST_IOC_SW_FREE		_IOW('g', 2, struct ghost_sw_info *)

/*
 * Status word region APIs.
 */
struct ghost_sw_region_header {
	uint32_t version;	/* ABI version */
	uint32_t id;		/* region id */
	uint32_t numa_node;
	uint32_t start;		/* offset of the first status word */
	uint32_t capacity;	/* total status words in this region */
	uint32_t available;	/* free status words in this region */
} __ghost_cacheline_aligned;
#define GHOST_SW_REGION_VERSION	0

/*
 * Align status words up to the next power of two so we do not cross cache lines
 * unnecessarily.  At our current size, that is one cache line per status word,
 * and two status words per cache line.
 */
struct ghost_status_word {
	uint32_t barrier;
	uint32_t flags;
	uint64_t gtid;
	int64_t switch_time;	/* time at which task was context-switched onto CPU */
	uint64_t runtime;	/* total time spent on the CPU in nsecs */
} __attribute__((__aligned__(32)));

#define GHOST_SW_F_INUSE	(1U << 0)    /* status_word in use */
#define GHOST_SW_F_CANFREE	(1U << 1)    /* status_word can be freed */

/* agent-specific status_word.flags */
#define GHOST_SW_CPU_AVAIL	(1U << 8)    /* CPU available hint */
#define GHOST_SW_BOOST_PRIO     (1U << 9)    /* agent with boosted prio */

/* task-specific status_word.flags */
#define GHOST_SW_TASK_ONCPU	(1U << 16)   /* task is oncpu */
#define GHOST_SW_TASK_RUNNABLE	(1U << 17)   /* task is runnable */
#define GHOST_SW_TASK_IS_AGENT  (1U << 18)

/*
 * Queue APIs.
 */
struct ghost_queue_header {
	uint32_t version;	/* ABI version */
	uint32_t start;		/* offset from the header to start of ring */
	uint32_t nelems;	/* power-of-2 size of ghost_ring.msgs[] */
} __ghost_cacheline_aligned;
#define GHOST_QUEUE_VERSION	0

struct ghost_msg {
	uint16_t type;		/* message type */
	uint16_t length;	/* length of this message including payload */
	uint32_t seqnum;	/* sequence number for this msg source */
	uint32_t payload[0];	/* variable length payload */
};

/*
 * Messages are grouped by type and each type can have up to 64 messages.
 * (the limit of 64 is arbitrary).
 */
#define _MSG_TASK_FIRST	64
#define _MSG_TASK_LAST	(_MSG_TASK_FIRST + 64 - 1)

#define _MSG_CPU_FIRST	128
#define _MSG_CPU_LAST	(_MSG_CPU_FIRST + 64 - 1)

/* ghost message types */
enum {
	/* misc msgs */
	MSG_NOP			= 0,

	/* task messages */
	MSG_TASK_DEAD		= _MSG_TASK_FIRST,
	MSG_TASK_BLOCKED,
	MSG_TASK_WAKEUP,
	MSG_TASK_NEW,
	MSG_TASK_PREEMPT,
	MSG_TASK_YIELD,
	MSG_TASK_DEPARTED,
	MSG_TASK_SWITCHTO,
	MSG_TASK_AFFINITY_CHANGED,

	/* cpu messages */
	MSG_CPU_TICK		= _MSG_CPU_FIRST,
	MSG_CPU_TIMER_EXPIRED,
	MSG_CPU_NOT_IDLE,	/* requested via run_flags: NEED_CPU_NOT_IDLE */
};

/* TODO: Move payload to header once all clients updated. */
struct ghost_msg_payload_task_new {
	uint64_t gtid;
	uint64_t runtime;	/* cumulative runtime in ns */
	uint16_t runnable;
	struct ghost_sw_info sw_info;
};

struct ghost_msg_payload_task_preempt {
	uint64_t gtid;
	uint64_t runtime;	/* cumulative runtime in ns */
	int cpu;
	char from_switchto;
};

struct ghost_msg_payload_task_yield {
	uint64_t gtid;
	uint64_t runtime;	/* cumulative runtime in ns */
	int cpu;
	char from_switchto;
};

struct ghost_msg_payload_task_blocked {
	uint64_t gtid;
	uint64_t runtime;	/* cumulative runtime in ns */
	int cpu;
	char from_switchto;
};

struct ghost_msg_payload_task_dead {
	uint64_t gtid;
};

struct ghost_msg_payload_task_departed {
	uint64_t gtid;
	int cpu;
	char from_switchto;
};

struct ghost_msg_payload_task_affinity_changed {
	uint64_t gtid;
};

struct ghost_msg_payload_task_wakeup {
	uint64_t gtid;
	char deferrable;	/* bool: 0 or 1 */

	int last_ran_cpu;	/*
				 * CPU that task last ran on (may be different
				 * than where it was last scheduled by the
				 * agent due to switchto).
				 */

	int wake_up_cpu;	/*
				 * CPU where the task was woken up (this is
				 * typically where the task last ran but it
				 * may also be the waker's cpu).
				 */

	int waker_cpu;		/* CPU of the waker task */
};

struct ghost_msg_payload_task_switchto {
	uint64_t gtid;
	uint64_t runtime;	/* cumulative runtime in ns */
	int cpu;
};

struct ghost_msg_payload_cpu_not_idle {
	int cpu;
	uint64_t next_gtid;
};

struct ghost_msg_payload_cpu_tick {
	int cpu;
};

struct ghost_msg_payload_timer {
	int cpu;
	uint64_t cookie;
};

#ifdef __cplusplus
#include <atomic>
typedef std::atomic<uint32_t> _ghost_ring_index_t;
#else
typedef volatile uint32_t _ghost_ring_index_t;
#endif

struct ghost_ring {
	/*
	 * kernel produces at 'head & (nelems-1)' and
	 * agent consumes from 'tail & (nelems-1)'.
	 *
	 * kernel increments 'overflow' any time there aren't enough
	 * free slots to produce a message.
	 */
	_ghost_ring_index_t head;
	_ghost_ring_index_t tail;
	_ghost_ring_index_t overflow;
	struct ghost_msg msgs[0];  /* array of size 'header->nelems' */
};

#define GHOST_MAX_QUEUE_ELEMS	65536	/* arbitrary */

/*
 * Define ghOSt syscall numbers here until they can be discovered via
 * <unistd.h>.
 */
#ifndef __NR_ghost_run
#define __NR_ghost_run	450
#endif
#ifndef __NR_ghost
#define __NR_ghost	451
#endif

/*
 * 'ops' supported by gsys_ghost().
 */
enum ghost_ops {
	GHOST_NULL,
	GHOST_CREATE_QUEUE,
	GHOST_ASSOCIATE_QUEUE,
	GHOST_CONFIG_QUEUE_WAKEUP,
	GHOST_SET_OPTION,
	GHOST_GET_CPU_TIME,
	GHOST_COMMIT_TXN,
	GHOST_SYNC_GROUP_TXN,
	GHOST_TIMERFD_SETTIME,
	GHOST_GTID_LOOKUP,
	GHOST_GET_GTID_10,	/* TODO: deprecate */
	GHOST_GET_GTID_11,	/* TODO: deprecate */
	GHOST_SET_DEFAULT_QUEUE,
};

/*
 * 'ops' supported by gsys_ghost() that are used by the 'base' library in
 * userspace. Arbitrary applications may use the 'base' library requiring
 * these 'ops' to be immutable (both in terms of the entry-point and the
 * functionality).
 *
 * The 'base' library can detect support for a particular op as follows:
 * - return value 0 indicates that the 'op' was successful.
 * - return value of -1 indicates that 'op' was not successful:
 *   - ENOSYS: kernel does not support ghost.
 *   - EOPNOTSUPP: kernel supports ghost but doesn't recognize this op.
 *     (for e.g. newer binary running on an older kernel).
 *   - any other errno indicates an op-specific error.
 */
enum ghost_base_ops {
	_GHOST_BASE_OP_FIRST = 65536,	/* avoid overlap with 'ghost_ops' */
	GHOST_BASE_GET_GTID = _GHOST_BASE_OP_FIRST,

	/*
	 * New ops must be added to the end of this enumeration.
	 */
};

/* status flags for GHOST_ASSOCIATE_QUEUE */
#define GHOST_ASSOC_SF_ALREADY		(1 << 0) /* Queue already set */
#define GHOST_ASSOC_SF_BRAND_NEW	(1 << 1) /* TASK_NEW not sent yet */

/* flags accepted by ghost_run() */
#define RTLA_ON_PREEMPT	  (1 << 0)  /* Return To Local Agent on preemption */
#define RTLA_ON_BLOCKED	  (1 << 1)  /* Return To Local Agent on block */
#define RTLA_ON_YIELD	  (1 << 2)  /* Return To Local Agent on yield */
#define RTLA_ON_IDLE	  (1 << 5)  /* Return To Local Agent on idle */
#define NEED_L1D_FLUSH	  (1 << 6)  /* Flush L1 dcache before entering guest */
#define NEED_CPU_NOT_IDLE (1 << 7)  /* Notify agent when a non-idle task is
				     * scheduled on the cpu.
				     */
#define ALLOW_TASK_ONCPU  (1 << 8)  /* If task is already running on remote
				     * cpu then let it keep running there.
				     */
#define ELIDE_PREEMPT     (1 << 9)  /* Do not send TASK_PREEMPT if we preempt
				     * a previous ghost task on this cpu */

/* txn->commit_flags */
enum txn_commit_at {
	/*
	 * commit_flags = 0 indicates a greedy commit (i.e. agent doesn't
	 * care where the commit happens). The kernel tries to apply the
	 * commit at the earliest opportunity (e.g. return-to-user).
	 */
	COMMIT_AT_SCHEDULE = 1,	    /* commit when oncpu task schedules */
	COMMIT_AT_TXN_COMMIT,	    /* commit in GHOST_COMMIT_TXN op */
};

/* special 'gtid' encodings that can be passed to ghost_run() */
#define GHOST_NULL_GTID		(0)
#define GHOST_AGENT_GTID	(-1)
#define GHOST_IDLE_GTID		(-2)

/* ghost transaction */

/*
 * (txn->state == GHOST_TXN_READY)		    transaction is ready
 * (txn->state >= 0 && txn->state < nr_cpu_ids)	    transaction is claimed
 * (txn->state < 0)				    transaction is committed
 */
enum ghost_txn_state {
	GHOST_TXN_COMPLETE		= INT_MIN,
	GHOST_TXN_ABORTED,
	GHOST_TXN_TARGET_ONCPU,
	GHOST_TXN_TARGET_STALE,
	GHOST_TXN_TARGET_NOT_FOUND,
	GHOST_TXN_TARGET_NOT_RUNNABLE,
	GHOST_TXN_AGENT_STALE,
	GHOST_TXN_CPU_OFFLINE,
	GHOST_TXN_CPU_UNAVAIL,
	GHOST_TXN_INVALID_FLAGS,
	GHOST_TXN_INVALID_TARGET,
	GHOST_TXN_NOT_PERMITTED,
	GHOST_TXN_INVALID_CPU,
	GHOST_TXN_NO_AGENT,
	GHOST_TXN_UNSUPPORTED_VERSION,
	GHOST_TXN_POISONED,

	/*
	 * Values [0-nr_cpu_ids) indicates that the transaction is claimed
	 * by the specific CPU.
	 */

	GHOST_TXN_READY			= INT_MAX,
};

/*
 * _ghost_txn_state_t is not expected to be used anywhere except as the type
 * of ghost_txn::state below. See cl/350179823 as an example of unintentional
 * consequences.
 */
#ifdef __cplusplus
#include <atomic>
typedef std::atomic<ghost_txn_state> _ghost_txn_state_t;
/*
 * To be safe, check that the size of '_ghost_txn_state_t' is equal to the size
 * of 'int32_t', which is the size that the kernel assumes the state is.
 */
static_assert(sizeof(_ghost_txn_state_t) == sizeof(int32_t));
typedef std::atomic<int32_t> _ghost_txn_owner_t;
#else
typedef int32_t _ghost_txn_state_t;
typedef int32_t _ghost_txn_owner_t;
#endif

struct ghost_txn {
	int32_t version;
	int32_t cpu;		/* readonly-after-init */
	_ghost_txn_state_t state;
	uint32_t agent_barrier;
	uint32_t task_barrier;
	uint16_t run_flags;
	uint8_t commit_flags;
	uint8_t unused;
	int64_t gtid;
	int64_t commit_time;	/* the time that the txn commit succeeded/failed */
	/*
	 * Context-dependent fields.
	 */
	union {
		/* only used during a sync-group commit */
		_ghost_txn_owner_t sync_group_owner;
	} u;
} __ghost_cacheline_aligned;

struct ghost_cpu_data {
	struct ghost_txn	txn;
} __ghost_page_aligned;

#define GHOST_TXN_VERSION	0

/* GHOST_TIMERFD_SETTIME */
struct timerfd_ghost {
	int cpu;
	int flags;
	uint64_t cookie;
};
#define TIMERFD_GHOST_ENABLED	(1 << 0)

/* GHOST_GTID_LOOKUP */
enum {
	GHOST_GTID_LOOKUP_TGID,		/* return group_leader pid */
};

/*
 * ghost tids referring to normal tasks always have a positive value:
 * (0 | 22 bits of actual pid_t | 41 bit non-zero seqnum)
 *
 * The embedded 'pid' following linux terminology is actually referring
 * to the thread id (i.e. what would be returned by syscall(__NR_gettid)).
 */
#define GHOST_TID_SEQNUM_BITS	41
#define GHOST_TID_PID_BITS	22

// clang-format on
// NOLINTEND

#endif	/* _SCHED_GHOST_H_ */
