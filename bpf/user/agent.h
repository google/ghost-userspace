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

#ifndef GHOST_BPF
// The definitions below are needed when the userspace code is compiled on a
// machine that is *not* running the ghOSt kernel and therefore does not have
// the ghOSt declarations below in the bpf.h UAPI header.

// From include/uapi/linux/bpf.h for the ghost kernel.

enum {
  BPF_PROG_TYPE_GHOST_SCHED = 1000,
  BPF_PROG_TYPE_GHOST_MSG,

  BPF_GHOST_SCHED_PNT = 2000,
  BPF_GHOST_MSG_SEND,
  __MAX_BPF_GHOST_ATTACH_TYPE
};

// end include/uapi/linux/bpf.h

#endif

// Generic BPF helpers

void *bpf_map__mmap(struct bpf_map *map);
int bpf_map__munmap(struct bpf_map *map, void *addr);
void bpf_program__set_types(struct bpf_program *prog, int prog_type,
                            int expected_attach_type);

// Common BPF initialization
//
// Returns 0 on success, -1 with errno set on failure.
int agent_bpf_init(void);

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

enum {
	AGENT_BPF_TRACE_SCHEDGHOSTIDLE,
	MAX_AGENT_BPF_TRACE,
};

int agent_bpf_trace_init(unsigned int type);
void agent_bpf_trace_output(FILE *to, unsigned int type);
void agent_bpf_trace_reset(unsigned int type);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif  // GHOST_BPF_USER_AGENT_H_
