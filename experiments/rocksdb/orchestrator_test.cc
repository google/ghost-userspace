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

#include "experiments/rocksdb/orchestrator.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "experiments/rocksdb/ingress.h"

// These tests check that the orchestrator can process requests.

namespace ghost_test {
namespace {

using ::testing::Ge;
using ::testing::IsTrue;

// This class fills in the pure virtual methods from 'Orchestrator' so that we
// can instantiate an orchestrator and test it.
//
// Example:
// Orchestrator::Options options;
// ... Fill in 'options'.
// TestOrchestrator orchestrator(options);
// ... Test 'orchestrator'.
class TestOrchestrator : public Orchestrator {
 public:
  explicit TestOrchestrator(Orchestrator::Options options)
      : Orchestrator(options, /*total_threads=*/0) {}
  ~TestOrchestrator() final {}

  void Terminate() final {}

  void Handle(Request& request) {
    absl::BitGen gen;
    HandleRequest(request, gen);
  }

 protected:
  void LoadGenerator(uint32_t sid) final {}
  void Dispatcher(uint32_t sid) final {}
  void Worker(uint32_t sid) final {}
};

// The actual thread CPU time per request must be within this amount of time of
// the target time.
constexpr absl::Duration kErrorRange = absl::Microseconds(2);
// The amount of time a Get request takes.
constexpr absl::Duration kGetRequestDuration = absl::Microseconds(10);
// The amount of time a Range Query takes.
constexpr absl::Duration kRangeQueryDuration = absl::Milliseconds(5);

// Returns true if 0 <= 'actual' - 'expected' <= 'bound'. Note that 'actual'
// should be at least 'expected' because 'expected' is the target time. 'actual'
// may be slightly larger than 'expected' due to request processing overhead
// (e.g., checking for loop conditions, handling stack frames, etc.).
bool IsWithin(absl::Duration actual, absl::Duration expected,
              absl::Duration bound) {
  absl::Duration difference = actual - expected;
  return difference >= absl::ZeroDuration() && difference <= bound;
}

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
  // The background threads run on CPU 0, so run the load generator on CPU 1.
  options.load_generator_cpu = 1;
  options.cfs_dispatcher_cpu = 2;
  options.num_workers = 2;
  options.worker_cpus = {3, 4};
  options.cfs_wait_type = CompletelyFairScheduler::WaitType::kWaitSpin;
  options.get_duration = kGetRequestDuration;
  options.range_duration = kRangeQueryDuration;
  options.get_exponential_mean = absl::ZeroDuration();
  options.batch = 1;
  options.experiment_duration = absl::Seconds(15);
  options.discard_duration = absl::Seconds(2);
  options.scheduler = ghost::GhostThread::KernelScheduler::kCfs;
  options.ghost_qos = 2;

  return options;
}

// Returns the thread's CPU time.
absl::Duration GetThreadCpuTime() {
  timespec ts;
  CHECK_EQ(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts), 0);
  return absl::Seconds(ts.tv_sec) + absl::Nanoseconds(ts.tv_nsec);
}

// This tests that the orchestrator can process a Get request and that the
// processing has a duration of 'kGetRequestDuration'.
TEST(OrchestratorTest, GetRequest) {
  TestOrchestrator orchestrator(GetOptions());
  std::vector<absl::Duration> handle_durations;

  absl::BitGen gen;

  for (int i = 0; i < 1000; i++) {
    Request get;
    get.work = Request::Get{
        .entry = absl::Uniform<uint32_t>(gen, 0, Database::kNumEntries)};

    absl::Duration start_cpu_time = GetThreadCpuTime();
    orchestrator.Handle(get);
    handle_durations.push_back(GetThreadCpuTime() - start_cpu_time);
  }

  std::sort(handle_durations.begin(), handle_durations.end());
  EXPECT_THAT(handle_durations[0], Ge(kGetRequestDuration));

  // There may be interrupts, locks, stack frames, etc. that the thread handles,
  // so check that the median Get request handle time is close to
  // 'kGetRequestDuration'.
  absl::Duration median = handle_durations[handle_durations.size() * 0.5];
  EXPECT_THAT(IsWithin(median, kGetRequestDuration, kErrorRange), IsTrue());
}

// This tests that the orchestrator can process a Range Query and that the
// processing has a duration of 'kRangeQueryDuration'.
TEST(OrchestratorTest, RangeQuery) {
  TestOrchestrator orchestrator(GetOptions());
  std::vector<absl::Duration> handle_durations;

  absl::BitGen gen;

  for (int i = 0; i < 1000; i++) {
    Request range;
    range.work = Request::Range{
        .start_entry = absl::Uniform<uint32_t>(
            gen, 0,
            Database::kNumEntries - SyntheticNetwork::kRangeQuerySize + 1),
        .size = SyntheticNetwork::kRangeQuerySize};

    absl::Duration start_cpu_time = GetThreadCpuTime();
    orchestrator.Handle(range);
    handle_durations.push_back(GetThreadCpuTime() - start_cpu_time);
  }

  std::sort(handle_durations.begin(), handle_durations.end());
  EXPECT_THAT(handle_durations[0], Ge(kRangeQueryDuration));

  // There may be interrupts, locks, stack frames, etc. that the thread handles,
  // so check that the median Range Query handle time is close to
  // 'kGetRequestDuration'.
  absl::Duration median = handle_durations[handle_durations.size() * 0.5];
  EXPECT_THAT(IsWithin(median, kRangeQueryDuration, kErrorRange), IsTrue());
}

}  // namespace
}  // namespace ghost_test
