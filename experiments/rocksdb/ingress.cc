// Copyright 2021 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "experiments/rocksdb/ingress.h"

#include "experiments/rocksdb/database.h"

namespace ghost_test {

SyntheticNetwork::SyntheticNetwork(double throughput, double range_query_ratio,
                                   Clock& clock)
    : ingress_(throughput, clock), range_query_ratio_(range_query_ratio) {
  CHECK_GE(range_query_ratio, 0.0);
  CHECK_LE(range_query_ratio, 1.0);
}

void SyntheticNetwork::Start() {
  CHECK(!start_.HasBeenNotified());

  ingress_.Start();
  start_.Notify();
}

bool SyntheticNetwork::Poll(Request& request) {
  CHECK(start_.HasBeenNotified());

  const auto [arrived, arrival_time] = ingress_.HasNewArrival();
  if (!arrived) {
    return false;
  }
  // A request is in the ingress queue
  absl::Time received = absl::Now();
  bool get = absl::Bernoulli(gen_, 1.0 - range_query_ratio_);
  if (get) {
    // Get request
    request.work = Request::Get{
        .entry = absl::Uniform<uint32_t>(gen_, 0, Database::kNumEntries)};
  } else {
    // Range query
    request.work = Request::Range{
        .start_entry = absl::Uniform<uint32_t>(
            gen_, 0, Database::kNumEntries - kRangeQuerySize + 1),
        .size = kRangeQuerySize};
  }
  request.request_generated = arrival_time;
  request.request_received = received;
  return true;
}

}  // namespace ghost_test
