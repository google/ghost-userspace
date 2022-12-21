// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef GHOST_SCHEDULERS_FIFO_FIFO_SCHEDULER_H
#define GHOST_SCHEDULERS_FIFO_FIFO_SCHEDULER_H

#include <deque>
#include <memory>

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
  inline bool queued() const { return run_state == FifoTaskState::kQueued; }
  inline bool oncpu() const { return run_state == FifoTaskState::kOnCpu; }

  // N.B. _runnable() is a transitory state typically used during runqueue
  // manipulation. It is not expected to be used from task msg callbacks.
  //
  // If you are reading this then you probably want to take a closer look
  // at queued() instead.
  inline bool _runnable() const {
    return run_state == FifoTaskState::kRunnable;
  }

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

  // Erase 'task' from the runqueue.
  //
  // Caller must ensure that 'task' is on the runqueue in the first place
  // (e.g. via task->queued()).
  void Erase(FifoTask* task);

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

  void DumpState(const Cpu& cpu, int flags) final;
  std::atomic<bool> debug_runqueue_ = false;

  int CountAllTasks() {
    int num_tasks = 0;
    allocator()->ForEachTask([&num_tasks](Gtid gtid, const FifoTask* task) {
      ++num_tasks;
      return true;
    });
    return num_tasks;
  }

  static constexpr int kDebugRunqueue = 1;
  static constexpr int kCountAllTasks = 2;

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
  void FifoSchedule(const Cpu& cpu, BarrierToken agent_barrier,
                    bool prio_boosted);
  void TaskOffCpu(FifoTask* task, bool blocked, bool from_switchto);
  void TaskOnCpu(FifoTask* task, Cpu cpu);
  void Migrate(FifoTask* task, Cpu cpu, BarrierToken seqnum);
  Cpu AssignCpu(FifoTask* task);
  void DumpAllTasks();

  struct CpuState {
    FifoTask* current = nullptr;
    std::unique_ptr<Channel> channel = nullptr;
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
class FifoAgent : public LocalAgent {
 public:
  FifoAgent(Enclave* enclave, Cpu cpu, FifoScheduler* scheduler)
      : LocalAgent(enclave, cpu), scheduler_(scheduler) {}

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
    this->TerminateAgentTasks();
  }

  std::unique_ptr<Agent> MakeAgent(const Cpu& cpu) override {
    return std::make_unique<FifoAgent>(&this->enclave_, cpu, scheduler_.get());
  }

  void RpcHandler(int64_t req, const AgentRpcArgs& args,
                  AgentRpcResponse& response) override {
    switch (req) {
      case FifoScheduler::kDebugRunqueue:
        scheduler_->debug_runqueue_ = true;
        response.response_code = 0;
        return;
      case FifoScheduler::kCountAllTasks:
        response.response_code = scheduler_->CountAllTasks();
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
