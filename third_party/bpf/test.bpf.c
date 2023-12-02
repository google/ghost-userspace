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

#include <linux/types.h>

// clang-format off
#include "libbpf/bpf_helpers.h"
#include "libbpf/bpf_tracing.h"
// clang-format on

#include "lib/ghost_uapi.h"
#include "third_party/bpf/common.bpf.h"

SEC("ghost_sched/pnt")
int test_pnt(struct bpf_ghost_sched *ctx)
{
	return 0;
}

char LICENSE[] SEC("license") = "GPL";
