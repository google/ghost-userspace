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

#include "schedulers/edf/edf_scheduler.h"

#include "absl/strings/str_format.h"
#include "bpf/user/agent.h"

namespace ghost {

void EdfTask::SetRuntime(absl::Duration new_runtime,
                         bool update_elapsed_runtime) {
  CHECK_GE(new_runtime, runtime);
  if (update_elapsed_runtime) {
    elapsed_runtime += new_runtime - runtime;
  }
  runtime = new_runtime;
}

void EdfTask::UpdateRuntime() {
  // We access the runtime from the status word rather than call
  // `Ghost::GetTaskRuntime()` as that function does a system call that acquires
  // a runqueue lock if the task is currently running. Acquiring this lock harms
  // tail latency. Reading the runtime from the status word is just a memory
  // access. However, the runtime in the status word may be stale if the task is
  // currently running, as the runtime in the status word is updated on each
  // call to `update_curr_ghost()` in the kernel. Thus, we accept some staleness
  // in the runtime for improved performance.
  //
  // Note that the runtime in the status word is updated when the task is taken
  // off of the CPU (e.g., on a block, yield, preempt, etc.). Additionally,
  // `Ghost::GetTaskRuntime()` does not need to acquire a runqueue lock when the
  // task is off of the CPU.
  SetRuntime(absl::Nanoseconds(status_word.runtime()),
             /*update_elapsed_runtime=*/true);
}

void EdfTask::CalculateSchedDeadline() {
  // When a task is picked for the first time:
  //   estimated_runtime = wc->exectime
  // If elapsed_runtime > estimated_runtime, we exponentially scale
  // 'estimated_runtime' such that the EDF scheduling algorithm will favor this
  // task to run next and let it run to completion ASAP.
  while (estimated_runtime <= elapsed_runtime) {
    estimated_runtime *= 2;
  }
  CHECK_GT(estimated_runtime, elapsed_runtime);
  absl::Duration remaining_time = estimated_runtime - elapsed_runtime;
  sched_deadline = deadline - remaining_time;
  GHOST_DPRINT(4, stderr, "Gtid/%s %s deadline. sched_deadline: %s\n",
               gtid.describe(), deadline < MonotonicNow() ? "missed" : "within",
               absl::FormatTime(sched_deadline, absl::UTCTimeZone()).c_str());
}

EdfScheduler::EdfScheduler(Enclave* enclave, CpuList cpulist,
                           std::shared_ptr<TaskAllocator<EdfTask>> allocator,
                           const GlobalConfig& config)
    : BasicDispatchScheduler(enclave, std::move(cpulist), std::move(allocator)),
      global_cpu_(config.global_cpu_.id()),
      global_channel_(GHOST_MAX_QUEUE_ELEMS, /*node=*/0) {
  if (!cpus().IsSet(global_cpu_)) {
    Cpu c = cpus().Front();
    CHECK(c.valid());
    global_cpu_ = c.id();
  }

  bpf_obj_ = edf_bpf__open();
  CHECK_NE(bpf_obj_, nullptr);

  bpf_map__resize(bpf_obj_->maps.cpu_data, libbpf_num_possible_cpus());

  bpf_program__set_types(bpf_obj_->progs.edf_skip_tick,
                         BPF_PROG_TYPE_GHOST_SCHED, BPF_GHOST_SCHED_SKIP_TICK);
  bpf_program__set_types(bpf_obj_->progs.edf_send_tick,
                         BPF_PROG_TYPE_GHOST_SCHED, BPF_GHOST_SCHED_SKIP_TICK);
  bpf_program__set_types(bpf_obj_->progs.edf_pnt,
                         BPF_PROG_TYPE_GHOST_SCHED, BPF_GHOST_SCHED_PNT);
  bpf_program__set_types(bpf_obj_->progs.edf_msg_send, BPF_PROG_TYPE_GHOST_MSG,
                         BPF_GHOST_MSG_SEND);

  CHECK_EQ(edf_bpf__load(bpf_obj_), 0);

  switch (config.edf_ticks_) {
    case CpuTickConfig::kNoTicks:
      CHECK_EQ(agent_bpf_register(bpf_obj_->progs.edf_skip_tick,
                                  BPF_GHOST_SCHED_SKIP_TICK),
               0);
      break;
    case CpuTickConfig::kAllTicks:
      CHECK_EQ(agent_bpf_register(bpf_obj_->progs.edf_send_tick,
                                  BPF_GHOST_SCHED_SKIP_TICK),
               0);
      break;
    case CpuTickConfig::kTickOnRequest:
      GHOST_ERROR("Must pick kAllTicks or kNoTicks");
  }
  CHECK_EQ(agent_bpf_register(bpf_obj_->progs.edf_pnt, BPF_GHOST_SCHED_PNT),
           0);
  CHECK_EQ(agent_bpf_register(bpf_obj_->progs.edf_msg_send, BPF_GHOST_MSG_SEND),
           0);

  bpf_data_ = static_cast<struct edf_bpf_per_cpu_data*>(
      bpf_map__mmap(bpf_obj_->maps.cpu_data));
  CHECK_NE(bpf_data_, MAP_FAILED);
}

EdfScheduler::~EdfScheduler() {
  bpf_map__munmap(bpf_obj_->maps.cpu_data, bpf_data_);
  edf_bpf__destroy(bpf_obj_);
}

void EdfScheduler::EnclaveReady() {
  for (const Cpu& cpu : cpus()) {
    CpuState* cs = cpu_state(cpu);
    cs->agent = enclave()->GetAgent(cpu);
    CHECK_NE(cs->agent, nullptr);
  }
}

bool EdfScheduler::Available(const Cpu& cpu) {
  CpuState* cs = cpu_state(cpu);
  return cs->agent->cpu_avail();
}

void EdfScheduler::DumpAllTasks() {
  fprintf(stderr, "task        state       rq_pos  P\n");
  allocator()->ForEachTask([](Gtid gtid, const EdfTask* task) {
    absl::FPrintF(stderr, "%-12s%-12s%-8d%c\n", gtid.describe(),
                  EdfTask::RunStateToString(task->run_state), task->rq_pos,
                  task->prio_boost ? 'P' : '-');
    return true;
  });

  for (auto const& it : orchs_) it.second->DumpSchedParams();
}

void EdfScheduler::DumpState(const Cpu& agent_cpu, int flags) {
  if (flags & kDumpAllTasks) {
    DumpAllTasks();
  }

  if (!(flags & kDumpStateEmptyRQ) && run_queue_.empty()) {
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
  fprintf(stderr, " rq_l=%ld", run_queue_.size());
  fprintf(stderr, "\n");
}

EdfScheduler::CpuState* EdfScheduler::cpu_state_of(const EdfTask* task) {
  CHECK(task->oncpu());
  CpuState* result = &cpu_states_[task->cpu];
  CHECK_EQ(result->current, task);
  return result;
}

// Map the leader's shared memory region if we haven't already done so.
void EdfScheduler::HandleNewGtid(EdfTask* task, pid_t tgid) {
  CHECK_GE(tgid, 0);

  if (orchs_.find(tgid) == orchs_.end()) {
    auto orch = absl::make_unique<Orchestrator>();
    if (!orch->Init(tgid)) {
      // If the task's group leader has already exited and closed the PrioTable
      // fd while we are handling TaskNew, it is possible that we cannot find
      // the PrioTable.
      // Just set has_work so that we schedule this task and allow it to exit.
      // We also need to give it an sp; various places call task->sp->qos_.
      static SchedParams dummy_sp;
      task->has_work = true;
      task->sp = &dummy_sp;
      return;
    }
    auto pair = std::make_pair(tgid, std::move(orch));
    orchs_.insert(std::move(pair));
  }
}

void EdfScheduler::UpdateTaskRuntime(EdfTask* task, absl::Duration new_runtime,
                                     bool update_elapsed_runtime) {
  task->SetRuntime(new_runtime, update_elapsed_runtime);
  if (task->sp) {
    // We only call 'UpdateEdfTaskRuntime' on a task that is on a CPU, not on a
    // task that is queued (note that a queued task cannot accumulate runtime)
    CHECK(task->oncpu());
    // Only call 'CalculateSchedDeadline()' if 'task' has been associated with a
    // sched item
    task->CalculateSchedDeadline();
  } else {
    // The task is not associated with a sched item, so it should not be queued
    CHECK(!task->queued());
  }
}

void EdfScheduler::TaskNew(EdfTask* task, const Message& msg) {
  const ghost_msg_payload_task_new* payload =
      static_cast<const ghost_msg_payload_task_new*>(msg.payload());

  DCHECK_LE(payload->runtime, task->status_word.runtime());

  UpdateTaskRuntime(task, absl::Nanoseconds(payload->runtime),
                    /* update_elapsed_runtime = */ false);
  task->seqnum = msg.seqnum();
  task->run_state =
      EdfTask::RunState::kBlocked;  // Need this in the runnable case anyway.

  const Gtid gtid(payload->gtid);
  const pid_t tgid = gtid.tgid();
  HandleNewGtid(task, tgid);
  if (payload->runnable) Enqueue(task);

  num_tasks_++;

  if (!in_discovery_) {
    // Get the task's scheduling parameters (potentially updating its position
    // in the runqueue).
    auto iter = orchs_.find(tgid);
    if (iter != orchs_.end()) {
      iter->second->GetSchedParams(task->gtid, kSchedCallbackFunc);
    }
  }
}

void EdfScheduler::TaskRunnable(EdfTask* task, const Message& msg) {
  const ghost_msg_payload_task_wakeup* payload =
      static_cast<const ghost_msg_payload_task_wakeup*>(msg.payload());

  CHECK(task->blocked());

  // A non-deferrable wakeup gets the same preference as a preempted task.
  // This is because it may be holding locks or resources needed by other
  // tasks to make progress.
  task->prio_boost = !payload->deferrable;

  Enqueue(task);
}

void EdfScheduler::TaskDeparted(EdfTask* task, const Message& msg) {}

void EdfScheduler::TaskDead(EdfTask* task, const Message& msg) {
  CHECK_EQ(task->run_state,
           EdfTask::RunState::kBlocked);  // Need to schedule to exit.
  allocator()->FreeTask(task);

  num_tasks_--;
}

void EdfScheduler::TaskBlocked(EdfTask* task, const Message& msg) {
  const ghost_msg_payload_task_blocked* payload =
      reinterpret_cast<const ghost_msg_payload_task_blocked*>(msg.payload());

  DCHECK_LE(payload->runtime, task->status_word.runtime());

  // States other than the typical kOnCpu are possible here:
  // We could be kPaused if agent-initiated preemption raced with task
  // blocking (then kPaused and kQueued can move between each other via
  // SCHED_ITEM_RUNNABLE edges).
  if (task->oncpu()) {
    UpdateTaskRuntime(task, absl::Nanoseconds(payload->runtime),
                      /* update_elapsed_runtime= */ true);
    CpuState* cs = cpu_state_of(task);
    CHECK_EQ(cs->current, task);
    cs->current = nullptr;
  } else if (task->queued()) {
    RemoveFromRunqueue(task);
  } else {
    CHECK(task->paused());
  }
  task->run_state = EdfTask::RunState::kBlocked;
}

void EdfScheduler::TaskPreempted(EdfTask* task, const Message& msg) {
  const ghost_msg_payload_task_preempt* payload =
      reinterpret_cast<const ghost_msg_payload_task_preempt*>(msg.payload());

  DCHECK_LE(payload->runtime, task->status_word.runtime());

  task->preempted = true;
  task->prio_boost = true;

  // States other than the typical kOnCpu are possible here:
  // We could be kQueued from a TASK_NEW that was immediately preempted.
  // We could be kPaused if agent-initiated preemption raced with kernel
  // preemption (then kPaused and kQueued can move between each other via
  // SCHED_ITEM_RUNNABLE edges).
  if (task->oncpu()) {
    UpdateTaskRuntime(task, absl::Nanoseconds(payload->runtime),
                      /* update_elapsed_runtime= */ true);
    CpuState* cs = cpu_state_of(task);
    CHECK_EQ(cs->current, task);
    cs->current = nullptr;
    Enqueue(task);
  } else if (task->queued()) {
    // task->preempted affects position in runqueue.
    UpdateRunqueue(task);
  } else {
    CHECK(task->paused());
  }
}

void EdfScheduler::TaskYield(EdfTask* task, const Message& msg) {
  const ghost_msg_payload_task_yield* payload =
      reinterpret_cast<const ghost_msg_payload_task_yield*>(msg.payload());

  DCHECK_LE(payload->runtime, task->status_word.runtime());

  // States other than the typical kOnCpu are possible here:
  // We could be kPaused if agent-initiated preemption raced with task
  // yielding (then kPaused and kQueued can move between each other via
  // SCHED_ITEM_RUNNABLE edges).
  if (task->oncpu()) {
    UpdateTaskRuntime(task, absl::Nanoseconds(payload->runtime),
                      /* update_elapsed_runtime= */ true);
    CpuState* cs = cpu_state_of(task);
    CHECK_EQ(cs->current, task);
    cs->current = nullptr;
    Yield(task);
  } else {
    CHECK(task->queued() || task->paused());
  }
}

void EdfScheduler::DiscoveryStart() { in_discovery_ = true; }

void EdfScheduler::DiscoveryComplete() {
  for (auto& scraper : orchs_) {
    scraper.second->RefreshAllSchedParams(kSchedCallbackFunc);
  }
  in_discovery_ = false;
}

bool EdfScheduler::PreemptTask(EdfTask* prev, EdfTask* next,
                               StatusWord::BarrierToken agent_barrier) {
  GHOST_DPRINT(2, stderr, "PREEMPT(%d)\n", prev->cpu);
  Cpu cpu = topology()->cpu(prev->cpu);

  CHECK_NE(prev, nullptr);
  CHECK(prev->oncpu());

  if (prev == next) {
    return true;
  }

  CHECK(!next || !next->oncpu());

  RunRequest* req = enclave()->GetRunRequest(cpu);
  if (next) {
    req->Open({
        .target = next->gtid,
        .target_barrier = next->seqnum,
        .agent_barrier = agent_barrier,
    });

    if (!req->Commit()) {
      return false;
    }
  } else {
    req->OpenUnschedule();
    CHECK(req->Commit());
  }

  CpuState* cs = cpu_state(cpu);
  cs->current = next;
  prev->run_state = EdfTask::RunState::kPaused;

  if (next) {
    next->run_state = EdfTask::RunState::kOnCpu;
    next->cpu = prev->cpu;
    // Normally, each task's runtime is updated when a message about that task
    // is received or when the task's sched item is updated by the orchestrator.
    // Neither of those events occurs for an agent-initiated QoS-related
    // preemption, so update the runtime of 'prev' here.
    prev->UpdateRuntime();
  }
  return true;
}

void EdfScheduler::Yield(EdfTask* task) {
  // An oncpu() task can do a sched_yield() and get here via EdfTaskYield().
  // We may also get here if the scheduler wants to inhibit a task from
  // being picked in the current scheduling round (see GlobalSchedule()).
  CHECK(task->oncpu() || task->queued());
  task->run_state = EdfTask::RunState::kYielding;
  yielding_tasks_.emplace_back(task);
}

void EdfScheduler::Unyield(EdfTask* task) {
  CHECK(task->yielding());

  auto it = std::find(yielding_tasks_.begin(), yielding_tasks_.end(), task);
  CHECK(it != yielding_tasks_.end());
  yielding_tasks_.erase(it);

  Enqueue(task);
}

void EdfScheduler::Enqueue(EdfTask* task) {
  // We'll re-enqueue when this EdfTask has work to do during
  // periodic scraping of PrioTable.
  if (!task->has_work) {
    task->run_state = EdfTask::RunState::kPaused;
    return;
  }

  // push_heap.
  CHECK_LT(task->rq_pos, 0);
  task->run_state = EdfTask::RunState::kQueued;
  run_queue_.emplace_back(task);
  task->rq_pos = run_queue_.size() - 1;
  UpdateRunqueuePosition(task->rq_pos);
}

static void swap(EdfTask*& t1, EdfTask*& t2) {
  std::swap(t1, t2);
  std::swap(t1->rq_pos, t2->rq_pos);
}

EdfTask* EdfScheduler::Dequeue() {
  if (run_queue_.empty()) return nullptr;

  // pop_heap.
  swap(run_queue_.front(), run_queue_.back());
  struct EdfTask* task = run_queue_.back();
  CHECK(task->has_work);
  CHECK_EQ(task->rq_pos, run_queue_.size() - 1);
  task->rq_pos = -1;
  run_queue_.pop_back();

  if (!run_queue_.empty()) UpdateRunqueuePosition(0);

  return task;
}

EdfTask* EdfScheduler::Peek() {
  if (run_queue_.empty()) {
    return nullptr;
  }

  EdfTask* task = run_queue_.front();
  CHECK(task->has_work);
  CHECK_EQ(task->rq_pos, 0);

  return task;
}

void EdfScheduler::CheckRunQueue() {
#if GHOST_DEBUG
  // Verify that 'run_queue_' is a proper heap.
  CHECK(std::is_heap(run_queue_.begin(), run_queue_.end(),
                     EdfTask::SchedDeadlineGreater()));

  // Verify that all queued tasks have proper 'run_state' and 'rq_pos'.
  for (auto iter = run_queue_.begin(); iter != run_queue_.end(); iter++) {
    const EdfTask* task = *iter;
    CHECK(task->queued());
    CHECK_EQ(task->rq_pos, iter - run_queue_.begin());
  }
#endif
}

void EdfScheduler::UpdateRunqueuePosition(uint32_t pos) {
  // Sift up.
  if (pos) {
    uint32_t parent = (pos - 1) / 2;

    if (EdfTask::SchedDeadlineGreater()(run_queue_[parent], run_queue_[pos])) {
      do {
        swap(run_queue_[pos], run_queue_[parent]);
        pos = parent;
        parent = (pos - 1) / 2;
      } while (pos && EdfTask::SchedDeadlineGreater()(run_queue_[parent],
                                                      run_queue_[pos]));
      CheckRunQueue();
      return;
    }
  }

  // Sift down.
  while (true) {
    uint32_t left = (pos * 2) + 1;
    uint32_t right = (pos * 2) + 2;

    if (right < run_queue_.size() &&
        EdfTask::SchedDeadlineGreater()(run_queue_[left], run_queue_[right])) {
      left = right;
    }

    if (left < run_queue_.size() &&
        EdfTask::SchedDeadlineGreater()(run_queue_[pos], run_queue_[left])) {
      swap(run_queue_[pos], run_queue_[left]);
      pos = left;
    } else {
      CheckRunQueue();
      return;
    }
  }
}

void EdfScheduler::RemoveFromRunqueue(EdfTask* task) {
  CHECK(task->queued());
  CHECK_GE(task->rq_pos, 0);
  CHECK_LT(task->rq_pos, run_queue_.size());

  bool need_update;
  int pos = task->rq_pos;
  if (pos == run_queue_.size() - 1) {
    need_update = false;  // heap property unchanged by pop_back().
  } else {
    swap(run_queue_[pos], run_queue_.back());
    need_update = true;  // needs update to maintain heap property.
  }

  CHECK_EQ(task->rq_pos, run_queue_.size() - 1);
  task->rq_pos = -1;
  run_queue_.pop_back();

  if (need_update) {
    UpdateRunqueuePosition(pos);
  } else {
    CheckRunQueue();
  }
  task->run_state = EdfTask::RunState::kPaused;
}

void EdfScheduler::UpdateRunqueue(EdfTask* task) {
  CHECK(task->queued());
  CHECK_GE(task->rq_pos, 0);
  CHECK_LT(task->rq_pos, run_queue_.size());
  UpdateRunqueuePosition(task->rq_pos);
}

void EdfScheduler::SchedParamsCallback(Orchestrator& orch,
                                       const SchedParams* sp, Gtid oldgtid) {
  Gtid gtid = sp->GetGtid();

  // TODO: Get it off cpu if it is running. Assumes that oldgpid wasn't moved
  // around to a later spot in the PrioTable. Get it off the runqueue if it is
  // queued. Implement Scheduler::EraseFromRunqueue(oldgpid);
  CHECK(!oldgtid || (oldgtid == gtid));

  if (!gtid) {  // empty sched_item.
    return;
  }

  // Normally, the agent writes the Runnable bit in the PrioTable for
  // Repeatables (see Case C, below).  However, the old agent may have crashed
  // before it could set the bit, so we must do it.
  if (in_discovery_ && orch.Repeating(sp) && !sp->HasWork()) {
    orch.MakeEngineRunnable(sp);
  }

  EdfTask* task = allocator()->GetTask(gtid);
  if (!task) {
    // We are too early (i.e. haven't seen MSG_TASK_NEW for gtid) in which
    // case ignore the update for now. We'll grab the latest SchedParams
    // from shmem in the MSG_TASK_NEW handler.
    //
    // We are too late (i.e have already seen MSG_TASK_DEAD for gtid) in
    // which case we just ignore the update.
    return;
  }

  // kYielding is an ephemeral state that prevents the task from being
  // picked in this scheduling iteration (the intent is to give other
  // tasks a chance to run). This is an implementation detail and is
  // not required by sched_yield() system call.
  //
  // To simplify state transitions we undo the kYielding behavior if
  // a SchedParams update is also detected in this scheduling iteration.
  if (task->yielding()) Unyield(task);

  // Copy updated params into task->..
  const bool had_work = task->has_work;
  const uint32_t old_wcid = task->wcid;
  task->sp = sp;
  task->has_work = sp->HasWork();
  task->wcid = sp->GetWorkClass();

  if (had_work) {
    task->UpdateRuntime();
    // The runtime for the ghOSt task needs to be updated as the sched item was
    // repurposed for a different closure. We don't want to bill the new closure
    // for CPU time that was consumed by the old closure.
    CHECK_LT(task->wcid, orch.NumWorkClasses());
    orch.UpdateWorkClassStats(old_wcid, task->elapsed_runtime, task->deadline);
    task->elapsed_runtime = absl::ZeroDuration();
  }

  task->estimated_runtime = orch.EstimateRuntime(task->sp->GetWorkClass());
  if (orch.Repeating(task->sp)) {
    task->deadline =
        MonotonicNow() + orch.GetWorkClassPeriod(sp->GetWorkClass());
  } else {
    task->deadline = sp->GetDeadline();
  }
  task->CalculateSchedDeadline();

  // A kBlocked task is not affected by any changes to SchedParams.
  if (task->blocked()) {
    return;
  }

  // Case#  had_work  has_work    run_state change
  //  (a)      0         0        none
  //  (b)      0         1        kPaused -> kQueued
  //  (c)      1         0        kQueued/kOnCpu -> kPaused
  //  (d)      1         1        none
  if (!had_work) {
    CHECK(task->paused());
    if (task->has_work) {
      // For repeatables, the orchestrator indicates that it is done polling via
      // the had_work -> !has_work edge. The agent exclusively generates the
      // !had_work -> has_work edge when it is time for the orchestrator to poll
      // again. We permit the latter edge here for expediency when handling a
      // new task.
      Enqueue(task);  // case (b).
    }
    return;  // case (a).
  }

  if (had_work) {
    CHECK(!task->paused());
    if (task->has_work) {  // case (d).
      if (task->queued()) {
        // Repeatables should not change their closure, so repeatables should
        // not travel the had_work -> has_work edge
        CHECK(!orch.Repeating(task->sp));
        UpdateRunqueue(task);
      }

      // An kOnCpu task continues to run with existing SchedParams
      // until it blocks, yields or is preempted (this is a policy
      // choice). A different policy might be to preempt and then
      // schedule it based on updated SchedParams.

    } else {  // case (c).
      if (task->oncpu()) {
        CHECK(PreemptTask(task, nullptr, 0));  // force offcpu.
      } else if (task->queued()) {
        RemoveFromRunqueue(task);
      } else {
        CHECK(0);
      }
      CHECK(task->paused());
      if (orch.Repeating(task->sp)) {
        orch.MakeEngineRunnable(task->sp);
        task->has_work = true;
        Enqueue(task);
      }
    }
  }
}

void EdfScheduler::UpdateSchedParams() {
  for (auto& scraper : orchs_) {
    scraper.second->RefreshSchedParams(kSchedCallbackFunc);
  }
}

void EdfScheduler::GlobalSchedule(const StatusWord& agent_sw,
                                  StatusWord::BarrierToken agent_sw_last) {
  CpuList updated_cpus = MachineTopology()->EmptyCpuList();

  for (const Cpu& cpu : cpus()) {
    CpuState* cs = cpu_state(cpu);

    if (!Available(cpu) || cpu.id() == GetGlobalCPUId()) continue;

  again:
    if (cs->current) {
      EdfTask* peek = Peek();
      // Only preempt the current task if there is a queued task in a higher
      // QoS class
      if (peek && peek->sp->GetQoS() <= cs->current->sp->GetQoS()) {
        continue;
      }
    }
    EdfTask* to_run = Dequeue();
    if (!to_run)  // No tasks left to schedule.
      break;

    // The chosen task was preempted earlier but hasn't gotten off the
    // CPU. Make it ineligible for selection in this scheduling round.
    if (to_run->status_word.on_cpu()) {
      Yield(to_run);
      goto again;
    }

    cs->next = to_run;

    updated_cpus.Set(cpu.id());
  }

  bool in_sync = true;
  for (const Cpu& cpu : updated_cpus) {
    CpuState* cs = cpu_state(cpu);

    EdfTask* next = cs->next;
    cs->next = nullptr;

    if (cs->current == next) continue;

    if (in_sync) {  // In the !in_sync case, we'll just reschedule.
      RunRequest* req = enclave()->GetRunRequest(cpu);
      if (next) {
        if (cs->current) {
          // We are preempting a lower-priority task
          EdfTask* prev = cs->current;
          in_sync = PreemptTask(cs->current, next, agent_sw_last);
          if (in_sync) {
            // The preemption succeeded, so we need to enqueue the preempted
            // task
            Enqueue(prev);
          }
        } else {
          req->Open({.target = next->gtid, .target_barrier = next->seqnum});
          in_sync = req->Commit();
        }
      } else {
        req->OpenUnschedule();
        in_sync = req->Commit();
      }
    }

    if (next) {
      if (!in_sync) {
        // Need to requeue in the stale case.
        Enqueue(next);
      } else {
        // EdfTask latched successfully; clear state from an earlier run.
        //
        // Note that 'preempted' influences a task's run_queue position
        // so we clear it only after SyncCpu() is successful.
        cs->current = next;
        next->run_state = EdfTask::RunState::kOnCpu;
        next->cpu = cpu.id();
        next->preempted = false;
        next->prio_boost = false;
      }
    }
  }

  // Yielding tasks are moved back to the runqueue having skipped one round
  // of scheduling decisions.
  if (!yielding_tasks_.empty()) {
    for (EdfTask* t : yielding_tasks_) {
      CHECK_EQ(t->run_state, EdfTask::RunState::kYielding);
      Enqueue(t);
    }
    yielding_tasks_.clear();
  }
}

void EdfScheduler::PickNextGlobalCPU() {
  // TODO: Select CPUs more intelligently.
  for (const Cpu& cpu : cpus()) {
    if (Available(cpu) && cpu.id() != GetGlobalCPUId()) {
      CpuState* cs = cpu_state(cpu);
      EdfTask* prev = cs->current;

      if (prev) {
        CHECK(prev->oncpu());
        // Vacate CPU for running Global agent.
        bool in_sync = PreemptTask(prev, nullptr, 0);
        if (in_sync) {
          // Set 'prio_boost' to make it reschedule asap in case 'prev' is
          // holding a critical resource.
          prev->prio_boost = true;
          Enqueue(prev);
          cs->current = nullptr;
        } else {
          // Try the next CPU.
          continue;
        }
      }

      SetGlobalCPU(cpu);
      enclave()->GetAgent(cpu)->Ping();
      break;
    }
  }
}

std::unique_ptr<EdfScheduler> SingleThreadEdfScheduler(Enclave* enclave,
                                                       CpuList cpulist,
                                                       GlobalConfig& config) {
  auto allocator = std::make_shared<SingleThreadMallocTaskAllocator<EdfTask>>();
  auto scheduler = absl::make_unique<EdfScheduler>(
      enclave, std::move(cpulist), std::move(allocator), config);
  return scheduler;
}

void GlobalSatAgent::AgentThread() {
  Channel& global_channel = global_scheduler_->GetDefaultChannel();
  gtid().assign_name("Agent:" + std::to_string(cpu().id()));
  if (verbose() > 1) {
    printf("Agent tid:=%d\n", gtid().tid());
  }
  SignalReady();
  WaitForEnclaveReady();

  PeriodicEdge debug_out(absl::Seconds(1));

  while (!Finished()) {
    StatusWord::BarrierToken agent_barrier = status_word().barrier();
    // Check if we're assigned as the Global agent.
    if (cpu().id() != global_scheduler_->GetGlobalCPUId()) {
      RunRequest* req = enclave()->GetRunRequest(cpu());

      if (verbose() > 1) {
        printf("Agent on cpu: %d Idled.\n", cpu().id());
      }
      req->LocalYield(agent_barrier, /*flags=*/0);
    } else {
      if (boosted_priority()) {
        global_scheduler_->PickNextGlobalCPU();
        continue;
      }

      Message msg;
      while (!(msg = global_channel.Peek()).empty()) {
        global_scheduler_->DispatchMessage(msg);
        global_channel.Consume(msg);
      }

      // Order matters here: when a worker is PAUSED we defer the
      // preemption until GlobalSchedule() hoping to combine the
      // preemption with a remote_run.
      //
      // To restrict the visibility of this awkward state (PAUSED
      // but on_cpu) we do this immediately before GlobalSchedule().
      global_scheduler_->UpdateSchedParams();

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
