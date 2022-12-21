// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

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
  absl::Time received = ghost::MonotonicNow();
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
