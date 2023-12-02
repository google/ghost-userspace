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

/* Prov: a provisioning scheduler for Flux. */

#define PROV_MAX_NR_PREEMPTS 5

#define prov_cpu(s, cpu) (cpu)->__sched_cpu_union(s).prov

#define prov_arr_list_first(head)					\
	arr_list_first(cpus, FLUX_MAX_CPUS, head)

#define prov_arr_list_pop_first(head, field)				\
	arr_list_pop_first(cpus, FLUX_MAX_CPUS, head,			\
			   __sched_cpu_union(s).field)

#define prov_arr_list_remove(head, elem, field)				\
	arr_list_remove(cpus, FLUX_MAX_CPUS, head, elem,		\
			__sched_cpu_union(s).field)

#define prov_arr_list_insert_head(head, elem, field)			\
	arr_list_insert_head(cpus, FLUX_MAX_CPUS, head, elem,		\
			     __sched_cpu_union(s).field)

#define prov_arr_list_insert_tail(head, elem, field)			\
	arr_list_insert_tail(cpus, FLUX_MAX_CPUS, head, elem,		\
			     __sched_cpu_union(s).field)


const volatile bool prov_debug_stats;
#define acct_increment(x) ({ if (prov_debug_stats) { (x)++; } })

#define acct_grant_prio(s) acct_increment((s)->prov.prio_grants)
#define acct_grant_next(s) acct_increment((s)->prov.next_grants)
#define acct_grant_last(s) acct_increment((s)->prov.last_grants)

#define acct_self_preempt_prio(s) acct_increment((s)->prov.prio_self_preempts)
#define acct_self_preempt_next(s) acct_increment((s)->prov.next_self_preempts)
#define acct_self_preempt_last(s) acct_increment((s)->prov.last_self_preempts)

#define acct_ipi_preempt_prio(s) acct_increment((s)->prov.prio_ipi_preempts)
#define acct_ipi_preempt_next(s) acct_increment((s)->prov.next_ipi_preempts)
#define acct_ipi_preempt_last(s) acct_increment((s)->prov.last_ipi_preempts)


/*
 * next and last cpus are on up to two lists: both the child_list itself, and if
 * the cpu is priority, then the priority_cpus.  Remove cpu from both.  Requires
 * several variables in scope, e.g. 's' and 'cpus'.
 *
 * Note we're using the prov.priority field to tell us if a cpu is priority or
 * not, so don't change that at runtime.
 */
#define __remove_from_child_list(cpu, child_list) ({			\
	prov_arr_list_remove(child_list, cpu, prov.child_link);		\
	if (prov_cpu(s, cpu).priority) {				\
		prov_arr_list_remove(&s->prov.priority_cpus, cpu,	\
				     prov.prio_link);			\
	}								\
})

/* Helper, returns how many cpus prio needs that we're willing to give it. */
static uint64_t prio_nr_needed(struct flux_sched *s, struct flux_sched *prio)
{
	int64_t clamped_nr_needed;

	clamped_nr_needed = min(READ_ONCE(prio->f.nr_cpus_wanted),
				READ_ONCE(s->prov.max_nr_prio_cpus)) -
			    READ_ONCE(prio->f.nr_cpus);
	return clamped_nr_needed < 0 ? 0 : clamped_nr_needed;
}

/*
 * Other things to consider for poking:
 * - if the child currently has no cpus
 * - if the child's needs exceed a percentage of their current allocation.
 * - handle bursts of requests.  The current method assumes an average
 *   spamminess.
 *
 * Note that if we return false, our caller (request_for_cpus()) won't
 * necessarily call us again anytime soon.  We're ignoring a spammy child's
 * request, and won't look at them again until their next request, which might
 * be a while longer than the threshold.
 */
static bool prov_can_poke(struct flux_sched *child,
			  struct prov_poke_tracker *poke)
{
	uint64_t now, at;

	if (!poke->threshold)
		return true;
	now = bpf_ktime_get_us();
	/*
	 * Using the test, test-and-set pattern.  It's possible that another cpu
	 * has a later 'now' than us and wins the race, such that our 'now' is
	 * less than 'at', which is fine.
	 */
	if (now - READ_ONCE(poke->poked_at) < poke->threshold)
		return false;
	at = __atomic_exchange_n(&poke->poked_at, now, __ATOMIC_ACQUIRE);
	return now - at >= poke->threshold;
}

