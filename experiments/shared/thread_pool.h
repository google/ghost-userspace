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

#ifndef GHOST_EXPERIMENTS_SHARED_THREAD_POOL_H_
#define GHOST_EXPERIMENTS_SHARED_THREAD_POOL_H_

#include <atomic>

#include "lib/base.h"
#include "lib/ghost.h"

namespace ghost_test {

// This class allows threads to be triggered by themselves or external code.
// Essentially, the "trigger" acts as a memory barrier that the caller and the
// thread can synchronize on. When the caller notices an event, it triggers the
// thread affected by the event. Each thread can only be triggered once.
//
// Example:
// ThreadTrigger triggers_(/*num_threads=*/5);
// ...
// Thread with SID 2: CHECK(!triggers_.Triggered(/*sid=*/2));
// Thread with SID 2: triggers_.WaitForTrigger(/*sid=*/2);
// Caller: /* Writes memory. */
// Caller: triggers_.Trigger(/*sid=*/2);
// (Thread with SID 2 wakes up and returns from `WaitForTrigger()`.).
// Thread with SID 2: CHECK(triggers_.Triggered(/*sid=*/2));
// Thread with SID 2: /* Reads memory written to by `Caller`. */
class ThreadTrigger {
 public:
  // Constructs this class to trigger `num_threads` threads.
  explicit ThreadTrigger(uint32_t num_threads) {
    thread_trigger_.reserve(num_threads);
    for (uint32_t i = 0; i < num_threads; i++) {
      thread_trigger_.push_back(std::make_unique<ghost::Notification>());
    }
  }

  // Returns true if the thread with SID `sid` has been triggered.
  // Returns false otherwise.
  bool Triggered(uint32_t sid) const {
    CHECK_LT(sid, thread_trigger_.size());
    return thread_trigger_[sid]->HasBeenNotified();
  }

  // Triggers the thread with SID `sid` if that thread has not already been
  // triggered. Returns true if the thread was triggered for the first time.
  // Returns false if the thread has already been triggered.
  bool Trigger(uint32_t sid) {
    CHECK_LT(sid, thread_trigger_.size());
    bool triggered = Triggered(sid);
    if (!triggered) {
      thread_trigger_[sid]->Notify();
    }
    return !triggered;
  }

  // Blocks the caller until the thread with SID `sid` is triggered. This
  // function returns immediately *without blocking* if the thread has already
  // been triggered.
  void WaitForTrigger(uint32_t sid) const {
    CHECK_LT(sid, thread_trigger_.size());
    thread_trigger_[sid]->WaitForNotification();
  }

 private:
  // The notification at index `i` is not "notified" by default. Once `Trigger`
  // is called for the thread with SID `i`, this notification is notified.
  std::vector<std::unique_ptr<ghost::Notification>> thread_trigger_;
};

// Creates and manages a pool of threads. These threads may be scheduled by
// ghOSt, CFS (Linux Completely Fair Scheduler), or a combination of the two.
//
// Example:
// ExperimentThreadPool thread_pool_;
// (Initialize with the schedulers and closures.)
// ...
// thread_pool_.MarkExit(i);
// (Call for each thread ID from 0 to thread_pool_.NumThreads() - 1.)
// ...
// thread_pool_.Join();
class ExperimentThreadPool {
 public:
  explicit ExperimentThreadPool(uint32_t num_threads)
      : num_threads_(num_threads), thread_triggers_(num_threads) {}

  ~ExperimentThreadPool();

  // Initializes the thread pool, which in turn spawns the threads. `ksched`
  // specifies the scheduler to use for each thread (specify a different
  // scheduler for each thread by adding different schedulers to each index).
  // `thread_work` is the closure that each thread will run before exiting. The
  // number of threads created is equal to the number of items in the
  // `thread_work` vector. Note that the number of items in `ksched` and in
  // `thread_work` must be equal or the constructor will crash.
  void Init(const std::vector<ghost::GhostThread::KernelScheduler>& ksched,
            const std::vector<std::function<void(uint32_t)>>& thread_work);

  // Returns a vector of the GTIDs of all threads managed by the thread pool.
  // The GTID for the thread with SID `i` is in index `i` of the vector.
  std::vector<ghost::Gtid> GetGtids() const {
    std::vector<ghost::Gtid> gtids;
    gtids.reserve(num_threads_);
    for (const std::unique_ptr<ghost::GhostThread>& t : threads_) {
      gtids.push_back(ghost::Gtid(t->gtid()));
    }
    CHECK_EQ(gtids.size(), num_threads_);
    return gtids;
  }

  // Marks thread with SID `sid` as ready to exit. The thread will exit after
  // finishing its current iteration.
  void MarkExit(uint32_t sid);

  // Joins all threads managed by the thread pool.
  void Join();

  // Returns the number of threads in the thread pool.
  uint32_t NumThreads() const { return num_threads_; }

  // Returns the number of threads that have exited, due to a call to
  // `MarkExit`. Remember that a thread cannot exit even after a call to
  // `MarkExit` until it is scheduled to run, which is especially relevant for
  // ghOSt.
  uint32_t NumExited() const {
    return num_exited_.load(std::memory_order_acquire);
  }

  // Returns true if the thread with SID `sid` has been marked as ready to exit.
  // Returns false otherwise.
  bool ShouldExit(uint32_t sid) const {
    return thread_triggers_.Triggered(sid);
  }

 private:
  // The main thread body.
  void ThreadMain(uint32_t i, std::function<void(uint32_t)> thread_work);

  const uint32_t num_threads_;

  // A thread is "triggered" when it is marked as ready to exit by `MarkExit`.
  // This class instance tracks the triggers.
  //
  // Note that this class instance is initialized before the `threads_` vector.
  // The threads managed by this thread pool access the `thread_triggers_`
  // class member, so `thread_triggers_` must be initialized before the threads
  // start.
  ThreadTrigger thread_triggers_;

  // The threads managed by the thread pool.
  std::vector<std::unique_ptr<ghost::GhostThread>> threads_;

  // The number of threads that have exited.
  std::atomic<uint32_t> num_exited_ = 0;
};

}  // namespace ghost_test

#endif  // GHOST_EXPERIMENTS_SHARED_THREAD_POOL_H_
