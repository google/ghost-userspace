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

#include "experiments/antagonist/ghost_orchestrator.h"

#include "absl/functional/bind_front.h"

namespace ghost_test {
namespace {
// We do not need a different class of service (e.g., different expected
// runtimes, different QoS (Quality-of-Service) classes, etc.) across workers in
// our experiments. Furthermore, all workers are ghOSt one-shots. Thus, put all
// worker sched items in the same work class.
static constexpr uint32_t kWorkClassIdentifier = 0;
}  // namespace

void GhostOrchestrator::InitThreadPool() {
  std::vector<ghost::GhostThread::KernelScheduler> kernel_schedulers(
      options().num_threads, ghost::GhostThread::KernelScheduler::kGhost);
  std::vector<std::function<void(uint32_t)>> thread_work(
      options().num_threads,
      absl::bind_front(&GhostOrchestrator::Worker, this));

  CHECK_EQ(kernel_schedulers.size(), options().num_threads);
  CHECK_EQ(kernel_schedulers.size(), thread_work.size());
  thread_pool().Init(kernel_schedulers, thread_work);
}

void GhostOrchestrator::InitGhost() {
  const std::vector<ghost::Gtid> gtids = thread_pool().GetGtids();
  CHECK_EQ(gtids.size(), options().num_threads);

  ghost::work_class wc;
  prio_table_helper_.GetWorkClass(kWorkClassIdentifier, wc);
  wc.id = kWorkClassIdentifier;
  wc.flags = WORK_CLASS_ONESHOT;
  wc.qos = options().ghost_qos;
  // Write the max unsigned 64-bit integer as the deadline just in case we want
  // to run the experiment with the ghOSt EDF (Earliest-Deadline-First)
  // scheduler.
  wc.exectime = std::numeric_limits<uint64_t>::max();
  // 'period' is irrelevant because all threads scheduled by ghOSt are
  // one-shots.
  wc.period = 0;
  prio_table_helper_.SetWorkClass(kWorkClassIdentifier, wc);

  for (size_t i = 0; i < gtids.size(); ++i) {
    ghost::sched_item si;
    prio_table_helper_.GetSchedItem(/*sid=*/i, si);
    si.sid = i;
    si.wcid = kWorkClassIdentifier;
    si.gpid = gtids[i].id();
    si.flags = SCHED_ITEM_RUNNABLE;
    si.deadline = 0;
    prio_table_helper_.SetSchedItem(/*sid=*/i, si);
  }
}

GhostOrchestrator::GhostOrchestrator(Orchestrator::Options opts)
    : Orchestrator(std::move(opts)),
      prio_table_helper_(/*num_sched_items=*/options().num_threads,
                         /*num_work_classes=*/1) {
  CHECK_ZERO(options().cpus.size());

  InitThreadPool();
  // This must be called after 'InitThreadPool' since it accesses the GTIDs of
  // the threads in the thread pool.
  InitGhost();
  set_start(absl::Now());
}

void GhostOrchestrator::Worker(uint32_t sid) {
  if (!thread_triggers().Triggered(sid)) {
    thread_triggers().Trigger(sid);
    printf("Worker (SID %u, TID: %ld, not affined to any CPU)\n", sid,
           syscall(SYS_gettid));
  }

  Soak(sid);
}

}  // namespace ghost_test
