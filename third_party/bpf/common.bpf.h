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

#include <sys/types.h>
#include <stdbool.h>

#include "libbpf/bpf_core_read.h"

typedef __u64 u64;
typedef __s64 s64;
typedef __u32 u32;
typedef __s32 s32;

// TODO: Remove the NULL macro definition below once the open source
// ghOSt kernel has it in libbpf/bpf_helpers.h (5.13 and newer, see
// https://github.com/torvalds/linux/commit/9ae2c26e43248b722e79fe867be38062c9dd1e5f).
#ifndef NULL
#define NULL ((void *)0)
#endif

#define MAX_PIDS 102400
#define SCHED_GHOST 18
#define TASK_RUNNING 0
#define TASK_DEAD 0x0080
#define MAX_RT_PRIO 100

#ifndef BPF_NO_PRESERVE_ACCESS_INDEX
#pragma clang attribute push (__attribute__((preserve_access_index)), apply_to = record)
#endif

struct sched_ghost_entity {
  unsigned int agent      : 1;
};

struct task_struct {
  unsigned int flags;
  unsigned int cpu;
  unsigned int policy;
  struct sched_ghost_entity ghost;
  pid_t pid;
  pid_t tgid;
  volatile long state;
  int static_prio;
};

#ifndef BPF_NO_PRESERVE_ACCESS_INDEX
#pragma clang attribute pop
#endif

static inline __u64 min(__u64 x, __u64 y)
{
  if (x < y)
    return x;
  return y;
}

static inline __u64 max(__u64 x, __u64 y)
{
  if (x > y)
    return x;
  return y;
}

static inline __u64 bpf_ktime_get_us() {
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

#ifdef GHOST_VERSION
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

/*
 * Deprecated helpers can be annotated with a comment indicating the version
 * they got deprecated (agents and/or bpf programs can still be compiled with
 * older ABIs). Alternatively we can limit visibility to a specific ABI range.
 */
#if GHOST_VERSION >= 65 && GHOST_VERSION < 79
/* 3002 is bpf_ghost_resched_cpu, which is deprecated with ABI 79 */
static long (*bpf_ghost_resched_cpu)(__u32 cpu, __u64 cpu_seqnum) = (void *) 3002;
#endif

/* New helpers should be guarded with the ABI in which they were introduced */
#if GHOST_VERSION >= 79
static long (*bpf_ghost_resched_cpu2)(__u32 cpu, __u32 flags) = (void *) 3003;
#endif

#if GHOST_VERSION >= 87
static long (*bpf_ghost_sync_commit)(struct bpf_dynptr *args) =
    (void *)3004;
#endif

#if GHOST_VERSION >= 91
static long (*bpf_ghost_get_affinity)(__s64 gtid, __u8 *mask, __u32 size) =
    (void *)3005;
#endif

#if GHOST_VERSION >= 91
static long (*bpf_ghost_get_comm)(__s64 gtid, char *buf, __u32 size) =
    (void *)3006;
#endif

#endif  // !GHOST_BPF */

#if GHOST_VERSION >= 84
enum {
  GHOST_PREPARE_HALT_POLL,
  GHOST_CONTINUE_HALT_POLL,
  GHOST_END_HALT_POLL,
};
#endif

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

const volatile int ghost_gtid_seqnum_bits;

static inline int gtid_to_pid(uint64_t gtid)
{
	return gtid >> ghost_gtid_seqnum_bits;
}

#pragma GCC diagnostic pop
#endif  // GHOST_VERSION

/* Helper to prevent the compiler from optimizing bounds check on x. */
#define BPF_MUST_CHECK(x) ({ asm volatile ("" : "+r"(x)); x; })

/*
 * Index to an array element within a bound.
 *
 * No matter what you do to idx, the compiler is free to use two different
 * registers for it, and use one to do the bounds check and another to do the
 * array index.
 *
 * For example, when the idx was a u32 in Flux's cpuid_to_cpu(), I tried
 *
 * 	cpu_id = BPF_MUST_CHECK(cpu_id)
 *
 * where the compiler now has no idea the value of cpu_id.  That defeats some
 * optimizations (e.g. the compiler removes the check because it knows the value
 * of cpu_id), but you'll still get:
 *
 *	r2 = r8
 *	r2 <<= 32
 *	r2 >>= 32
 *	if r2 > 0x3ff goto pc+29
 *	r8 <<= 32
 *	r8 >>= 32
 *	r8 <<= 6
 *	r0 += r8
 *	*(u64 *)(r0 +48) = r3
 *
 * And that dereference of r0 wasn't bounds checked.  r2 was bounds checked, not
 * r8, and the verifier doesn't know that r2 = r8.  (The shift by 6 was due to
 * the size of the cpu struct at the time).  Perhaps the compiler wanted to keep
 * r2 around.  Who knows.
 *
 * The shifting by 32 was because cpu_id was a u32, but depending on the BPF
 * instruction set, we didn't have a conditional 32 bit jump.
 *
 * Eventually, we just have to bite the inline asm bullet to force the use of
 * the same register for the bounds check and the array access.  This macro asm
 * is the equivalent of:
 *
 *	if (!arr)
 *		return NULL;
 *	if (idx >= arr_sz)
 *		return NULL;
 *	return &arr[idx];
 *
 * The idx (___idx below) needs to be a u64, at least for certain versions
 * of the BPF ISA, since there aren't u32 conditional jumps.
 */
#define BOUNDED_ARRAY_IDX(arr, arr_sz, idx) ({				\
	typeof(&(arr)[0]) ___arr = arr;					\
	u64 ___idx = idx;						\
	if (___arr) {							\
		asm volatile("if %[__idx] >= %[__bound] goto 1f;	\
			      %[__idx] *= %[__size];		\
			      %[__arr] += %[__idx];		\
			      goto 2f;				\
			      1:;				\
			      %[__arr] = 0;			\
			      2:				\
			      "						\
			     : [__arr]"+r"(___arr), [__idx]"+r"(___idx)	\
			     : [__bound]"i"((arr_sz)),		        \
			       [__size]"i"(sizeof(typeof((arr)[0])))	\
			     : "cc");					\
	}								\
	___arr;								\
})

/*
 * Forces the verifier to ensure idx is less than bound.  Returns 0 if idx is
 * not less than bound.  The compiler won't know the value of idx after this, so
 * it can't copy idx to another register *before* this bounds check and then use
 * it in place of idx.
 *
 * Interestingly enough, both "less than" and "greater-equal" versions of this
 * worked.  I was under the impression BPF_JLT wasn't in BPF ISA v1, but
 * apparently it works.  (BPF ISA v2 is supported as of 4.14 and LLVM 6.0).
 */
static inline size_t bounded_idx(size_t idx, int bound)
{
	asm volatile("if %[__idx] < %[__bound] goto 1f;	\
		      %[__idx] = 0;			\
		      1:"
		      : [__idx]"+r"(idx) : [__bound]"i"(bound) : "cc");
	return idx;
}

#endif  // GHOST_LIB_BPF_COMMON_BPF_H_
