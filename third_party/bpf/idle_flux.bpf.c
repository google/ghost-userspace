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

/* Idle scheduler implementation for Flux. */

static void idle_request_for_cpus(struct flux_sched *i, int child_id,
				  int nr_cpus, int *ret)
{
	/* Never called, we have no children. */
}

static void idle_cpu_allocated(struct flux_sched *i, struct flux_cpu *cpu)
{
	/* Don't care. */
}

static void idle_cpu_returned(struct flux_sched *i, int child_id,
			      struct flux_cpu *cpu)
{
	/* Never called, we have no children. */
}

static void idle_cpu_preempted(struct flux_sched *i, int child_id,
			       struct flux_cpu *cpu)
{
	/* Don't care. */
}

static void idle_cpu_preemption_completed(struct flux_sched *i, int child_id,
					  struct flux_cpu *cpu)
{
	/* Don't care. */
}

static void idle_cpu_ticked(struct flux_sched *i, int child_id,
			    struct flux_cpu *cpu)
{
	/* Don't care. */
}

static void idle_pick_next_task(struct flux_sched *i, struct flux_cpu *cpu,
				struct bpf_ghost_sched *ctx)
{
	flux_run_idle(cpu, ctx);
}
