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
