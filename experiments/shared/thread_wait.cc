// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "experiments/shared/thread_wait.h"

#include "lib/base.h"

namespace ghost_test {

ThreadWait::ThreadWait(uint32_t num_threads, WaitType wait_type)
    : num_threads_(num_threads), wait_type_(wait_type) {
  runnability_.reserve(num_threads);
  for (uint32_t i = 0; i < num_threads_; i++) {
    runnability_.push_back(std::make_unique<std::atomic<int>>(0));
  }
}

void ThreadWait::MarkRunnable(uint32_t sid) {
  CHECK_LT(sid, num_threads_);

  runnability_[sid]->store(1, std::memory_order_release);
  if (wait_type_ == WaitType::kFutex) {
    ghost::Futex::Wake(runnability_[sid].get(), 1);
  }
}

void ThreadWait::MarkIdle(uint32_t sid) {
  CHECK_LT(sid, num_threads_);

  runnability_[sid]->store(0, std::memory_order_release);
}

void ThreadWait::WaitUntilRunnable(uint32_t sid) const {
  CHECK_LT(sid, num_threads_);

  const std::unique_ptr<std::atomic<int>>& r = runnability_[sid];
  if (wait_type_ == WaitType::kSpin) {
    while (r->load(std::memory_order_acquire) == 0) {
      ghost::Pause();
    }
  } else {
    CHECK_EQ(wait_type_, WaitType::kFutex);

    ghost::Futex::Wait(r.get(), 0);
  }
}

}  // namespace ghost_test
