// Copyright 2021 Google LLC
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// version 2 as published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

// vmlinux.h must be included before bpf_helpers.h
// clang-format off
#include "kernel/vmlinux_ghost_5_11.h"
#include "libbpf/bpf_helpers.h"
#include "libbpf/bpf_tracing.h"
// clang-format on

#include "third_party/bpf/common.bpf.h"
#include "third_party/bpf/edf.h"

bool skip_tick = false;

/* max_entries is patched at runtime to num_possible_cpus */
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1024);
	__type(key, u32);
	__type(value, struct edf_bpf_per_cpu_data);
	__uint(map_flags, BPF_F_MMAPABLE);
} cpu_data SEC(".maps");

SEC("ghost_sched/pnt")
int edf_pnt(struct bpf_ghost_sched *ctx)
{
	return 0;
}

/*
 * You have to play games to get the compiler to not modify the context pointer
 * (msg).  You can load X bytes off a ctx, but if you add to ctx, then load,
 * you'll get the dreaded: "dereference of modified ctx ptr" error.
 *
 * You can also sprinkle asm volatile ("" ::: "memory") to help reduce compiler
 * optimizations on the context.
 */
static void __attribute__((noinline)) handle_yield(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_yield *yield = &msg->yield;

	yield->agent_data = 1;
}

static void __attribute__((noinline)) handle_wakeup(struct bpf_ghost_msg *msg)
{
	struct ghost_msg_payload_task_wakeup *wakeup = &msg->wakeup;

	wakeup->agent_data = 1;
}

SEC("ghost_msg/msg_send")
int edf_msg_send(struct bpf_ghost_msg *msg)
{
	switch (msg->type) {
	case MSG_TASK_WAKEUP:
		handle_wakeup(msg);
		break;
	case MSG_TASK_YIELD:
		handle_yield(msg);
		break;
	case MSG_CPU_TICK:
		if (skip_tick)
			return 1;
		break;
	case MSG_CPU_AGENT_BLOCKED:
	case MSG_CPU_AGENT_WAKEUP:
		/*
		 * Suppress these messages.  Having this in BPF ensures that
		 * our vmlinux.h knows about these message types.
		 */
		return 1;
	}

	return 0;
}

char LICENSE[] SEC("license") = "GPL";
