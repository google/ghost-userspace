// Copyright 2022 Google LLC
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

#include "schedulers/cfs/cfs_scheduler.h"

#include <sys/timerfd.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "lib/logging.h"
#include "lib/topology.h"

namespace ghost {

CfsScheduler::CfsScheduler(Enclave* enclave, CpuList cpulist,
                           std::shared_ptr<TaskAllocator<CfsTask>> allocator)
    : BasicDispatchScheduler(enclave, std::move(cpulist),
                             std::move(allocator)) {
  for (const Cpu& cpu : cpus()) {
    CpuState* cs = cpu_state(cpu);
    cs->channel = std::make_unique<ghost::LocalChannel>(
        GHOST_MAX_QUEUE_ELEMS, cpu.numa_node(),
        MachineTopology()->ToCpuList({cpu}));
    // This channel pointer is valid for the lifetime of CfsScheduler
    if (!default_channel_) {
      default_channel_ = cs->channel.get();
    }
  }
}

void CfsScheduler::DumpAllTasks() {
  fprintf(stderr, "task        state   cpu\n");
  allocator()->ForEachTask([](Gtid gtid, const CfsTask* task) {
    absl::FPrintF(stderr, "%-12s%-8d%-8d%c\n", gtid.describe(), task->run_state,
                  task->cpu, task->preempted ? 'P' : '-');
    return true;
  });
}

void CfsScheduler::DumpState(const Cpu& cpu, int flags) {
  if (flags & Scheduler::kDumpAllTasks) {
    DumpAllTasks();
  }

  CpuState* cs = cpu_state(cpu);
  if (!(flags & Scheduler::kDumpStateEmptyRQ) && !cs->current &&
      cs->run_queue.Empty()) {
    return;
  }

  const CfsTask* current = cs->current;
  const CfsRq* rq = &cs->run_queue;
  absl::FPrintF(stderr, "SchedState[%d]: %s rq_l=%lu\n", cpu.id(),
                current ? current->gtid.describe() : "none", rq->Size());
}

void CfsScheduler::EnclaveReady() {
  for (const Cpu& cpu : cpus()) {
    CpuState* cs = cpu_state(cpu);
    Agent* agent = enclave()->GetAgent(cpu);

    // AssociateTask may fail if agent barrier is stale.
    while (!cs->channel->AssociateTask(agent->gtid(), agent->barrier(),
                                       /*status=*/nullptr)) {
      CHECK_EQ(errno, ESTALE);
    }
  }
}

// Implicitly thread-safe because it is only called from one agent associated
// with the default queue.
Cpu CfsScheduler::AssignCpu(CfsTask* task) {
  static auto begin = cpus().begin();
  static auto end = cpus().end();
  static auto next = end;

  if (next == end) {
    next = begin;
  }
  return next++;
}

void CfsScheduler::Migrate(CfsTask* task, Cpu cpu,
                           StatusWord::BarrierToken seqnum) {
  CHECK_EQ(task->run_state, CfsTaskState::kRunnable);
  CHECK_EQ(task->cpu, -1);

  CpuState* cs = cpu_state(cpu);
  const Channel* channel = cs->channel.get();
  CHECK(channel->AssociateTask(task->gtid, seqnum, /*status=*/nullptr));

  GHOST_DPRINT(3, stderr, "Migrating task %s to cpu %d", task->gtid.describe(),
               cpu.id());
  task->cpu = cpu.id();

  // Make task visible in the new runqueue *after* changing the association
  // (otherwise the task can get oncpu while producing into the old queue).
  cs->run_queue.EnqueueTask(task);

  // Get the agent's attention so it notices the new task.
  enclave()->GetAgent(cpu)->Ping();
}

void CfsScheduler::TaskNew(CfsTask* task, const Message& msg) {
  const ghost_msg_payload_task_new* payload =
      static_cast<const ghost_msg_payload_task_new*>(msg.payload());

  task->seqnum = msg.seqnum();
  task->run_state = CfsTaskState::kBlocked;
  if (payload->runnable) {
    task->run_state = CfsTaskState::kRunnable;
    Cpu cpu = AssignCpu(task);
    Migrate(task, cpu, msg.seqnum());
  } else {
    // Wait until task becomes runnable to avoid race between migration
    // and MSG_TASK_WAKEUP showing up on the default channel.
  }
}

void CfsScheduler::TaskRunnable(CfsTask* task, const Message& msg) {
  CHECK(task->blocked());
  task->run_state = CfsTaskState::kRunnable;

  if (task->cpu < 0) {
    // There cannot be any more messages pending for this task after a
    // MSG_TASK_WAKEUP (until the agent puts it oncpu) so it's safe to
    // migrate.
    Cpu cpu = AssignCpu(task);
    Migrate(task, cpu, msg.seqnum());
  } else {
    CpuState* cs = cpu_state_of(task);
    cs->run_queue.EnqueueTask(task);
  }
}

void CfsScheduler::TaskDeparted(CfsTask* task, const Message& msg) {
  const ghost_msg_payload_task_departed* payload =
      static_cast<const ghost_msg_payload_task_departed*>(msg.payload());

  CpuState* cs = cpu_state_of(task);
  if (task->oncpu() || payload->from_switchto) {
    TaskOffCpu(task, /*blocked=*/false, payload->from_switchto);
  } else if (task->queued()) {
    cs->run_queue.Erase(task);
  } else {
    CHECK(task->blocked());
  }

  if (payload->from_switchto) {
    Cpu cpu = topology()->cpu(payload->cpu);
    enclave()->GetAgent(cpu)->Ping();
  }

  allocator()->FreeTask(task);
}

void CfsScheduler::TaskDead(CfsTask* task, const Message& msg) {
  CHECK(task->blocked());
  allocator()->FreeTask(task);
}

void CfsScheduler::TaskYield(CfsTask* task, const Message& msg) {
  const ghost_msg_payload_task_yield* payload =
      static_cast<const ghost_msg_payload_task_yield*>(msg.payload());

  CpuState* cs = cpu_state_of(task);

  TaskOffCpu(task, /*blocked=*/false, payload->from_switchto);

  // This task in transition from RUNNING to RUNNABLE, so its vruntime
  // is valid w.r.t. the vruntimes in the tree currently.
  // We call PutPrevTask so we don't mess with its current accounting,
  // unlike EnqueueTask().
  cs->run_queue.PutPrevTask(task);

  if (payload->from_switchto) {
    Cpu cpu = topology()->cpu(payload->cpu);
    enclave()->GetAgent(cpu)->Ping();
  }
}

void CfsScheduler::TaskBlocked(CfsTask* task, const Message& msg) {
  const ghost_msg_payload_task_blocked* payload =
      static_cast<const ghost_msg_payload_task_blocked*>(msg.payload());

  TaskOffCpu(task, /*blocked=*/true, payload->from_switchto);

  if (payload->from_switchto) {
    Cpu cpu = topology()->cpu(payload->cpu);
    enclave()->GetAgent(cpu)->Ping();
  }
}

void CfsScheduler::TaskPreempted(CfsTask* task, const Message& msg) {
  const ghost_msg_payload_task_preempt* payload =
      static_cast<const ghost_msg_payload_task_preempt*>(msg.payload());

  TaskOffCpu(task, /*blocked=*/false, payload->from_switchto);

  task->preempted = true;
  CpuState* cs = cpu_state_of(task);
  cs->run_queue.PutPrevTask(task);

  if (payload->from_switchto) {
    Cpu cpu = topology()->cpu(payload->cpu);
    enclave()->GetAgent(cpu)->Ping();
  }
}

void CfsScheduler::TaskSwitchto(CfsTask* task, const Message& msg) {
  TaskOffCpu(task, /*blocked=*/true, /*from_switchto=*/false);
}

void CfsScheduler::ValidatePreExitState() {
  for (const Cpu& cpu : cpus()) {
    CpuState* cs = cpu_state(cpu);
    CHECK(cs->run_queue.Empty());
  }
}

void CfsScheduler::TaskOffCpu(CfsTask* task, bool blocked, bool from_switchto) {
  GHOST_DPRINT(3, stderr, "Task %s offcpu %d", task->gtid.describe(),
               task->cpu);
  CpuState* cs = cpu_state_of(task);

  if (task->oncpu()) {
    CHECK_EQ(cs->current, task);
    cs->current = nullptr;
  } else {
    CHECK(from_switchto);
    CHECK_EQ(task->run_state, CfsTaskState::kBlocked);
  }

  task->run_state = blocked ? CfsTaskState::kBlocked : CfsTaskState::kRunnable;
}

void CfsScheduler::CfsSchedule(const Cpu& cpu,
                               StatusWord::BarrierToken agent_barrier,
                               bool prio_boost) {
  CpuState* cs = cpu_state(cpu);
  CfsTask* next = nullptr;

  CHECK_EQ(cs->current, nullptr);

  if (!prio_boost) {
    next = cs->current;
    // Tell the runqueue to give us a pointer to our next task
    // and remove it from its backing data structure.
    if (!next) next = cs->run_queue.PickNextTask();
  }

  GHOST_DPRINT(3, stderr, "CfsSchedule %s on %s cpu %d ",
               next ? next->gtid.describe() : "idling",
               prio_boost ? "prio-boosted" : "", cpu.id());

  RunRequest* req = enclave()->GetRunRequest(cpu);
  if (next) {
    // Wait for 'next' to get offcpu before switching to it. This might seem
    // superfluous because we don't migrate tasks past the initial assignment
    // of the task to a cpu. However a SwitchTo target can migrate and run on
    // another CPU behind the agent's back. This is usually undetectable from
    // the agent's pov since the SwitchTo target is blocked and thus !on_rq.
    //
    // However if 'next' happens to be the last task in a SwitchTo chain then
    // it is possible to observe TASK_PREEMPT(next) or TASK_YIELD(next) before
    // it has gotten off the remote cpu. The 'on_cpu()' check below handles
    // this scenario.
    //
    // See go/switchto-ghost-redux for more details.
    while (next->status_word.on_cpu()) {
      Pause();
    }

    req->Open({
        .target = next->gtid,
        .target_barrier = next->seqnum,
        .agent_barrier = agent_barrier,
        .commit_flags = COMMIT_AT_TXN_COMMIT,
    });

    uint64_t previous_runtime_ns = next->status_word.runtime();
    if (req->Commit()) {
      // Txn commit succeeded and 'next' is oncpu.
      cs->current = next;

      GHOST_DPRINT(3, stderr, "Task %s oncpu %d", next->gtid.describe(),
                   cpu.id());

      next->run_state = CfsTaskState::kOnCpu;
      next->cpu = cpu.id();
      next->preempted = false;
      next->vruntime +=
          absl::Nanoseconds(next->status_word.runtime() - previous_runtime_ns);
    } else {
      GHOST_DPRINT(3, stderr, "CfsSchedule: commit failed (state=%d)",
                   req->state());

      cs->run_queue.PutPrevTask(next);
    }
  } else {
    // If LocalYield is due to 'prio_boost' then instruct the kernel to
    // return control back to the agent when CPU is idle.
    int flags = 0;
    if (prio_boost && !cs->run_queue.Empty()) {
      flags = RTLA_ON_IDLE;
    }
    req->LocalYield(agent_barrier, flags);
  }
}

void CfsScheduler::Schedule(const Cpu& cpu, const StatusWord& agent_sw) {
  StatusWord::BarrierToken agent_barrier = agent_sw.barrier();
  CpuState* cs = cpu_state(cpu);

  GHOST_DPRINT(3, stderr, "Schedule: agent_barrier[%d] = %d\n", cpu.id(),
               agent_barrier);

  Message msg;
  while (!(msg = Peek(cs->channel.get())).empty()) {
    DispatchMessage(msg);
    Consume(cs->channel.get(), msg);
  }

  CfsSchedule(cpu, agent_barrier, agent_sw.boosted_priority());
}

void CfsRq::EnqueueTask(CfsTask* task) {
  absl::MutexLock lock(&mu_);

  CHECK_GE(task->cpu, 0);
  CHECK_EQ(task->run_state, CfsTaskState::kRunnable);

  task->run_state = CfsTaskState::kQueued;

  // We never want to enqueue a new task with a smaller vruntime that we have
  // currently. We also never want to have a task's vruntime go backwards,
  // so we take the max of our current min vruntime and the tasks current one.
  // Until load balancing is implented, this should just evaluate to
  // min_vruntime_.
  task->vruntime = std::max(min_vruntime_, task->vruntime);

  rq_.insert(task);
}

void CfsRq::PutPrevTask(CfsTask* task) {
  CHECK_GE(task->cpu, 0);
  CHECK_EQ(task->run_state, CfsTaskState::kRunnable);

  task->run_state = CfsTaskState::kQueued;

  absl::MutexLock lock(&mu_);

  rq_.insert(task);
  min_vruntime_ = (*rq_.begin())->vruntime;
}

CfsTask* CfsRq::PickNextTask() {
  absl::MutexLock lock(&mu_);
  if (rq_.empty()) return nullptr;

  // Get a pointer to the first task. std::{set, multiset} orders by the ::Less
  // function, implying that, in our case, the first element has the smallest
  // vruntime (https://www.cplusplus.com/reference/set/set/).
  auto start_it = rq_.begin();
  CfsTask* task = *start_it;

  CHECK(task->queued());
  task->run_state = CfsTaskState::kRunnable;

  // Remove the task from the timeline.
  rq_.erase(start_it);

  // min_vruntime is used for Enqueing new tasks. We want to place them at
  // at least the current moment in time. Placing them before min_vruntime,
  // would give them an inordinate amount of runtime on the CPU as they would
  // need to catch up to other tasks that have accummulated a large runtime.
  // For easy access, we cache the value.
  if (!rq_.empty()) {
    CHECK_GE((*rq_.begin())->vruntime, min_vruntime_);
    min_vruntime_ = (*rq_.begin())->vruntime;
  } else {
    min_vruntime_ = absl::ZeroDuration();
  }
  return task;
}

void CfsRq::Erase(CfsTask* task) {
  CHECK_EQ(task->run_state, CfsTaskState::kQueued);
  absl::MutexLock lock(&mu_);
  rq_.erase(task);
}

std::unique_ptr<CfsScheduler> MultiThreadedCfsScheduler(Enclave* enclave,
                                                        CpuList cpulist) {
  auto allocator = std::make_shared<ThreadSafeMallocTaskAllocator<CfsTask>>();
  auto scheduler = std::make_unique<CfsScheduler>(enclave, std::move(cpulist),
                                                  std::move(allocator));
  return scheduler;
}

void CfsAgent::AgentThread() {
  gtid().assign_name("Agent:" + std::to_string(cpu().id()));
  if (verbose() > 1) {
    printf("Agent tid:=%d\n", gtid().tid());
  }
  SignalReady();
  WaitForEnclaveReady();

  PeriodicEdge debug_out(absl::Seconds(1));

  while (!Finished() || !scheduler_->Empty(cpu())) {
    scheduler_->Schedule(cpu(), status_word());

    if (verbose() && debug_out.Edge()) {
      static const int flags = verbose() > 1 ? Scheduler::kDumpStateEmptyRQ : 0;
      if (scheduler_->debug_runqueue_) {
        scheduler_->debug_runqueue_ = false;
        scheduler_->DumpState(cpu(), Scheduler::kDumpAllTasks);
      } else {
        scheduler_->DumpState(cpu(), flags);
      }
    }
  }
}

std::ostream& operator<<(std::ostream& os, const CfsTaskState& state) {
  switch (state) {
    case CfsTaskState::kBlocked:
      return os << "kBlocked";
    case CfsTaskState::kRunnable:
      return os << "kRunnable";
    case CfsTaskState::kQueued:
      return os << "kQueued";
    case CfsTaskState::kOnCpu:
      return os << "kOnCpu";
      // No default (exhaustive switch)
  }

  return os << static_cast<int>(state);  // 'state' has a non-enumerator value.
}

}  //  namespace ghost
