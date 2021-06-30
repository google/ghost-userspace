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

#ifdef __cplusplus
extern "C" {
#endif

#define GHOST_BPF_NONE 0
#define GHOST_BPF_TICK_ON_REQUEST 1
#define GHOST_BPF_MAX_NR_PROGS 2

// Initializes BPF, loading the programs into the kernel.
// Returns 0 on success, -1 with errno set on failure.
int bpf_init(void);
// Inserts the programs you select.  Must call bpf_init() first.  Pass an array
// of GHOST_BPF program numbers.  Returns 0 on success, -1 with errno set on
// failure.  Any programs inserted are not removed; call bpf_destroy() or just
// exit your process.
int bpf_insert(int ctl_fd, int *progs, size_t nr_progs);
// Gracefully unlinks and unloads the BPF programs.  When agents call this, they
// explicitly close (and thus unlink/detach) BPF programs from the enclave,
// which will speed up agent upgrade/handoff.
void bpf_destroy(void);

// Returns 0 on success, -1 with errno set on failure.  Must have selected
// GHOST_BPF_TICK_ON_REQUEST.
int request_tick_on_cpu(int cpu);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif  // GHOST_BPF_USER_AGENT_H_
