// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef GHOST_EXPERIMENTS_SHARED_THREAD_WAIT_H_
#define GHOST_EXPERIMENTS_SHARED_THREAD_WAIT_H_

#include <stdint.h>

#include "lib/base.h"

namespace ghost_test {

// Support class for test apps that run experiments with threads that need to
// wait. This class allows threads to be marked as idle/runnable and lets them
// wait if they are idle until they are marked runnable again either by spinning
// or sleeping on a futex.
//
// Example:
// ThreadWait thread_wait_;
// (Initialize with the number of threads you are using and the wait type.)
// ...
// Main Thread: thread_wait_.MarkIdle(/*sid=*/2);
// ...
// Thread 2: thread_wait_.WaitUntilRunnable(/*sid=*/2);
// (Thread 2 now waits.)
// ...
// Thread 1: thread_wait_.MarkRunnable(/*sid=*/2);
// (Thread 2 now returns from 'WaitUntilRunnable()' and does other work.)
class ThreadWait {
 public:
  // When 'WaitUntilRunnable' is called, there are different ways to wait. Each
  // way affects performance differently.
  enum class WaitType {
    // Wait by spinning. Threads will return from 'WaitUntilRunnable' more
    // quickly when marked runnable but will burn up their CPU while waiting.
    kSpin,
    // Wait by sleeping on a futex. Threads will not burn up their CPU while
    // waiting but will return from 'WaitUntilRunnable' more slowly when marked
    // runnable.
    kFutex,
  };

  ThreadWait(uint32_t num_threads, WaitType wait_type);

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
                                ThreadWait::WaitType wait_type) {
  switch (wait_type) {
    case ThreadWait::WaitType::kSpin:
      return os << "Spin";
    case ThreadWait::WaitType::kFutex:
      return os << "Futex";
  }
}

}  // namespace ghost_test

#endif  // GHOST_EXPERIMENTS_SHARED_THREAD_WAIT_H_
