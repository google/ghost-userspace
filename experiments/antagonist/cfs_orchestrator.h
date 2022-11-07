// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

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
