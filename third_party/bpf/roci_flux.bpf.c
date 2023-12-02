/*
 * Copyright 2023 Google LLC
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 or later as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

/* ROCI scheduler implementation for Flux. */

#define roci_cpu(s, cpu) (cpu)->__sched_cpu_union(s).roci

#define roci_arr_list_pop_first(head, field)				\
	arr_list_pop_first(cpus, FLUX_MAX_CPUS, head,			\
			   __sched_cpu_union(r).field)

#define roci_arr_list_remove(head, elem, field)				\
	arr_list_remove(cpus, FLUX_MAX_CPUS, head, elem,		\
			__sched_cpu_union(r).field)

#define roci_arr_list_insert_head(head, elem, field)			\
	arr_list_insert_head(cpus, FLUX_MAX_CPUS, head, elem,		\
			     __sched_cpu_union(r).field)

#define roci_arr_list_insert_tail(head, elem, field)			\
	arr_list_insert_tail(cpus, FLUX_MAX_CPUS, head, elem,		\
			     __sched_cpu_union(r).field)

/*
 * We need a limit for the BPF verifier.
 *
 * Recall that the return value for request_for_cpus() is a hint to the child.
 * There's never a guarantee that a parent will respond immediately to a
 * request_for_cpus, and even if it did, the victim cpus could also be preempted
 * before being allocated.  So if a caller depends on getting its exact request,
 * it needs to handle that on its own.  Check the FAQ for more info.
 */
#define ROCI_MAX_NR_PREEMPTS 5

/*
 * The policy is to give primary whatever it wants and take from idle.
 */
static void roci_request_for_cpus(struct flux_sched *r, int child_id,
				  int nr_cpus, int *ret)
{
	struct flux_sched *c = get_sched(child_id);
	struct flux_cpu *cpus = get_cpus();
	struct flux_cpu *victim;
	uint64_t nr_needed;

	*ret = 0;

	if (child_id == r->roci.idle_id)
		return;
	if (!c)
		return;
	if (!cpus)
		return;
	/*
	 * Recall that child->nr_cpus_wanted is already set.  In that sense,
	 * this function is a poke to see if we should do anything.
	 */
	nr_needed = flux_sched_nr_cpus_needed(c);
	if (!nr_needed)
		return;
	for (int i = 0; i < ROCI_MAX_NR_PREEMPTS; i++) {
		if (i >= nr_needed)
			continue;

		bpf_spin_lock(&r->lock);
		victim = roci_arr_list_pop_first(&r->roci.idle_cpus, roci.link);
		/*
		 * preempt_pending is an earmark/signal that we already removed
		 * the cpu from the idle list.  That matters in case of a
		 * concurrent yield/preempt.
		 */
		if (victim)
			roci_cpu(r, victim).preempt_pending = true;
		bpf_spin_unlock(&r->lock);
		if (!victim)
			break;
		flux_preempt_cpu(r, victim);
		(*ret)++;
	}
}

static void roci_cpu_allocated(struct flux_sched *r, struct flux_cpu *cpu)
{
	/* Do nothing.  Give it to primary or idle in PNT. */
}

/* Do the accounting for a cpu returned via either yield or a preemption */
static void __roci_cpu_returned(struct flux_sched *r, int child_id,
				struct flux_cpu *cpu)
{
	struct arr_list *child_list;
	struct flux_cpu *cpus = get_cpus();

	if (child_id == r->roci.primary_id)
		child_list = &r->roci.primary_cpus;
	else
		child_list = &r->roci.idle_cpus;

	if (!cpus)
		return;

	bpf_spin_lock(&r->lock);
	if (roci_cpu(r, cpu).preempt_pending) {
		/*
		 * We attempted to preempt this cpu, but our child yielded
		 * before the preemption hit and got the roci lock first.
		 */
		roci_cpu(r, cpu).preempt_pending = false;

	} else {
		roci_arr_list_remove(child_list, cpu, roci.link);
	}

	bpf_spin_unlock(&r->lock);
}

static void roci_cpu_returned(struct flux_sched *r, int child_id,
			      struct flux_cpu *cpu)
{
	__roci_cpu_returned(r, child_id, cpu);

	/*
	 * That’s it.  in general, we’d like to cpu_grant() to some child or
	 * even schedule a thread here.  Do that in PNT.
	 */
}

static void roci_cpu_preempted(struct flux_sched *r, int child_id,
			       struct flux_cpu *cpu)
{
	int dont_care;

