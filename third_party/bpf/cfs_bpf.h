/*
* Copyright 2022 Google LLC
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

#ifndef GHOST_BPF_BPF_CFS_BPF_H_
#define GHOST_BPF_BPF_CFS_BPF_H_

#ifndef __BPF__
#include <stdint.h>
#endif

#include "lib/queue.bpf.h"

#define CFS_MAX_CPUS   1024
#define CFS_MAX_GTIDS 65536

/*
 * The array map of these, called `cpu_data`, can be mmapped by userspace.
 *
 * Need to track the cpu_seqnum to use bpf_ghost_resched_cpu().
 */
struct cfs_bpf_cpu_data {
	uint64_t current;
	uint64_t cpu_seqnum;
        bool available;
} __attribute__((aligned(64)));

/*
 * Per-cpu runqueue for CFS using Linked list.
 */
struct cfs_bpf_rq {
        uint64_t current; 
        uint64_t weight;
        uint64_t nr_running;
        uint64_t min_vruntime;
        struct arr_list rq_root;
#ifdef __BPF__
        struct bpf_spin_lock lock;
#else
        uint32_t lock;
#endif
}__attribute__((aligned(64)));

/*
 * Thread struct to store the values required for cfs tasks. Think of this as
 * the same as a task struct for cfs. It brings its own memory for the runqueue
 * (LL).
 * aligned(8) since this is a bpf map value.
 */
struct cfs_bpf_thread {
        uint64_t gtid; 
        uint64_t task_barrier; 
	uint64_t ran_at;
	uint64_t ran_until;
	uint64_t runnable_at;
        uint64_t weight;
        uint64_t real_time;
        uint64_t sum_exec_runtime;
        uint64_t prev_sum_exec_runtime;
        uint64_t vruntime;
        uint64_t on_rq; 
        struct arr_list_entry next_task;
} __attribute__((aligned(8)));





#endif  // GHOST_BPF_BPF_CFS_BPF_H_
