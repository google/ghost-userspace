// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef GHOST_EXPERIMENTS_ROCKSDB_GHOST_ORCHESTRATOR_H_
#define GHOST_EXPERIMENTS_ROCKSDB_GHOST_ORCHESTRATOR_H_

#include "experiments/rocksdb/latency.h"
#include "experiments/rocksdb/orchestrator.h"
#include "experiments/rocksdb/request.h"
#include "experiments/shared/prio_table_helper.h"

namespace ghost_test {

// This is the orchestrator for the ghOSt experiments. The workers are scheduled
// by ghOSt whereas the load generator is scheduled by CFS. The load generator
// is pinned to a CPU using CFS and spins to ensure that its ability to generate
// the target throughput is not impacted by scheduling. Note that no dispatcher
// exists because the ghOSt global agent has that role.
//
// Example:
// Options options;
// ... Fill in the options.
// GhostOrchestrator orchestrator_(options);
// (Constructs orchestrator with options.)
// ...
// orchestrator_.Terminate();
// (Tells orchestrator to stop the experiment and print the results.)
class GhostOrchestrator final : public Orchestrator {
 public:
  explicit GhostOrchestrator(Options opts);
  ~GhostOrchestrator() final {}

  void Terminate() final;

 protected:
  // For ghOSt, the load generator passes requests to workers and marks the
  // workers runnable in the ghOSt PrioTable.
  void LoadGenerator(uint32_t sid) final;

  // There is no dispatcher thread as the ghOSt global agent has that role.
  // Thus, this method is a no-op. Calling this method will trigger a
  // 'CHECK(false)'.
  void Dispatcher(uint32_t sid) final {
    // No-op.
    CHECK(false);
  }

  void Worker(uint32_t sid) final;

 private:
  // Initializes the thread pool.
  void InitThreadPool();

  // Initializes the ghOSt PrioTable. Note that this should only be called when
  // the ghOSt wait type is `Orchestrator::GhostWaitType::kPrioTable`.
  void InitPrioTable();

  // Returns true if RocksDB workers wait on a PrioTable.
  bool UsesPrioTable() const {
    return options().ghost_wait_type == GhostWaitType::kPrioTable;
  }
  // Returns true if RocksDB workers wait on futexes.
  bool UsesFutex() const {
    return options().ghost_wait_type == GhostWaitType::kFutex;
  }

  // Used by `GetIdleWorkerSIDs()`. Returns true if the idle worker with SID
  // `worker_sid` should be skipped this round. Returns false if the worker
  // should not be skipped.
  bool SkipIdleWorker(uint32_t worker_sid);

  // The load generator calls this method to populate 'idle_sids_' with a list
  // of the SIDs of idle workers. Note that this method clears 'idle_sids_'
  // before filling it in.
  void GetIdleWorkerSIDs(uint32_t sid);

  // We do not need a different class of service (e.g., different expected
  // runtimes, different QoS (Quality-of-Service) classes, etc.) across workers
  // in our experiments. Furthermore, all workers are ghOSt one-shots and the
  // only candidate for a repeatable -- the load generator -- runs in CFS. Thus,
  // put all worker sched items in the same work class.
  static constexpr uint32_t kWorkClassIdentifier = 0;

  // Allows runnable threads to run and keeps idle threads sleeping on a futex
  // until they are marked runnable again. Note this is only used when the ghOSt
  // wait type is `Orchestrator::GhostWaitType::kFutex`. Otherwise, the pointer
  // is null.
  std::unique_ptr<ThreadWait> thread_wait_;

  // Manages communication with ghOSt via the shared PrioTable. Note this is
  // only used when the ghOSt wait type is
  // `Orchestrator::GhostWaitType::kPrioTable`. Otherwise, the pointer is null.
  std::unique_ptr<PrioTableHelper> prio_table_helper_;

  // 'threads_ready_' is notified once all threads have been spawned and the
  // ghOSt PrioTable has been initialized with the work class and all worker
  // thread sched items.
  ghost::Notification threads_ready_;

  // The load generators use this to store idle SIDs. We make this a class
  // member rather than a local variable in the 'LoadGenerator' method to avoid
  // repeatedly allocating memory for the list backing in the load generators'
  // common case, which is expensive.
  std::vector<std::vector<uint32_t>> idle_sids_;
};

}  // namespace ghost_test

#endif  // GHOST_EXPERIMENTS_ROCKSDB_GHOST_ORCHESTRATOR_H_
