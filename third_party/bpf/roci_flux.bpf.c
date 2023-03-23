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
	int nr_needed;
	int nr_preempted = 0;
	struct flux_cpu *cpus = get_cpus();
	struct flux_cpu *victim;

	/* Idle should never make cpu requests. */
	if (child_id == __roci_idle_sched_id()) {
		*ret = -1;
		return;
	}
	if (!c) {
		*ret = -1;
		return;
	}
	/*
	 * Recall that child->nr_cpus_wanted is already set.  In that sense,
	 * this function is a poke to see if we should do anything.
	 *
	 * It's ok if we race with changes to nr_cpus_wanted or nr_cpus.
	 * request_for_cpus() is best effort.
	 */
	nr_needed = READ_ONCE(c->f.nr_cpus_wanted) - READ_ONCE(c->f.nr_cpus);
	if (nr_needed <= 0) {
		*ret = 0;
		return;
	}
	if (!cpus) {
		*ret = -1;
		return;
	}
	for (int i = 0; i < nr_needed && i < ROCI_MAX_NR_PREEMPTS; i++) {
		bpf_spin_lock(&r->lock);
		victim = arr_list_pop_first(cpus, FLUX_MAX_CPUS,
					    &r->roci.idle_cpus, roci.link);
		/*
		 * preempt_pending is an earmark/signal that we already removed
		 * the cpu from the idle list.  That matters in case of a
		 * concurrent yield/preempt.
		 */
		if (victim)
			victim->roci.preempt_pending = true;
		bpf_spin_unlock(&r->lock);
		if (!victim)
			break;
		flux_preempt_cpu(r, victim);
		nr_preempted++;
	}
	*ret = nr_preempted;
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

	if (child_id == __roci_primary_sched_id())
		child_list = &r->roci.primary_cpus;
	else
		child_list = &r->roci.idle_cpus;

	if (!cpus)
		return;

	bpf_spin_lock(&r->lock);
	if (cpu->roci.preempt_pending) {
		/*
		 * We attempted to preempt this cpu, but our child yielded
		 * before the preemption hit and got the roci lock first.
		 */
		cpu->roci.preempt_pending = false;

	} else {
		arr_list_remove(cpus, FLUX_MAX_CPUS, child_list, cpu, roci.link);
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
	bool wasnt_pending;

	bpf_spin_lock(&r->lock);
	wasnt_pending = !cpu->roci.preempt_pending;
	cpu->roci.preempt_pending = false;
	bpf_spin_unlock(&r->lock);

	if (wasnt_pending)
		bpf_printd("preemption_complete without preempt_pending!");
}

static void roci_cpu_ticked(struct flux_sched *r, int child_id,
			    struct flux_cpu *cpu)
{
}

static void roci_pick_next_task(struct flux_sched *r, struct flux_cpu *cpu,
				struct bpf_ghost_sched *ctx)
{
	struct flux_sched *b = get_sched(__roci_primary_sched_id());
	int child_id;
	struct arr_list *child_list;
	struct flux_cpu *cpus = get_cpus();

	if (!b)
		return;
	if (READ_ONCE(b->f.nr_cpus_wanted) > READ_ONCE(b->f.nr_cpus)) {
		child_id = b->f.id;
		child_list = &r->roci.primary_cpus;
	} else {
		child_id = __roci_idle_sched_id();
		child_list = &r->roci.idle_cpus;
	}

	if (!cpus)
		return;
	bpf_spin_lock(&r->lock);
	arr_list_insert_head(cpus, FLUX_MAX_CPUS, child_list, cpu, roci.link);
	bpf_spin_unlock(&r->lock);

	flux_cpu_grant(r, child_id, cpu);
}
