/*
 * Copyright 2021 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef GHOST_BPF_USER_AGENT_H_
#define GHOST_BPF_USER_AGENT_H_

#include <stdlib.h>

#include "libbpf/bpf.h"
#include "libbpf/libbpf.h"

#ifdef __cplusplus
extern "C" {
#endif

// From include/uapi/linux/bpf.h for the ghost kernel.

struct bpf_ghost_sched {};

enum {
  BPF_PROG_TYPE_GHOST_SCHED = 35,

  BPF_GHOST_SCHED_SKIP_TICK = 50,
  BPF_GHOST_SCHED_PNT,
  BPF_GHOST_SCHED_MAX_ATTACH_TYPE,  // __MAX_BPF_ATTACH_TYPE
};

// end include/uapi/linux/bpf.h

// Generic BPF helpers

void *bpf_map__mmap(struct bpf_map *map);
int bpf_map__munmap(struct bpf_map *map, void *addr);
void bpf_program__set_types(struct bpf_program *prog, int prog_type,
                            int expected_attach_type);

// Initializes the BPF infrastructure.
//
// Additionally, this loads and registers programs for scheduler-independent
// policies, such as how to handle the timer tick.  If a scheduler plans to
// insert its own timer tick program, then unset tick_on_request.
//
// Returns 0 on success, -1 with errno set on failure.
int agent_bpf_init(bool tick_on_request);

// Registers `prog` to be inserted at attach point `eat` during
// agent_bpf_insert_registered().  You must load the programs before calling
// insert.  You may call this repeatedly, and it will only insert each program
// once.  In particular, you may temporarily get EBUSY during an agent handoff.
//
// Returns 0 on success, -1 with errno set on failure.
int agent_bpf_register(struct bpf_program *prog, int eat);

// Inserts the programs you previously registered and loaded.
//
// Returns 0 on success, -1 with errno set on failure.  Any programs inserted
// are not removed on error; call bpf_destroy() or just exit your process.
int agent_bpf_insert_registered(int ctl_fd);

// Gracefully unlinks and unloads the BPF programs.  When agents call this, they
// explicitly close (and thus unlink/detach) BPF programs from the enclave,
// which will speed up agent upgrade/handoff.
void agent_bpf_destroy(void);

// Returns 0 on success, -1 with errno set on failure.  Must have called
// agent_bpf_init() with tick_on_request.
int agent_bpf_request_tick_on_cpu(int cpu);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif  // GHOST_BPF_USER_AGENT_H_
