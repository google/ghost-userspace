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

#ifndef GHOST_SCHEDULERS_SOL_SOL_SCHEDULER_H
#define GHOST_SCHEDULERS_SOL_SOL_SCHEDULER_H

#include <cstdint>
#include <map>

#include "absl/time/time.h"
#include "lib/agent.h"
#include "lib/scheduler.h"

namespace ghost {

// Store information about a scheduled task.
struct SolTask : public Task {
  enum class RunState {
    kBlocked,
    kQueued,
    kRunnable,
    kOnCpu,
    kYielding,
    kPending,
  };

  explicit SolTask(Gtid sol_task_gtid, struct ghost_sw_info sw_info)
      : Task(sol_task_gtid, sw_info) {}
  ~SolTask() override {}

  bool blocked() const { return run_state == RunState::kBlocked; }
  bool queued() const { return run_state == RunState::kQueued; }
  bool runnable() const { return run_state == RunState::kRunnable; }
  bool oncpu() const { return run_state == RunState::kOnCpu; }
  bool yielding() const { return run_state == RunState::kYielding; }
  bool pending() const { return run_state == RunState::kPending; }

  static std::string_view RunStateToString(SolTask::RunState run_state) {
    switch (run_state) {
      case SolTask::RunState::kBlocked:
        return "Blocked";
      case SolTask::RunState::kQueued:
        return "Queued";
      case SolTask::RunState::kRunnable:
        return "Runnable";
      case SolTask::RunState::kOnCpu:
        return "OnCpu";
      case SolTask::RunState::kYielding:
        return "Yielding";
      case SolTask::RunState::kPending:
        return "Pending";
        // We will get a compile error if a new member is added to the
        // `SolTask::RunState` enum and a corresponding case is not added
        // here.
    }
    CHECK(false);
    return "Unknown run state";
  }

  friend std::ostream& operator<<(std::ostream& os,
                                  SolTask::RunState run_state) {
    os << RunStateToString(run_state);
    return os;
  }

  RunState run_state = RunState::kBlocked;
  Cpu cpu{Cpu::UninitializedType::kUninitialized};

  // Whether the last execution was preempted or not.
  bool preempted = false;
  bool prio_boost = false;
};

class SolScheduler : public BasicDispatchScheduler<SolTask> {
 public:
  explicit SolScheduler(Enclave* enclave, CpuList cpulist,
                        std::shared_ptr<TaskAllocator<SolTask>> allocator,
                        int32_t global_cpu);
  ~SolScheduler() final;

  void EnclaveReady() final;
  Channel& GetDefaultChannel() final { return global_channel_; };

  // Handles task messages received from the kernel via shared memory queues.
  void TaskNew(SolTask* task, const Message& msg) final;
  void TaskRunnable(SolTask* task, const Message& msg) final;
  void TaskDeparted(SolTask* task, const Message& msg) final;
  void TaskDead(SolTask* task, const Message& msg) final;
  void TaskYield(SolTask* task, const Message& msg) final;
  void TaskBlocked(SolTask* task, const Message& msg) final;
  void TaskPreempted(SolTask* task, const Message& msg) final;

  // Handles cpu "not idle" message. Currently a nop.
  void CpuNotIdle(const Message& msg) final;

  // Handles cpu "timer expired" messages. Currently a nop.
  void CpuTimerExpired(const Message& msg) final;

  bool Empty() { return num_tasks_ == 0; }

  // We validate state is consistent before actually tearing anything down since
  // tear-down involves pings and agents potentially becoming non-coherent as
  // they are removed sequentially.
  void ValidatePreExitState();

  // Removes 'task' from the runqueue.
  void RemoveFromRunqueue(SolTask* task);

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

  void EnterSchedule() {
    CHECK_EQ(schedule_timer_start_, absl::UnixEpoch());
    schedule_timer_start_ = ghost::MonotonicNow();
  }

  void ExitSchedule() {
    CHECK_NE(schedule_timer_start_, absl::UnixEpoch());
    schedule_durations_ += ghost::MonotonicNow() - schedule_timer_start_;
    schedule_timer_start_ = absl::UnixEpoch();
    ++iterations_;
  }

  absl::Duration SchedulingOverhead() {
    absl::Duration ret = schedule_durations_ / iterations_;
    schedule_durations_ = absl::ZeroDuration();
    return ret;
  }

  // When a different scheduling class (e.g., CFS) has a task to run on the
  // global agent's CPU, the global agent calls this function to try to pick a
  // new CPU to move to and, if a new CPU is found, to initiate the handoff
  // process.
  bool PickNextGlobalCPU(StatusWord::BarrierToken agent_barrier,
                         const Cpu& this_cpu);