static void __prio_request_for_cpus(struct flux_sched *s,
				    struct flux_sched *prio, int nr_cpus,
				    int *ret)
{
	struct flux_cpu *cpus = get_cpus();
	uint64_t nr_needed;

	/*
	 * TODO: consider clamping to nr_cpus too, e.g.
	 * nr_needed = min(nr_needed, nr_cpus);
	 * and/or recalculating nr_needed each loop to avoid excess poking
	 */
	if (!prov_can_poke(prio, &s->prov.prio_poke))
		return;
	nr_needed = prio_nr_needed(s, prio);

	for (int i = 0; i < nr_needed && i < PROV_MAX_NR_PREEMPTS; i++) {
		struct flux_cpu *victim;
		struct arr_list *child_list;

		bpf_spin_lock(&s->lock);

		victim = prov_arr_list_first(&s->prov.priority_cpus) ?:
			 prov_arr_list_first(&s->prov.lasts_cpus) ?:
			 prov_arr_list_first(&s->prov.nexts_cpus);
		if (!victim) {
			bpf_spin_unlock(&s->lock);
			return;
		}
		if (prov_cpu(s, victim).owning_child == s->prov.next_id) {
			child_list = &s->prov.nexts_cpus;
			acct_ipi_preempt_next(s);
		} else {
			child_list = &s->prov.lasts_cpus;
			acct_ipi_preempt_last(s);
		}

		__remove_from_child_list(victim, child_list);
		prov_cpu(s, victim).preempt_pending = true;
		bpf_spin_unlock(&s->lock);

		flux_preempt_cpu(s, victim);
		(*ret)++;
	}
}

static void __next_request_for_cpus(struct flux_sched *s,
				    struct flux_sched *next, int nr_cpus,
				    int *ret)
{
	struct flux_cpu *cpus = get_cpus();
	uint64_t nr_needed;

	/*
	 * Next is taking cpus from last - whether we clamp them to nr_cpus or
	 * slow the pokes depends on if last is 'idle' or doing useful work.
	 */
	if (!prov_can_poke(next, &s->prov.next_poke))
		return;
	nr_needed = flux_sched_nr_cpus_needed(next);

	for (int i = 0; i < nr_needed && i < PROV_MAX_NR_PREEMPTS; i++) {
		struct flux_cpu *victim;

		bpf_spin_lock(&s->lock);

		victim = prov_arr_list_first(&s->prov.lasts_cpus);
		if (!victim) {
			bpf_spin_unlock(&s->lock);
			return;
		}
		acct_ipi_preempt_last(s);

		__remove_from_child_list(victim, &s->prov.lasts_cpus);
		prov_cpu(s, victim).preempt_pending = true;
		bpf_spin_unlock(&s->lock);

		flux_preempt_cpu(s, victim);
		(*ret)++;
	}
}

static void __last_request_for_cpus(struct flux_sched *s,
				    struct flux_sched *last, int nr_cpus,
				    int *ret)
{
}

static void prov_request_for_cpus(struct flux_sched *s, int child_id,
				  int nr_cpus, int *ret)
{
	struct flux_sched *child = get_sched(child_id);

	*ret = 0;

	/*
	 * TODO: for non-root scenarios, ask our parent before taking.  Esp from
	 * next, but potentially from last too (unless last is idle).
	 *
	 * Also, at some point we need to trickle the request up.  flux doesn't
	 * do that for us, so unless we're smart, we need to do something like
	 * "always pass up our child requests".
	 */
	if (nr_cpus < 0)
		return;
	if (!child)
		return;

	if (child->f.id == s->prov.prio_id)
		__prio_request_for_cpus(s, child, nr_cpus, ret);
	else if (child->f.id == s->prov.next_id)
		__next_request_for_cpus(s, child, nr_cpus, ret);
	else if (child->f.id == s->prov.last_id)
		__last_request_for_cpus(s, child, nr_cpus, ret);
}

static void prov_cpu_allocated(struct flux_sched *s, struct flux_cpu *cpu)
{
	/* Do nothing.  Allocate it in PNT. */
}

/* Do the accounting for a cpu returned via either yield or a preemption */
static void __prov_cpu_returned(struct flux_sched *s, int child_id,
				struct flux_cpu *cpu)
{
	struct arr_list *child_list = NULL;
	struct flux_cpu *cpus = get_cpus();

	if (!cpus)
		return;
	if (child_id == s->prov.next_id)
		child_list = &s->prov.nexts_cpus;
	else if (child_id == s->prov.last_id)
		child_list = &s->prov.lasts_cpus;

	bpf_spin_lock(&s->lock);
	prov_cpu(s, cpu).owning_child = 0;
	if (prov_cpu(s, cpu).preempt_pending) {
		/*
		 * We attempted to preempt this cpu, but our child yielded
		 * before the preemption hit and got the prov lock first.
		 */
		prov_cpu(s, cpu).preempt_pending = false;

	} else {
		if (child_list)
			__remove_from_child_list(cpu, child_list);
	}
	bpf_spin_unlock(&s->lock);
}

static void prov_cpu_returned(struct flux_sched *s, int child_id,
			      struct flux_cpu *cpu)
{
	__prov_cpu_returned(s, child_id, cpu);
}

/* Our cpu is being taken away from us */
static void prov_cpu_preempted(struct flux_sched *s, int child_id,
			       struct flux_cpu *cpu)
{
	int dont_care;

