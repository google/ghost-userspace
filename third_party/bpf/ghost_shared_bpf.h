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

#ifndef GHOST_LIB_BPF_GHOST_SHARED_BPF_H_
#define GHOST_LIB_BPF_GHOST_SHARED_BPF_H_

// Keep this file's structs in sync with bpf/ghost_shared.h.
// We need different headers for BPF and C programs due to various Google3
// reasons.

// From include/uapi/linux/bpf.h for the ghost kernel.  When we build the BPF
// program from Google3, we're getting the vmlinux from third_party/linux_tools.
// That header was not generated from the ghost/ghost kernel, so we need to
// manually include these bits.
//
// We can't just include the uapi header directly, since the uapi header pulls
// in extra things that will conflict when BPF builds.  The long term fix is to
// use a vmlinux generated against the ghost kernel.
struct bpf_scheduler {
};

#define BPF_PROG_TYPE_SCHEDULER 35
#define BPF_SCHEDULER_TICK 50

// end uapi/linux/bpf.h

struct per_cpu_data {
  __u8 want_tick;
} __attribute__((aligned(64)));

#endif  // GHOST_LIB_BPF_GHOST_SHARED_BPF_H_