  // Print debug details about the current tasks managed by the global agent,
  // CPU state, and runqueue stats.
  void DumpState(const Cpu& cpu, int flags) final;
  std::atomic<bool> debug_runqueue_ = false;

  static const int kDebugRunqueue = 1;
  static const int kGetSchedOverhead = 2;

 private:
  struct CpuState {
    SolTask* current = nullptr;
    SolTask* next = nullptr;
    const Agent* agent = nullptr;
  } ABSL_CACHELINE_ALIGNED;

  bool SyncCpuState(const Cpu& cpu);
  void SyncTaskState(SolTask* task);
  bool PreemptTask(SolTask* prev, SolTask* next,
                   StatusWord::BarrierToken agent_barrier);

  // Marks a task as yielded.
  void Yield(SolTask* task);

  // Adds a task to the FIFO runqueue.
  void Enqueue(SolTask* task);

  // Removes and returns the task at the front of the runqueue.
  SolTask* Dequeue();

  // Prints all tasks (includin tasks not running or on the runqueue) managed by
  // the global agent.
  void DumpAllTasks();

  // Returns 'true' if a CPU can be scheduled by ghOSt. Returns 'false'
  // otherwise, usually because a higher-priority scheduling class (e.g., CFS)
  // is currently using the CPU.
  bool Available(const Cpu& cpu);

  CpuState* cpu_state_of(const SolTask* task);

  CpuState* cpu_state(const Cpu& cpu) { return &cpu_states_[cpu.id()]; }

  size_t RunqueueSize() const { return run_queue_.size(); }

  bool RunqueueEmpty() const { return RunqueueSize() == 0; }

  CpuState cpu_states_[MAX_CPUS];

  int global_cpu_core_;
  std::atomic<int32_t> global_cpu_;
  Channel global_channel_;
  int num_tasks_ = 0;

  std::deque<SolTask*> run_queue_;
  std::vector<SolTask*> yielding_tasks_;

  absl::Time schedule_timer_start_;
  absl::Duration schedule_durations_;
  uint64_t iterations_ = 0;
};

// Initializes the task allocator and the Sol scheduler.
std::unique_ptr<SolScheduler> SingleThreadSolScheduler(Enclave* enclave,
                                                       CpuList cpulist,
                                                       int32_t global_cpu);

// Operates as the Global or Satellite agent depending on input from the
// global_scheduler->GetGlobalCPU callback.
class SolAgent : public Agent {
 public:
  SolAgent(Enclave* enclave, Cpu cpu, SolScheduler* global_scheduler)
      : Agent(enclave, cpu), global_scheduler_(global_scheduler) {}

  void AgentThread() override;
  Scheduler* AgentScheduler() const override { return global_scheduler_; }

 private:
  SolScheduler* global_scheduler_;
};

class SolConfig : public AgentConfig {
 public:
  SolConfig() {}
  SolConfig(Topology* topology, CpuList cpulist, Cpu global_cpu)
      : AgentConfig(topology, std::move(cpulist)), global_cpu_(global_cpu) {}

  Cpu global_cpu_{Cpu::UninitializedType::kUninitialized};
};

// An global agent scheduler.  It runs a single-threaded Sol scheduler on the
// global_cpu.
template <class ENCLAVE>
class FullSolAgent : public FullAgent<ENCLAVE> {
 public:
  explicit FullSolAgent(SolConfig config) : FullAgent<ENCLAVE>(config) {
    global_scheduler_ = SingleThreadSolScheduler(
        &this->enclave_, *this->enclave_.cpus(), config.global_cpu_.id());
    this->StartAgentTasks();
    this->enclave_.Ready();
  }

  ~FullSolAgent() override {
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
    return absl::make_unique<SolAgent>(&this->enclave_, cpu,
                                       global_scheduler_.get());
  }

  void RpcHandler(int64_t req, const AgentRpcArgs& args,
                  AgentRpcResponse& response) override {
    switch (req) {
      case SolScheduler::kDebugRunqueue:
        global_scheduler_->debug_runqueue_ = true;
        response.response_code = 0;
        return;
      case SolScheduler::kGetSchedOverhead:
        response.response_code = absl::ToInt64Nanoseconds(
            global_scheduler_->SchedulingOverhead());
        return;
      default:
        response.response_code = -1;
        return;
    }
  }

 private:
  std::unique_ptr<SolScheduler> global_scheduler_;
};

}  // namespace ghost

#endif  // GHOST_SCHEDULERS_SOL_SOL_SCHEDULER_H
