/* Copyright 2021 Google LLC
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifndef GHOST_LIB_BPF_BPF_EDF_H_
#define GHOST_LIB_BPF_BPF_EDF_H_

#ifndef __BPF__
#include <stdint.h>
#endif

struct edf_bpf_per_cpu_data {
	uint8_t example_bool;
} __attribute__((aligned(64)));

#endif  // GHOST_LIB_BPF_BPF_EDF_H_
