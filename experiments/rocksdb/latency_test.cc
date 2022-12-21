// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "experiments/rocksdb/latency.h"

#include <algorithm>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "experiments/rocksdb/request.h"

// These tests check that 'latency::Print' prints the expected results.

namespace ghost_test {
namespace {

using ::testing::Eq;

// Removes all of the spaces (but not other whitespace, such as newlines) from
// 'str' and returns the result. We use this to check the "pretty" results; the
// "pretty" results add spaces to make the results easy for a human to read, so
// we strip the spaces from the output in order to make it easier for the tests
// to check the correctness of the results.
std::string RemoveSpaces(std::string str) {
  str.erase(std::remove(str.begin(), str.end(), ' '), str.end());
  return str;
}

// Returns the pretty preface printed by the 'latency::Print' method. If
// printing in units of nanoseconds, set 'ns' to true. If printing in units of
// microseconds, set 'ns' to false.
std::string GetPrettyPreface(bool ns) {
  std::string preface;
  if (ns) {
    preface =
        "Stage Total Requests Throughput (req/s) Min (ns) 50% (ns) 99% (ns) "
        "99.5% (ns) 99.9% (ns) Max (ns)\n";
  } else {
    preface =
        "Stage Total Requests Throughput (req/s) Min (us) 50% (us) 99% (us) "
        "99.5% (us) 99.9% (us) Max (us)\n";
  }
  absl::StrAppend(&preface, std::string(latency::kNumDashes, '-'), "\n");
  return preface;
}

// Tests that 'latency::Print' properly prints an empty results set when the
// pretty print option is set.
TEST(LatencyTest, EmptyResultsPretty) {
  std::vector<Request> requests;
  std::ostringstream actual;
  latency::PrintOptions options = {
      .pretty = true, .distribution = false, .ns = false, .os = &actual};
  latency::Print(requests, absl::Seconds(4), options);

  std::string expected = absl::StrCat(GetPrettyPreface(/*ns=*/false),
                                      R"(Ingress Queue Time - - - - - - - -
Repeatable Handle Time - - - - - - - -
Worker Queue Time - - - - - - - -
Worker Handle Time - - - - - - - -
Total - - - - - - - -
)");

  EXPECT_THAT(RemoveSpaces(actual.str()), Eq(RemoveSpaces(expected)));
}

// Tests that 'latency::Print' properly prints an empty results set when the CSV
// print option is set.
TEST(LatencyTest, EmptyResultsCsv) {
  std::vector<Request> requests;
  std::ostringstream actual;
  latency::PrintOptions options = {
      .pretty = false, .distribution = false, .ns = false, .os = &actual};
  latency::Print(requests, absl::Seconds(4), options);

  std::string expected = R"(-,-,-,-,-,-,-,-
-,-,-,-,-,-,-,-
-,-,-,-,-,-,-,-
-,-,-,-,-,-,-,-
-,-,-,-,-,-,-,-
)";