	if (child_id == FLUX_SCHED_NONE)
		return;
	__prov_cpu_returned(s, child_id, cpu);
	prov_request_for_cpus(s, child_id, 1, &dont_care);
}

/* We instigated a preemption back to us, and it is now complete. */
static void prov_cpu_preemption_completed(struct flux_sched *s, int child_id,
					  struct flux_cpu *cpu)
{
	if (child_id == FLUX_SCHED_NONE)
		return;
	__prov_cpu_returned(s, child_id, cpu);
}

/* Helper: should we give this cpu to prio or not */
static bool give_to_prio(struct flux_sched *s, struct flux_sched *prio,
			 struct flux_cpu *cpu)
{
	return prio_nr_needed(s, prio) &&
		(prov_cpu(s, cpu).priority ||
		 arr_list_empty(&s->prov.priority_cpus));
}

static void prov_cpu_ticked(struct flux_sched *s, int child_id,
			    struct flux_cpu *cpu)
{
	struct flux_sched *prio;

	if (child_id == FLUX_SCHED_NONE)
		return;
	/*
	 * A couple notes:
	 * - We're doing lockless peeks at the priority_cpus list.  It's fine if
	 *   we're wrong here.  If we should preempt, but don't, we'll get it
	 *   next time.  If we preempt when we didn't need to, we'll realloc to
	 *   prio later.
	 * - Never set preempt_pending here.  preempt_pending is the signal to
	 *   say "your child's list membership was dealt with", which has not
	 *   happened yet.
	 */
	if (child_id == s->prov.prio_id) {
		if (!prov_cpu(s, cpu).priority &&
		    !arr_list_empty(&s->prov.priority_cpus)) {
			acct_self_preempt_prio(s);
			flux_preempt_cpu(s, cpu);
		}
		return;
	}

	prio = get_sched(s->prov.prio_id);
	if (!prio)
		return;
	if (give_to_prio(s, prio, cpu)) {
		if (child_id == s->prov.next_id)
			acct_self_preempt_next(s);
		else
			acct_self_preempt_last(s);
		flux_preempt_cpu(s, cpu);
	}
}

static void prov_pick_next_task(struct flux_sched *s, struct flux_cpu *cpu,
				struct bpf_ghost_sched *ctx)
{
	struct flux_sched *prio, *next, *last;
	unsigned int prio_id, next_id, last_id, grantee = FLUX_SCHED_NONE;
	struct flux_cpu *cpus = get_cpus();

	if (!cpus)
		return;
	prio_id = s->prov.prio_id;
	next_id = s->prov.next_id;
	last_id = s->prov.last_id;

	prio = get_sched(prio_id);
	if (!prio)
		return;
	next = get_sched(next_id);
	if (!next)
		return;
	last = get_sched(last_id);
	if (!last)
		return;

	bpf_spin_lock(&s->lock);

	if (give_to_prio(s, prio, cpu)) {
		prov_cpu(s, cpu).owning_child = prio_id;
		acct_grant_prio(s);
		grantee = prio_id;
	} else if (flux_sched_nr_cpus_needed(next)) {
		prov_arr_list_insert_head(&s->prov.nexts_cpus, cpu,
					  prov.child_link);
		/*
		 * A note on heads vs tails: in cpu_request, we always pull from
		 * the head of the priority_cpus list when finding a victim.  In
		 * most cases, put the cpu on the head for LIFO behavior.  e.g.
		 * recently idled cpus will be reused again, and long-held cpus
		 * will remain with their owners.  The exception is when we give
		 * a priority cpu to next.  Put it at the tail, so that we never
		 * preempt from next when last also has a priority cpu.
	 	 */
		if (prov_cpu(s, cpu).priority) {
			prov_arr_list_insert_tail(&s->prov.priority_cpus, cpu,
						  prov.prio_link);
		}
		acct_grant_next(s);
		prov_cpu(s, cpu).owning_child = next_id;
		grantee = next_id;
	} else if (flux_sched_nr_cpus_needed(last)) {
		prov_arr_list_insert_head(&s->prov.lasts_cpus, cpu,
					  prov.child_link);
		if (prov_cpu(s, cpu).priority) {
			prov_arr_list_insert_head(&s->prov.priority_cpus, cpu,
						  prov.prio_link);
		}
		acct_grant_last(s);
		prov_cpu(s, cpu).owning_child = last_id;
		grantee = last_id;
	} else {
		prov_cpu(s, cpu).owning_child = 0;
	}

	bpf_spin_unlock(&s->lock);

	if (grantee != FLUX_SCHED_NONE) {
		flux_cpu_grant(s, grantee, cpu);
	} else {
		/*
		 * If we do not have idle as a child or otherwise have children
		 * that always want cpus, yield the cpu so our parent (possibly
		 * a VMM!) can find another use for the cpu.  If we're the ghost
		 * root scheduler, and there is no-one higher, we'll end up
		 * restarting PNT.
		 */
		flux_cpu_yield(s, cpu);
	}
}
