// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

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
  options.cpus =
      ghost::MachineTopology()->ToCpuList(std::vector<int>{1, 2, 3, 4});
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
