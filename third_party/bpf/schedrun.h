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

#ifndef GHOST_LIB_BPF_BPF_SCHEDRUN_H_
#define GHOST_LIB_BPF_BPF_SCHEDRUN_H_

#include <stdint.h>

#define MAX_PIDS 102400
#define MAX_NR_HIST_SLOTS 25

/*
 * Power of 2 histogram, <=1 us, 2us, 4us, etc.  This struct must be at least
 * 8-byte aligned, since it is a value for a BPF map.
 */
struct hist {
	uint32_t slots[MAX_NR_HIST_SLOTS];
} __attribute__((aligned(64)));

enum {
	RUNTIMES_PREEMPTED_YIELDED,
	RUNTIMES_BLOCKED,
	RUNTIMES_ALL,
	NR_HISTS,
};

#endif  // GHOST_LIB_BPF_BPF_SCHEDRUN_H_
