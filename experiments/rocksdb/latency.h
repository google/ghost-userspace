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

#ifndef GHOST_EXPERIMENTS_ROCKSDB_LATENCY_H_
#define GHOST_EXPERIMENTS_ROCKSDB_LATENCY_H_

#include "absl/time/clock.h"
#include "experiments/rocksdb/request.h"

namespace ghost_test {

namespace latency {

struct PrintOptions {
  // If true, prints the results in human-readable form. Otherwise, prints the
  // results in CSV form.
  bool pretty;
  // If true, prints the entire distribution.
  bool distribution;
  // If true, prints the latencies in units of nanoseconds. If false, prints the
  // latencies in units of microseconds.
  bool ns;
  // The output stream to send the results to. We make 'os' a pointer rather
  // than a reference since a reference cannot be reassigned.
  std::ostream* os;
};

void Print(const std::vector<Request>& requests, absl::Duration runtime,
           PrintOptions options);

// We put these in the header rather than in latency.cc since latency_test needs
// these in order to generate the correct number of dashes for the pretty print
// prefix.
constexpr size_t kStageLen = 28;
constexpr size_t kTotalRequestsLen = 18;
constexpr size_t kThroughputLen = 22;
constexpr size_t kResultLen = 12;
// Add 8 to the end to account for the space between each column in the results.
constexpr size_t kNumDashes =
    kStageLen + kTotalRequestsLen + kThroughputLen + (6 * kResultLen) + 8;

}  // namespace latency

}  // namespace ghost_test

#endif  // GHOST_EXPERIMENTS_ROCKSDB_LATENCY_H_
