/*
 * Copyright 2021 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef GHOST_EXPERIMENTS_SHARED_CFS_H_
#define GHOST_EXPERIMENTS_SHARED_CFS_H_

#include <stdint.h>

#include "lib/base.h"

namespace ghost_test {

// Support class for test apps that run experiments with threads scheduled by
// the Linux Completely Fair Scheduler (CFS). This class allows threads to be
// marked as idle/runnable and lets them wait if they are idle until they are
// marked runnable again either by spinning or sleeping on a futex.
//
// Example:
// CompletelyFairScheduler cfs_;
// (Initialize with the number of threads you are using and the wait type.)
// ...
// Main Thread: cfs_.MarkIdle(/*sid=*/2);
// ...
// Thread 2: cfs_.WaitUntilRunnable(/*sid=*/2);
// (Thread 2 now waits.)
// ...
// Thread 1: cfs_.MarkRunnable(/*sid=*/2);
// (Thread 2 now returns from 'WaitUntilRunnable()' and does other work.)
class CompletelyFairScheduler {
 public:
  // When 'WaitUntilRunnable' is called, there are different ways to wait. Each
  // way affects performance differently.
  enum class WaitType {
    // Wait by spinning. Threads will return from 'WaitUntilRunnable' more
    // quickly when marked runnable but will burn up their CPU while waiting.
    kWaitSpin,
    // Wait by sleeping on a futex. Threads will not burn up their CPU while
    // waiting but will return from 'WaitUntilRunnable' more slowly when marked
    // runnable.
    kWaitFutex,
  };

  CompletelyFairScheduler(uint32_t num_threads, WaitType wait_type);

  // Marks 'sid' as runnable.
  void MarkRunnable(uint32_t sid);
  // Marks 'sid' as idle.
  void MarkIdle(uint32_t sid);
  // Waits until 'sid' is runnable.
  void WaitUntilRunnable(uint32_t sid) const;

 private:
  const uint32_t num_threads_;
  const WaitType wait_type_;
  std::vector<std::unique_ptr<std::atomic<int>>> runnability_;
};

inline std::ostream& operator<<(std::ostream& os,
                                CompletelyFairScheduler::WaitType wait_type) {
  switch (wait_type) {
    case CompletelyFairScheduler::WaitType::kWaitSpin:
      os << "Spin";
      break;
    case CompletelyFairScheduler::WaitType::kWaitFutex:
      os << "Futex";
      break;
      // We will get a compile error if a new member is added to the
      // 'CompletelyFairScheduler::WaitType' enum and a corresponding case is
      // not added here.
  }
  return os;
}

}  // namespace ghost_test

#endif  // GHOST_EXPERIMENTS_SHARED_CFS_H_
