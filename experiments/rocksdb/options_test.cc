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
#include "experiments/rocksdb/orchestrator.h"

// These tests check that the application prints options and parses command line
// flags properly.

namespace ghost_test {
namespace {

using ::testing::Eq;

// Returns orchestrator options suitable for the tests.
Orchestrator::Options GetOptions() {
  Orchestrator::Options options;

  options.print_options.pretty = true;
  options.print_options.distribution = false;
  options.print_options.ns = false;
  options.print_options.os = &std::cout;
  options.print_get = true;
  options.print_range = false;
  options.rocksdb_db_path = "/tmp/orch_db";
  options.throughput = 20'000.0;
  options.range_query_ratio = 0.005;
  options.load_generator_cpu = 1;
  options.cfs_dispatcher_cpu = 2;
  options.num_workers = 2;
  options.worker_cpus = {3, 4};
  options.cfs_wait_type = CompletelyFairScheduler::WaitType::kWaitSpin;
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

// The '<<' operator for 'Orchestrator::Options' should print all options and
// their values in alphabetical order by option name.
std::string GetExpectedOutput() {
  return R"(batch: 1
cfs_dispatcher_cpu: 2
cfs_wait_type: spin
discard_duration: 2s
experiment_duration: 15s
get_duration: 10us
get_exponential_mean: 0
ghost_qos: 2
load_generator_cpu: 1
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
  Orchestrator::Options options = GetOptions();
  std::ostringstream os;

  os << options;
  EXPECT_THAT(os.str(), Eq(GetExpectedOutput()));
}

}  // namespace
}  // namespace ghost_test
