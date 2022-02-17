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

#include "experiments/shared/thread_wait.h"

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
      asm volatile("pause");
    }
  } else {
    CHECK_EQ(wait_type_, WaitType::kFutex);

    ghost::Futex::Wait(r.get(), 0);
  }
}

}  // namespace ghost_test
