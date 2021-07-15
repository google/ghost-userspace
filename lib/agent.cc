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

#include "agent.h"

#include <linux/sched.h>

#include "lib/scheduler.h"

namespace ghost {

Agent::~Agent() {
  enclave_->DetachAgent(this);
  CHECK(!thread_.joinable());
  status_word_.Free();
}

void Agent::StartBegin() { thread_ = std::thread(&Agent::ThreadBody, this); }

void Agent::StartComplete() { ready_.WaitForNotification(); }

void Agent::ThreadBody() {
  int queue_fd;
  Scheduler* s = AgentScheduler();
  if (!s) {
    // Some tests don't have a scheduler.  Those that don't need to set a
    // default channel before starting the agents, which the kernel will use.
    // If they did not set a default, then SchedAgentEnterGhost will fail.
    // TODO Once we move queues to ghostfs, we might be able to CHECK that
    // there is a default for the enclave.
    queue_fd = -1;
  } else {
    queue_fd = s->GetDefaultChannel().GetFd();
  }

  gtid_ = Gtid::Current();
  CHECK_EQ(Ghost::SchedSetAffinity(Gtid::Current(),
                                   MachineTopology()->ToCpuList({cpu_})),
           0);
  enclave_->WaitForOldAgent();

  // setsched may fail with EBUSY, which is when there is an old agent that has
  // not left the cpu yet.  Spin until we can.  The old agent has priority; the
  // kernel will preempt us when it is runnable, since we are still in CFS.  We
  // know that the old agent is gone or in the act of dying, because we called
  // WaitForOldAgent.
  int ret;
  do {
    ret = SchedAgentEnterGhost(enclave_->GetCtlFd(), queue_fd);
  } while (ret && errno == EBUSY);
  CHECK_EQ(ret, 0);

  status_word_ = StatusWord(StatusWord::AgentSW{});
  CHECK(!status_word_.empty());

  enclave_->AttachAgent(cpu_, this);

  AgentThread();
  WaitForExitNotification();
}

bool Agent::Ping() {
  RunRequest* req = enclave()->GetRunRequest(cpu_);
  return req->Ping();
}

void Agent::TerminateBegin() {
  finished_.Notify();

  // Ensure that we return control to agent to observe finished.
  Ping();

  do_exit_.Notify();
}

void Agent::TerminateComplete() {
  thread_.join();

  // pthread_join() can return before the dying task has released all
  // of its resources (CLONE_CHILD_CLEARTID based synchronization via
  // do_exit()->exit_mm()->mm_release() happens much earlier than the
  // 'sched_class.task_dead' callback).
  //
  // Since agent state transitions don't produce task messages we use
  // the GHOST_SW_F_CANFREE bit to check whether the kernel has invoked
  // the 'task_dead' callback.
  while (!status_word_.can_free()) absl::SleepFor(absl::Milliseconds(1));
}

// static
const bool Agent::kVersionCheck = Ghost::CheckVersion();

}  // namespace ghost
