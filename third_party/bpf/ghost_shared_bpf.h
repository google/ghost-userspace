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

struct ghost_per_cpu_data {
  __u8 want_tick;
} __attribute__((aligned(64)));

#endif  // GHOST_LIB_BPF_GHOST_SHARED_BPF_H_
