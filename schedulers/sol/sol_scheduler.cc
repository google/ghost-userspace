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

#include "schedulers/sol/sol_scheduler.h"

#include "absl/strings/str_format.h"

namespace ghost {

void SolScheduler::CpuNotIdle(const Message& msg) { CHECK(0); }

void SolScheduler::CpuTimerExpired(const Message& msg) { CHECK(0); }

SolScheduler::SolScheduler(Enclave* enclave, CpuList cpulist,
                           std::shared_ptr<TaskAllocator<SolTask>> allocator,
                           int32_t global_cpu)
    : BasicDispatchScheduler(enclave, std::move(cpulist), std::move(allocator)),
      global_cpu_(global_cpu),
      global_channel_(GHOST_MAX_QUEUE_ELEMS, /*node=*/0) {
  if (!cpus().IsSet(global_cpu_)) {
    Cpu c = cpus().Front();
    CHECK(c.valid());
    global_cpu_ = c.id();
  }
}

SolScheduler::~SolScheduler() {}

void SolScheduler::EnclaveReady() {
  for (const Cpu& cpu : cpus()) {
    CpuState* cs = cpu_state(cpu);
    cs->agent = enclave()->GetAgent(cpu);
    CHECK_NE(cs->agent, nullptr);
  }
}

bool SolScheduler::Available(const Cpu& cpu) {
  CpuState* cs = cpu_state(cpu);

  if (cs->agent) return cs->agent->cpu_avail();

  return false;
}

void SolScheduler::ValidatePreExitState() {
  CHECK_EQ(num_tasks_, 0);
  CHECK_EQ(RunqueueSize(), 0);
}

void SolScheduler::DumpAllTasks() {
  fprintf(stderr, "task        state       rq_pos  P\n");
  allocator()->ForEachTask([](Gtid gtid, const SolTask* task) {
    absl::FPrintF(stderr, "%-12s%-12s%d\n", gtid.describe(),
                  SolTask::RunStateToString(task->run_state),
                  task->cpu.valid() ? task->cpu.id() : -1);
    return true;
  });
}

void SolScheduler::DumpState(const Cpu& agent_cpu, int flags) {
  if (flags & kDumpAllTasks) {
    DumpAllTasks();
  }

  if (!(flags & kDumpStateEmptyRQ) && RunqueueEmpty()) {
    return;
  }

  fprintf(stderr, "SchedState: ");
  for (const Cpu& cpu : cpus()) {
    CpuState* cs = cpu_state(cpu);
    fprintf(stderr, "%d:", cpu.id());
    if (!cs->current) {
      fprintf(stderr, "none ");
    } else {
      Gtid gtid = cs->current->gtid;
      absl::FPrintF(stderr, "%s ", gtid.describe());
    }
  }
  fprintf(stderr, " rq_l=%ld", RunqueueSize());
  fprintf(stderr, "\n");
}

SolScheduler::CpuState* SolScheduler::cpu_state_of(const SolTask* task) {
  CHECK(task->cpu.valid());
  CHECK(task->oncpu() || task->pending());
  CpuState* cs = cpu_state(task->cpu);
  CHECK(task == cs->current || task == cs->next);
  return cs;
}

void SolScheduler::TaskNew(SolTask* task, const Message& msg) {
  const ghost_msg_payload_task_new* payload =
      static_cast<const ghost_msg_payload_task_new*>(msg.payload());

  task->seqnum = msg.seqnum();
  task->run_state = SolTask::RunState::kBlocked;

  const Gtid gtid(payload->gtid);
  if (payload->runnable) {
    task->run_state = SolTask::RunState::kRunnable;
    Enqueue(task);
  }

  num_tasks_++;
}

void SolScheduler::TaskRunnable(SolTask* task, const Message& msg) {
  const ghost_msg_payload_task_wakeup* payload =
      static_cast<const ghost_msg_payload_task_wakeup*>(msg.payload());

  CHECK(task->blocked());

  task->run_state = SolTask::RunState::kRunnable;
  task->prio_boost = !payload->deferrable;
  Enqueue(task);
}

void SolScheduler::TaskDeparted(SolTask* task, const Message& msg) {
  if (task->pending()) {
    RunRequest* req = enclave()->GetRunRequest(task->cpu);
    // Wait for txn to complete, because we might not get another message
    // against this task to asynchronously complete it. Ignore return value,
    // since SyncCpuState will check it anyway.
    enclave()->CompleteRunRequest(req);
    // Put the task either back into runqueue (txn failed) or oncpu (txn
    // completed). It will be adjusted again appropriately by the checks below.
    SyncCpuState(task->cpu);
  }

  if (task->oncpu()) {
    CpuState* cs = cpu_state_of(task);
    CHECK_EQ(cs->current, task);
    cs->current = nullptr;
  } else if (task->queued()) {
    RemoveFromRunqueue(task);
  } else {
    CHECK(task->blocked());
  }

  allocator()->FreeTask(task);
  num_tasks_--;
}

void SolScheduler::TaskDead(SolTask* task, const Message& msg) {
  CHECK_EQ(task->run_state, SolTask::RunState::kBlocked);
  allocator()->FreeTask(task);
  num_tasks_--;
}

bool SolScheduler::SyncCpuState(const Cpu& cpu) {
  CHECK(cpu.valid());
  CpuState* cs = cpu_state(cpu);
  CHECK_NE(cs->next, nullptr);
  CHECK_EQ(cs->current, nullptr);

  SolTask* next = cs->next;
  CHECK(next->pending());

  RunRequest* req = enclave()->GetRunRequest(cpu);
  CHECK(req->committed());
  cs->next = nullptr;

  CHECK(!next->preempted);

  if (req->succeeded()) {
    cs->current = next;
    next->run_state = SolTask::RunState::kOnCpu;
    next->prio_boost = false;
    return true;
  }

  next->run_state = SolTask::RunState::kRunnable;
  Enqueue(next);
  return false;
}

void SolScheduler::SyncTaskState(SolTask* task) {
  CHECK(task->pending());
  CpuState* cs = cpu_state_of(task);
  CHECK_EQ(cs->current, nullptr);
  CHECK_EQ(cs->next, task);
  CHECK(SyncCpuState(task->cpu));
}

void SolScheduler::TaskBlocked(SolTask* task, const Message& msg) {
  if (task->pending()) {
    SyncTaskState(task);
  }

  if (task->oncpu()) {
    CpuState* cs = cpu_state_of(task);
    CHECK_EQ(cs->current, task);
    CHECK_EQ(cs->next, nullptr);
    cs->current = nullptr;
  } else {
    CHECK(task->queued());
    RemoveFromRunqueue(task);
  }

  task->run_state = SolTask::RunState::kBlocked;
}

void SolScheduler::TaskPreempted(SolTask* task, const Message& msg) {
  if (task->pending()) {
    SyncTaskState(task);
  }

  task->preempted = true;

  if (task->oncpu()) {
    CpuState* cs = cpu_state_of(task);
    CHECK_EQ(cs->current, task);
    CHECK_EQ(cs->next, nullptr);
    cs->current = nullptr;
    task->run_state = SolTask::RunState::kRunnable;
    Enqueue(task);
  } else {
    CHECK(task->queued());
  }
}

void SolScheduler::TaskYield(SolTask* task, const Message& msg) {
  if (task->pending()) {
    SyncTaskState(task);
  }

  if (task->oncpu()) {
    CpuState* cs = cpu_state_of(task);
    CHECK_EQ(cs->current, task);
    CHECK_EQ(cs->next, nullptr);
    cs->current = nullptr;
    Yield(task);
  } else {
    CHECK(task->queued());
  }
}

void SolScheduler::Yield(SolTask* task) {
  // An oncpu() task can do a sched_yield() and get here via SolTaskYield().
  // We may also get here if the scheduler wants to inhibit a task from being
  // picked in the current scheduling round (see GlobalSchedule()).
  CHECK(task->oncpu() || task->runnable());
  task->run_state = SolTask::RunState::kYielding;
  yielding_tasks_.emplace_back(task);
}

void SolScheduler::Enqueue(SolTask* task) {
  CHECK_EQ(task->run_state, SolTask::RunState::kRunnable);
  task->run_state = SolTask::RunState::kQueued;
  if (task->prio_boost || task->preempted)
    run_queue_.push_front(task);
  else
    run_queue_.push_back(task);
}

SolTask* SolScheduler::Dequeue() {
  if (RunqueueEmpty()) {
    return nullptr;
  }

  SolTask* task = run_queue_.front();
  CHECK_EQ(task->run_state, SolTask::RunState::kQueued);
  task->run_state = SolTask::RunState::kRunnable;
  run_queue_.pop_front();

  return task;
}

void SolScheduler::RemoveFromRunqueue(SolTask* task) {
  CHECK(task->queued());

  for (int pos = run_queue_.size() - 1; pos >= 0; pos--) {
    // The [] operator for 'std::deque' is constant time
    if (run_queue_[pos] == task) {
      // Caller is responsible for updating 'run_state' if task is
      // no longer runnable.
      task->run_state = SolTask::RunState::kRunnable;
      run_queue_.erase(run_queue_.cbegin() + pos);
      return;
    }
  }

  // This state is unreachable because the task is queued
  CHECK(false);
}

void SolScheduler::GlobalSchedule(const StatusWord& agent_sw,
                                  StatusWord::BarrierToken agent_sw_last) {
  const int global_cpu_id = GetGlobalCPUId();
  CpuList available = topology()->EmptyCpuList();
  CpuList assigned = topology()->EmptyCpuList();

  for (const Cpu& cpu : cpus()) {
    CpuState* cs = cpu_state(cpu);
    RunRequest* req = enclave()->GetRunRequest(cpu);

    if (cpu.id() == global_cpu_id) {
      CHECK_EQ(cs->current, nullptr);
      CHECK_EQ(cs->next, nullptr);
      CHECK(req->committed());
      continue;
    }

    if (cs->current) {
      CHECK_EQ(cs->next, nullptr);
      continue;
    }

    if (cs->next) {
      if (req->committed()) {
        // Note that txn could have failed to commit in which case the
        // 'cs->next' will go back into the run queue.
        SyncCpuState(cpu);
      }
    }

    // CPU has a pending txn that we haven't reaped yet.
    if (cs->next) continue;

    if (!cs->current) available.Set(cpu);
  }

  while (!available.Empty()) {
    SolTask* next = Dequeue();
    if (!next) break;

    if (next->status_word.on_cpu() ||
        next->seqnum != next->status_word.barrier()) {
      Yield(next);
      continue;
    }

    if (!next->cpu.valid()) {
      next->cpu = available.Front();
    } else if (!available.IsSet(next->cpu)) {
      bool found = false;
      for (const Cpu& sibling : next->cpu.siblings()) {
        if (available.IsSet(sibling)) {
          found = true;
          next->cpu = sibling;
          break;
        }
      }
      if (!found) next->cpu = available.Front();
    } else {
      // previous cpu is available.
    }

    CHECK(next->cpu.valid());
    CHECK(available.IsSet(next->cpu));
    available.Clear(next->cpu);
    assigned.Set(next->cpu);

    CpuState* cs = cpu_state(next->cpu);
    CHECK_EQ(cs->next, nullptr);
    CHECK_EQ(cs->current, nullptr);

    RunRequest* req = enclave()->GetRunRequest(next->cpu);
    req->Open({
        .target = next->gtid,
        .target_barrier = next->seqnum,
    });

    cs->next = next;
    next->run_state = SolTask::RunState::kPending;
    if (next->preempted) {
      next->preempted = false;
      next->prio_boost = true;  // boosted priority if txn commit fails.
    }
  }

  // Commit on all cpus with open transactions.
  if (!assigned.Empty()) enclave()->SubmitRunRequests(assigned);

  // Yielding tasks are moved back to the runqueue having skipped one round
  // of scheduling decisions.
  if (!yielding_tasks_.empty()) {
    for (SolTask* t : yielding_tasks_) {
      CHECK_EQ(t->run_state, SolTask::RunState::kYielding);
      t->run_state = SolTask::RunState::kRunnable;
      Enqueue(t);
    }
    yielding_tasks_.clear();
  }
}

bool SolScheduler::PickNextGlobalCPU(StatusWord::BarrierToken agent_barrier,
                                     const Cpu& this_cpu) {
  Cpu target(Cpu::UninitializedType::kUninitialized);
  Cpu global_cpu = topology()->cpu(GetGlobalCPUId());
  int numa_node = global_cpu.numa_node();

  // Let's make sure we do some useful work before moving to another cpu.
  if (iterations_ & 0xff) return false;

  for (const Cpu& cpu : global_cpu.siblings()) {
    if (cpu.id() == global_cpu.id()) continue;

    if (Available(cpu)) {
      target = cpu;
      goto found;
    }
  }

  for (const Cpu& cpu : global_cpu.l3_siblings()) {
    if (cpu.id() == global_cpu.id()) continue;

    if (Available(cpu)) {
      target = cpu;
      goto found;
    }
  }

again:
  for (const Cpu& cpu : cpus()) {
    if (cpu.id() == global_cpu.id()) continue;

    if (numa_node >= 0 && cpu.numa_node() != numa_node) continue;

    if (Available(cpu)) {
      target = cpu;
      goto found;
    }
  }

  if (numa_node >= 0) {
    numa_node = -1;
    goto again;
  }

found:
  if (!target.valid()) return false;

  CHECK(target != this_cpu);

  CpuState* cs = cpu_state(target);
  RunRequest* req = enclave()->GetRunRequest(target);
  if (cs->next) {
    if (!req->Abort()) {
      enclave()->CompleteRunRequest(req);  // XXX try to avoid?
    }
    SyncCpuState(cs->next->cpu);
  }
  CHECK(req->committed());
  CHECK_EQ(cs->next, nullptr);

  SolTask* prev = cs->current;
  if (prev) {
    CHECK(prev->oncpu());

    // We ping the agent on `target` below. Once that agent wakes up, it
    // automatically preempts `prev`. The kernel generates a TASK_PREEMPT
    // message for `prev`, which allows the scheduler to update the state for
    // `prev`.
    //
    // This also allows the scheduler to gracefully handle the case where `prev`
    // actually blocks/yields/etc. before it is preempted by the agent on
    // `target`. In any of those cases, a TASK_BLOCKED/TASK_YIELD/etc. message
    // is delivered for `prev` instead of a TASK_PREEMPT, so the state is still
    // updated correctly for `prev` even if it is not preempted by the agent.
  }

  SetGlobalCPU(target);
  enclave()->GetAgent(target)->Ping();

  return true;
}

std::unique_ptr<SolScheduler> SingleThreadSolScheduler(Enclave* enclave,
                                                       CpuList cpus,
                                                       int32_t global_cpu) {
  auto allocator = std::make_shared<SingleThreadMallocTaskAllocator<SolTask>>();
  auto scheduler = absl::make_unique<SolScheduler>(
      enclave, std::move(cpus), std::move(allocator), global_cpu);
  return scheduler;
}

void SolAgent::AgentThread() {
  Channel& global_channel = global_scheduler_->GetDefaultChannel();
  gtid().assign_name("Agent:" + std::to_string(cpu().id()));
  if (verbose() > 1) {
    printf("Agent tid:=%d\n", gtid().tid());
  }
  SignalReady();
  WaitForEnclaveReady();

  PeriodicEdge debug_out(absl::Seconds(1));

  while (!Finished() || !global_scheduler_->Empty()) {
    StatusWord::BarrierToken agent_barrier = status_word().barrier();
    // Check if we're assigned as the Global agent.
    if (cpu().id() != global_scheduler_->GetGlobalCPUId()) {
      RunRequest* req = enclave()->GetRunRequest(cpu());

      if (verbose() > 1) {
        printf("Agent on cpu: %d Idled.\n", cpu().id());
      }
      req->LocalYield(agent_barrier, /*flags=*/0);
    } else {
      if (boosted_priority() &&
          global_scheduler_->PickNextGlobalCPU(agent_barrier, cpu())) {
        continue;
      }

      global_scheduler_->EnterSchedule();

      Message msg;
      while (!(msg = global_channel.Peek()).empty()) {
        global_scheduler_->DispatchMessage(msg);
        global_channel.Consume(msg);
      }

      global_scheduler_->GlobalSchedule(status_word(), agent_barrier);

      global_scheduler_->ExitSchedule();

      if (verbose() && debug_out.Edge()) {
        static const int flags =
            verbose() > 1 ? Scheduler::kDumpStateEmptyRQ : 0;
        if (global_scheduler_->debug_runqueue_) {
          global_scheduler_->debug_runqueue_ = false;
          global_scheduler_->DumpState(cpu(), Scheduler::kDumpAllTasks);
        } else {
          global_scheduler_->DumpState(cpu(), flags);
        }
      }
    }
  }
}

}  //  namespace ghost
