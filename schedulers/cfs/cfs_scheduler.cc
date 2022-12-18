// Copyright 2022 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "schedulers/cfs/cfs_scheduler.h"

#include <sys/timerfd.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_format.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "kernel/ghost_uapi.h"
#include "lib/agent.h"
#include "lib/logging.h"
#include "lib/topology.h"

#define DPRINT_CFS(level, message)                                           \
  do {                                                                       \
    if (ABSL_PREDICT_TRUE(verbose() < level)) break;                         \
    std::cerr << absl::StrFormat("DCFS: [%.6f] cpu %d: ",                    \
                                 absl::ToDoubleSeconds(absl::Now() - start), \
                                 sched_getcpu())                             \
              << message << std::endl;                                       \
  } while (0)

namespace ghost {

void PrintDebugTaskMessage(std::string message_name, CpuState* cs,
                           CfsTask* task) {
  DPRINT_CFS(2, absl::StrFormat(
                    "[%s]: %s with state %s, %scurrent", message_name,
                    task->gtid.describe(),
                    absl::FormatStreamed(CfsTaskState(task->run_state.Get())),
                    (cs && cs->current == task) ? "" : "!"));
}

CfsScheduler::CfsScheduler(Enclave* enclave, CpuList cpulist,
                           std::shared_ptr<TaskAllocator<CfsTask>> allocator,
                           absl::Duration min_granularity,
                           absl::Duration latency)
    : BasicDispatchScheduler(enclave, std::move(cpulist), std::move(allocator)),
      min_granularity_(min_granularity),
      latency_(latency) {
  for (const Cpu& cpu : cpus()) {
    CpuState* cs = cpu_state(cpu);

    // CfsRq has a default constructor, meaning these parameters will initially
    // be set to 0, so set them to the correct value.
    {
      absl::MutexLock l(&cs->run_queue.mu_);
      cs->run_queue.SetMinGranularity(min_granularity_);
      cs->run_queue.SetLatency(latency_);
    }

    cs->channel = enclave->MakeChannel(GHOST_MAX_QUEUE_ELEMS, cpu.numa_node(),
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
    absl::FPrintF(stderr, "%-12s%-8d%-8d\n", gtid.describe(),
                  task->run_state.Get(), task->cpu);
    return true;
  });
}

void CfsScheduler::DumpState(const Cpu& cpu, int flags) {
  if (flags & Scheduler::kDumpAllTasks) {
    DumpAllTasks();
  }

  CpuState* cs = cpu_state(cpu);
  CfsRq* rq = &cs->run_queue;

  {
    absl::MutexLock l(&cs->run_queue.mu_);
    if (!(flags & Scheduler::kDumpStateEmptyRQ) && !cs->current &&
        cs->run_queue.Empty()) {
      return;
    }

    const CfsTask* current = cs->current;
    // TODO: Convert the rest of the FPrintFs and GHOST_DPRINTs to
    // DPRINT_CFS.
    absl::FPrintF(stderr, "SchedState[%d]: %s rq_l=%lu\n", cpu.id(),
                  current ? current->gtid.describe() : "none", rq->Size());
  }
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

// The in kernel SelectTaskRq attempts to do the following:
// - If sched_energy_enabled(), find an energy efficient CPU (not applicable to
// us)
// - If the affine flag is set, walks up the sched domain tree to see if we can
// find a cpu in the same domain as our previous cpu, but that will allow us to
// run sonner
// - If the above two fail, then we find the idlest cpu within the highest level
// sched domain assuming the sd_flag is on
// - If the above fails, we try to find the most idle core inside the same LLC
// assuming WF_TTWU is set
// - Otherwise fallback to the old cpu
// Our CFS agent has no notion of energy efficiency or scheduling domaims. So,
// we can simplify our algorithm to:
// - Check if the current CPU is idle, if so, place it there (this avoids a
// ping)
// - Otherwise, check if our prev_cpu is idle.
// - Otherwise, try to find an idle CPU in the L3 sibiling list of our prev_cpu
// - Otherwise, just use the least utilized CPU
// In general, there are many, many, many heuristic in kernel CFS, so I tried to
// just grab the general idea and translate it to ghost code. In the future, we
// will probably end up tweaking this code.
// TODO: We probably want to favor placing in a L3 cache sibling even if
// there is no idle sibling. To do this, we can introduce a load_bias variable,
// where we consider < load_bias load to be idle.
// TODO: Collect some data about placing on this cpu if idle vs an idle
// L3 sibling.
// TODO: Once we add nice values and possibly a cgroup interface, we
// need to update our load calculating logic from .Size() to something more
// robust.
// NOTE: This is inherently racy, since we only synchronize on individual rq's
// we are not guaranteed to see a consistent view of rq loads.
Cpu CfsScheduler::SelectTaskRq(CfsTask* task, const CpuList& eligible_cpus) {
  PrintDebugTaskMessage("SelectTaskRq", nullptr, task);

  uint64_t min_load = UINT64_MAX;
  Cpu min_load_cpu = topology()->cpu(MyCpu());

  // Updates the min cpu load variables and returns true if empty.
  auto update_min = [&min_load, &min_load_cpu, &eligible_cpus](uint64_t this_load,
                                               const Cpu& this_cpu) {
    if (eligible_cpus.IsSet(this_cpu)) {
      if (min_load >= this_load) {
        min_load = this_load;
        min_load_cpu = this_cpu;
      }
      return this_load == 0;
    }
    return false;
  };

  // Check if this cpu is empty.
  // NOTE: placing on this cpu is safe as it is in cpus() by virtue of
  // us recieving a message on its queue
  const Cpu this_cpu = topology()->cpu(MyCpu());
  CpuState* cs = cpu_state(this_cpu);
  {
    absl::MutexLock l(&cs->run_queue.mu_);
    if (update_min(cs->run_queue.Size(), this_cpu)) {
      return this_cpu;
    }
  }

  // Check our prev cpu and its siblings
  // NOTE: placing on this cpu is safe as it is in cpus() by virtue of
  // it being a valid cpu beforehand.
  if (task->cpu >= 0) {
    // Check if prev cpu is empty.
    const Cpu prev_cpu = topology()->cpu(task->cpu);
    cs = cpu_state(prev_cpu);
    {
      absl::MutexLock l(&cs->run_queue.mu_);
      if (update_min(cs->run_queue.Size(), prev_cpu)) {
        return prev_cpu;
      }
    }

    // Check if we can find an idle l3 sibling.
    for (const Cpu& cpu : prev_cpu.l3_siblings()) {
      // We can't schedule on this cpu.
      if (!cpus().IsSet(cpu)) continue;
      cs = cpu_state(cpu);
      {
        absl::MutexLock l(&cs->run_queue.mu_);
        if (update_min(cs->run_queue.Size(), cpu)) {
          return cpu;
        }
      }
    }
  }

  // Check if we can find any idle cpu.
  for (const Cpu& cpu : cpus()) {
    cs = cpu_state(cpu);
    {
      absl::MutexLock l(&cs->run_queue.mu_);
      if (update_min(cs->run_queue.Size(), cpu)) {
        return cpu;
      }
    }
  }

  // We couldn't find an idle cpu, so just use the least loaded one.
  return min_load_cpu;
}

void CfsScheduler::Migrate(CfsTask* task, Cpu cpu, BarrierToken seqnum) {
  // The task is not visible to anyone except the agent currently proccessing
  // the task as the only way to get to migrate is if the task is not
  // currently on a rq, so it would be impossible for anyone else to touch the
  // task.
  //
  // In the future, when we add load balancing or work stealing, it still would
  // only be possible to "see" the task once it is on a rq as there is not a
  // place where we modify task state outside of getting a pointer to it from an
  // rq. Since it isn't on the rq yet, there is not anyway for anything else to
  // modify the task state at the same time.
  //
  // Even if we got a task_departed message at the same time as we are executing
  // Migrate, there is no channel associated with the task yet, so it'll go to
  // the default channel, which is proccessed by the agent executing Migrate.
  CpuState* cs = cpu_state(cpu);
  const Channel* channel = cs->channel.get();

  // There is a dangerous interleaving where we hang inside AssociateTask, after
  // changing the task's queue from the current CPU A to the target CPU B (the
  // one we are migrating to). Once this happens, we will recieve messages on
  // the new queue. Then, we recieve a TaskDeparted messaged, which deletes the
  // task on another CPU. This leads to a use-after-free bug on the task in
  // question. To avoid those, lock the entire reference to task.
  {
    absl::MutexLock l(&cs->run_queue.mu_);
    if (!channel->AssociateTask(task->gtid, seqnum, /*status=*/nullptr)) {
      GHOST_DPRINT(3, stderr,
                   "Could not associate task %s to cpu %d. This is only "
                   "correct if a TaskDeparted message follows.",
                   task->gtid.describe(), cpu.id());
      return;
    }

    GHOST_DPRINT(3, stderr, "Migrating task %s to cpu %d",
                 task->gtid.describe(), cpu.id());
    task->cpu = cpu.id();

    cs->run_queue.EnqueueTask(task);
  }

  // Get the agent's attention so it notices the new task.
  PingCpu(cpu);
}

void CfsScheduler::TaskNew(CfsTask* task, const Message& msg) {
  const ghost_msg_payload_task_new* payload =
      static_cast<const ghost_msg_payload_task_new*>(msg.payload());

  PrintDebugTaskMessage("TaskNew", nullptr, task);

  CpuList eligible_cpus = cpus();

  task->seqnum = msg.seqnum();

  // Our task does not have an rq assigned to it yet, so we do not need to hold
  // an rq lock to set the state.
  task->run_state.Set(CfsTaskState::kBlocked);

  if (payload->runnable) {
    Cpu cpu = SelectTaskRq(task, eligible_cpus);
    Migrate(task, cpu, msg.seqnum());
  } else {
    // Wait until task becomes runnable to avoid race between migration
    // and MSG_TASK_WAKEUP showing up on the default channel.
  }
}

void CfsScheduler::TaskRunnable(CfsTask* task, const Message& msg) {
  PrintDebugTaskMessage(
      "TaskRunnable",
      cpu_state(topology()->cpu(task->cpu >= 0 ? task->cpu : sched_getcpu())),
      task);

  CpuList eligible_cpus = cpus();

  // If this is our current task, then we will defer its proccessing until
  // PickNextTask. Otherwise, use the normal wakeup logic.
  if (task->cpu >= 0) {
    CpuState* cs = cpu_state_of(task);
    if (cs->current == task) {
      absl::MutexLock l(&cs->run_queue.mu_);
      cs->current = nullptr;
    }
  }

  Cpu cpu = SelectTaskRq(task, eligible_cpus);
  Migrate(task, cpu, msg.seqnum());
}

void CfsScheduler::HandleTaskDone(CfsTask* task, bool from_switchto) {
  CpuState* cs = cpu_state_of(task);
  // We might pair the state transition with pulling task of its rq, so lock
  // it. If we don't, we run the risk of the following race: CPU 1:
  // TaskRunnable(T1) CPU 1: T1->state = runnable CPU 5: TaskDeparted(T1) CPU
  // 5: rq->erase(T1) - bad because T1 has not been inserted into the rq yet.
  absl::MutexLock l(&cs->run_queue.mu_);
  CfsTaskState::State prev_state = task->run_state.Get();
  task->run_state.Set(CfsTaskState::kDone);

  if ((prev_state == CfsTaskState::kRunning || from_switchto) ||
      prev_state == CfsTaskState::kRunnable ||
      prev_state == CfsTaskState::kBlocked) {
    if (cs->current != task) {
      // Remove from the rq and free it.
      cs->run_queue.Erase(task);
      allocator()->FreeTask(task);
      cs->run_queue.UpdateMinVruntime(cs);
    }
    // if cs->current == task, then we will take care of it in PickNextTask.
  } else {
    // Our assertion in ->run_state.Set(), should keep this from every
    // happening.
    DPRINT_CFS(1, absl::StrFormat(
                      "TaskDeparted/Dead cases were not exhaustive, got %s",
                      absl::FormatStreamed(CfsTaskState(prev_state))));
  }
}

void CfsScheduler::TaskDeparted(CfsTask* task, const Message& msg) {
  const ghost_msg_payload_task_departed* payload =
      static_cast<const ghost_msg_payload_task_departed*>(msg.payload());
  PrintDebugTaskMessage("TaskDeparted", cpu_state_of(task), task);

  HandleTaskDone(task, payload->from_switchto);

  if (payload->from_switchto) {
    Cpu cpu = topology()->cpu(payload->cpu);
    PingCpu(cpu);
  }
}

void CfsScheduler::TaskDead(CfsTask* task, const Message& msg) {
  PrintDebugTaskMessage("TaskDead", cpu_state_of(task), task);
  HandleTaskDone(task, false);
}

void CfsScheduler::TaskYield(CfsTask* task, const Message& msg) {
  const ghost_msg_payload_task_yield* payload =
      static_cast<const ghost_msg_payload_task_yield*>(msg.payload());
  Cpu cpu = topology()->cpu(payload->cpu);
  CpuState* cs = cpu_state(cpu);
  PrintDebugTaskMessage("TaskYield", cs, task);

  CHECK_EQ(cs->current, task);
  {
    absl::MutexLock l(&cs->run_queue.mu_);
    task->run_state.Set(CfsTaskState::kRunnable);  // Setting to runnable will
                                                   // trigger a PutPrevTask.
  }

  if (payload->from_switchto) {
    Cpu cpu = topology()->cpu(payload->cpu);
    PingCpu(cpu);
  }
}

void CfsScheduler::TaskBlocked(CfsTask* task, const Message& msg) {
  const ghost_msg_payload_task_blocked* payload =
      static_cast<const ghost_msg_payload_task_blocked*>(msg.payload());
  Cpu cpu = topology()->cpu(payload->cpu);
  CpuState* cs = cpu_state(cpu);
  PrintDebugTaskMessage("TaskBlocked", cs, task);

  CHECK_EQ(cs->current, task);

  {
    absl::MutexLock l(&cs->run_queue.mu_);
    task->run_state.Set(CfsTaskState::kBlocked);
  }

  if (payload->from_switchto) {
    Cpu cpu = topology()->cpu(payload->cpu);
    PingCpu(cpu);
  }
}

void CfsScheduler::TaskPreempted(CfsTask* task, const Message& msg) {
  const ghost_msg_payload_task_preempt* payload =
      static_cast<const ghost_msg_payload_task_preempt*>(msg.payload());
  Cpu cpu = topology()->cpu(payload->cpu);
  CpuState* cs = cpu_state(cpu);
  PrintDebugTaskMessage("TaskPreempted", cs, task);

  // Skip the check in case cs->current == nullptr. TaskPreempted
  // invoked after the task has been removed from the RQ.
  if (cs->current) {
    CHECK_EQ(cs->current, task);
  }

  // no-op. the task doesn't change any state.

  if (payload->from_switchto) {
    Cpu cpu = topology()->cpu(payload->cpu);
    PingCpu(cpu);
  }
}

void CfsScheduler::TaskSwitchto(CfsTask* task, const Message& msg) {
  PrintDebugTaskMessage("TaskSwitchTo", cpu_state_of(task), task);
  CpuState* cs = cpu_state_of(task);

  {
    absl::MutexLock l(&cs->run_queue.mu_);
    task->run_state.Set(CfsTaskState::kBlocked);
  }
}

void CfsScheduler::ValidatePreExitState() {
  for (const Cpu& cpu : cpus()) {
    CpuState* cs = cpu_state(cpu);
    {
      absl::MutexLock l(&cs->run_queue.mu_);
      CHECK(cs->run_queue.Empty());
    }
  }
}

void CfsScheduler::CheckPreemptTick(const Cpu& cpu) {
  CpuState* cs = cpu_state(cpu);
  if (cs->current) {
    absl::MutexLock l(&cs->run_queue.mu_);
    // If we were on cpu, check if we have run for longer than
    // Granularity(). If so, force picking another task via setting current
    // to nullptr.
    if (absl::Nanoseconds(cs->current->status_word.runtime() -
                          cs->current->runtime_at_first_pick_ns) >
        cs->run_queue.MinPreemptionGranularity()) {
      cs->preempt_curr = true;
    }
  }
}

void CfsScheduler::CpuTick(const Message& msg) {
  const ghost_msg_payload_cpu_tick* payload =
      static_cast<const ghost_msg_payload_cpu_tick*>(msg.payload());
  // We do not actually need any logic in CpuTick for preemption. Since
  // CpuTick messages wake up the agent, CfsSchedule will eventually be
  // called, which contains the logic for figuring out if we should run the
  // task that was running before we got preempted the agent or if we should
  // reach into our rb tree.
  CheckPreemptTick(topology()->cpu(payload->cpu));
}

void CfsScheduler::CfsSchedule(const Cpu& cpu, BarrierToken agent_barrier,
                               bool prio_boost) {
  RunRequest* req = enclave()->GetRunRequest(cpu);
  CpuState* cs = cpu_state(cpu);

  CfsTask* prev = cs->current;

  if (prio_boost) {
    // If we are currently running a task, we need to put it back onto the
    // queue.
    if (prev) {
      absl::MutexLock l(&cs->run_queue.mu_);
      switch (prev->run_state.Get()) {
        case CfsTaskState::kNumStates:
          CHECK(false);
          break;
        case CfsTaskState::kBlocked:
          break;
        case CfsTaskState::kDone:
          cs->run_queue.Erase(prev);
          allocator()->FreeTask(prev);
          break;
        case CfsTaskState::kRunnable:
          // This case exclusively handles a task yield:
          // - TaskYield: task->state goes from kRunning -> kRunnable
          // - PickNextTask: we need to put the task back in the rq.
          cs->run_queue.PutPrevTask(prev);
          break;
        case CfsTaskState::kRunning:
          cs->run_queue.PutPrevTask(prev);
          prev->run_state.Set(CfsTaskState::kRunnable);
          break;
      }

      cs->preempt_curr = false;
      cs->current = nullptr;
      cs->run_queue.UpdateMinVruntime(cs);
    }
    // If we are prio_boost'ed, then we are temporarily running at a higher
    // priority than (kernel) CFS. The purpose of this is so that we can
    // reconcile our state with the fact that any task we wanted to be running
    // on the CPU will no longer be running. In our case, since we only sync
    // up our CpuState in PickNextTask, we can simply RTLA yield. This works
    // because:
    // - We get prio_boosted
    // - We rtla yield
    // - eventually the cpu goes idle
    // - we go directly back into the scheduling loop (without consuming any
    // new messages as none will be generated).
    req->LocalYield(agent_barrier, RTLA_ON_IDLE);
    return;
  }

  cs->run_queue.mu_.Lock();
  CfsTask* next = cs->run_queue.PickNextTask(prev, allocator(), cs);
  cs->run_queue.mu_.Unlock();

  cs->current = next;

  if (next) {
    DPRINT_CFS(2, absl::StrFormat("[%s]: Picked via PickNextTask",
                                  next->gtid.describe()));

    req->Open({
        .target = next->gtid,
        .target_barrier = next->seqnum,
        .agent_barrier = agent_barrier,
        .commit_flags = COMMIT_AT_TXN_COMMIT,
    });

    // Although unlikely it's possible for an oncpu task to enter ghOSt on
    // any cpu. In this case there is a race window between producing the
    // MSG_TASK_NEW and getting off that cpu (a race that is exacerbated
    // by CFS dropping the rq->lock in PNT). During this window an agent
    // can observe the MSG_TASK_NEW on the default queue and because the
    // task is runnable it becomes a candidate to be put oncpu immediately.
    //
    // In this case we wait for `next` to fully get offcpu before trying
    // to Commit().
    while (next->status_word.on_cpu()) {
      Pause();
    }

    uint64_t before_runtime = next->status_word.runtime();
    if (req->Commit()) {
      GHOST_DPRINT(3, stderr, "Task %s oncpu %d", next->gtid.describe(),
                   cpu.id());
      next->vruntime +=
          absl::Nanoseconds(next->status_word.runtime() - before_runtime);
    } else {
      GHOST_DPRINT(3, stderr, "CfsSchedule: commit failed (state=%d)",
                   req->state());
      // If our transaction failed, it is because our agent was stale.
      // Processing the remaining messages will bring our view up to date.
      // Since only the last state of cs->current matters, it is okay to keep
      // cs->current as what was picked by PickNextTask.
    }
  } else {
    req->LocalYield(agent_barrier, 0);
  }
}

void CfsScheduler::Schedule(const Cpu& cpu, const StatusWord& agent_sw) {
  BarrierToken agent_barrier = agent_sw.barrier();
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

void CfsScheduler::PingCpu(const Cpu& cpu) {
  Agent* agent = enclave()->GetAgent(cpu);
  if (agent) {
    agent->Ping();
  }
}

void CfsScheduler::TaskAffinityChanged(CfsTask* task, const Message& msg) {
  const ghost_msg_payload_task_affinity_changed* payload =
    static_cast<const ghost_msg_payload_task_affinity_changed*>(msg.payload());

  CHECK_EQ(task->gtid.id(), payload->gtid);
  CHECK_EQ(task->run_state.Get(), CfsTaskState::kRunning);

  CpuList eligible_cpus = MachineTopology()->EmptyCpuList();
  if (GhostHelper()->SchedGetAffinity(task->gtid, eligible_cpus) != 0) {
    DPRINT_CFS(3, absl::StrFormat("[%s]: Cannot retrieve the CPU mask.",
      task->gtid.describe()));
  }

  // Get the intersection of the eligible CPUs and the enclave CPUs.
  eligible_cpus.Intersection(cpus());
  if (eligible_cpus.Empty()) {
    // Not able to migrate as the eligible CPUs are outside of the enclave.
    DPRINT_CFS(3, absl::StrFormat("[%s]: No CPUs eligible for migration.",
      task->gtid.describe()));
    return;
  }

  // Make sure to remove the task from the current cpu.
  CpuState* cs = cpu_state_of(task);
  CHECK_EQ(cs->current, task);
  {
      absl::MutexLock l(&cs->run_queue.mu_);
      cs->current = nullptr;
      cs->run_queue.Erase(task);
  }

  // Get a target CPU from the eligible CPU list.
  Cpu target_cpu = SelectTaskRq(task, eligible_cpus);

  // When not removing the task from the current RQ, seqnum+1 is necessary
  // because migrating a running task generates a new message, which increments
  // seqnum and fails Migrate (passed seqnum less than the current seqnum). Even
  // after erasing the task, we need seqnum+1, otherwise the task enters a weird
  // state (no failures though, just hangs).
  Migrate(task, target_cpu, msg.seqnum()+1);
}

#ifndef NDEBUG
void CfsTaskState::AssertValidTransition(State next) {
  uint64_t validStates = GetTransitionMap().at(next);

  // Check if next is actually a valid state to come from.
  if ((validStates & (1 << int(state_))) == 0) {
    DPRINT_CFS(1, absl::StrFormat("[%s]: Cannot go from %s -> %s",
                                  absl::FormatStreamed(task_name_),
                                  absl::FormatStreamed(state_),
                                  absl::FormatStreamed(next)));
    DPRINT_CFS(1, absl::StrFormat("[%s]: Valid transitions -> %s are:",
                                  absl::FormatStreamed(task_name_),
                                  absl::FormatStreamed(next)));

    // Extract all the valid from states.
    for (int i = 0; i < kNumStates; i++) {
      if ((validStates & (1 << int(i))) != 0) {
        DPRINT_CFS(1, absl::StrFormat(
                          "%s", absl::FormatStreamed(CfsTaskState(State(i)))));
      }
    }

    DPRINT_CFS(1, absl::StrFormat("[%s]: State trace:", task_name_));
    for (auto s : state_trace_) {
      DPRINT_CFS(1, absl::StrFormat("[%s]: %s", task_name_,
                                    absl::FormatStreamed(CfsTaskState(s))));
    }

    // We want to crash since we tranisitioned to an invalid state.
    CHECK(false);
  }
}
#endif  // !NDEBUG

CfsRq::CfsRq() : min_vruntime_(absl::ZeroDuration()), rq_(&CfsTask::Less) {}

void CfsRq::EnqueueTask(CfsTask* task) {
  CHECK_GE(task->cpu, 0);

  DPRINT_CFS(2, absl::StrFormat("[%s]: Enqueing task", task->gtid.describe()));

  // We never want to enqueue a new task with a smaller vruntime that we have
  // currently. We also never want to have a task's vruntime go backwards,
  // so we take the max of our current min vruntime and the tasks current one.
  // Until load balancing is implented, this should just evaluate to
  // min_vruntime_.
  // TODO: come up with more logical way of handling new tasks with
  // existing vruntimes (e.g. migration from another rq).
  task->vruntime = std::max(min_vruntime_, task->vruntime);
  task->run_state.Set(CfsTaskState::kRunnable);
  InsertTaskIntoRq(task);
}

void CfsRq::PutPrevTask(CfsTask* task) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
  CHECK_GE(task->cpu, 0);

  DPRINT_CFS(2,
             absl::StrFormat("[%s]: Putting prev task", task->gtid.describe()));

  InsertTaskIntoRq(task);
}

CfsTask* CfsRq::PickNextTask(CfsTask* prev, TaskAllocator<CfsTask>* allocator,
                             CpuState* cs) {
  // Check if we can just keep running the current task.
  if (prev && prev->run_state.Get() == CfsTaskState::kRunning &&
      !cs->preempt_curr) {
    return prev;
  }

  // Past here, we will return a new task to run, so reset our preemption flag.
  cs->preempt_curr = false;

  // Check what happened to our previously running task and reconcile our
  // runqueue. No scheduling decision is made here unless our prev task still
  // wants to be oncpu, then we check if it needs to be preempted or not. If
  // it does not, we just transact prev if it does, then we go through to
  // PickNextTask.
  if (prev) {
    switch (prev->run_state.Get()) {
      case CfsTaskState::kNumStates:
        CHECK(false);
        break;
      case CfsTaskState::kBlocked:
        break;
      case CfsTaskState::kDone:
        Erase(prev);
        allocator->FreeTask(prev);
        break;
      case CfsTaskState::kRunnable:
        PutPrevTask(prev);
        break;
      case CfsTaskState::kRunning:
        // We had the preempt curr flag set, so we need to put our current task
        // back into the rq.
        PutPrevTask(prev);
        prev->run_state.Set(CfsTaskState::kRunnable);
        break;
    }
  }

  // First, we reconcile our CpuState with the messaging relating to prev.
  if (rq_.empty()) {
    UpdateMinVruntime(cs);
    return nullptr;
  }

  // Get a pointer to the first task. std::{set, multiset} orders by the ::Less
  // function, implying that, in our case, the first element has the smallest
  // vruntime (https://www.cplusplus.com/reference/set/set/).
  auto start_it = rq_.begin();
  CfsTask* task = *start_it;

  task->run_state.Set(CfsTaskState::kRunning);
  task->runtime_at_first_pick_ns = task->status_word.runtime();

  // Remove the task from the timeline.
  rq_.erase(start_it);

  // min_vruntime is used for Enqueing new tasks. We want to place them at
  // at least the current moment in time. Placing them before min_vruntime,
  // would give them an inordinate amount of runtime on the CPU as they would
  // need to catch up to other tasks that have accummulated a large runtime.
  // For easy access, we cache the value.
  UpdateMinVruntime(cs);
  return task;
}

void CfsRq::Erase(CfsTask* task) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
  DPRINT_CFS(2, absl::StrFormat("[%s]: Erasing task", task->gtid.describe()));
  if (rq_.erase(task) == 0) {
    // TODO: Figure out the case where we call Erase, but the task is not
    // actually in the rq. This seems to sporadically happen when processing a
    // TaskDeparted message. In reality, this is harmless as adding a check for
    // is my task in the rq currently would be equivalent.
    // DPRINT_CFS(
    //     1, absl::StrFormat(
    //            "[%s] Attempted to remove task with state %d while not in rq",
    //            task->gtid.describe(), task->run_state.Get()));
    // CHECK(false);
  }
}

void CfsRq::UpdateMinVruntime(CpuState* cs) {
  // We want to make sure min_vruntime_ is set to the min of curr's vruntime and
  // the vruntime of our leftmost node. We do this so that:
  // - if curr is immediately placed back into the rq, we don't go back in time
  // wrt vruntime
  // - if a new task is inserted into the rq, it doesn't get treated unfairly
  // wrt to curr
  CfsTask* curr = cs->current;
  CfsTask* leftmost = (rq_.empty()) ? nullptr : *rq_.begin();

  absl::Duration vruntime = min_vruntime_;

  // If our curr task should/is on the rq then it should be in contention
  // for the min vruntime.
  if (curr) {
    if (curr->run_state.Get() == CfsTaskState::kRunnable ||
        curr->run_state.Get() == CfsTaskState::kRunning) {
      vruntime = curr->vruntime;
    } else {
      curr = nullptr;
    }
  }

  // non-empty rq
  if (leftmost) {
    if (!curr) {
      vruntime = leftmost->vruntime;
    } else {
      vruntime = std::min(vruntime, leftmost->vruntime);
    }
  }

  min_vruntime_ = std::max(min_vruntime_, vruntime);
}

void CfsRq::SetMinGranularity(absl::Duration t) {
  min_preemption_granularity_ = t;
}

void CfsRq::SetLatency(absl::Duration t) { latency_ = t; }

absl::Duration CfsRq::MinPreemptionGranularity() {
  // Get the number of tasks our cpu is handling. As we only call this to check
  // if cs->current should be pulled be preempted, the number of tasks
  // associated with the cpu is rq_.size() + 1;
  std::multiset<CfsTask*, bool (*)(CfsTask*, CfsTask*)>::size_type tasks =
      rq_.size() + 1;
  if (tasks * min_preemption_granularity_ > latency_) {
    // If we target latency_, each task will run for less than min_granularity
    // so we just return min_granularity_.
    return min_preemption_granularity_;
  }

  // We want ceil(latency_/num_tasks) here. If we take the floor (normal
  // integer division), then we might go below min_granularity in the edge
  // case.
  return (latency_ + absl::Nanoseconds(tasks - 1)) / tasks;
}

void CfsRq::InsertTaskIntoRq(CfsTask* task) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
  rq_.insert(task);
  min_vruntime_ = (*rq_.begin())->vruntime;
  DPRINT_CFS(2, absl::StrFormat("[%s]: Inserted into run queue",
                                task->gtid.describe()));
}

std::unique_ptr<CfsScheduler> MultiThreadedCfsScheduler(
    Enclave* enclave, CpuList cpulist, absl::Duration min_granularity,
    absl::Duration latency) {
  auto allocator = std::make_shared<ThreadSafeMallocTaskAllocator<CfsTask>>();
  auto scheduler = std::make_unique<CfsScheduler>(enclave, std::move(cpulist),
                                                  std::move(allocator),
                                                  min_granularity, latency);
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
  switch (state.Get()) {
    case CfsTaskState::kBlocked:
      return os << "kBlocked";
    case CfsTaskState::kDone:
      return os << "kDone";
    case CfsTaskState::kRunning:
      return os << "kRunning";
    case CfsTaskState::kRunnable:
      return os << "kRunnable";
    case CfsTaskState::kNumStates:
      return os << "SENTINEL";
      break;
      // No default (exhaustive switch)
  }
}

}  //  namespace ghost
