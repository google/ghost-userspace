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
 * The function ID numbers (e.g. 204) come from the ghost kernel's bpf.h
 * header's enum bpf_func_id (built from __BPF_FUNC_MAPPER).  The format is the
 * same as what bpf_doc.py would auto-generate.
 */
static long (*bpf_ghost_wake_agent)(__u32 cpu) = (void *) 204;
static long (*bpf_ghost_run_gtid)(__s64 gtid, __u32 task_barrier, __s32 run_flags) = (void *) 205;


#define MAX_PIDS 102400
#define SCHED_GHOST 18
#define TASK_RUNNING 0
#define TASK_DEAD 0x0080

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

#endif  // GHOST_LIB_BPF_COMMON_BPF_H_
