// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "experiments/antagonist/cfs_orchestrator.h"

#include "absl/functional/bind_front.h"

namespace ghost_test {

void CfsOrchestrator::InitThreadPool() {
  std::vector<ghost::GhostThread::KernelScheduler> kernel_schedulers(
      options().num_threads, ghost::GhostThread::KernelScheduler::kCfs);
  std::vector<std::function<void(uint32_t)>> thread_work(
      options().num_threads, absl::bind_front(&CfsOrchestrator::Worker, this));

  CHECK_EQ(kernel_schedulers.size(), options().num_threads);
  CHECK_EQ(kernel_schedulers.size(), thread_work.size());
  thread_pool().Init(kernel_schedulers, thread_work);
}

CfsOrchestrator::CfsOrchestrator(Orchestrator::Options opts)
    : Orchestrator(std::move(opts)), threads_ready_(options().num_threads + 1) {
  CHECK_EQ(options().num_threads, options().cpus.Size());

  InitThreadPool();
  threads_ready_.Block();
  set_start(absl::Now());
}

void CfsOrchestrator::Worker(uint32_t sid) {
  if (!thread_triggers().Triggered(sid)) {
    thread_triggers().Trigger(sid);
    const ghost::Cpu cpu = options().cpus.GetNthCpu(sid);
    CHECK_EQ(
        ghost::GhostHelper()->SchedSetAffinity(
            ghost::Gtid::Current(), ghost::MachineTopology()->ToCpuList({cpu})),
        0);
    printf("Worker (SID %u, TID: %ld, affined to CPU %u)\n", sid,
           syscall(SYS_gettid), cpu.id());
    threads_ready_.Block();
  }

  Soak(sid);
}

}  // namespace ghost_test
