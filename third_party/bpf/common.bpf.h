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

#ifndef GHOST_LIB_BPF_COMMON_BPF_H_
#define GHOST_LIB_BPF_COMMON_BPF_H_

#include "libbpf/bpf_core_read.h"

// TODO: Remove the NULL macro definition below once the open source
// ghOSt kernel has it in libbpf/bpf_helpers.h (5.13 and newer, see
// https://github.com/torvalds/linux/commit/9ae2c26e43248b722e79fe867be38062c9dd1e5f).
#ifndef NULL
#define NULL ((void *)0)
#endif

/*
 * Declarations for ghost's bpf_helpers.  These functions would normally be
 * available in linux_tools/.../bpf_helpers.h, however that file was generated
 * from the uapi/linux/bpf.h from a non-ghost kernel.
 *
 * The function ID numbers come from the ghost kernel's bpf.h header's.  The
 * format is the same as what bpf_doc.py would auto-generate.
 */
#ifndef GHOST_BPF
static long (*bpf_ghost_wake_agent)(__u32 cpu) = (void *) 3000;
static long (*bpf_ghost_run_gtid)(__s64 gtid, __u32 task_barrier, __s32 run_flags) = (void *) 3001;
/* 3002 is bpf_ghost_resched_cpu, which is deprecated with ABI 79 */
static long (*bpf_ghost_resched_cpu2)(__u32 cpu, __u32 flags) = (void *) 3003;
#endif

#define MAX_PIDS 102400
#define SCHED_GHOST 18
#define TASK_RUNNING 0
#define TASK_DEAD 0x0080
#define MAX_RT_PRIO 100
#define SEND_TASK_ON_CPU (1 << 10)

/* flags accepted by bpf_ghost_resched_cpu2 */
#define RESCHED_ANY_CLASS      (1 << 0)
#define RESCHED_GHOST_CLASS    (1 << 1)
#define RESCHED_IDLE_CLASS     (1 << 2)
#define RESCHED_OTHER_CLASS    (1 << 3)
#define SET_MUST_RESCHED       (1 << 4)
#define WAKE_AGENT             (1 << 5)
#define GHOST_RESCHED_CPU_MAX  (1 << 6)

static inline u64 min(u64 x, u64 y)
{
  if (x < y)
    return x;
  return y;
}

static inline u64 max(u64 x, u64 y)
{
  if (x > y)
    return x;
  return y;
}

static inline u64 bpf_ktime_get_us() {
  return bpf_ktime_get_ns() / 1000;
}

static inline bool task_has_ghost_policy(struct task_struct *p)
{
  return BPF_CORE_READ(p, policy) == SCHED_GHOST;
}

static inline bool is_agent(struct task_struct *p)
{
  struct sched_ghost_entity *p_ghost;

  /* Gotta love bitfields... */
  p_ghost = (void*)p + __CORE_RELO(p, ghost, BYTE_OFFSET);

  return BPF_CORE_READ_BITFIELD(p_ghost, agent) == 1;
}

static inline bool is_traced_ghost(struct task_struct *p) {
  return task_has_ghost_policy(p) && !is_agent(p);
}

#define READ_ONCE(x) (*(volatile typeof(x) *)&(x))
#define WRITE_ONCE(x, val) ((*(volatile typeof(x) *)&(x)) = val)

/*
 * TODO: This works for x86, but probably not for arm, which has
 * store-release instructions.
 *
 * We'd rather avoid the overhead of another atomic, and none of the __sync or
 * __atomic builtins work with clang -target bpf.
 */
#define smp_store_release(p, v) ({                                      \
	asm volatile ("" ::: "memory");                                 \
	WRITE_ONCE(*(p), v);                                            \
})

/*
 * Since this is in rodata, the verifier will drop all the bpf_printks, since
 * they are dead code.  That allows us to pass verification if we lack the CAP
 * to make a bpf_printk call.
 */
const volatile bool enable_bpf_printd;
#define bpf_printd(...) ({						\
	if (enable_bpf_printd)						\
		bpf_printk(__VA_ARGS__);				\
})

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
/*
 * No inline to avoid the dreaded:
 * "dereference of modified ctx ptr R6 off=3 disallowed"
 */
static void __attribute__((noinline)) set_dont_idle(struct bpf_ghost_sched *ctx)
{
	ctx->dont_idle = true;
}

static void __attribute__((noinline)) clr_dont_idle(struct bpf_ghost_sched *ctx)
{
	ctx->dont_idle = false;
}

static void __attribute__((noinline))
set_must_resched(struct bpf_ghost_sched *ctx)
{
	ctx->must_resched = true;
}

static void __attribute__((noinline))
clr_must_resched(struct bpf_ghost_sched *ctx)
{
	ctx->must_resched = false;
}
#pragma GCC diagnostic pop

/* Helper to prevent the compiler from optimizing bounds check on x. */
#define BPF_MUST_CHECK(x) ({ asm volatile ("" : "+r"(x)); x; })

#endif  // GHOST_LIB_BPF_COMMON_BPF_H_
