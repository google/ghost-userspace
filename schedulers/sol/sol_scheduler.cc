// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "schedulers/sol/sol_scheduler.h"

#include <memory>

#include "absl/strings/str_format.h"

namespace ghost {

void SolScheduler::CpuNotIdle(const Message& msg) { CHECK(0); }

void SolScheduler::CpuTimerExpired(const Message& msg) { CHECK(0); }

SolScheduler::SolScheduler(Enclave* enclave, CpuList cpulist,
                           std::shared_ptr<TaskAllocator<SolTask>> allocator,
                           int32_t global_cpu,
                           int32_t numa_node,
                           absl::Duration preemption_time_slice)
    : BasicDispatchScheduler(enclave, std::move(cpulist), std::move(allocator)),
      global_cpu_(global_cpu),
      global_channel_(GHOST_MAX_QUEUE_ELEMS, numa_node),
      preemption_time_slice_(preemption_time_slice) {
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

void SolScheduler::DumpStats() {
  fprintf(stderr, "\n------------------------------------------------\n");

  float t_d = absl::ToDoubleMicroseconds(dispatch_durations_total_) /
                iterations_;
  float t_t = absl::ToDoubleMicroseconds(schedule_durations_total_) /
                iterations_;
  float t_a = t_t - t_d;
  float msg_per_iter = 1.0 * nr_msgs_ / iterations_;
  float A = msg_per_iter / t_t;

  fprintf(stderr, "global iterations: %lu, nr_msgs %lu, msg/iter %.2f, "
          "total msg rate (A) %.2f\n",
          iterations_, nr_msgs_, msg_per_iter, A);
  fprintf(stderr, "T_d: %.2f, T_a: %.2f, T_t: %.2f\n", t_d, t_a, t_t);
  fprintf(stderr,
          "A/S = T_d/T_t: %.2f, Computed S: %.2f, L: %.2f\n",
          t_d / t_t, A * t_t / t_d, t_a * A);

  fprintf(stderr, "------------------------------------------------\n");
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

void SolScheduler::TaskOffCpu(SolTask* task, bool blocked, bool from_switchto) {
  if (task->oncpu()) {
    CpuState* cs = cpu_state_of(task);
    CHECK_EQ(cs->current, task);
    cs->current = nullptr;
  } else {
    CHECK(from_switchto);
    CHECK(task->blocked());
  }

  task->run_state =
      blocked ? SolTask::RunState::kBlocked : SolTask::RunState::kRunnable;
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
  const ghost_msg_payload_task_departed* payload =
      static_cast<const ghost_msg_payload_task_departed*>(msg.payload());
  bool from_switchto = payload->from_switchto;

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

  if (task->yielding()) {
    Unyield(task);
  }

  if (task->oncpu() || from_switchto) {
    TaskOffCpu(task, /*blocked=*/false, from_switchto);
  } else if (task->queued()) {
    RemoveFromRunqueue(task);
  } else {
    CHECK(task->blocked());
  }

  allocator()->FreeTask(task);
  num_tasks_--;

  // The msg exiting a switchto chain may wakeup a different agent than the
  // local CPU's agent. (See go/kcl/373024.)
  // However, since this is a global scheduler, we don't need a Ping() here.
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

  SolTask* next = cs->next;
  CHECK(next->pending());

  RunRequest* req = enclave()->GetRunRequest(cpu);
  CHECK(req->committed());
  cs->next = nullptr;

  CHECK(!next->preempted);

  if (req->succeeded()) {
    if (cs->current) {
      // `cs->current` is not a nullptr when we are trying to preempt the
      // currently running task because its preemption time slice expired.
      CHECK(cs->current->oncpu());
      cs->current->run_state = SolTask::RunState::kPreemptedByAgent;
    }
    cs->current = next;
    next->run_state = SolTask::RunState::kOnCpu;
    next->prio_boost = false;
    return true;
  }

  // The txn failed, so leave `cs->current` as is. If `cs->current` is not a
  // nullptr, then the task we tried to preempt is still running on the CPU. We
  // should put `next` back into the runqueue since it is not running.
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
  const ghost_msg_payload_task_blocked* payload =
      static_cast<const ghost_msg_payload_task_blocked*>(msg.payload());
  bool from_switchto = payload->from_switchto;

  if (task->pending()) {
    SyncTaskState(task);
  }

  if (task->oncpu() || from_switchto) {
    TaskOffCpu(task, /*blocked=*/true, from_switchto);
  } else if (task->preempted_by_agent()) {
    // Do nothing. We cannot enqueue the task since it is blocked.
  } else {
    CHECK(task->queued());
    RemoveFromRunqueue(task);
  }

  task->run_state = SolTask::RunState::kBlocked;

  // The msg exiting a switchto chain may wakeup a different agent than the
  // local CPU's agent. (See go/kcl/373024.)
  // However, since this is a global scheduler, we don't need a Ping() here.
}

void SolScheduler::TaskPreempted(SolTask* task, const Message& msg) {
  const ghost_msg_payload_task_preempt* payload =
      static_cast<const ghost_msg_payload_task_preempt*>(msg.payload());
  bool from_switchto = payload->from_switchto;

  if (task->pending()) {
    SyncTaskState(task);
  }

  task->preempted = true;

  if (task->oncpu() || from_switchto) {
    TaskOffCpu(task, /*blocked=*/false, from_switchto);
    Enqueue(task);
  } else if (task->preempted_by_agent()) {
    task->run_state = SolTask::RunState::kRunnable;
    Enqueue(task);
  } else {
    CHECK(task->queued());
  }

  // The msg exiting a switchto chain may wakeup a different agent than the
  // local CPU's agent. (See go/kcl/373024.)
  // However, since this is a global scheduler, we don't need a Ping() here.
}

void SolScheduler::TaskSwitchto(SolTask* task, const Message& msg) {
  if (task->pending()) {
    SyncTaskState(task);
  }

  TaskOffCpu(task, /*blocked=*/true, /*from_switchto=*/false);
}

void SolScheduler::TaskYield(SolTask* task, const Message& msg) {
  const ghost_msg_payload_task_yield* payload =
      static_cast<const ghost_msg_payload_task_yield*>(msg.payload());
  bool from_switchto = payload->from_switchto;

  if (task->pending()) {
    SyncTaskState(task);
  }

  if (task->oncpu() || from_switchto) {
    TaskOffCpu(task, /*blocked=*/false, from_switchto);
    Yield(task);
  } else if (task->preempted_by_agent()) {
    task->run_state = SolTask::RunState::kRunnable;
    Enqueue(task);
  } else {
    CHECK(task->queued());
  }

  // The msg exiting a switchto chain may wakeup a different agent than the
  // local CPU's agent. (See go/kcl/373024.)
  // However, since this is a global scheduler, we don't need a Ping() here.
}

void SolScheduler::Yield(SolTask* task) {
  // An oncpu() task can do a sched_yield() and get here via SolTaskYield().
  // We may also get here if the scheduler wants to inhibit a task from being
  // picked in the current scheduling round (see GlobalSchedule()).
  CHECK(task->oncpu() || task->runnable());
  task->run_state = SolTask::RunState::kYielding;
  yielding_tasks_.emplace_back(task);
}

void SolScheduler::Unyield(SolTask* task) {
  CHECK(task->yielding());

  auto it = std::find(yielding_tasks_.begin(), yielding_tasks_.end(), task);
  CHECK(it != yielding_tasks_.end());
  yielding_tasks_.erase(it);

  task->run_state = SolTask::RunState::kRunnable;
  Enqueue(task);
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
                                  BarrierToken agent_sw_last) {
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

    if (cs->next) {
      if (req->committed()) {
        // Note that txn could have failed to commit in which case the
        // 'cs->next' will go back into the run queue.
        SyncCpuState(cpu);
      } else {
        // This CPU has a pending txn that we have not reaped yet.
        continue;
      }
    }

    if (cs->current &&
        (MonotonicNow() - cs->last_commit) < preemption_time_slice_) {
      // A task is currently running on this CPU and it has not exceeded its
      // preemption time slice, so do not schedule this CPU.
      continue;
    }

    // No task is running on this CPU, so designate this CPU as available.
    available.Set(cpu);
  }

  while (!available.Empty()) {
    SolTask* next = Dequeue();
    if (!next) {
      break;
    }

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
      if (!found) {
        next->cpu = available.Front();
      }
    } else {
      // The previous CPU is available.
    }

    CHECK(next->cpu.valid());
    CHECK(available.IsSet(next->cpu));
    available.Clear(next->cpu);
    assigned.Set(next->cpu);

    CpuState* cs = cpu_state(next->cpu);
    CHECK_EQ(cs->next, nullptr);

    RunRequest* req = enclave()->GetRunRequest(next->cpu);
    req->Open({
        .target = next->gtid,
        .target_barrier = next->seqnum,
    });

    cs->next = next;
    next->run_state = SolTask::RunState::kPending;
    if (next->preempted) {
      next->preempted = false;
      next->prio_boost = true;  // Boosted priority if txn commit fails.
    }
  }

  // Commit on all CPUs with open transactions. We may preempt currently running
  // tasks if their time slice has expired. The ghOSt kernel will deliver a
  // TASK_PREEMPT message for those preempted tasks, so we do not need to put
  // those tasks back onto the runqueue here.
  if (!assigned.Empty()) {
    enclave()->SubmitRunRequests(assigned);
    absl::Time now = MonotonicNow();
    for (const Cpu& cpu : assigned) {
      cpu_state(cpu)->last_commit = now;
    }
  }

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

bool SolScheduler::PickNextGlobalCPU(BarrierToken agent_barrier,
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

std::unique_ptr<SolScheduler> SingleThreadSolScheduler(
    Enclave* enclave, CpuList cpulist, int32_t global_cpu,
    int32_t numa_node, absl::Duration preemption_time_slice) {
  auto allocator = std::make_shared<SingleThreadMallocTaskAllocator<SolTask>>();
  auto scheduler = std::make_unique<SolScheduler>(
      enclave, std::move(cpulist), std::move(allocator), global_cpu,
      numa_node, preemption_time_slice);
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
    BarrierToken agent_barrier = status_word().barrier();
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

      global_scheduler_->EnterDispatch();
      Message msg;
      uint64_t nr_msgs = 0;
      while (!(msg = global_channel.Peek()).empty()) {
        global_scheduler_->DispatchMessage(msg);
        global_channel.Consume(msg);
        nr_msgs++;
      }
      global_scheduler_->ExitDispatch(nr_msgs);

      global_scheduler_->GlobalSchedule(status_word(), agent_barrier);

      global_scheduler_->ExitSchedule();

      if (global_scheduler_->dump_stats_) {
        global_scheduler_->dump_stats_ = false;
        global_scheduler_->DumpStats();
      }

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
