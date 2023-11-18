// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "experiments/rocksdb/orchestrator.h"

// These tests check that the application prints options and parses command line
// flags properly.

namespace ghost_test {
namespace {

using ::testing::Eq;

// Returns orchestrator options suitable for the tests.
Options GetOptions() {
  Options options;

  options.print_options.pretty = true;
  options.print_options.distribution = false;
  options.print_options.ns = false;
  options.print_options.os = &std::cout;
  options.print_get = true;
  options.print_range = false;
  options.rocksdb_db_path = "/tmp/orch_db";
  options.throughput = 20'000.0;
  options.range_query_ratio = 0.005;
  options.load_generator_cpus =
      ghost::MachineTopology()->ToCpuList(std::vector<int>{1});
  options.cfs_dispatcher_cpus =
      ghost::MachineTopology()->ToCpuList(std::vector<int>{2});
  options.num_workers = 2;
  options.cfs_wait_type = ThreadWait::WaitType::kSpin;
  options.worker_cpus =
      ghost::MachineTopology()->ToCpuList(std::vector<int>{3, 4});
  options.ghost_wait_type = GhostWaitType::kFutex;
  options.get_duration = absl::Microseconds(10);
  options.range_duration = absl::Milliseconds(5);
  options.get_exponential_mean = absl::ZeroDuration();
  options.batch = 1;
  options.experiment_duration = absl::Seconds(15);
  options.discard_duration = absl::Seconds(2);
  options.scheduler = ghost::GhostThread::KernelScheduler::kCfs;
  options.ghost_qos = 2;

  return options;
}

// The '<<' operator for 'Options' should print all options and
// their values in alphabetical order by option name.
std::string GetExpectedOutput() {
  return R"(batch: 1
cfs_dispatcher_cpus: 2
cfs_wait_type: spin
discard_duration: 2s
experiment_duration: 15s
get_duration: 10us
get_exponential_mean: 0
ghost_qos: 2
ghost_wait_type: futex
load_generator_cpus: 1
num_workers: 2
print_distribution: false
print_format: pretty
print_get: true
print_ns: false
print_range: false
range_duration: 5ms
range_query_ratio: 0.005000
rocksdb_db_path: /tmp/orch_db
scheduler: cfs
throughput: 20000.000000
worker_cpus: 3 4)";
}

// This tests that the '<<' operator prints all options and their values in
// alphabetical order by option name.
TEST(OptionsTest, PrintOptions) {
  Options options = GetOptions();
  std::ostringstream os;

  os << options;
  EXPECT_THAT(os.str(), Eq(GetExpectedOutput()));
}

}  // namespace
}  // namespace ghost_test
