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

#ifndef GHOST_EXPERIMENTS_ANTAGONIST_CFS_ORCHESTRATOR_H_
#define GHOST_EXPERIMENTS_ANTAGONIST_CFS_ORCHESTRATOR_H_

#include "absl/synchronization/barrier.h"
#include "experiments/antagonist/orchestrator.h"

namespace ghost_test {

// This is the orchestrator for the CFS (Linux Completely Fair Scheduler)
// experiments. All threads are scheduled by CFS.
//
// Example:
// Orchestrator::Options options;
// ... Fill in the options.
// CfsOrchestrator orchestrator(options);
// (Constructs orchestrator with options.)
// ...
// orchestrator.Terminate();
// (Tells orchestrator to stop the experiment and print the results.)
class CfsOrchestrator : public Orchestrator {
 public:
  explicit CfsOrchestrator(Orchestrator::Options opts);
  ~CfsOrchestrator() final {}

 private:
  // Initializes the thread pool.
  void InitThreadPool();

  void Worker(uint32_t sid) final;

  // Used so that the main thread does not start the timer (and workers do not
  // start spinning) until the worker threads have initialized.
  absl::Barrier threads_ready_;
};

}  // namespace ghost_test

#endif  // GHOST_EXPERIMENTS_ANTAGONIST_CFS_ORCHESTRATOR_H_
