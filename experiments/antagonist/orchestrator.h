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

#ifndef GHOST_EXPERIMENTS_ANTAGONIST_ORCHESTRATOR_H_
#define GHOST_EXPERIMENTS_ANTAGONIST_ORCHESTRATOR_H_

#include "absl/time/clock.h"
#include "experiments/antagonist/results.h"
#include "experiments/shared/thread_pool.h"

namespace ghost_test {

// This class is the main component of the Antagonist application. It manages
// the worker threads and prints the results when the experiment is finished.
//
// Note that this is an abstract class and it cannot be constructed.
class Orchestrator {
 public:
  // Orchestrator configuration options.
  struct Options {
    // This pass a string representation of 'options' to 'os'. The options are
    // printed in alphabetical order by name.
    friend std::ostream& operator<<(std::ostream& os, const Options& options);

    PrintOptions print_options;

    // Each thread tries to target this share of the cycles on a CPU. For
    // example, if 'work_share' is 0.5, each thread tries to target 50% of
    // cycles on a CPU. Note that 'work_share' must be greater than or equal to
    // 0.0 and less than or equal to 1.0.
    double work_share;

    // The number of threads to use (excluding the main thread and background
    // threads).
    size_t num_threads;

    // The CPUs to affine threads to for threads scheduled by CFS. One thread is
    // affined to each CPU. Note that this vector cannot include
    // 'kBackgroundThreadCpu'.
    //
    // Note that 'cpus.size()' must equal 'num_threads'.
    std::vector<int> cpus;

    // The experiment duration.
    absl::Duration experiment_duration;

    // The scheduler that schedules the experiment. This is either CFS (Linux
    // Completely Fair Scheduler) or ghOSt.
    ghost::GhostThread::KernelScheduler scheduler;

    // For the ghOSt experiments, this is the QoS (Quality-of-Service) class
    // for the PrioTable work class that all worker sched items are added to.
    uint32_t ghost_qos;
  };

  virtual ~Orchestrator() = 0;

  // Stops the experiment, joins all threads, and prints the results.
  //
  // The RocksDB application implements different versions of 'Terminate' for
  // CFS (Linux Completely Fair Scheduler) and ghOSt. This is because threads
  // for the CFS (Linux Completely Fair Scheduler) and ghOSt experiments could
  // be idle and need to be marked runnable, which is done differently for CFS
  // threads and ghOSt threads. In the Antagonist application, all threads are
  // always runnable so the CFS and ghOSt experiments can share the 'Terminate'
  // method.
  void Terminate();

  // Returns the CPU time that the calling thread has consumed.
  static absl::Duration ThreadUsage();

  // This method tries to consume 'options_.work_share' share of cycles on one
  // CPU for one period.
  void Soak(uint32_t sid) { SoakHelper(sid, options_.work_share); }

  std::vector<absl::Duration>& run_duration() { return run_duration_; }

  // Affine all background threads to this CPU.
  static constexpr int kBackgroundThreadCpu = 0;

 protected:
  explicit Orchestrator(Options opts);

  // This method is executed in a loop by worker threads. This method tries to
  // consume 'options_.work_share' share of cycles on a CPU.
  virtual void Worker(uint32_t sid) = 0;

  // Prints all results (total numbers of requests, throughput, and latency
  // percentiles). 'runtime' is the duration of the experiment.
  void PrintResults(absl::Duration runtime) const;

  const Options& options() const { return options_; }

  ExperimentThreadPool& thread_pool() { return thread_pool_; }

  absl::Time start() const { return start_; }
  void set_start(absl::Time start) { start_ = start; }

  ThreadTrigger& thread_triggers() { return thread_triggers_; }

 private:
  // This method tries to consume 'soak_share' share of cycles on one CPU for
  // one period.
  void SoakHelper(uint32_t sid, double soak_share);

  // Orchestrator options.
  const Options options_;

  // The time that the experiment started at (after initialization).
  absl::Time start_;

  // The duration that each worker ran.
  std::vector<absl::Duration> run_duration_;

  // The time that each thread started doing synthetic work.
  std::vector<absl::Time> soak_start_;

  // The CPU time that the thread had consumed so far when it starting doing
  // synthetic work.
  std::vector<absl::Duration> usage_start_;

  // A thread triggers itself the first time it runs. Each thread does work on
  // its first iteration, so each thread uses the trigger to know if it is in
  // its first iteration or not.
  ThreadTrigger thread_triggers_;

  // The thread pool.
  ExperimentThreadPool thread_pool_;
};

}  // namespace ghost_test

#endif  // GHOST_EXPERIMENTS_ANTAGONIST_ORCHESTRATOR_H_
