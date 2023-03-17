// Copyright 2022 Google LLC
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// version 2 as published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

#ifndef GHOST_LIB_BPF_TOPOLOGY_BPF_H_
#define GHOST_LIB_BPF_TOPOLOGY_BPF_H_

#ifdef __BPF__

/*
 * Topology helpers.
 *
 * Userspace sets these values before loading the BPF program.  They are
 * read-only once the progam is loaded and appear in the bpf object's rodata
 * map.  The verifier may use their values to eliminate dead code, such as in
 * numa_node_of().
 *
 * See below for the userspace helpers.
 */
const volatile __u32 nr_cpus;
const volatile __u32 nr_siblings_per_core;
const volatile __u32 nr_ccx;
const volatile __u32 nr_numa;
const volatile __u32 highest_node_idx;

/*
 * These can be derived from nr_{cpus,siblings,numa}, but userspace precomputes
 * them so we can avoid divisions.
 */
const volatile __u32 nr_cores;
const volatile __u32 nr_cores_per_numa;

static inline __u32 sibling_of(__u32 cpu)
{
	/* TODO doesn't handle nr_siblings_per_core > 2 . */
	if (nr_siblings_per_core == 1)
		return cpu;
	if (cpu >= nr_cores)
		return cpu - nr_cores;
	else
		return cpu + nr_cores;
}

static inline __u32 core_of(__u32 cpu)
{
	/* Optimizing to avoid division for the common case. */
	if (nr_siblings_per_core == 2) {
		if (cpu >= nr_cores)
			return cpu - nr_cores;
		else
			return cpu;
	} else {
		return cpu % nr_cores;
	}
}

/* TODO: compute this */
static inline __u32 ccx_of(__u32 cpu)
{
	return 0;
}

static inline __u32 numa_node_of(__u32 cpu)
{
	__u32 core_id = core_of(cpu);

	/* Optimizing to avoid division for the common case. */
	if (nr_numa <= 2)
		return core_id >= nr_cores_per_numa ? 1 : 0;
	else
		return core_id / nr_cores_per_numa;
}

#else  // !__BPF__

/*
 * The 'const volatile' variables appear in the rodata map from your BPF
 * skeleton object.  You can set them after opening the bpf_obj, but before
 * loading.
 *
 * Pass this macro your bpf_obj->rodata (which is a pointer).
 */
#define __set_bpf_topology_vars(rodata, __nr_cpus, __smt_per_core, __nr_ccx,   \
			      __nr_numa, __highest_node_idx) ({                            \
	(rodata)->nr_cpus = (__nr_cpus);                                       \
	(rodata)->nr_siblings_per_core = (__smt_per_core);                     \
	(rodata)->nr_ccx = (__nr_ccx);                                         \
	(rodata)->nr_numa = (__nr_numa);                                       \
	(rodata)->highest_node_idx = (__highest_node_idx);                     \
	(rodata)->nr_cores = (rodata)->nr_cpus /(rodata)->nr_siblings_per_core;\
	(rodata)->nr_cores_per_numa = (rodata)->nr_cores / (rodata)->nr_numa;  \
})

/* Convenience wrapper for a Ghost Topology */
#define SetBpfTopologyVars(rodata, topo)                                       \
	__set_bpf_topology_vars(rodata,                                        \
				(topo)->num_cpus(),                            \
				(topo)->smt_count(),                           \
				(topo)->num_ccxs(),                            \
				(topo)->num_numa_nodes(),                      \
				(topo)->highest_node_idx());

#endif  // __BPF__

#endif  // GHOST_LIB_BPF_TOPOLOGY_BPF_H_
