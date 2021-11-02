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

#ifndef GHOST_LIB_BPF_GHOST_SHARED_H_
#define GHOST_LIB_BPF_GHOST_SHARED_H_

// Keep this file's structs in sync with bpf/ghost_shared_bpf.h.
// We need different headers for BPF and C programs due to various Google3
// reasons.

#include <stdint.h>

struct ghost_per_cpu_data {
  uint8_t want_tick;
} __attribute__((aligned(64)));

#endif  // GHOST_LIB_BPF_GHOST_SHARED_H_
