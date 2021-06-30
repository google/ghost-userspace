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

#ifndef GHOST_LIB_BPF_SCHEDLAT_SHARED_H_
#define GHOST_LIB_BPF_SCHEDLAT_SHARED_H_

// Keep this file's structs in sync with bpf/schedlat_shared_bpf.h.
// We need different headers for BPF and C programs due to various Google3
// reasons.

#include <stdint.h>

#define MAX_PIDS 102400
#define MAX_NR_HIST_SLOTS 25

struct task_stat {
  uint64_t runnable_at;
  uint64_t latched_at;
  uint64_t ran_at;
};

/*
 * Power of 2 histogram, <=1 us, 2us, 4us, etc.  This struct must be at least
 * 8-byte aligned, since it is a value for a BPF map.
 */
struct hist {
  uint32_t slots[MAX_NR_HIST_SLOTS];
} __attribute__((aligned(64)));

enum {
  RUNNABLE_TO_LATCHED,
  LATCHED_TO_RUN,
  RUNNABLE_TO_RUN,
  NR_HISTS,
};

#endif  // GHOST_LIB_BPF_SCHEDLAT_SHARED_H_
