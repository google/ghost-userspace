/*
 * Copyright 2021 Google LLC
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

#ifndef GHOST_SCHEDULERS_FIFO_FIFO_SCHEDULER_H
#define GHOST_SCHEDULERS_FIFO_FIFO_SCHEDULER_H

#include <deque>

#include "lib/agent.h"
#include "lib/scheduler.h"

namespace ghost {

enum class FifoTaskState {
  kBlocked,   // not on runqueue.
  kRunnable,  // transitory state:
              // 1. kBlocked->kRunnable->kQueued
              // 2. kQueued->kRunnable->kOnCpu
  kQueued,    // on runqueue.
  kOnCpu,     // running on cpu.
};

// For CHECK and friends.
std::ostream& operator<<(std::ostream& os, const FifoTaskState& state);

struct FifoTask : public Task<> {
  explicit FifoTask(Gtid fifo_task_gtid, ghost_sw_info sw_info)
      : Task<>(fifo_task_gtid, sw_info) {}
  ~FifoTask() override {}

  inline bool blocked() const { return run_state == FifoTaskState::kBlocked; }
  inline bool runnable() const { return run_state == FifoTaskState::kRunnable; }
  inline bool queued() const { return run_state == FifoTaskState::kQueued; }
  inline bool oncpu() const { return run_state == FifoTaskState::kOnCpu; }

  FifoTaskState run_state = FifoTaskState::kBlocked;
  int cpu = -1;

  // Whether the last execution was preempted or not.
  bool preempted = false;

  // A task's priority is boosted on a kernel preemption or a !deferrable
  // wakeup - basically when it may be holding locks or other resources
  // that prevent other tasks from making progress.
  bool prio_boost = false;
};

class FifoRq {
 public:
  FifoRq() = default;
  FifoRq(const FifoRq&) = delete;
  FifoRq& operator=(FifoRq&) = delete;

  FifoTask* Dequeue();
  void Enqueue(FifoTask* task);

  // Returns true if 'task' was found and erased from the runqueue and false
  // otherwise.
  bool Erase(const FifoTask* task);

  size_t Size() const {
    absl::MutexLock lock(&mu_);
    return rq_.size();
  }

  bool Empty() const { return Size() == 0; }

 private:
  mutable absl::Mutex mu_;
  std::deque<FifoTask*> rq_ ABSL_GUARDED_BY(mu_);
};

class FifoScheduler : public BasicDispatchScheduler<FifoTask> {
 public:
  explicit FifoScheduler(Enclave* enclave, CpuList cpulist,
                         std::shared_ptr<TaskAllocator<FifoTask>> allocator);
  ~FifoScheduler() final {}

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

  static constexpr int kDebugRunqueue = 1;

 protected:
  void TaskNew(FifoTask* task, const Message& msg) final;
  void TaskRunnable(FifoTask* task, const Message& msg) final;
  void TaskDeparted(FifoTask* task, const Message& msg) final;
  void TaskDead(FifoTask* task, const Message& msg) final;
  void TaskYield(FifoTask* task, const Message& msg) final;
  void TaskBlocked(FifoTask* task, const Message& msg) final;
  void TaskPreempted(FifoTask* task, const Message& msg) final;
  void TaskSwitchto(FifoTask* task, const Message& msg) final;

 private:
  void FifoSchedule(const Cpu& cpu, StatusWord::BarrierToken agent_barrier,
                    bool prio_boosted);
  void TaskOffCpu(FifoTask* task, bool blocked, bool from_switchto);
  void TaskOnCpu(FifoTask* task, Cpu cpu);
  void Migrate(FifoTask* task, Cpu cpu, StatusWord::BarrierToken seqnum);
  Cpu AssignCpu(FifoTask* task);
  void DumpAllTasks();

  struct CpuState {
    FifoTask* current = nullptr;
    std::unique_ptr<ghost::Channel> channel = nullptr;
    FifoRq run_queue;
  } ABSL_CACHELINE_ALIGNED;

  inline CpuState* cpu_state(const Cpu& cpu) { return &cpu_states_[cpu.id()]; }

  inline CpuState* cpu_state_of(const FifoTask* task) {
    CHECK_GE(task->cpu, 0);
    CHECK_LT(task->cpu, MAX_CPUS);
    return &cpu_states_[task->cpu];
  }

  CpuState cpu_states_[MAX_CPUS];
  Channel* default_channel_ = nullptr;
};

std::unique_ptr<FifoScheduler> MultiThreadedFifoScheduler(Enclave* enclave,
                                                          CpuList cpulist);
class FifoAgent : public Agent {
 public:
  FifoAgent(Enclave* enclave, Cpu cpu, FifoScheduler* scheduler)
      : Agent(enclave, cpu), scheduler_(scheduler) {}

  void AgentThread() override;
  Scheduler* AgentScheduler() const override { return scheduler_; }

 private:
  FifoScheduler* scheduler_;
};

template <class EnclaveType>
class FullFifoAgent : public FullAgent<EnclaveType> {
 public:
  explicit FullFifoAgent(AgentConfig config) : FullAgent<EnclaveType>(config) {
    scheduler_ =
        MultiThreadedFifoScheduler(&this->enclave_, *this->enclave_.cpus());
    this->StartAgentTasks();
    this->enclave_.Ready();
  }

  ~FullFifoAgent() override {
    scheduler_->ValidatePreExitState();
    this->TerminateAgentTasks();
  }

  std::unique_ptr<Agent> MakeAgent(const Cpu& cpu) override {
    return absl::make_unique<FifoAgent>(&this->enclave_, cpu, scheduler_.get());
  }

  void RpcHandler(int64_t req, const AgentRpcArgs& args,
                  AgentRpcResponse& response) override {
    switch (req) {
      case FifoScheduler::kDebugRunqueue:
        scheduler_->debug_runqueue_ = true;
        response.response_code = 0;
        return;
      default:
        response.response_code = -1;
        return;
    }
  }

 private:
  std::unique_ptr<FifoScheduler> scheduler_;
};

}  // namespace ghost

#endif  // GHOST_SCHEDULERS_FIFO_FIFO_SCHEDULER_H
