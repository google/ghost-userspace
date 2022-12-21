// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

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
// Options options;
// ... Fill in 'options'.
// TestOrchestrator orchestrator(options);
// ... Test 'orchestrator'.
class TestOrchestrator : public Orchestrator {
 public:
  explicit TestOrchestrator(Options options)
      : Orchestrator(options, /*total_threads=*/0) {}
  ~TestOrchestrator() final {}

  void Terminate() final {}

  void Handle(Request& request) {
    static thread_local std::string response;
    absl::BitGen gen;
    HandleRequest(request, response, gen);
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
  // The background threads run on CPU 0, so run the load generator on CPU 1.
  options.load_generator_cpus =
      ghost::MachineTopology()->ToCpuList(std::vector<int>{1});
  options.cfs_dispatcher_cpus =
      ghost::MachineTopology()->ToCpuList(std::vector<int>{2});
  options.num_workers = 2;
  options.cfs_wait_type = ThreadWait::WaitType::kSpin;
  options.worker_cpus =
      ghost::MachineTopology()->ToCpuList(std::vector<int>{3, 4});
  options.ghost_wait_type = GhostWaitType::kFutex;
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
