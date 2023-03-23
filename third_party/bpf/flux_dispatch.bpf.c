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

/*
 * Manually #include this C file in your BPF program after your scheduler
 * helpers (e.g. get_sched()) and your gen_case macros, and before your sched
 * ops.
 */

/*
 * You can't hold bpf spinlocks and make helper calls, which include looking up
 * map elements.  To use 'intrusive' list entries embedded in structs (e.g.  a
 * 'next' index/pointer for a singly-linked list) and to manipulate those
 * structs while holding a lock, we need to safely access fields by index
 * without calling bpf_map_lookup_elem().
 *
 * We can do so with...  drumroll... another layer of indirection.  (Sort of).
 * The Array map is a map of a single struct, which contains the entire array
 * that we want to access.  So once we do a single lookup, we have access to the
 * entire array.  e.g. lookup(cpu_data, 0), and now we can access cpu[x],
 * cpu[y], cpu[z], etc.
 *
 * The data layout of the Array map is still just an array of structs; we just
 * have an intermediate struct (e.g. __cpu_arr) to convince the verifier this is
 * safe.  Userspace still can cast the array map to an array of
 * flux_cpu[FLUX_MAX_CPUS].
 */
struct __cpu_arr {
	struct flux_cpu e[FLUX_MAX_CPUS];
};
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct __cpu_arr);
	__uint(map_flags, BPF_F_MMAPABLE);
} cpu_data SEC(".maps");

struct __thread_arr {
	struct flux_thread e[FLUX_MAX_GTIDS];
};
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct __thread_arr);
	__uint(map_flags, BPF_F_MMAPABLE);
} thread_data SEC(".maps");

/*
 * Hash map of task_sw_info, indexed by gtid, used for getting the SW info to
 * lookup the *real* per-task data: the thread_data.
 *
 * aligned(8) since this is a bpf map value.
 */
struct task_sw_info {
	uint32_t id;
	uint32_t index;
} __attribute__((aligned(8)));

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, FLUX_MAX_GTIDS);
	__type(key, u64);
	__type(value, struct task_sw_info);
} sw_lookup SEC(".maps");

static struct flux_cpu *get_cpus(void)
{
	struct __cpu_arr *__ca;
	u32 zero = 0;

	__ca = bpf_map_lookup_elem(&cpu_data, &zero);
	if (!__ca)
		return NULL;
	return __ca->e;
}

/* Helper, from cpu id to per-cpu data blob */
static struct flux_cpu *cpuid_to_cpu(u32 cpu_id)
{
	struct flux_cpu *cpus = get_cpus();

	if (!cpus)
		return NULL;
	/*
	 * Do the index bounds check as close to the use as possible.  That will
	 * decrease the chances that the compiler drops the register and e.g.
	 * uses the function argument's register.  The verifier knows the bounds
	 * check only for a particular register.
	 *
	 * BPF_MUST_CHECK has helped prevent the compiler from dropping the
	 * register, though it doesn't seem to matter all the time.
	 */
	BPF_MUST_CHECK(cpu_id);
	if (cpu_id >= FLUX_MAX_CPUS)
		return NULL;
	return &cpus[cpu_id];
}

static struct flux_cpu *get_this_cpu(void)
{
	return cpuid_to_cpu(bpf_get_smp_processor_id());
}

static struct flux_thread *get_thread_array(void)
{
	struct __thread_arr *__t_arr;
	u32 zero = 0;

	__t_arr = bpf_map_lookup_elem(&thread_data, &zero);
	if (!__t_arr)
		return NULL;
	return __t_arr->e;
}

static struct flux_thread *gtid_to_thread(u64 gtid)
{
	struct task_sw_info *swi;
	struct flux_thread *__t_arr;
	u32 idx;

	/* Convenience for our callers: no process has gtid == 0 */
	if (gtid == 0)
		return NULL;
	swi = bpf_map_lookup_elem(&sw_lookup, &gtid);
	if (!swi)
		return NULL;
	__t_arr = get_thread_array();
	if (!__t_arr)
		return NULL;
	idx = swi->index;
	BPF_MUST_CHECK(idx);
	if (idx >= FLUX_MAX_GTIDS)
		return NULL;
	return &__t_arr[idx];
}

/*
 * Assumes you have a valid reference before increffing.  We can add a
 * maybe-incref-if-not-zero helper if necessary.  (Constructing a refcnt
 * conditioned on there being one already somewhere).
 */
static inline void flux_thread_incref(struct flux_thread *t)
{
	__atomic_add_fetch(&t->f.refcnt, 1, __ATOMIC_ACQUIRE);
}

static inline void flux_thread_decref(struct flux_thread *t)
{
	if (__atomic_sub_fetch(&t->f.refcnt, 1, __ATOMIC_RELEASE) == 0)
		bpf_map_delete_elem(&sw_lookup, &t->f.gtid);
}

