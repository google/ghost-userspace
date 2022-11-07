// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "experiments/shared/thread_pool.h"

namespace ghost_test {

ExperimentThreadPool::~ExperimentThreadPool() {
  // Check that all threads have been joined.
  CHECK(absl::c_all_of(threads_,
                       [](const std::unique_ptr<ghost::GhostThread>& thread) {
                         return !thread->Joinable();
                       }));
}

void ExperimentThreadPool::Init(
    const std::vector<ghost::GhostThread::KernelScheduler>& ksched,
    const std::vector<std::function<void(uint32_t)>>& thread_work) {
  CHECK_EQ(ksched.size(), num_threads_);
  CHECK_EQ(ksched.size(), thread_work.size());

  threads_.reserve(num_threads_);
  for (uint32_t i = 0; i < num_threads_; i++) {
    threads_.push_back(std::make_unique<ghost::GhostThread>(
        ksched[i],
        std::bind(&ExperimentThreadPool::ThreadMain, this, i, thread_work[i])));
  }
}

void ExperimentThreadPool::MarkExit(uint32_t sid) {
  thread_triggers_.Trigger(sid);
}

void ExperimentThreadPool::ThreadMain(
    uint32_t i, std::function<void(uint32_t)> thread_work) {
  while (!ShouldExit(i)) {
    thread_work(i);
  }
  num_exited_.fetch_add(1, std::memory_order_release);
}

void ExperimentThreadPool::Join() {
  // Check that all threads have already been notified to exit. If not, the call
  // to `Join` below will hang on one the threads because that thread will not
  // exit.
  for (uint32_t i = 0; i < num_threads_; i++) {
    CHECK(thread_triggers_.Triggered(/*sid=*/i));
  }
  for (std::unique_ptr<ghost::GhostThread>& thread : threads_) {
    // Check that `thread` is joinable. `thread` will not be joinable if it has
    // already been joined.
    CHECK(thread->Joinable());
    thread->Join();
  }
}

}  // namespace ghost_test
