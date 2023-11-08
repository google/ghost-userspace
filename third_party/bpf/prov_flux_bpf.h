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

#ifndef GHOST_LIB_BPF_BPF_PROV_FLUX_BPF_H_
#define GHOST_LIB_BPF_BPF_PROV_FLUX_BPF_H_

#ifndef __BPF__
#include <stdint.h>
#endif

#include "lib/queue.bpf.h"

/*
 * Prov: a provisioning scheduler.
 *
 * We have three children: prio, next, and last.
 *
 * The policy is to give prio up to max_nr_prio_cpus, preferring to pick cpus
 * flagged "priority".  Else take from last, then next.  Next takes from last.
 *
 * max_nr_prio_cpus and the per-cpu priority fields are configured by userspace.
 * You can do things like set max_nr_prio_cpus = 10, and pick your ten favorite
 * cpus (e.g. sharing a LLC).  Prio will get those 10, assuming they are
 * available to us at all (kernel CFS or our parent could have them instead).
 * If those chosen 10 aren't available, we'll find non-priority cpus to give you
 * instead.
 *
 * If prio has a cpu that isn't priority and there are available priority cpus,
 * we'll preempt prio (on timer tick) and move it to its desired cpu.  It's a
 * tradeoff - if you don't do that, prio will get scattered around the machine.
 * Note that if no cpus are marked priority, prio will just get any
 * max_nr_prio_cpus.
 *
 * max_nr_prio_cpus could be changed at runtime, or we can make it a function of
 * our cpus (future work).  Don't change cpu->priority at runtime without adding
 * some other state tracking bools.
 *
 * In the original version of Prov, priority was an int and these were stored in
 * a tree.  However, the AVL code is expensive in terms of instructions, and it
 * was really easy to blow out of our 1 million instruction budget...
 */

struct prov_poke_tracker {
	uint64_t threshold;	/* how many usec between pokes */
	uint64_t poked_at;	/* last time we poked, in usec */
};

struct prov_flux_sched {
	unsigned int prio_id;
	unsigned int next_id;
	unsigned int last_id;

	unsigned int max_nr_prio_cpus;

	/*
	 * The first place prio looks for a victim.  These cpus are next's and
	 * last's cpus.
	 *
	 * There is a window of time when a priority cpu is granted to us
	 * (prov), but not granted to any child scheduler yet.  It won't be on
	 * this list.  If prio has an outstanding nr_cpus_wanted, when we get to
	 * PNT, we'll hand out this cpu.  (Recall that cpu_grant happens in
	 * PNT).  It's possible that there is a concurrent request on another
	 * cpu that won't see this newly-granted cpu, and we may give out a
	 * non-priority cpu to prio when this cpu would have been better.  I'm
	 * fine with that.
	 */
	struct arr_list priority_cpus;

	struct arr_list nexts_cpus;
	struct arr_list lasts_cpus;

	struct prov_poke_tracker prio_poke;
	struct prov_poke_tracker next_poke;

	/* debug stats, disabled at load time if prov_debug_stats is false */
	uint64_t prio_grants;
	uint64_t next_grants;
	uint64_t last_grants;

	uint64_t prio_self_preempts;
	uint64_t next_self_preempts;
	uint64_t last_self_preempts;

	uint64_t prio_ipi_preempts;
	uint64_t next_ipi_preempts;
	uint64_t last_ipi_preempts;
};

struct prov_flux_cpu {
	struct arr_list_entry prio_link;
	struct arr_list_entry child_link;
	bool priority;
	/*
	 * preempt_pending is an earmark/signal that we already removed the cpu
	 * from the appropriate child list(s).
	 */
	bool preempt_pending;
	unsigned int owning_child;
};

#endif // GHOST_LIB_BPF_BPF_PROV_FLUX_BPF_H_
