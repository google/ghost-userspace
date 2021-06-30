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

#include "experiments/antagonist/orchestrator.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/functional/bind_front.h"

// These tests check that the orchestrator can soak the machine.

namespace ghost_test {
namespace {

using ::testing::IsTrue;
using ::testing::Values;

// This class fills in the pure virtual methods from 'Orchestrator' so that we
// can instantiate an orchestrator and test it.
//
// Example:
// Orchestrator::Options options;
// ... Fill in 'options'.
// SimpleOrchestrator orchestrator(options);
// ... Test 'orchestrator'.
class SimpleOrchestrator : public Orchestrator {
 public:
  explicit SimpleOrchestrator(Orchestrator::Options options)
      : Orchestrator(options) {
    // 'Orchestrator::Soak(sid)' calls methods on the thread pool under the
    // assumption that a thread with SID 'sid' exists in the thread pool. Add a
    // thread to the thread pool and have it sleep until the test ends so that
    // (1) the test does not crash since the thread pool has been initialized
    // properly and (2) the thread managed by the thread pool does not interfere
    // with the thread calling 'Orchestrator::Soak'.
    thread_pool().Init({ghost::GhostThread::KernelScheduler::kCfs},
                       {absl::bind_front(&SimpleOrchestrator::Worker, this)});
  }

  ~SimpleOrchestrator() final {
    thread_pool().MarkExit(/*sid=*/0);
    notification_.Notify();
    thread_pool().Join();
  }

 protected:
  void Worker(uint32_t sid) final { notification_.WaitForNotification(); }

 private:
  // The worker sleeps on this notification until the test ends.
  ghost::Notification notification_;
};

class OrchestratorTest : public ::testing::TestWithParam<double> {
 public:
  void Run() {
    const double work_share = GetParam();
    CHECK_GE(work_share, 0.0);
    CHECK_LE(work_share, 1.0);

    Orchestrator::Options options = GetOptions(work_share);
    SimpleOrchestrator orchestrator(options);

    absl::Time start = absl::Now();
    absl::Duration start_usage = Orchestrator::ThreadUsage();
    while (absl::Now() - start < options.experiment_duration) {
      // This is the only thread running, so its SID (sched item identifier) is
      // 0.
      orchestrator.Soak(/*sid=*/0);
    }
    absl::Duration usage = Orchestrator::ThreadUsage() - start_usage;
    absl::Duration expected_usage = options.experiment_duration * work_share;

    // Checks that the CPU time consumption reported by the orchestrator matches
    // the CPU time consumption we measured in the test.
    EXPECT_THAT(IsWithin(usage, orchestrator.run_duration()[0]), IsTrue());
    // Checks that the CPU time consumption reported by the orchestrator matches
    // the expected CPU time consumption.
    EXPECT_THAT(IsWithin(orchestrator.run_duration()[0], expected_usage),
                IsTrue());
  }

 private:
  // Returns true if `actual` is greater than or equal to `expected` * 0.995 and
  // less than or equal to `expected` * 1.005.
  //
  // For example, if:
  //   `actual` = absl::Microseconds(99.7);
  //   `expected` = absl::Microseconds(100);
  // This method will return true because 99.5 <= 99.7 <= 100.5.
  bool IsWithin(absl::Duration actual, absl::Duration expected) {
    constexpr double kEpsilon = 0.005;

    const absl::Duration lower_bound = expected * (1.0 - kEpsilon);
    const absl::Duration upper_bound = expected * (1.0 + kEpsilon);
    return actual >= lower_bound && actual <= upper_bound;
  }

  // Returns orchestrator options suitable for the tests. Sets the work share to
  // 'work_share'.
  Orchestrator::Options GetOptions(double work_share) {
    Orchestrator::Options options;

    options.print_options.pretty = true;
    options.work_share = work_share;
    options.num_threads = 1;
    options.cpus = {1};
    options.experiment_duration = absl::Seconds(15);
    options.scheduler = ghost::GhostThread::KernelScheduler::kCfs;
    options.ghost_qos = 2;

    return options;
  }
};

INSTANTIATE_TEST_SUITE_P(
    OrchestratorTestGroup, OrchestratorTest,
    // Each value is a target work share that is tested on one CPU. For example,
    // 0.1 means that the test tries to consume 10% of one CPU.
    Values(
        // We have trouble soaking less than ~10% of the CPU because there is
        // overhead to getting the current time, branching, etc.
        0.1, 0.25,
        // We should only try larger work shares on dedicated machines. On
        // Forge, it is likely that our tests are co-located with other tests,
        // so our tests will not have exclusive access to the machine. Thus,
        // tests with larger work shares may fail on Forge. On dedicated
        // machines, our tests have exclusive access, so the tests with larger
        // work shares should succeed.
        0.9, 1.0));

TEST_P(OrchestratorTest, OrchestratorTests) { Run(); }

}  // namespace
}  // namespace ghost_test
