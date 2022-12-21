// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "experiments/rocksdb/clock.h"
#include "experiments/rocksdb/ingress.h"
#include "experiments/rocksdb/request.h"

// These tests check that 'Ingress' can generate low (100 requests per second),
// medium (500,000 requests per second), and high (70,000,000 requests per
// second) throughputs.
//
// They also check that 'SyntheticNetwork' can generate the same throughputs
// with no Range queries, though we test with a high throughput of 5,000,000
// requests per second rather than 70,000,000 requests per second since the test
// seems to use too much memory with 70,000,000 requests per second.
// Furthermore, the tests check that 'SyntheticNetwork' can generate a medium
// throughput with 0.5% of requests as Range queries and 75% of requests as
// Range queries.

namespace ghost_test {
namespace {

using ::testing::Eq;
using ::testing::IsTrue;

// The actual results must be within this fraction of the expected results in
// order for each test to pass. We do not expect the actual results to match the
// expected results because the ingress queue is backed by a Poisson arrival
// process and because Range queries are generated via a Bernoulli process that
// tries to make a certain share of requests as Range queries.
constexpr double kErrorEpsilon = 0.01;
// The throughput to use for the 'LowThroughput' test.
constexpr double kLowThroughput = 100.0;
// The throughput to use for the 'MediumThroughput' test.
constexpr double kMediumThroughput = 500'000.0;
// The throughput to use for the 'HighThroughput' Ingress test. We set
// 'kHighIngressThroughput' to 70,000,000 requests per second because this is a
// larger throughput than RocksDB could ever handle. We want to ensure that the
// ingress queue will not experience floating-point precision issues for any
// throughput that RocksDB may want to generate.
constexpr double kHighIngressThroughput = 70'000'000.0;
// The throughput to use for the 'HighThroughput' Synthetic Network test. We set
// 'kHighSyntheticNetworkThroughput' to 5,000,000 requests per second because we
// want to test a high throughput but the test runs out of memory when we test
// with a throughput of 70,000,000 requests per second like above. 5,000,000
// requests per second is still higher than the current RocksDB benchmarks we
// have today could handle, so test 5,000,000 requests per second instead.
constexpr double kHighSyntheticNetworkThroughput = 5'000'000.0;

// Converts a deque of requests to a deque of times. Uses the
// 'request_generated' time from each request.
std::deque<absl::Time> RequestsToTimes(const std::deque<Request>& requests) {
  std::deque<absl::Time> times;
  for (const Request& request : requests) {
    times.push_back(request.request_generated);
  }
  return times;
}

// Returns true if `times` is sorted in ascending order and false otherwise.
bool IsInAscendingOrder(const std::deque<absl::Time>& times) {
  return std::is_sorted(times.begin(), times.end());
}

// Returns the generated throughput for `times` by looking at when the requests
// were generated and how many requests there are.
//
// `times` must be sorted in ascending order.
double CalculateThroughput(const std::deque<absl::Time>& times) {
  if (times.empty()) {
    // Avoid a divide by zero error since 'duration' will be 0 if there are no
    // requests.
    return 0.0;
  }

  CHECK_GE(times.back(), times.front());
  const absl::Duration duration = times.back() - times.front();
  // We need one of the two operands to the division operator to be a double in
  // order to do floating point division.
  // We make `duration` a double rather than 'times.size()' because `duration`
  // is likely to not be an integer number of seconds whereas 'times.size()'
  // will always be an integer number of requests. If we were to cast
  // 'times.size()' to be a double and use 'absl::ToInt64Seconds(duration)' for
  // `duration`, then `duration` would be rounded down to the next lowest
  // integer, which would make our throughput calculation incorrect.
  return times.size() / absl::ToDoubleSeconds(duration);
}

// Returns the share of 'requests' that are Range queries.
double CalculateRangeQueryRatio(const std::deque<Request>& requests) {
  if (requests.empty()) {
    return 0.0;
  }

  size_t num_range_queries = 0;
  for (const Request& request : requests) {
    if (request.IsRange()) {
      num_range_queries++;
    }
  }
  return static_cast<double>(num_range_queries) / requests.size();
}

// Returns true if `actual` is greater than or equal to `expected` * (1.0 -
// `epsilon`) and less than or equal to `expected` * (1.0 + `epsilon`).
//
// For example, if:
//   `actual` = 98.0
//   `expected` = 100.0
//   `epsilon` = 0.05
// This method will return true because 95.0 <= 98.0 <= 105.0.
//
// Note that 0.0 <= `epsilon` <= 1.0.
bool IsWithin(double actual, double expected, double epsilon) {
  CHECK_GE(epsilon, 0.0);
  CHECK_LE(epsilon, 1.0);

  const double kLowerBound = expected * (1.0 - epsilon);
  const double kUpperBound = expected * (1.0 + epsilon);
  return actual >= kLowerBound && actual <= kUpperBound;
}

// Tests that 'Ingress' can generate a low throughput of 100 requests/sec.
TEST(IngressTest, LowThroughput) {
  // The duration to generate requests. We choose a longer time for this test
  // than other tests since the Poisson arrival process seems to vary more
  // widely percentage-wise when the lambda is low.
  constexpr absl::Duration kGenerationDuration = absl::Seconds(1000);
  SimulatedClock clock;
  Ingress ingress(kLowThroughput, clock);

  clock.SetTime(ghost::MonotonicNow());
  ingress.Start();
  clock.AdvanceTime(kGenerationDuration);

  std::deque<absl::Time> times;
  while (true) {
    const auto& [arrived, arrival] = ingress.HasNewArrival();
    if (!arrived) {
      // We have dequeued all pending requests in the ingress queue.
      break;
    }
    times.push_back(arrival);
  }

  EXPECT_THAT(IsInAscendingOrder(times), IsTrue());
  EXPECT_THAT(
      IsWithin(CalculateThroughput(times), kLowThroughput, kErrorEpsilon),
      IsTrue());
}

// Tests that 'Ingress' can generate a medium throughput of 500,000
// requests/sec.
TEST(IngressTest, MediumThroughput) {
  // The duration to generate requests.
  constexpr absl::Duration kGenerationDuration = absl::Seconds(10);
  SimulatedClock clock;
  Ingress ingress(kMediumThroughput, clock);

  clock.SetTime(ghost::MonotonicNow());
  ingress.Start();
  clock.AdvanceTime(kGenerationDuration);

  std::deque<absl::Time> times;
  while (true) {
    const auto& [arrived, arrival] = ingress.HasNewArrival();
    if (!arrived) {
      // We have dequeued all pending requests in the ingress queue.
      break;
    }
    times.push_back(arrival);
  }

  EXPECT_THAT(IsInAscendingOrder(times), IsTrue());
  EXPECT_THAT(
      IsWithin(CalculateThroughput(times), kMediumThroughput, kErrorEpsilon),
      IsTrue());
}

// Tests that 'Ingress' can generate a high throughput of 70,000,000
// requests/sec.
TEST(IngressTest, HighThroughput) {
  // The duration to generate requests.
  constexpr absl::Duration kGenerationDuration = absl::Seconds(10);
  SimulatedClock clock;
  Ingress ingress(kHighIngressThroughput, clock);

  clock.SetTime(ghost::MonotonicNow());
  ingress.Start();
  clock.AdvanceTime(kGenerationDuration);

  std::deque<absl::Time> times;
  while (true) {
    const auto& [arrived, arrival] = ingress.HasNewArrival();
    if (!arrived) {
      // We have dequeued all pending requests in the ingress queue.
      break;
    }
    times.push_back(arrival);
  }

  EXPECT_THAT(IsInAscendingOrder(times), IsTrue());
  EXPECT_THAT(IsWithin(CalculateThroughput(times), kHighIngressThroughput,
                       kErrorEpsilon),
              IsTrue());
}

// Tests that 'SyntheticNetwork' can generate a low throughput of 100
// requests/sec with no Range queries.
TEST(SyntheticNetworkTest, LowThroughput) {
  // The duration to generate requests. We choose a longer time for this test
  // than other tests since the Poisson arrival process seems to vary more
  // widely percentage-wise when the lambda is low.
  constexpr absl::Duration kGenerationDuration = absl::Seconds(1000);
  constexpr double kNoRangeQueryRatio = 0.0;
  SimulatedClock clock;
  SyntheticNetwork network(kLowThroughput, kNoRangeQueryRatio, clock);

  clock.SetTime(ghost::MonotonicNow());
  network.Start();
  clock.AdvanceTime(kGenerationDuration);

  std::deque<Request> requests;
  Request request;

  // When `network.Poll` returns false, we have dequeued all pending requests in
  // the synthetic network.
  while (network.Poll(request)) {
    requests.push_back(request);
  }

  EXPECT_THAT(IsInAscendingOrder(RequestsToTimes(requests)), IsTrue());
  EXPECT_THAT(IsWithin(CalculateThroughput(RequestsToTimes(requests)),
                       kLowThroughput, kErrorEpsilon),
              IsTrue());
  // Since there are no range queries, the range query ratio must be exactly
  // equal to 'kNoRangeQueryRatio', which is 0.0.
  EXPECT_THAT(CalculateRangeQueryRatio(requests), Eq(kNoRangeQueryRatio));
}

// Tests that 'SyntheticNetwork' can generate a medium throughput of 500,000
// requests/sec with no Range queries.
TEST(SyntheticNetworkTest, MediumThroughput) {
  // The duration to generate requests.
  constexpr absl::Duration kGenerationDuration = absl::Seconds(10);
  constexpr double kNoRangeQueryRatio = 0.0;
  SimulatedClock clock;
  SyntheticNetwork network(kMediumThroughput, kNoRangeQueryRatio, clock);

  clock.SetTime(ghost::MonotonicNow());
  network.Start();
  clock.AdvanceTime(kGenerationDuration);

  std::deque<Request> requests;
  Request request;

  // When `network.Poll` returns false, we have dequeued all pending requests in
  // the synthetic network.
  while (network.Poll(request)) {
    requests.push_back(request);
  }

  EXPECT_THAT(IsInAscendingOrder(RequestsToTimes(requests)), IsTrue());
  EXPECT_THAT(IsWithin(CalculateThroughput(RequestsToTimes(requests)),
                       kMediumThroughput, kErrorEpsilon),
              IsTrue());
  // Since there are no range queries, the range query ratio must be exactly
  // equal to 'kNoRangeQueryRatio', which is 0.0.
  EXPECT_THAT(CalculateRangeQueryRatio(requests), Eq(kNoRangeQueryRatio));
}

// Tests that 'SyntheticNetwork' can generate a high throughput of 5,000,000
// requests/sec with no Range queries.
TEST(SyntheticNetworkTest, HighThroughput) {
  // The duration to generate requests.
  constexpr absl::Duration kGenerationDuration = absl::Seconds(10);
  constexpr double kNoRangeQueryRatio = 0.0;
  SimulatedClock clock;
  SyntheticNetwork network(kHighSyntheticNetworkThroughput, kNoRangeQueryRatio,
                           clock);

  clock.SetTime(ghost::MonotonicNow());
  network.Start();
  clock.AdvanceTime(kGenerationDuration);

  std::deque<Request> requests;
  Request request;

  // When `network.Poll` returns false, we have dequeued all pending requests in
  // the synthetic network.
  while (network.Poll(request)) {
    requests.push_back(request);
  }

  EXPECT_THAT(IsInAscendingOrder(RequestsToTimes(requests)), IsTrue());
  EXPECT_THAT(IsWithin(CalculateThroughput(RequestsToTimes(requests)),
                       kHighSyntheticNetworkThroughput, kErrorEpsilon),
              IsTrue());
  // Since there are no range queries, the range query ratio must be exactly
  // equal to 'kNoRangeQueryRatio', which is 0.0.
  EXPECT_THAT(CalculateRangeQueryRatio(requests), Eq(kNoRangeQueryRatio));
}

// Tests that 'SyntheticNetwork' can generate a medium throughput of 500,000
// requests/sec with 0.5% of requests as Range queries.
TEST(SyntheticNetworkTest, MediumThroughputLowRangeQueries) {
  // The duration to generate requests. We choose a longer time for this test
  // than other tests since the Bernoulli process seems to vary more widely
  // percentage-wise when 'p' is low.
  constexpr absl::Duration kGenerationDuration = absl::Seconds(30);
  constexpr double kLowRangeQueryRatio = 0.005;
  SimulatedClock clock;
  SyntheticNetwork network(kMediumThroughput, kLowRangeQueryRatio, clock);

  clock.SetTime(ghost::MonotonicNow());
  network.Start();
  clock.AdvanceTime(kGenerationDuration);

  std::deque<Request> requests;
  Request request;

  // When `network.Poll` returns false, we have dequeued all pending requests in
  // the synthetic network.
  while (network.Poll(request)) {
    requests.push_back(request);
  }

  EXPECT_THAT(IsInAscendingOrder(RequestsToTimes(requests)), IsTrue());
  EXPECT_THAT(IsWithin(CalculateThroughput(RequestsToTimes(requests)),
                       kMediumThroughput, kErrorEpsilon),
              IsTrue());
  EXPECT_THAT(IsWithin(CalculateRangeQueryRatio(requests), kLowRangeQueryRatio,
                       kErrorEpsilon),
              IsTrue());
}

// Tests that 'SyntheticNetwork' can generate a medium throughput of 500,000
// requests/sec with 75% of requests as Range queries.
TEST(SyntheticNetworkTest, MediumThroughputHighRangeQueries) {
  // The duration to generate requests.
  constexpr absl::Duration kGenerationDuration = absl::Seconds(10);
  constexpr double kHighRangeQueryRatio = 0.75;
  SimulatedClock clock;
  SyntheticNetwork network(kMediumThroughput, kHighRangeQueryRatio, clock);

  clock.SetTime(ghost::MonotonicNow());
  network.Start();
  clock.AdvanceTime(kGenerationDuration);

  std::deque<Request> requests;
  Request request;

  // When `network.Poll` returns false, we have dequeued all pending requests in
  // the synthetic network.
  while (network.Poll(request)) {
    requests.push_back(request);
  }

  EXPECT_THAT(IsInAscendingOrder(RequestsToTimes(requests)), IsTrue());
  EXPECT_THAT(IsWithin(CalculateThroughput(RequestsToTimes(requests)),
                       kMediumThroughput, kErrorEpsilon),
              IsTrue());
  EXPECT_THAT(IsWithin(CalculateRangeQueryRatio(requests), kHighRangeQueryRatio,
                       kErrorEpsilon),
              IsTrue());
}

}  // namespace
}  // namespace ghost_test
