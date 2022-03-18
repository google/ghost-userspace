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

#ifndef GHOST_SCHEDULERS_FIFO_CENTRALIZED_FIFO_SCHEDULER_H
#define GHOST_SCHEDULERS_FIFO_CENTRALIZED_FIFO_SCHEDULER_H

#include <cstdint>
#include <map>

#include "absl/time/time.h"
#include "lib/agent.h"
#include "lib/scheduler.h"

namespace ghost {

// Store information about a scheduled task.
struct FifoTask : public Task<> {
  enum class RunState {
    kBlocked,
    kQueued,
    kRunnable,
    kOnCpu,
    kYielding,
  };

  FifoTask(Gtid fifo_task_gtid, ghost_sw_info sw_info)
      : Task<>(fifo_task_gtid, sw_info) {}
  ~FifoTask() override {}

  bool blocked() const { return run_state == RunState::kBlocked; }
  bool queued() const { return run_state == RunState::kQueued; }
  bool runnable() const { return run_state == RunState::kRunnable; }
  bool oncpu() const { return run_state == RunState::kOnCpu; }
  bool yielding() const { return run_state == RunState::kYielding; }

  static std::string_view RunStateToString(FifoTask::RunState run_state) {
    switch (run_state) {
      case FifoTask::RunState::kBlocked:
        return "Blocked";
      case FifoTask::RunState::kQueued:
        return "Queued";
      case FifoTask::RunState::kRunnable:
        return "Runnable";
      case FifoTask::RunState::kOnCpu:
        return "OnCpu";
      case FifoTask::RunState::kYielding:
        return "Yielding";
        // We will get a compile error if a new member is added to the
        // `FifoTask::RunState` enum and a corresponding case is not added
        // here.
    }
    CHECK(false);
    return "Unknown run state";
  }

  friend std::ostream& operator<<(std::ostream& os,
                                  FifoTask::RunState run_state) {
    os << RunStateToString(run_state);
    return os;
  }

  RunState run_state = RunState::kBlocked;
  Cpu cpu{Cpu::UninitializedType::kUninitialized};

  // Whether the last execution was preempted or not.
  bool preempted = false;
  bool prio_boost = false;
};

class FifoScheduler : public BasicDispatchScheduler<FifoTask> {
 public:
  FifoScheduler(Enclave* enclave, CpuList cpulist,
                std::shared_ptr<TaskAllocator<FifoTask>> allocator,
                int32_t global_cpu);
  ~FifoScheduler();

  void EnclaveReady();
  Channel& GetDefaultChannel() { return global_channel_; };

  // Handles task messages received from the kernel via shared memory queues.
  void TaskNew(FifoTask* task, const Message& msg);
  void TaskRunnable(FifoTask* task, const Message& msg);
  void TaskDeparted(FifoTask* task, const Message& msg);
  void TaskDead(FifoTask* task, const Message& msg);
  void TaskYield(FifoTask* task, const Message& msg);
  void TaskBlocked(FifoTask* task, const Message& msg);
  void TaskPreempted(FifoTask* task, const Message& msg);

  // Handles cpu "not idle" message. Currently a nop.
  void CpuNotIdle(const Message& msg);

  // Handles cpu "timer expired" messages. Currently a nop.
  void CpuTimerExpired(const Message& msg);

  bool Empty() { return num_tasks_ == 0; }

  // We validate state is consistent before actually tearing anything down since
  // tear-down involves pings and agents potentially becoming non-coherent as
  // they are removed sequentially.
  void ValidatePreExitState();

  // Removes 'task' from the runqueue.
  void RemoveFromRunqueue(FifoTask* task);

  // Main scheduling function for the global agent.
  void GlobalSchedule(const StatusWord& agent_sw,
                      StatusWord::BarrierToken agent_sw_last);

  int32_t GetGlobalCPUId() {
    return global_cpu_.load(std::memory_order_acquire);
  }

  void SetGlobalCPU(const Cpu& cpu) {
    global_cpu_core_ = cpu.core();
    global_cpu_.store(cpu.id(), std::memory_order_release);
  }

  // When a different scheduling class (e.g., CFS) has a task to run on the
  // global agent's CPU, the global agent calls this function to try to pick a
  // new CPU to move to and, if a new CPU is found, to initiate the handoff
  // process.
  bool PickNextGlobalCPU(StatusWord::BarrierToken agent_barrier,
                         const Cpu& this_cpu);

  // Print debug details about the current tasks managed by the global agent,
  // CPU state, and runqueue stats.
  void DumpState(const Cpu& cpu, int flags);
  std::atomic<bool> debug_runqueue_ = false;

  static const int kDebugRunqueue = 1;

 private:
  struct CpuState {
    FifoTask* current = nullptr;
    const Agent* agent = nullptr;
  } ABSL_CACHELINE_ALIGNED;

  // Unschedule `prev` from the CPU it is currently running on. If `next` is not
  // NULL, then `next` is scheduled in place of `prev` (all done in a single
  // transaction). If `next` is NULL, then `prev` is just unscheduled (all done
  // in a single transaction).
  bool PreemptTask(FifoTask* prev, FifoTask* next,
                   StatusWord::BarrierToken agent_barrier);

