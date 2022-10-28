/*
 * Copyright 2022 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef GHOST_SCHEDULERS_CFS_CFS_SCHEDULER_H_
#define GHOST_SCHEDULERS_CFS_CFS_SCHEDULER_H_

#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <set>
#include <vector>

#include "absl/status/status.h"
#include "absl/time/time.h"
#include "lib/agent.h"
#include "lib/base.h"
#include "lib/scheduler.h"

namespace ghost {

enum class CfsTaskState {
  kBlocked,   // not on runqueue.
  kRunnable,  // transitory state:
              // 1. kBlocked->kRunnable->kQueued
              // 2. kQueued->kRunnable->kOnCpu
  kQueued,    // on runqueue.
  kOnCpu,     // running on cpu.
};

// For CHECK and friends.
std::ostream& operator<<(std::ostream& os, const CfsTaskState& state);

struct CfsTask : public Task<> {
  explicit CfsTask(Gtid d_task_gtid, ghost_sw_info sw_info)
      : Task<>(d_task_gtid, sw_info), vruntime(absl::ZeroDuration()) {}
  ~CfsTask() override {}

  inline bool blocked() const { return run_state == CfsTaskState::kBlocked; }
  inline bool queued() const { return run_state == CfsTaskState::kQueued; }
  inline bool oncpu() const { return run_state == CfsTaskState::kOnCpu; }

  // N.B. _runnable() is a transitory state typically used during runqueue
  // manipulation. It is not expected to be used from task msg callbacks.
  //
  // If you are reading this then you probably want to take a closer look
  // at queued() instead.
  inline bool _runnable() const { return run_state == CfsTaskState::kRunnable; }

  // std::multiset expects one to pass a strict (< not <=) weak ordering
  // function as a template parameter. Technically, this doesn't have to be
  // inside of the struct, but it seems logical to keep this here.
  static inline bool Less(CfsTask* a, CfsTask* b) {
    return a->vruntime < b->vruntime;
  }

  CfsTaskState run_state = CfsTaskState::kBlocked;
  int cpu = -1;

  // Whether the last execution was preempted or not.
  bool preempted = false;

  // Cfs sorts tasks by runtime, so we need to keep track of how long a task has
  // been running during this period.
  absl::Duration vruntime;

  // runtime_at_first_pick is how much runtime this task had at its initial
  // picking. This timestamp does not change unless we are put back in the
  // runqueue. IOW, if we bounce between oncpu and put_prev_task_elision_,
  // the timestamp is not reset. The timestamp is used to figure out
  // if a task has run for granularity_ yet.
  uint64_t runtime_at_first_pick_ns;
};

class CfsRq {
 public:
  explicit CfsRq();
  CfsRq(const CfsRq&) = delete;
  CfsRq& operator=(CfsRq&) = delete;

  // See CfsRq::granularity_ for a description of how these parameters work.
  void SetMinGranularity(absl::Duration t);
  void SetLatency(absl::Duration t);

  // Returns the length of time that the task should run in real time before it
  // is preempted. This value is equivalent to:
  // IF min_granularity * num_tasks > latency THEN min_granularity
  // ELSE latency / num_tasks
  // The purpose of having granularity is so that even if a task has a lot
  // of vruntime to makeup, it doesn't hog all the cputime.
  // TODO: update this when we introduce nice values.
  // NOTE: This needs to be updated everytime we change the number of tasks
  // associated with the runqueue changes. e.g. simply pulling a task out of
  // rq to give it time on the cpu doesn't require a change as we still manage
  // the same number of tasks. But a task blocking, departing, or adding
  // a new task, does require an update.
  absl::Duration Granularity();

  // Removes and returns the task with the smallest vruntime from the
  // underlying container. It is the responsibility of the caller to ensure
  // PutPrevTask is called to re-enque the task.
  // NOTE: If PickNextTask observes that the task in put_prev_task_elision_
  // has not run for granularity_ yet, then it will return that task.
  CfsTask* PickNextTask();

  // Enqueues a new task or a task that is transitioning to RUNNABLE from
  // another state.
  void EnqueueTask(CfsTask* task);

  // Enqueue a task that is transitioning from being on the cpu to off the cpu.
  // The task might have its requeuing into the tree delayed.
  void PutPrevTask(CfsTask* task, bool can_elide);

  // Same as PutPrevTask, but force putting the task into the tree.
  void NonElidedPutPrevTask(CfsTask* task);

  // Erase 'task' from the runqueue.
  //
  // Caller must ensure that 'task' is on the runqueue in the first place
  // (e.g. via task->queued()).
  void Erase(CfsTask* task);

  size_t Size() const {
    absl::MutexLock lock(&mu_);
    return rq_.size();
  }

  bool Empty() const { return Size() == 0; }

 private:
  // Inserts a task into the backing runqueue.
  // Preconditons: task->vruntime has been set to a logical value.
  void InsertTaskIntoRq(CfsTask* task) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  mutable absl::Mutex mu_;
  // We use a multiset as the backing data structure as, according to the
  // C++ standard, it is backed by a red-black tree, which is the backing
  // data structure in CFS in the the kernel. While opaque, using an std::
  // container is easiest way to use a red-black tree short of writing or
  // importing our own.
  std::multiset<CfsTask*, decltype(&CfsTask::Less)> rq_ ABSL_GUARDED_BY(mu_);
  absl::Duration min_vruntime_ ABSL_GUARDED_BY(mu_);

  // When PutPrevTask is called, there is a non-zero chance that we want the
  // subsequent PickNextTask to evaluate to the task we just Put. The
  // canonical example of this is when the agent preempts the task
  // to consume a CpuTick, which causes PutPrevTask to be called on the task.
  // After consuming the messages, we will reach Schedule(), which calls
  // PickNextTask. If the previously running task (put_prev_task_elision_)
  // has not run for granularity_ yet, then we put it back on the cpu,
  // regardless of where it is in the rb-tree. Given this, we defer placing
  // a task that had PutPrevTask on it, back in the rb-tree, until we are sure
  // the subsequent PickNextTask will not pick it.
  CfsTask* put_prev_task_elision_ ABSL_GUARDED_BY(mu_);

  absl::Duration min_granularity_ ABSL_GUARDED_BY(mu_);
  absl::Duration latency_ ABSL_GUARDED_BY(mu_);
};

class CfsScheduler : public BasicDispatchScheduler<CfsTask> {
 public:
  explicit CfsScheduler(Enclave* enclave, CpuList cpulist,
                        std::shared_ptr<TaskAllocator<CfsTask>> allocator,
                        absl::Duration min_granularity, absl::Duration latency);
  ~CfsScheduler() final {}

  void Schedule(const Cpu& cpu, const StatusWord& sw);

  void EnclaveReady() final;
  Channel& GetDefaultChannel() final { return *default_channel_; };

  bool Empty(const Cpu& cpu) {
    CpuState* cs = cpu_state(cpu);
    return cs->run_queue.Empty();
  }

  void ValidatePreExitState();

  void DumpState(const Cpu& cpu, int flags) final;
  std::atomic<bool> debug_runqueue_ = false;

  int CountAllTasks() {
    int num_tasks = 0;
    allocator()->ForEachTask([&num_tasks](Gtid gtid, const CfsTask* task) {
      ++num_tasks;
      return true;
    });
    return num_tasks;
  }

  static constexpr int kDebugRunqueue = 1;
  static constexpr int kCountAllTasks = 2;

 protected:
  void TaskNew(CfsTask* task, const Message& msg) final;
  void TaskRunnable(CfsTask* task, const Message& msg) final;
  void TaskDeparted(CfsTask* task, const Message& msg) final;
  void TaskDead(CfsTask* task, const Message& msg) final;
  void TaskYield(CfsTask* task, const Message& msg) final;
  void TaskBlocked(CfsTask* task, const Message& msg) final;
  void TaskPreempted(CfsTask* task, const Message& msg) final;
  void TaskSwitchto(CfsTask* task, const Message& msg) final;
  void CpuTick(const Message& msg) final;

 private:
  void CfsSchedule(const Cpu& cpu, StatusWord::BarrierToken agent_barrier,
                   bool prio_boost);
  void TaskOffCpu(CfsTask* task, bool blocked, bool from_switchto);
  void Migrate(CfsTask* task, Cpu cpu, StatusWord::BarrierToken seqnum);
  int DiscoverTask(CfsTask* task);
  Cpu AssignCpu(CfsTask* task);
  void DumpAllTasks();

  struct CpuState {
    CfsTask* current = nullptr;
    std::unique_ptr<ghost::LocalChannel> channel = nullptr;
    CfsRq run_queue;
  } ABSL_CACHELINE_ALIGNED;

  inline CpuState* cpu_state(const Cpu& cpu) { return &cpu_states_[cpu.id()]; }

  inline CpuState* cpu_state_of(const CfsTask* task) {
    CHECK_GE(task->cpu, 0);
    CHECK_LT(task->cpu, MAX_CPUS);
    return &cpu_states_[task->cpu];
  }

  CpuState cpu_states_[MAX_CPUS];
  LocalChannel* default_channel_ = nullptr;

  absl::Duration min_granularity_;
  absl::Duration latency_;
};

std::unique_ptr<CfsScheduler> MultiThreadedCfsScheduler(
    Enclave* enclave, CpuList cpulist, absl::Duration min_granularity,
    absl::Duration latency);
class CfsAgent : public LocalAgent {
 public:
  CfsAgent(Enclave* enclave, Cpu cpu, CfsScheduler* scheduler)
      : LocalAgent(enclave, cpu), scheduler_(scheduler) {}

  void AgentThread() override;
  Scheduler* AgentScheduler() const override { return scheduler_; }

 private:
  CfsScheduler* scheduler_;
};

class CfsConfig : public AgentConfig {
 public:
  CfsConfig() {}
  CfsConfig(Topology* topology, CpuList cpulist, absl::Duration min_granularity,
            absl::Duration latency)
      : AgentConfig(topology, std::move(cpulist)),
        min_granularity_(min_granularity),
        latency_(latency) {
    tick_config_ = CpuTickConfig::kAllTicks;
  }

  absl::Duration min_granularity_;
  absl::Duration latency_;
};

template <class EnclaveType>
class FullCfsAgent : public FullAgent<EnclaveType> {
 public:
  explicit FullCfsAgent(CfsConfig config) : FullAgent<EnclaveType>(config) {
    scheduler_ =
        MultiThreadedCfsScheduler(&this->enclave_, *this->enclave_.cpus(),
                                  config.min_granularity_, config.latency_);
    this->StartAgentTasks();
    this->enclave_.Ready();
  }

  ~FullCfsAgent() override {
    scheduler_->ValidatePreExitState();
    this->TerminateAgentTasks();
  }

  std::unique_ptr<Agent> MakeAgent(const Cpu& cpu) override {
    return std::make_unique<CfsAgent>(&this->enclave_, cpu, scheduler_.get());
  }

  void RpcHandler(int64_t req, const AgentRpcArgs& args,
                  AgentRpcResponse& response) override {
    switch (req) {
      case CfsScheduler::kDebugRunqueue:
        scheduler_->debug_runqueue_ = true;
        response.response_code = 0;
        return;
      case CfsScheduler::kCountAllTasks:
        response.response_code = scheduler_->CountAllTasks();
        return;
      default:
        response.response_code = -1;
        return;
    }
  }

 private:
  std::unique_ptr<CfsScheduler> scheduler_;
};

}  // namespace ghost

#endif  // GHOST_SCHEDULERS_Cfs_Cfs_SCHEDULER_H_
