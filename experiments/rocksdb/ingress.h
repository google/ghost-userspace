// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef GHOST_EXPERIMENTS_ROCKSDB_INGRESS_H_
#define GHOST_EXPERIMENTS_ROCKSDB_INGRESS_H_

#include "absl/random/bit_gen_ref.h"
#include "absl/time/clock.h"
#include "experiments/rocksdb/clock.h"
#include "experiments/rocksdb/request.h"
#include "lib/base.h"

namespace ghost_test {
namespace {
// Returns a shared instance of `RealClock`.
RealClock& GetRealClock() {
  static RealClock* clock = new RealClock();
  return *clock;
}
}  // namespace

// This is an ingress queue that synthetically generates requests. A load with a
// given throughput (units of requests per second) is generated. The ingress
// queue is backed by a Poisson arrival process with a lambda equal to the given
// throughput.
//
// Example:
// Ingress ingress_(/*throughput=*/20000.0);
// (Constructs an ingress queue with a target throughput of 20,000 requests per
// second.)
// ...
// ingress_.Start();
// (The ingress queue starts generating synthetic load once 'Start' is called.)
// ...
// std::pair<bool, absl::Time> pair = ingress_.HasNewArrival();
// if (pair.first) {
//   (A new synthetic request is waiting on the ingress queue. 'pair.second' is
//   the time that the request arrived and was added to the ingress queue.)
// } else {
//   (No synthetic request is waiting on the ingress queue. The value of
//   'pair.second' is undefined.)
// }
class Ingress {
 public:
  // Constructs the ingress queue. `throughput` is the generated throughput (so
  // `throughput` is used as the lambda for the Possion arrival process).
  // `clock` is the clock to use and is a `RealClock` by default. A
  // `SimulatedClock` could be passed instead though generally only unit tests
  // will want to do this so they can test deterministic behavior.
  explicit Ingress(double throughput, Clock& clock = GetRealClock())
      : throughput_(throughput), clock_(clock) {
    CHECK_GE(throughput_, 0.0);
  }

  // Starts the ingress queue.
  void Start() { start_ = clock_.TimeNow(); }

  // Models a Poisson arrival process with a lambda of `throughput_`. Returns a
  // pair with 'true' and the arrival time when at least one request is waiting
  // in the ingress queue. Returns a pair with 'false' and an undefined arrival
  // time when no request is waiting in the ingress queue.
  std::pair<bool, absl::Time> HasNewArrival() {
    CHECK_NE(start_, absl::UnixEpoch());

    if (clock_.TimeNow() >= start_) {
      absl::Time arrival = start_;
      start_ += NextDuration();
      return std::make_pair(true, arrival);
    }
    return std::make_pair(false, absl::UnixEpoch());
  }

 private:
  // Chooses the next interarrival time for a Poisson process with a lambda of
  // `throughput_` (units of requests per second). The interarrival time in a
  // Possion process with a throughput of `throughput_` (i.e., `throughput_`
  // arrivals per second) is modeled with an exponential distribution with a
  // lambda of `throughput_`.
  absl::Duration NextDuration() {
    // To help avoid issues due to double precision, we convert `throughput_`
    // from units of 'requests per second' to 'requests per millisecond'.
    double duration_msec = absl::Exponential(gen_, throughput_ / 1000.0);
    return absl::Milliseconds(duration_msec);
  }

  // The target throughput for the ingress queue. This throughput is used as the
  // lambda for the Poisson arrival process.
  const double throughput_;
  // The clock. This can either be a 'RealCock' (which the RocksDB application
  // will want) or a 'SimulatedClock' (which generally only tests will want so
  // they can test deterministic behavior).
  Clock& clock_;
  // The time that the ingress queue started at.
  absl::Time start_ = absl::UnixEpoch();
  // 'absl::BitGen' is not thread safe, but each instance of this class will be
  // used by one thread.
  absl::BitGen gen_;
};

// This is the synthetic load generator. The load generator generates the given
// throughput of synthetic requests and is backed by a Poisson arrival process
// (via the 'Ingress' class). The fraction of synthetic requests that are Get
// requests and the fraction that are Range queries is specified via the
// constructor.
//
// Example:
// SyntheticNetwork network_(/*throughput=*/20000.0,
//                           /*range_query_ratio=*/0.2);
// (Constructs the synthetic load generator with a target throughput of 20,000
// requests per second. 20% of requests are Range queries and therefore the
// remaining 80% of requests are Get requests.
// ...
// network_.Start();
// (The synthetic load generator starts generating synthetic load once 'Start'
// is called.)
// ...
// Request request;
// if (network_.Poll(request)) {
//   (There is a request that arrived. 'request' was filled in.)
// } else {
//   (No new request arrived. The contents of 'request' are undefined.)
// }
class SyntheticNetwork {
 public:
  // Constructs the synthetic load generator. The load generator generates a
  // throughput of `throughput` and is backed by a Poisson arrival process. The
  // fraction of requests that are Range queries is `range_query_ratio` and the
  // fraction of requests that are Get requests is 1 - `range_query_ratio_`.
  // Note that `range_query_ratio` must be greater than or equal to 0.0 and less
  // than or equal to 1.0. `clock` is the clock to use and is a `RealClock` by
  // default. A `SimulatedClock` could be passed instead though generally only
  // unit tests will want to do this so they can test deterministic behavior.
  SyntheticNetwork(double throughput, double range_query_ratio,
                   Clock& clock = GetRealClock());

  // Starts the synthetic network. No requests are synthetically generated until
  // this method is called.
  void Start();
  // Polls the synthetic ingress queue. Returns true and fills in `request` with
  // the request at the front of the queue, if one exists. Returns false if
  // there is no request in the queue; the value at the memory location pointed
  // to by `request` is undefined in this case.
  bool Poll(Request& request);

  // The size of range queries.
  static constexpr uint32_t kRangeQuerySize = 5000;

 private:
  // The synthetic ingress queue.
  Ingress ingress_;
  // The fraction of requests that are Range queries is `range_query_ratio_` and
  // the fraction of requests that are Get requests is 1 - `range_query_ratio_`.
  // Note that `range_query_ratio_` is greater than or equal to 0.0 and less
  // than or equal to 1.0.
  const double range_query_ratio_;
  // Notifies when 'Start' has been called (i.e., the synthetic network has
  // started generating load).
  ghost::Notification start_;
  // A bit generator used to randomly determine request type and the entry or
  // entries accessed by the request.
  absl::BitGen gen_;
};

}  // namespace ghost_test

#endif  // GHOST_EXPERIMENTS_ROCKSDB_INGRESS_H_
