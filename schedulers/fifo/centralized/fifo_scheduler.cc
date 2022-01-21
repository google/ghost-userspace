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

#include "schedulers/fifo/centralized/fifo_scheduler.h"

#include "absl/strings/str_format.h"

namespace ghost {

void FifoScheduler::CpuNotIdle(const Message& msg) { CHECK(0); }

void FifoScheduler::CpuTimerExpired(const Message& msg) { CHECK(0); }

FifoScheduler::FifoScheduler(Enclave* enclave, CpuList cpulist,
                             std::shared_ptr<TaskAllocator<FifoTask>> allocator,
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

FifoScheduler::~FifoScheduler() {}

void FifoScheduler::EnclaveReady() {
  for (const Cpu& cpu : cpus()) {
    CpuState* cs = cpu_state(cpu);
    cs->agent = enclave()->GetAgent(cpu);
    CHECK_NE(cs->agent, nullptr);
  }
}

bool FifoScheduler::Available(const Cpu& cpu) {
  CpuState* cs = cpu_state(cpu);

  if (cs->agent) return cs->agent->cpu_avail();

  return false;
}

void FifoScheduler::ValidatePreExitState() {
  CHECK_EQ(num_tasks_, 0);
  CHECK_EQ(RunqueueSize(), 0);
}

void FifoScheduler::DumpAllTasks() {
  fprintf(stderr, "task        state       rq_pos  P\n");
  allocator()->ForEachTask([](Gtid gtid, const FifoTask* task) {
    absl::FPrintF(stderr, "%-12s%-12s%d\n", gtid.describe(),
                  FifoTask::RunStateToString(task->run_state),
                  task->cpu.valid() ? task->cpu.id() : -1);
    return true;
  });
}

void FifoScheduler::DumpState(const Cpu& agent_cpu, int flags) {
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

FifoScheduler::CpuState* FifoScheduler::cpu_state_of(const FifoTask* task) {
  CHECK(task->cpu.valid());
  CHECK(task->oncpu());
  CpuState* cs = cpu_state(task->cpu);
  CHECK(task == cs->current);
  return cs;
}

void FifoScheduler::TaskNew(FifoTask* task, const Message& msg) {
  const ghost_msg_payload_task_new* payload =
      static_cast<const ghost_msg_payload_task_new*>(msg.payload());

  task->seqnum = msg.seqnum();
  task->run_state = FifoTask::RunState::kBlocked;

  const Gtid gtid(payload->gtid);
  if (payload->runnable) {
    task->run_state = FifoTask::RunState::kRunnable;
    Enqueue(task);
  }

  num_tasks_++;
}

void FifoScheduler::TaskRunnable(FifoTask* task, const Message& msg) {
  const ghost_msg_payload_task_wakeup* payload =
      static_cast<const ghost_msg_payload_task_wakeup*>(msg.payload());

  CHECK(task->blocked());

  task->run_state = FifoTask::RunState::kRunnable;
  task->prio_boost = !payload->deferrable;
  Enqueue(task);
}

void FifoScheduler::TaskDeparted(FifoTask* task, const Message& msg) {
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

void FifoScheduler::TaskDead(FifoTask* task, const Message& msg) {
  CHECK_EQ(task->run_state, FifoTask::RunState::kBlocked);
  allocator()->FreeTask(task);
  num_tasks_--;
}

void FifoScheduler::TaskBlocked(FifoTask* task, const Message& msg) {
  if (task->oncpu()) {
    CpuState* cs = cpu_state_of(task);
    CHECK_EQ(cs->current, task);
    cs->current = nullptr;
  } else {
    CHECK(task->queued());
    RemoveFromRunqueue(task);
  }

  task->run_state = FifoTask::RunState::kBlocked;
}

void FifoScheduler::TaskPreempted(FifoTask* task, const Message& msg) {
  task->preempted = true;

  if (task->oncpu()) {
    CpuState* cs = cpu_state_of(task);
    CHECK_EQ(cs->current, task);
    cs->current = nullptr;
    task->run_state = FifoTask::RunState::kRunnable;
    Enqueue(task);
  } else {
    CHECK(task->queued());
  }
}

void FifoScheduler::TaskYield(FifoTask* task, const Message& msg) {
  if (task->oncpu()) {
    CpuState* cs = cpu_state_of(task);
    CHECK_EQ(cs->current, task);
    cs->current = nullptr;
    Yield(task);
  } else {
    CHECK(task->queued());
  }
}

void FifoScheduler::Yield(FifoTask* task) {
  // An oncpu() task can do a sched_yield() and get here via TaskYield().
  // We may also get here if the scheduler wants to inhibit a task from being
  // picked in the current scheduling round (see GlobalSchedule()).
  CHECK(task->oncpu() || task->runnable());
  task->run_state = FifoTask::RunState::kYielding;
  yielding_tasks_.emplace_back(task);
}

void FifoScheduler::Enqueue(FifoTask* task) {
  CHECK_EQ(task->run_state, FifoTask::RunState::kRunnable);
  task->run_state = FifoTask::RunState::kQueued;
  if (task->prio_boost || task->preempted) {
    run_queue_.push_front(task);
  } else {
    run_queue_.push_back(task);
  }
}

FifoTask* FifoScheduler::Dequeue() {
  if (RunqueueEmpty()) {
    return nullptr;
  }

  FifoTask* task = run_queue_.front();
  CHECK_EQ(task->run_state, FifoTask::RunState::kQueued);
  task->run_state = FifoTask::RunState::kRunnable;
  run_queue_.pop_front();

  return task;
}

void FifoScheduler::RemoveFromRunqueue(FifoTask* task) {
  CHECK(task->queued());

  for (int pos = run_queue_.size() - 1; pos >= 0; pos--) {
    // The [] operator for 'std::deque' is constant time
    if (run_queue_[pos] == task) {
      // Caller is responsible for updating 'run_state' if task is
      // no longer runnable.
      task->run_state = FifoTask::RunState::kRunnable;
      run_queue_.erase(run_queue_.cbegin() + pos);
      return;
    }
  }

  // This state is unreachable because the task is queued.
  CHECK(false);
}

bool FifoScheduler::PreemptTask(FifoTask* prev, FifoTask* next,
                                StatusWord::BarrierToken agent_barrier) {
  GHOST_DPRINT(2, stderr, "PREEMPT(%d)\n", prev->cpu.id());

  CHECK_NE(prev, nullptr);
  CHECK(prev->oncpu());

  if (prev == next) {
    return true;
  }

  CHECK(!next || !next->oncpu());

  CpuState* cs = cpu_state(prev->cpu);
  CHECK_NE(cs->current, nullptr);

  RunRequest* req = enclave()->GetRunRequest(prev->cpu);
  if (next) {
    req->Open({.target = next->gtid,
               .target_barrier = next->seqnum,
               .agent_barrier = agent_barrier,
               .commit_flags = COMMIT_AT_TXN_COMMIT});

    if (!req->Commit()) {
      return false;
    }
  } else {
    req->OpenUnschedule();
    CHECK(req->Commit());
  }

  cs->current = next;

  if (next) {
    TaskOnCpu(next, prev->cpu);
  }
  return true;
}

void FifoScheduler::TaskOnCpu(FifoTask* task, const Cpu& cpu) {
  CpuState* cs = cpu_state(cpu);
  CHECK_EQ(task, cs->current);

  GHOST_DPRINT(3, stderr, "Task %s oncpu %d", task->gtid.describe(), cpu.id());

  task->run_state = FifoTask::RunState::kOnCpu;
  task->cpu = cpu;
  task->preempted = false;
  task->prio_boost = false;
}

void FifoScheduler::GlobalSchedule(const StatusWord& agent_sw,
                                   StatusWord::BarrierToken agent_sw_last) {
  const int global_cpu_id = GetGlobalCPUId();
  CpuList available = topology()->EmptyCpuList();
  CpuList assigned = topology()->EmptyCpuList();

  for (const Cpu& cpu : cpus()) {
    CpuState* cs = cpu_state(cpu);

    if (cpu.id() == global_cpu_id) {
      CHECK_EQ(cs->current, nullptr);
      continue;
    }

    if (cs->current) {
      // This CPU is currently running a task, so do not schedule a different
      // task on it.
      continue;
    }
    // No task is running on this CPU, so designate this CPU as available.
    available.Set(cpu);
  }

  while (!available.Empty()) {
    FifoTask* next = Dequeue();
    if (!next) {
      break;
    }

    // If `next->status_word.on_cpu()` is true, then `next` was previously
    // preempted by this scheduler but hasn't been moved off the CPU it was
    // previously running on yet.
    //
    // If `next->seqnum != next->status_word.barrier()` is true, then there are
    // pending messages for `next` that we have not read yet. Thus, do not
    // schedule `next` since we need to read the messages. We will schedule
    // `next` in a future iteration of the global scheduling loop.
    if (next->status_word.on_cpu() ||
        next->seqnum != next->status_word.barrier()) {
      Yield(next);
      continue;
    }

    // Assign `next` to run on the CPU at the front of `available`.
    const Cpu& next_cpu = available.Front();
    CpuState* cs = cpu_state(next_cpu);
    cs->current = next;
    available.Clear(next_cpu);
    assigned.Set(next_cpu);

    RunRequest* req = enclave()->GetRunRequest(next_cpu);
    req->Open({.target = next->gtid,
               .target_barrier = next->seqnum,
               // No need to set `agent_barrier` because the agent barrier is
               // not checked when a global agent is scheduling a CPU other than
               // the one that the global agent is currently running on.
               .commit_flags = COMMIT_AT_TXN_COMMIT});
  }

  // Commit on all CPUs with open transactions.
  if (!assigned.Empty()) {
    enclave()->CommitRunRequests(assigned);
  }
  for (const Cpu& next_cpu : assigned) {
    CpuState* cs = cpu_state(next_cpu);
    RunRequest* req = enclave()->GetRunRequest(next_cpu);
    if (req->succeeded()) {
      // The transaction succeeded and `next` is running on `next_cpu`.
      TaskOnCpu(cs->current, next_cpu);
    } else {
      GHOST_DPRINT(3, stderr, "FifoSchedule: commit failed (state=%d)",
                   req->state());

      // The transaction commit failed so push `next` to the front of runqueue.
      cs->current->prio_boost = true;
      Enqueue(cs->current);
      // The task failed to run on `next_cpu`, so clear out `cs->current`.
      cs->current = nullptr;
    }
  }

  // Yielding tasks are moved back to the runqueue having skipped one round
  // of scheduling decisions.
  if (!yielding_tasks_.empty()) {
    for (FifoTask* t : yielding_tasks_) {
      CHECK_EQ(t->run_state, FifoTask::RunState::kYielding);
      t->run_state = FifoTask::RunState::kRunnable;
      Enqueue(t);
    }
    yielding_tasks_.clear();
  }
}

bool FifoScheduler::PickNextGlobalCPU(StatusWord::BarrierToken agent_barrier,
                                      const Cpu& this_cpu) {
  Cpu target(Cpu::UninitializedType::kUninitialized);
  Cpu global_cpu = topology()->cpu(GetGlobalCPUId());
  int numa_node = global_cpu.numa_node();

  // Let's make sure we do some useful work before moving to another CPU.
  if (iterations_ & 0xff) {
    return false;
  }

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
  FifoTask* prev = cs->current;
  if (prev) {
    CHECK(prev->oncpu());

    // Vacate CPU for running Global agent.
    CHECK(PreemptTask(prev, nullptr, 0));
    CHECK_EQ(cs->current, nullptr);

    // Set 'prio_boost' to make it reschedule asap in case 'prev' is
    // holding a critical resource.
    prev->prio_boost = true;
    prev->run_state = FifoTask::RunState::kRunnable;
    Enqueue(prev);
  }

  SetGlobalCPU(target);
  enclave()->GetAgent(target)->Ping();

  return true;
}

std::unique_ptr<FifoScheduler> SingleThreadFifoScheduler(Enclave* enclave,
                                                         CpuList cpulist,
                                                         int32_t global_cpu) {
  auto allocator =
      std::make_shared<SingleThreadMallocTaskAllocator<FifoTask>>();
  auto scheduler = absl::make_unique<FifoScheduler>(
      enclave, std::move(cpulist), std::move(allocator), global_cpu);
  return scheduler;
}

void FifoAgent::AgentThread() {
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

      Message msg;
      while (!(msg = global_channel.Peek()).empty()) {
        global_scheduler_->DispatchMessage(msg);
        global_channel.Consume(msg);
      }

      global_scheduler_->GlobalSchedule(status_word(), agent_barrier);

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
