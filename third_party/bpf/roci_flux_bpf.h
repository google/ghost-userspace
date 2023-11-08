/*
 * Copyright 2023 Google LLC
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

#ifndef GHOST_LIB_BPF_BPF_ROCI_FLUX_BPF_H_
#define GHOST_LIB_BPF_BPF_ROCI_FLUX_BPF_H_

#ifndef __BPF__
#include <stdint.h>
#endif

#include "lib/queue.bpf.h"

/*
 * TODO: ROCI assumes it is the top of the hierarchy and that "idle_id" is
 * actually the idle scheduler.  There are a few assumptions baked in here:
 * - the idle_id (secondary child) always wants a cpu.  So we never yield in
 *   PNT.
 * - We never ask our parent for cpus, since we assume there is no parent to
 *   ask.
 * - We're extremely aggressive about taking cpus from idle.  This is fine if it
 *   is actually idle, but can get excessive.  Specifically, we look at
 *   nr_cpus_needed, not nr_cpus (which was the request from that call).  If you
 *   have two cpus making requests at the same time, ROCI might double-up and
 *   preempt 2x the cpus needed.
 */
struct roci_flux_sched {
	struct arr_list primary_cpus;
	struct arr_list idle_cpus;
	unsigned int primary_id;
	unsigned int idle_id;
};

struct roci_flux_cpu {
	struct arr_list_entry link;
	bool preempt_pending;
};

#endif // GHOST_LIB_BPF_BPF_ROCI_FLUX_BPF_H_