  EXPECT_THAT(RemoveSpaces(actual.str()), Eq(RemoveSpaces(expected)));
}

// Returns a randomly sorted vector of 1,000 requests. Each stage has the
// following latency percentiles:
// Min: 1 us (= 1000 ns)
// 50%: 500 us (= 500000 ns)
// 99%: 990 us (= 990000 ns)
// 99.5%: 995 us (= 995000 ns)
// 99.9%: 999 us (= 999000 ns)
// Max: 1000 us (= 1000000 ns)
std::vector<Request> GetData(std::default_random_engine& random_engine) {
  constexpr size_t kNumRequests = 1000;
  std::vector<Request> requests;
  requests.reserve(kNumRequests);
  absl::Time now = ghost::MonotonicNow();
  for (size_t i = 1; i <= kNumRequests; i++) {
    Request r;
    r.request_generated = now + 1 * absl::Microseconds(i);
    r.request_received = now + 2 * absl::Microseconds(i);
    r.request_assigned = now + 3 * absl::Microseconds(i);
    r.request_start = now + 4 * absl::Microseconds(i);
    r.request_finished = now + 5 * absl::Microseconds(i);
    requests.push_back(r);
  }
  // Randomly shuffle the vector of requests so that we know the latency methods
  // are properly sorting the latencies by percentile.
  std::shuffle(requests.begin(), requests.end(), random_engine);
  return requests;
}

// Tests that 'latency::Print' properly prints a standard, randomly-ordered
// results set when the pretty print option is set.
TEST(LatencyTest, StandardResultsPretty) {
  std::ostringstream actual;
  latency::PrintOptions options = {
      .pretty = true, .distribution = false, .ns = false, .os = &actual};
  std::random_device random_device;
  std::default_random_engine random_engine(random_device());
  latency::Print(GetData(random_engine), absl::Seconds(4), options);

  std::string expected =
      absl::StrCat(GetPrettyPreface(/*ns=*/false),
                   R"(Ingress Queue Time 1000 250 1 500 990 995 999 1000
Repeatable Handle Time 1000 250 1 500 990 995 999 1000
Worker Queue Time 1000 250 1 500 990 995 999 1000
Worker Handle Time 1000 250 1 500 990 995 999 1000
Total 1000 250 4 2000 3960 3980 3996 4000
)");

  EXPECT_THAT(RemoveSpaces(actual.str()), Eq(RemoveSpaces(expected)));
}

// Tests that 'latency::Print' properly prints a standard, randomly-ordered
// results set when the CSV print option is set.
TEST(LatencyTest, StandardResultsCsv) {
  std::ostringstream actual;
  latency::PrintOptions options = {
      .pretty = false, .distribution = false, .ns = false, .os = &actual};
  absl::Duration runtime = absl::Seconds(4);
  std::random_device random_device;
  std::default_random_engine random_engine(random_device());
  latency::Print(GetData(random_engine), runtime, options);

  std::string expected = R"(1000,250,1,500,990,995,999,1000
1000,250,1,500,990,995,999,1000
1000,250,1,500,990,995,999,1000
1000,250,1,500,990,995,999,1000
1000,250,4,2000,3960,3980,3996,4000
)";

  EXPECT_THAT(RemoveSpaces(actual.str()), Eq(RemoveSpaces(expected)));
}

// Tests that 'latency::Print' properly prints a standard, randomly-ordered
// results set when the pretty print option and the nanosecond print option is
// set.
TEST(LatencyTest, StandardResultsPrettyNs) {
  std::ostringstream actual;
  latency::PrintOptions options = {
      .pretty = true, .distribution = false, .ns = true, .os = &actual};
  std::random_device random_device;
  std::default_random_engine random_engine(random_device());
  latency::Print(GetData(random_engine), absl::Seconds(4), options);

  std::string expected = absl::StrCat(
      GetPrettyPreface(/*ns=*/true),
      R"(Ingress Queue Time 1000 250 1000 500000 990000 995000 999000 1000000
Repeatable Handle Time 1000 250 1000 500000 990000 995000 999000 1000000
Worker Queue Time 1000 250 1000 500000 990000 995000 999000 1000000
Worker Handle Time 1000 250 1000 500000 990000 995000 999000 1000000
Total 1000 250 4000 2000000 3960000 3980000 3996000 4000000
)");

  EXPECT_THAT(RemoveSpaces(actual.str()), Eq(RemoveSpaces(expected)));
}

// Tests that 'latency::Print' properly prints a standard, randomly-ordered
// results set when the CSV print option and the nanosecond print option is set.
TEST(LatencyTest, StandardResultsCsvNs) {
  std::ostringstream actual;
  latency::PrintOptions options = {
      .pretty = false, .distribution = false, .ns = true, .os = &actual};
  absl::Duration runtime = absl::Seconds(4);
  std::random_device random_device;
  std::default_random_engine random_engine(random_device());
  latency::Print(GetData(random_engine), runtime, options);

  std::string expected = R"(1000,250,1000,500000,990000,995000,999000,1000000
1000,250,1000,500000,990000,995000,999000,1000000
1000,250,1000,500000,990000,995000,999000,1000000
1000,250,1000,500000,990000,995000,999000,1000000
1000,250,4000,2000000,3960000,3980000,3996000,4000000
)";

  EXPECT_THAT(RemoveSpaces(actual.str()), Eq(RemoveSpaces(expected)));
}

}  // namespace
}  // namespace ghost_test
