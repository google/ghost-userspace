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

#include "experiments/antagonist/results.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/strings/str_cat.h"

// These tests check that 'Print' prints the expected results.

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

// Returns the pretty preface printed by the 'Print' method.
std::string GetPrettyPreface() {
  constexpr size_t kNumDashes = 42;
  return absl::StrCat("Worker Run Duration (ns) Work Share\n",
                      std::string(kNumDashes, '-'), "\n");
}

// Tests that 'Print' properly prints an empty results set when the pretty print
// option is set.
TEST(LatencyTest, EmptyResultsPretty) {
  constexpr size_t kNumWorkers = 8;
  constexpr absl::Duration kDuration = absl::Seconds(4);
  std::vector<absl::Duration> run_durations(kNumWorkers);

  std::ostringstream actual;
  PrintOptions options = {.pretty = true, .os = &actual};
  Print(run_durations, kDuration, options);

  std::string expected = absl::StrCat(GetPrettyPreface(), R"(0 0 0
1 0 0
2 0 0
3 0 0
4 0 0
5 0 0
6 0 0
7 0 0
Total 0 0
)");

  EXPECT_THAT(RemoveSpaces(actual.str()), Eq(RemoveSpaces(expected)));
}

// Tests that 'Print' properly prints an empty results set when the CSV print
// option is set.
TEST(LatencyTest, EmptyResultsCsv) {
  constexpr size_t kNumWorkers = 8;
  constexpr absl::Duration kDuration = absl::Seconds(4);
  std::vector<absl::Duration> run_durations(kNumWorkers);

  std::ostringstream actual;
  PrintOptions options = {.pretty = false, .os = &actual};
  Print(run_durations, kDuration, options);

  std::string expected = R"(0,0,0
1,0,0
2,0,0
3,0,0
4,0,0
5,0,0
6,0,0
7,0,0
Total,0,0
)";

  EXPECT_THAT(actual.str(), Eq(expected));
}

// If 'num_workers' = 8, 'unit' = 100 milliseconds, and the total runtime is 4
// seconds, then:
//
// Worker 'i' has a run duration of 'i' * kUnit = 'i' * 100 milliseconds. Thus,
// worker 'i' has a work share of 'i' * 100 / 4,000 = 'i' * 0.025.
//
// The total run duration is 0 + 100 + 200 + ... + 700 milliseconds = 2,800
// milliseconds. Thus, the total work share is 2,800 / (4,000 * 8) = 0.0875.
std::vector<absl::Duration> GetData(size_t num_workers, absl::Duration unit) {
  std::vector<absl::Duration> run_durations;

  for (size_t i = 0; i < num_workers; i++) {
    run_durations.push_back(i * unit);
  }
  return run_durations;
}

// Tests that 'Print' properly prints a standard results set when the pretty
// print option is set.
TEST(LatencyTest, StandardResultsPretty) {
  constexpr size_t kNumWorkers = 8;
  constexpr absl::Duration kUnit = absl::Milliseconds(100);
  constexpr absl::Duration kDuration = absl::Seconds(4);
  std::vector<absl::Duration> run_durations = GetData(kNumWorkers, kUnit);

  std::ostringstream actual;
  PrintOptions options = {.pretty = true, .os = &actual};
  Print(run_durations, kDuration, options);

  std::string expected = absl::StrCat(GetPrettyPreface(), R"(0 0 0
1 100000000 0.025
2 200000000 0.05
3 300000000 0.075
4 400000000 0.1
5 500000000 0.125
6 600000000 0.15
7 700000000 0.175
Total 2800000000 0.0875
)");

  EXPECT_THAT(RemoveSpaces(actual.str()), Eq(RemoveSpaces(expected)));
}

// Tests that 'Print' properly prints a standard results set when the CSV print
// option is set.
TEST(LatencyTest, StandardResultsCsv) {
  constexpr size_t kNumWorkers = 8;
  constexpr absl::Duration kUnit = absl::Milliseconds(100);
  constexpr absl::Duration kDuration = absl::Seconds(4);
  std::vector<absl::Duration> run_durations = GetData(kNumWorkers, kUnit);

  std::ostringstream actual;
  PrintOptions options = {.pretty = false, .os = &actual};
  Print(run_durations, kDuration, options);

  std::string expected = R"(0,0,0
1,100000000,0.025
2,200000000,0.05
3,300000000,0.075
4,400000000,0.1
5,500000000,0.125
6,600000000,0.15
7,700000000,0.175
Total,2800000000,0.0875
)";

  EXPECT_THAT(actual.str(), Eq(expected));
}

}  // namespace
}  // namespace ghost_test