static inline struct flux_sched *get_parent(struct flux_sched *s)
{
	return get_sched(get_parent_id(s));
}

/*
 * Flux callback dispatch.
 *
 * In BPF, We can't dispatch functions via function pointers, like we can in
 * normal C code.  Instead, hardcode switch statements on the sched's type.
 * Many schedulers (with different IDs) can have the same type.
 *
 * Note we still pass the struct flux_sched to each function.  Keep in mind we
 * may have more than one sched of a given type (e.g. multiple biffs with
 * different sets of cpus), so we'd need to pass either the sched or the id.
 * Most ops need their actual sched struct, so just look it up for them.
 *
 * Same goes for 'cpu'.  You could do a bpf_get_smp_processor_id() and
 * cpuid_to_cpu(), but no need for the helpers since our caller already knows
 * cpu.
 *
 * The including scheduler must define macros to generate the case statements:
 * __gen_thread_op_cases and __gen_cpu_op_cases.
 *
 * TODO: consider adding pre and post hooks around the switch statements.
 * That way, the "composing" scheduler can do a little work for certain messages
 * without changing a child scheduler.  e.g. if you want to use prio change to
 * mean "move the thread from biff to cfs-bpf", you can do that without changing
 * biff_flux.bpf.c.  Something like pre_thread_op(thr, msg, op) that expands to
 * the right function for each message type, all of which are conditionally
 * defined to an empty function.
 */

#define __cat_op(SCHED, OP) SCHED ## OP
#define __thread_cat_op(SCHED, OP) __cat_op(SCHED, _thread_ ## OP)
#define __cpu_cat_op(SCHED, OP) __cat_op(SCHED, _cpu_ ## OP)

#define __thread_op_thr(thr, seq, msg, op) ({				\
	struct flux_sched *__sched = get_sched((thr)->f.sched);		\
	if (!__sched)							\
		return;							\
	switch (__sched->f.type) {					\
	__gen_thread_op_cases(__thread_cat_op, op, __sched, thr, msg)	\
	};								\
	smp_store_release(&(thr)->f.seqnum, seq);			\
})

#define __thread_op(gtid, seq, msg, op) ({				\
	struct flux_thread *__thr = gtid_to_thread(gtid);		\
	if (!__thr)							\
		return;							\
	__thread_op_thr(__thr, seq, msg, op);				\
	__thr;								\
})

#define __cpu_op(sched, cpu, op) ({					\
	switch ((sched)->f.type) {					\
	__gen_cpu_op_cases(__cpu_cat_op, op, sched, cpu)		\
	};								\
})

#define __cpu_op_child(sched, child_id, cpu, op) ({			\
	switch ((sched)->f.type) {					\
	__gen_cpu_op_cases(__cpu_cat_op, op, sched, child_id, cpu)	\
	};								\
})

#define __request_for_cpus(sched, child_id, nr_cpus) ({			\
	int __ret = -1;							\
	switch ((sched)->f.type) {					\
	__gen_cpu_op_cases(__cat_op, _request_for_cpus, sched,		\
			   child_id, nr_cpus, &__ret)			\
	};								\
	__ret;								\
})

#define __pick_next_task(sched, cpu, ctx) ({				\
	switch ((sched)->f.type) {					\
	__gen_cpu_op_cases(__cat_op, _pick_next_task, sched, cpu, ctx)	\
	};								\
})

/*
 * The flux_thread ops all take an args struct of the appropriate type, e.g.
 * thread_blocked takes a flux_args_blocked.
 *
 * Essentially, we're using the args payload like a 'va_args' to carry all the
 * info we have to the thread scheduler, such that the thread_ops macro can be
 * used for each message without worrying about types.
 *
 * For the most part, these structs are the same as their ghost_msg_payload
 * counterparts.  Some are not for various reasons: due to scheduler switching,
 * flux doesn't track all fields in task_new, e.g.  'runtime'.  So we're paring
 * down the ghost interface slightly for flux sched ops.  Similarly, for
 * prio_changed, we pass the old_nice as a helper to the child sched.
 *
 * Another note: we synthesize a task_new when a thread changes schedulers, and
 * BPF won't let you call the same function with different pointer types (e.g.
 * PTR_TO_CTX and pointer to stack).  You'll get the error:
 *
 * 	same insn cannot be used with different pointers
 *
 * This isn't a reason to use a custom struct in itself, since we could put a
 * copy of the ghost_msg_payload_task_new on the stack too.
 */

/*
 * Fields are either in struct flux_thread or not tracked by flux.  'runnable'
 * is managed by flux: we'll call flux_thread_new() and flux_thread_wakeup() for
 * newly runnable threads.
 */
struct flux_args_new {
};