  // Updates the state of `task` to reflect that it is now running on `cpu`.
  // This method should be called after a transaction scheduling `task` onto
  // `cpu` succeeds.
  void TaskOnCpu(FifoTask* task, const Cpu& cpu);

  // Marks a task as yielded.
  void Yield(FifoTask* task);

  // Adds a task to the FIFO runqueue.
  void Enqueue(FifoTask* task);

  // Removes and returns the task at the front of the runqueue.
  FifoTask* Dequeue();

  // Prints all tasks (includin tasks not running or on the runqueue) managed by
  // the global agent.
  void DumpAllTasks();

  // Returns 'true' if a CPU can be scheduled by ghOSt. Returns 'false'
  // otherwise, usually because a higher-priority scheduling class (e.g., CFS)
  // is currently using the CPU.
  bool Available(const Cpu& cpu);

  CpuState* cpu_state_of(const FifoTask* task);

  CpuState* cpu_state(const Cpu& cpu) { return &cpu_states_[cpu.id()]; }

  size_t RunqueueSize() const { return run_queue_.size(); }

  bool RunqueueEmpty() const { return RunqueueSize() == 0; }

  CpuState cpu_states_[MAX_CPUS];

  int global_cpu_core_;
  std::atomic<int32_t> global_cpu_;
  Channel global_channel_;
  int num_tasks_ = 0;

  std::deque<FifoTask*> run_queue_;
  std::vector<FifoTask*> yielding_tasks_;

  absl::Time schedule_timer_start_;
  absl::Duration schedule_durations_;
  uint64_t iterations_ = 0;
};

// Initializes the task allocator and the FIFO scheduler.
std::unique_ptr<FifoScheduler> SingleThreadFifoScheduler(Enclave* enclave,
                                                         CpuList cpulist,
                                                         int32_t global_cpu);

// Operates as the Global or Satellite agent depending on input from the
// global_scheduler->GetGlobalCPU callback.
class FifoAgent : public Agent {
 public:
  FifoAgent(Enclave* enclave, Cpu cpu, FifoScheduler* global_scheduler)
      : Agent(enclave, cpu), global_scheduler_(global_scheduler) {}

  void AgentThread() override;
  Scheduler* AgentScheduler() const override { return global_scheduler_; }

 private:
  FifoScheduler* global_scheduler_;
};

class FifoConfig : public AgentConfig {
 public:
  FifoConfig() {}
  FifoConfig(Topology* topology, CpuList cpulist, Cpu global_cpu)
      : AgentConfig(topology, std::move(cpulist)), global_cpu_(global_cpu) {}

  Cpu global_cpu_{Cpu::UninitializedType::kUninitialized};
};

// A global agent scheduler. It runs a single-threaded FIFO scheduler on the
// global_cpu.
template <class EnclaveType>
class FullFifoAgent : public FullAgent<EnclaveType> {
 public:
  explicit FullFifoAgent(FifoConfig config) : FullAgent<EnclaveType>(config) {
    global_scheduler_ = SingleThreadFifoScheduler(
        &this->enclave_, *this->enclave_.cpus(), config.global_cpu_.id());
    this->StartAgentTasks();
    this->enclave_.Ready();
  }

  ~FullFifoAgent() override {
    global_scheduler_->ValidatePreExitState();

    // Terminate global agent before satellites to avoid a false negative error
    // from ghost_run(). e.g. when the global agent tries to schedule on a CPU
    // without an active satellite agent.
    auto global_cpuid = global_scheduler_->GetGlobalCPUId();

    if (this->agents_.front()->cpu().id() != global_cpuid) {
      // Bring the current globalcpu agent to the front.
      for (auto it = this->agents_.begin(); it != this->agents_.end(); it++) {
        if (((*it)->cpu().id() == global_cpuid)) {
          auto d = std::distance(this->agents_.begin(), it);
          std::iter_swap(this->agents_.begin(), this->agents_.begin() + d);
          break;
        }
      }
    }

    CHECK_EQ(this->agents_.front()->cpu().id(), global_cpuid);

    this->TerminateAgentTasks();
  }

  std::unique_ptr<Agent> MakeAgent(const Cpu& cpu) override {
    return absl::make_unique<FifoAgent>(&this->enclave_, cpu,
                                        global_scheduler_.get());
  }

  void RpcHandler(int64_t req, const AgentRpcArgs& args,
                  AgentRpcResponse& response) override {
    switch (req) {
      case FifoScheduler::kDebugRunqueue:
        global_scheduler_->debug_runqueue_ = true;
        response.response_code = 0;
        return;
      default:
        response.response_code = -1;
        return;
    }
  }

 private:
  std::unique_ptr<FifoScheduler> global_scheduler_;
};

}  // namespace ghost

#endif  // GHOST_SCHEDULERS_FIFO_CENTRALIZED_FIFO_SCHEDULER_H
