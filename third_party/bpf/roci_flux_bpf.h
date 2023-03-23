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

struct roci_flux_sched {
	struct arr_list primary_cpus;
	struct arr_list idle_cpus;
};

struct roci_flux_cpu {
	struct arr_list_entry link;
	bool preempt_pending;
};

#endif // GHOST_LIB_BPF_BPF_ROCI_FLUX_BPF_H_