/* wake_up_cpu is the calling cpu, and the sched knows last_ran_cpu */
struct flux_args_wakeup {
	bool deferrable;
	int waker_cpu;
};

/* t->f.nice is set before calling the thread op. */
struct flux_args_prio_changed {
	int old_nice;
};

/*
 * thread_runnable() does not correspond to a ghost message.
 * Runnable is "the ball is yours".  Wakeup is freshly on_rq + runnable.
 */
struct flux_args_runnable {
};

#define flux_args_on_cpu ghost_msg_payload_task_on_cpu
#define flux_args_blocked ghost_msg_payload_task_blocked
#define flux_args_yield ghost_msg_payload_task_yield
#define flux_args_preempt ghost_msg_payload_task_preempt
#define flux_args_switchto ghost_msg_payload_task_switchto
#define flux_args_affinity_changed ghost_msg_payload_task_affinity_changed
#define flux_args_departed ghost_msg_payload_task_departed
#define flux_args_dead ghost_msg_payload_task_dead

#define flux_thread_new(gtid, seq, args)				\
	__thread_op(gtid, seq, args, new)
#define flux_thread_on_cpu(gtid, seq, args)				\
	__thread_op(gtid, seq, args, on_cpu)
#define flux_thread_blocked(gtid, seq, args)				\
	__thread_op(gtid, seq, args, blocked)
#define flux_thread_runnable(gtid, seq, args)				\
	__thread_op(gtid, seq, args, runnable)
#define flux_thread_wakeup(gtid, seq, args)				\
	__thread_op(gtid, seq, args, wakeup)
#define flux_thread_yielded(gtid, seq, args)				\
	__thread_op(gtid, seq, args, yielded)
#define flux_thread_preempted(gtid, seq, args)				\
	__thread_op(gtid, seq, args, preempted)
#define flux_thread_switchto(gtid, seq, args)				\
	__thread_op(gtid, seq, args, switchto)
#define flux_thread_affinity_changed(gtid, seq, args)			\
	__thread_op(gtid, seq, args, affinity_changed)
#define flux_thread_prio_changed(gtid, seq, args)			\
	__thread_op(gtid, seq, args, prio_changed)
#define flux_thread_departed(gtid, seq, args)				\
	__thread_op(gtid, seq, args, departed)
#define flux_thread_dead(gtid, seq, args)				\
	__thread_op(gtid, seq, args, dead)

#define flux_thread_new_thr(thr, seq, args)				\
	__thread_op_thr(thr, seq, args, new)
#define flux_thread_runnable_thr(thr, seq, args)			\
	__thread_op_thr(thr, seq, args, runnable)
#define flux_thread_prio_changed_thr(thr, seq, args)			\
	__thread_op_thr(thr, seq, args, prio_changed)

#define flux_cpu_allocated(sched, cpu)					\
	__cpu_op(sched, cpu, allocated)
#define flux_cpu_returned(sched, child_id, cpu) 			\
	__cpu_op_child(sched, child_id, cpu, returned)
#define flux_cpu_preempted(sched, child_id, cpu) 			\
	__cpu_op_child(sched, child_id, cpu, preempted)
#define flux_cpu_preemption_completed(sched, child_id, cpu) 		\
	__cpu_op_child(sched, child_id, cpu, preemption_completed)
#define flux_cpu_ticked(sched, child_id, cpu) 				\
	__cpu_op_child(sched, child_id, cpu, ticked)

#define flux_request_for_cpus(sched, child_id, nr_cpus) 		\
	__request_for_cpus(sched, child_id, nr_cpus)
#define flux_pick_next_task(sched, cpu, ctx)				\
	__pick_next_task(sched, cpu, ctx)

/* API declarations, which are used by the flux schedulers sched ops */
#pragma GCC diagnostic ignored "-Wunused-function"
static int flux_request_cpus(struct flux_sched *s, int nr_cpus);
static void flux_cpu_grant(struct flux_sched *s, int child_id,
			   struct flux_cpu *cpu);
static void flux_preempt_cpu(struct flux_sched *s, struct flux_cpu *cpu);
static void flux_cpu_yield(struct flux_sched *s, struct flux_cpu *cpu);
static void flux_prepare_to_run(struct flux_thread *t, struct flux_cpu *cpu);
static int flux_run_thread(struct flux_thread *t, struct flux_cpu *cpu,
			   struct bpf_ghost_sched *ctx);
static void flux_run_idle(struct flux_cpu *cpu, struct bpf_ghost_sched *ctx);
static void flux_run_current(struct flux_cpu *cpu, struct bpf_ghost_sched *ctx);
static void flux_restart_pnt(struct flux_cpu *cpu, struct bpf_ghost_sched *ctx);
static void flux_join_scheduler(struct flux_thread *t, int new_sched_id,
				bool runnable);
