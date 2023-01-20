// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "agent.h"

#include <linux/sched.h>

#include "lib/scheduler.h"

namespace ghost {

Agent::~Agent() {
  enclave_->DetachAgent(this);
  CHECK(!thread_.joinable());
}

void Agent::StartBegin() { thread_ = std::thread(&Agent::ThreadBody, this); }

void Agent::StartComplete() { ready_.WaitForNotification(); }

void LocalAgent::ThreadBody() {
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
    queue_fd = s->GetAgentChannel(cpu_).GetFd();
  }

  CHECK_EQ(prctl(PR_SET_NAME, absl::StrCat("ap_task_", cpu().id()).c_str()), 0);

  gtid_ = Gtid::Current();
  enclave_->WaitForOldAgent();

  // setsched may fail with EBUSY, which is when there is an old agent that has
  // not left the cpu yet.  Spin until we can.  The old agent has priority; the
  // kernel will preempt us when it is runnable, since we are still in CFS.  We
  // know that the old agent is gone or in the act of dying, because we called
  // WaitForOldAgent.
  int ret;
  do {
    ret = GhostHelper()->SchedAgentEnterGhost(enclave_->GetCtlFd(), cpu_,
                                              queue_fd);
  } while (ret && errno == EBUSY);
  CHECK_EQ(ret, 0);

  status_word_ = LocalStatusWord(StatusWord::AgentSW{});
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
  while (!status_word().can_free()) {
    absl::SleepFor(absl::Milliseconds(1));
  }
}

// static
const bool Agent::kVersionCheck = Ghost::CheckVersion();

}  // namespace ghost
