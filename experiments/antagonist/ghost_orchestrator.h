// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef GHOST_EXPERIMENTS_ANTAGONIST_GHOST_ORCHESTRATOR_H_
#define GHOST_EXPERIMENTS_ANTAGONIST_GHOST_ORCHESTRATOR_H_

#include "experiments/antagonist/orchestrator.h"
#include "experiments/shared/prio_table_helper.h"

namespace ghost_test {

// This is the orchestrator for the ghOSt experiments. All threads are scheduled
// by ghOSt.
//
// Example:
// Orchestrator::Options options;
// ... Fill in the options.
// GhostOrchestrator orchestrator(options);
// (Constructs orchestrator with options.)
// ...
// orchestrator.Terminate();
// (Tells orchestrator to stop the experiment and print the results.)
class GhostOrchestrator : public Orchestrator {
 public:
  explicit GhostOrchestrator(Orchestrator::Options opts);
  ~GhostOrchestrator() final {}

 private:
  // Initializes the thread pool.
  void InitThreadPool();

  // Initializes the ghOSt PrioTable.
  void InitGhost();

  void Worker(uint32_t sid) final;

  // Manages communication with ghOSt via the shared PrioTable.
  PrioTableHelper prio_table_helper_;
};

}  // namespace ghost_test

#endif  // GHOST_EXPERIMENTS_ANTAGONIST_GHOST_ORCHESTRATOR_H_
