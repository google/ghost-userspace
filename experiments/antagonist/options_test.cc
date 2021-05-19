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

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "experiments/antagonist/orchestrator.h"

// These tests check that the application prints options and parses command line
// flags properly.

namespace ghost_test {
namespace {

using ::testing::Eq;

// Returns orchestrator options suitable for the tests.
Orchestrator::Options GetOptions() {
  Orchestrator::Options options;

  options.print_options.pretty = true;
  options.work_share = 0.9;
  options.num_threads = 4;
  options.cpus = {1, 2, 3, 4};
  options.experiment_duration = absl::Seconds(15);
  options.scheduler = ghost::GhostThread::KernelScheduler::kCfs;
  options.ghost_qos = 2;

  return options;
}

// This tests that the '<<' operator prints all options and their values in
// alphabetical order by option name.
TEST(OptionsTest, PrintOptions) {
  Orchestrator::Options options = GetOptions();
  std::ostringstream os;

  os << options;
  std::string expected = R"(cpus: 1 2 3 4
experiment_duration: 15s
ghost_qos: 2
num_threads: 4
print_format: pretty
scheduler: cfs
work_share: 0.9)";
  EXPECT_THAT(os.str(), Eq(expected));
}

}  // namespace
}  // namespace ghost_test
