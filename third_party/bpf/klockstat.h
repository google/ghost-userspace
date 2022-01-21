/* Copyright 2021 Google LLC
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

#ifndef GHOST_LIB_BPF_BPF_KLOCKSTAT_H_
#define GHOST_LIB_BPF_BPF_KLOCKSTAT_H_

#ifndef __BPF__
#include <stdint.h>
#endif

#define MAX_ENTRIES 102400
#define TASK_COMM_LEN 16
#define PERF_MAX_STACK_DEPTH 127

struct lock_stat {
	uint64_t acq_count;
	uint64_t acq_total_time;
	uint64_t acq_max_time;
	uint64_t acq_max_id;
	char acq_max_comm[TASK_COMM_LEN];
	uint64_t hld_count;
	uint64_t hld_total_time;
	uint64_t hld_max_time;
	uint64_t hld_max_id;
	char hld_max_comm[TASK_COMM_LEN];
};

#endif  // GHOST_LIB_BPF_BPF_KLOCKSTAT_H_
