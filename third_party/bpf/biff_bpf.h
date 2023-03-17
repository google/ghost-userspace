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

#ifndef GHOST_LIB_BPF_BPF_BIFF_BPF_H_
#define GHOST_LIB_BPF_BPF_BIFF_BPF_H_

#ifndef __BPF__
#include <stdint.h>
#endif

#define BIFF_MAX_CPUS	1024
#define BIFF_MAX_GTIDS 65536

/*
 * The array map of these, called `cpu_data`, can be mmapped by userspace.
 */
struct biff_bpf_cpu_data {
	uint64_t current;
	uint64_t cpu_seqnum;
	bool available;
} __attribute__((aligned(64)));

/*
 * bpf can quickly access hash maps, but userspace can't.  Ghost already gives
 * us the "status_word region" (SWR), which is an mmappable file in ghostfs that
 * exports read-only data from the kernel.  Every task has a status word ID
 * (identifier for SWR) the and index within the SWR.
 *
 * The sw_data is logically an extension of the status word.  It is read-write
 * by userspace and bpf.
 *
 * For each SW region (and there is 1, with BIFF_MAX_GTIDS slots), there is a
 * corresponding bpf array map, called `sw_data`,  with the same number of
 * "words", such that given a task's sw index, we can find its sw_data.  In bpf,
 * the index is stored in struct task_sw_info and is maintained by bpf-msg.
 *
 * Since userspace doesn't receive messages, it will have to scan the SWR to
 * discover tasks and their SW {id, index} pairs.  (You can start scanning from
 * the last-new spot, since the kernel allocates linearly, with wrapping.)
 *
 * This may seem like an extra level of indirection and pointer chasing, but bpf
 * autogenerates the array map access code, so even if we don't use sw_data from
 * userspace yet, it's not hard to have it ready.
 *
 * aligned(8) since this is a bpf map value.
 */
struct biff_bpf_sw_data {
	uint64_t ran_at;
	uint64_t ran_until;
	uint64_t runnable_at;
	uint64_t parent;
} __attribute__((aligned(8)));


#endif  // GHOST_LIB_BPF_BPF_BIFF_BPF_H_
