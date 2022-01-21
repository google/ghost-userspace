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

#ifndef GHOST_LIB_BPF_BPF_SCHEDLAT_H_
#define GHOST_LIB_BPF_BPF_SCHEDLAT_H_

#ifndef __BPF__
#include <stdint.h>
#endif

#define MAX_PIDS 102400
#define MAX_NR_HIST_SLOTS 25

struct task_stat {
	uint64_t runnable_at;
	uint64_t latched_at;
	uint64_t ran_at;
};

/*
 * Power of 2 histogram, <=1 us, 2us, 4us, etc.  This struct must be at least
 * 8-byte aligned, since it is a value for a BPF map.  The kernel will round up
 * the size of any map value to 8 bytes internally.  If we have an array of
 * these objects, the kernel will think each object is 8-byte aligned each.
 * When we read the per-cpu map in schedlat.c, we get an array of struct hist.
 * The compiler needs to agree with the kernel on the size of the objects, or
 * you'll corrupt your stats.
 */
struct hist {
	uint32_t slots[MAX_NR_HIST_SLOTS];
} __attribute__((aligned(8)));

enum {
	RUNNABLE_TO_LATCHED,
	LATCHED_TO_RUN,
	RUNNABLE_TO_RUN,
	NR_HISTS,
};

#endif  // GHOST_LIB_BPF_BPF_SCHEDLAT_H_
