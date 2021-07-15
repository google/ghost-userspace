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

#include "experiments/antagonist/cfs_orchestrator.h"

#include "absl/functional/bind_front.h"
#include "experiments/shared/cfs.h"

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
  CHECK_EQ(options().num_threads, options().cpus.size());

  InitThreadPool();
  threads_ready_.Block();
  set_start(absl::Now());
}

void CfsOrchestrator::Worker(uint32_t sid) {
  if (!thread_triggers().Triggered(sid)) {
    thread_triggers().Trigger(sid);
    CHECK_ZERO(ghost::Ghost::SchedSetAffinity(
        ghost::Gtid::Current(), ghost::MachineTopology()->ToCpuList(
                                    std::vector<int>{options().cpus[sid]})));
    printf("Worker (SID %u, TID: %ld, affined to CPU %u)\n", sid,
           syscall(SYS_gettid), options().cpus[sid]);
    threads_ready_.Block();
  }

  Soak(sid);
}

}  // namespace ghost_test