	if (child_id == FLUX_SCHED_NONE)
		return;
	/*
	 * Our cpu is being removed from us, i.e. a kernel level preemption
	 * since we are the highest sched in the hierarchy.  Our child’s preempt
	 * handler was already called.
	 *
	 * Note that we do the accounting of cpu_returned(), and take action
	 * like request_for_cpus().  cpu_returned and cpu_preempted(child) are
	 * similar: both involve removing the cpu from the child: voluntarily or
	 * involuntarily, respectively.
	 *
	 * If we wanted to get fancy, we could write a single method for both,
	 * so that we don't have to unlock and relock.
	 */
	__roci_cpu_returned(r, child_id, cpu);
	roci_request_for_cpus(r, child_id, 1, &dont_care);
}

static void roci_cpu_preemption_completed(struct flux_sched *r, int child_id,
					  struct flux_cpu *cpu)
{
	if (child_id == FLUX_SCHED_NONE)
		return;
	/*
	 * In older versions of flux, I thought that preempt_pending could not
	 * be clear here, since preemption_completed() is for preemptions we
	 * instigated, and we set preempt_pending before we call
	 * flux_preempt_cpu().
	 *
	 * However, this is susceptible to the "it happened twice" race
	 * scenario:
	 *
	 *
	 * CPU A:			CPU B:
	 * ------------------------------------------------------------
	 * request_for_cpus
	 * 	lock
	 * 	remove victim B from list
	 * 	set preempt_pending
	 * 	unlock
	 *
	 * 				cpu yield from child
	 * 					cpu_returned
	 * 					lock
	 * 					see preempt_pending, clear it
	 * 					already off list, nothing to do
	 * 					unlock
	 *
	 * 				grant cpu back to child
	 * 					lock
	 * 					add to list
	 * 					preempt_pending is still clear
	 * 					unlock
	 * 					grant to child
	 *
	 * 	flux_preempt_cpu
	 *
	 * 				cpu_preemption_completed()
	 * 					lock
	 * 					see preempt_pending is clear
	 *
	 *
	 * There are three ways the cpu returns to us from our child: yield,
	 * preemption, preemption_completed.  All three need to do the same
	 * accounting for a cpu returning, i.e. check for preemption_pending.
	 */
	__roci_cpu_returned(r, child_id, cpu);
}

static void roci_cpu_ticked(struct flux_sched *r, int child_id,
			    struct flux_cpu *cpu)
{
	if (child_id == FLUX_SCHED_NONE)
		return;
}

static void roci_pick_next_task(struct flux_sched *r, struct flux_cpu *cpu,
				struct bpf_ghost_sched *ctx)
{
	struct flux_sched *b = get_sched(r->roci.primary_id);
	int child_id;
	struct arr_list *child_list;
	struct flux_cpu *cpus = get_cpus();

	if (!b)
		return;
	if (!cpus)
		return;
	/*
	 * I used to do an unlocked peek at nr_cpus_needed here, then grab the
	 * lock, then insert, etc.
	 *
	 * It was possible to have a cpu go idle when primary (e.g. biff) wanted
	 * a cpu.
	 *
	 * Up in request_for_cpus, the pattern is
	 *
	 * 	set nr_cpus_wanted
	 * 	lock
	 * 	check for idle cpus
	 * 	unlock
	 *
	 * But we could have had this:
	 *
	 * CPU A:			CPU B:
	 * ----------------------------------------------------------
	 * 				PNT()
	 * 				  see primary wants no cpus
	 *
	 * request_for_cpus()
	 *   set nr_cpus_wanted
	 *   lock
	 *   check for idle cpus,
	 *     see none
	 *   unlock
	 *   				  lock
	 *   				  add cpu to idle list
	 *   				  unlock
	 *
	 * CPU B is idle, and the primary scheduler wants a cpu.  Until we get
	 * another request from primary, we won't look at the idle list.  If
	 * that was the last request we ever get, e.g. an enclave with a single
	 * thread that just woke up, and primary was asking for one cpu, we'll
	 * never look again.  (Recall that idle doesn't get cpu_ticks in the
	 * current version of ghost too).
	 */
	bpf_spin_lock(&r->lock);
	if (flux_sched_nr_cpus_needed(b)) {
		child_id = b->f.id;
		child_list = &r->roci.primary_cpus;
	} else {
		child_id = r->roci.idle_id;
		child_list = &r->roci.idle_cpus;
	}
	roci_arr_list_insert_head(child_list, cpu, roci.link);
	bpf_spin_unlock(&r->lock);

	flux_cpu_grant(r, child_id, cpu);
}
