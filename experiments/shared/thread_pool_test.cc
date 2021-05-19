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

#include "experiments/shared/thread_pool.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/functional/bind_front.h"
#include "absl/synchronization/barrier.h"
#include "lib/base.h"

// These tests check that the thread pool properly launches threads and reports
// their GTIDs.

namespace ghost_test {
namespace {

using ::testing::Eq;
using ::testing::Ge;
using ::testing::IsFalse;
using ::testing::IsTrue;
using ::testing::Lt;
using ::testing::TestWithParam;
using ::testing::Values;

// Test class to test that the thread pool launches various numbers of threads.
class LaunchTest : public TestWithParam<uint32_t> {
 public:
  // Creates a thread pool that launches `GetParam()` threads and checks that
  // each thread runs and decrements `start_barrier_`. Once every thread has
  // decremented `start_barrier_`, the thread pool is joined.
  void Run() {
    const uint32_t num_threads = GetParam();
    start_barrier_ = std::make_unique<absl::Barrier>(num_threads + 1);

    std::vector<ghost::GhostThread::KernelScheduler> kernel_schedulers(
        num_threads, ghost::GhostThread::KernelScheduler::kCfs);
    std::vector<std::function<void(uint32_t)>> thread_work(
        num_threads, absl::bind_front(&LaunchTest::ThreadBody, this));

    ExperimentThreadPool thread_pool(num_threads);
    thread_pool.Init(kernel_schedulers, thread_work);
    EXPECT_THAT(thread_pool.NumThreads(), Eq(num_threads));

    start_barrier_->Block();
    for (uint32_t i = 0; i < num_threads; i++) {
      thread_pool.MarkExit(i);
    }
    exit_notification_.Notify();
    while (thread_pool.NumExited() < num_threads) {
    }
    thread_pool.Join();
  }

 private:
  // This is the thread body the thread pool threads.
  void ThreadBody(uint32_t sid) {
    start_barrier_->Block();
    exit_notification_.WaitForNotification();
  }

  // This barrier is decremented by each thread pool thread that runs. The main
  // test method waits for the barrier to be decremented to 0 before joining
  // threads. This ensures that all threads have truly run.
  std::unique_ptr<absl::Barrier> start_barrier_;
  // All thread pool threads wait on this notification before exiting. This
  // ensures that no thread pool thread executes the `ThreadBody` method
  // multiple times.
  ghost::Notification exit_notification_;
};

// Tests that the thread pool launches various numbers of threads. The maximum
// number of threads tested is 1,000 threads to stress test the thread pool.
INSTANTIATE_TEST_SUITE_P(LaunchTestGroup, LaunchTest,
                         Values(1, 10, 100, 1'000));

TEST_P(LaunchTest, Launch) { Run(); }

// Test class to test that the thread pool correctly returns GTIDs for various
// numbers of threads.
class GtidTest : public TestWithParam<uint32_t> {
 public:
  // Creates a thread pool that launches `GetParam()` threads and checks that
  // each thread runs and writes its GTID to a vector. Once every thread has
  // written its GTID, the GTIDs that the thread pool itself reports are checked
  // and then the thread pool is joined.
  void Run() {
    const uint32_t num_threads = GetParam();
    barrier_ = std::make_unique<absl::Barrier>(num_threads + 1);
    std::vector<ghost::GhostThread::KernelScheduler> kernel_schedulers(
        num_threads, ghost::GhostThread::KernelScheduler::kCfs);
    std::vector<std::function<void(uint32_t)>> thread_work(
        num_threads, absl::bind_front(&GtidTest::ThreadBody, this));
    gtids_.resize(num_threads);

    ExperimentThreadPool thread_pool(num_threads);
    thread_pool.Init(kernel_schedulers, thread_work);
    EXPECT_THAT(thread_pool.NumThreads(), Eq(num_threads));

    barrier_->Block();
    // `GetGtids` returns a vector of GTIDs such that the GTID for the thread
    // with SID `i` is in index `i` of the vector. Thus, the two vectors should
    // match below without sorting.
    std::vector<ghost::Gtid> thread_pool_gtids = thread_pool.GetGtids();
    ASSERT_THAT(gtids_.size(), Eq(thread_pool_gtids.size()));
    for (int i = 0; i < gtids_.size(); i++) {
      EXPECT_THAT(gtids_[i], Eq(thread_pool_gtids[i]));
    }
    for (uint32_t i = 0; i < num_threads; i++) {
      thread_pool.MarkExit(i);
    }
    while (thread_pool.NumExited() < num_threads) {
    }
    thread_pool.Join();
  }

 private:
  // This is the thread body for the GTID tests. The thread writes its GTID at
  // index `sid` in `gtids_`.
  void ThreadBody(uint32_t sid) {
    ASSERT_THAT(sid, Ge(0));
    ASSERT_THAT(sid, Lt(gtids_.size()));

    // This thread body is called in a loop. Make sure we only fill in the GTID
    // once.
    if (gtids_[sid] == ghost::Gtid()) {
      gtids_[sid] = ghost::Gtid::Current();
      barrier_->Block();
    }
  }

  // This barrier is decremented by each thread that runs. The main test method
  // waits for the barrier to be decremented to 0 before checking the GTIDs in
  // `gtids_`.
  std::unique_ptr<absl::Barrier> barrier_;
  // The GTID at index `i` is filled in by the thread with SID `i` with its own
  // GTID.
  std::vector<ghost::Gtid> gtids_;
};

// Tests that the thread pool correctly returns GTIDs for various numbers of
// threads. The maximum number of threads tested is 1,000 threads to stress test
// the thread pool.
INSTANTIATE_TEST_SUITE_P(GtidTestGroup, GtidTest, Values(1, 10, 100, 1'000));

TEST_P(GtidTest, Gtid) { Run(); }

// Tests that the `ThreadTrigger` class properly triggers threads and waits for
// triggers.
TEST(ThreadPoolTest, Trigger) {
  ThreadTrigger trigger(/*num_threads=*/1);
  EXPECT_THAT(trigger.Triggered(/*sid=*/0), IsFalse());

  ghost::GhostThread thread(
      ghost::GhostThread::KernelScheduler::kCfs, [&trigger]() {
        trigger.WaitForTrigger(/*sid=*/0);
        // This thread is now triggered. There should be no spurious wakeups.
        EXPECT_THAT(trigger.Triggered(/*sid=*/0), IsTrue());
      });
  EXPECT_THAT(trigger.Trigger(/*sid=*/0), IsTrue());
  // Any subsequent trigger should fail.
  EXPECT_THAT(trigger.Trigger(/*sid=*/0), IsFalse());

  thread.Join();
}

}  // namespace
}  // namespace ghost_test
