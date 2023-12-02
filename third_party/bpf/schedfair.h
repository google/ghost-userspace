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

#ifndef GHOST_LIB_BPF_BPF_SCHEDFAIR_H_
#define GHOST_LIB_BPF_BPF_SCHEDFAIR_H_

#include <stdint.h>

#define MAX_PIDS 102400

struct task_info {
	/* state tracking */
	uint8_t load_tracked;
	int user_prio;

	/* intermediate variables */
	uint64_t share_at_wake;
	uint64_t ran_at;
	uint64_t cpu_runtime_since_wake;

	/* output for userspace */
	uint64_t total_cpu_runtime;
	uint64_t total_cpu_share;
};

#endif  // GHOST_LIB_BPF_BPF_SCHEDFAIR_H_
