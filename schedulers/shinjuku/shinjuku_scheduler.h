// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef GHOST_SCHEDULERS_SHINJUKU_SHINJUKU_SCHEDULER_H
#define GHOST_SCHEDULERS_SHINJUKU_SHINJUKU_SCHEDULER_H

#include <cstdint>
#include <map>
#include <memory>

#include "absl/container/flat_hash_map.h"
#include "absl/functional/bind_front.h"
#include "absl/time/time.h"
#include "lib/agent.h"
#include "lib/scheduler.h"
#include "schedulers/shinjuku/shinjuku_orchestrator.h"
#include "shared/prio_table.h"

namespace ghost {

// Store information about a scheduled task.
struct ShinjukuTask : public Task<> {
  enum class RunState {
    kBlocked,
    kQueued,
    kOnCpu,
    kYielding,
    kPaused,
  };

  // Represents a deferred unschedule.
  enum class UnscheduleLevel {
    // No pending deferred unschedule.
    kNoUnschedule,
    // This task was assigned a new closure (noticed via an update to this
    // task's sched item in the PrioTable) and may be preempted so another
    // waiting task may run. However, if there are no waiting tasks, keep this
    // task running to avoid the unschedule-and-reschedule overhead.
    kCouldUnschedule,
    // This task was marked idle via the PrioTable and should stop running.
    kMustUnschedule,
  };

  explicit ShinjukuTask(Gtid shinjuku_task_gtid, struct ghost_sw_info sw_info)
      : Task<>(shinjuku_task_gtid, sw_info) {}
  ~ShinjukuTask() override {}

  bool paused() const { return run_state == RunState::kPaused; }
  bool blocked() const { return run_state == RunState::kBlocked; }
  bool queued() const { return run_state == RunState::kQueued; }
  bool oncpu() const { return run_state == RunState::kOnCpu; }
  bool yielding() const { return run_state == RunState::kYielding; }

  // Sets the task's runtime to 'runtime' and updates the elapsed runtime if
  // 'update_elapsed_runtime' is true.
  void SetRuntime(absl::Duration runtime, bool update_elapsed_runtime);
  // Updates the task's runtime to match what the kernel last stored in the
  // task's status word. Note that this method does not use the syscall to get
  // the task's current runtime.
  void UpdateRuntime();

  static std::string_view RunStateToString(ShinjukuTask::RunState run_state) {
    switch (run_state) {
      case ShinjukuTask::RunState::kBlocked:
        return "Blocked";
      case ShinjukuTask::RunState::kQueued:
        return "Queued";
      case ShinjukuTask::RunState::kOnCpu:
        return "OnCpu";
      case ShinjukuTask::RunState::kYielding:
        return "Yielding";
      case ShinjukuTask::RunState::kPaused:
        return "Paused";
        // We will get a compile error if a new member is added to the
        // `ShinjukuTask::RunState` enum and a corresponding case is not added
        // here.
    }
    CHECK(false);
    return "Unknown run state";
  }

  friend std::ostream& operator<<(std::ostream& os,
                                  ShinjukuTask::RunState run_state) {
    return os << RunStateToString(run_state);
  }

  friend std::ostream& operator<<(
      std::ostream& os, ShinjukuTask::UnscheduleLevel unschedule_level) {
    switch (unschedule_level) {
      case ShinjukuTask::UnscheduleLevel::kNoUnschedule:
        return os << "No Unschedule";
      case ShinjukuTask::UnscheduleLevel::kCouldUnschedule:
        return os << "Could Unschedule";
      case ShinjukuTask::UnscheduleLevel::kMustUnschedule:
        return os << "Must Unschedule";
    }
  }

  RunState run_state = RunState::kBlocked;
  int cpu = -1;

  // Priority boosting for jumping past regular shinjuku ordering in the
  // runqueue.
  //
  // A task's priority is boosted on a kernel preemption or a !deferrable
  // wakeup - basically when it may be holding locks or other resources
  // that prevent other tasks from making progress.
  bool prio_boost = false;

  // Cumulative runtime in ns.
  absl::Duration runtime;
  // Accrued CPU time in ns.
  absl::Duration elapsed_runtime;
  // The time that the task was last scheduled.
  absl::Time last_ran = absl::UnixEpoch();

  // Whether the last execution was preempted or not.
  bool preempted = false;

  std::shared_ptr<ShinjukuOrchestrator> orch;
  const ShinjukuSchedParams* sp = nullptr;
  bool has_work = false;
  uint32_t wcid = std::numeric_limits<uint32_t>::max();

  // Indicates whether there is a pending deferred unschedule for this task, and
  // if so, whether the unschedule could optionally happen or must happen.
  UnscheduleLevel unschedule_level = UnscheduleLevel::kNoUnschedule;
};

// Implements the global agent policy layer and the Shinjuku scheduling
// algorithm. Can optionally be turned into a centralized queuing algorithm by
// setting the preemption time slice to an infinitely large duration. Can
// optionally be turned into a Shenango algorithm by using QoS classes; assign a
// low-latency app a high QoS class and an antagonist (that consumes CPU cycles)
// a low QoS class.
class ShinjukuScheduler : public BasicDispatchScheduler<ShinjukuTask> {
 public:
  explicit ShinjukuScheduler(
      Enclave* enclave, CpuList cpulist,
      std::shared_ptr<TaskAllocator<ShinjukuTask>> allocator,
      int32_t global_cpu, absl::Duration preemption_time_slice);
  ~ShinjukuScheduler() final;

  void EnclaveReady() final;
  Channel& GetDefaultChannel() final { return global_channel_; };

  // Handles task messages received from the kernel via shared memory queues.
  void TaskNew(ShinjukuTask* task, const Message& msg) final;
  void TaskRunnable(ShinjukuTask* task, const Message& msg) final;
  void TaskDeparted(ShinjukuTask* task, const Message& msg) final;
  void TaskDead(ShinjukuTask* task, const Message& msg) final;
  void TaskYield(ShinjukuTask* task, const Message& msg) final;
  void TaskBlocked(ShinjukuTask* task, const Message& msg) final;
  void TaskPreempted(ShinjukuTask* task, const Message& msg) final;

  void DiscoveryStart() final;
  void DiscoveryComplete() final;

  bool Empty() { return num_tasks_ == 0; }

  // Refreshes updated sched items. Note that all sched items may be refreshed,
  // regardless of whether they have been updated or not, if the stream has
  // overflown.
  void UpdateSchedParams();

  // Removes 'task' from the runqueue.
  void RemoveFromRunqueue(ShinjukuTask* task);

  // Helper function to 'GlobalSchedule' that determines whether it should skip
  // scheduling a CPU right now (returns 'true') or if it can schedule a CPU
  // right now (returns 'false').
  bool SkipForSchedule(int iteration, const Cpu& cpu);

  // Main scheduling function for the global agent.
  void GlobalSchedule(const StatusWord& agent_sw, BarrierToken agent_sw_last);

  int32_t GetGlobalCPUId() {
    return global_cpu_.load(std::memory_order_acquire);
  }

  void SetGlobalCPU(const Cpu& cpu) {
    global_cpu_.store(cpu.id(), std::memory_order_release);
  }

  // When a different scheduling class (e.g., CFS) has a task to run on the
  // global agent's CPU, the global agent calls this function to try to pick a
  // new CPU to move to and, if a new CPU is found, to initiate the handoff
  // process.
  void PickNextGlobalCPU(BarrierToken agent_barrier);

  // Print debug details about the current tasks managed by the global agent,
  // CPU state, and runqueue stats.
  void DumpState(const Cpu& cpu, int flags) final;
  std::atomic<bool> debug_runqueue_ = false;

  static constexpr int kDebugRunqueue = 1;

 private:
  struct CpuState {
    ShinjukuTask* current = nullptr;
    ShinjukuTask* next = nullptr;
    const Agent* agent = nullptr;
  } ABSL_CACHELINE_ALIGNED;

  // Stop 'task' from running and schedule nothing in its place. 'task' must be
  // currently running on a CPU.
  void UnscheduleTask(ShinjukuTask* task);

  // Marks a task as yielded.
  void Yield(ShinjukuTask* task);

  // Unmarks a task as yielded.
  void Unyield(ShinjukuTask* task);

  // Adds a task to the FIFO runqueue. By default, the task is added to the back
  // of the FIFO ('back' == true), but will be added to the front of the FIFO if
  // 'back' == false or if 'task->prio_boost' == true. When 'task->prio_boost'
  // == true, the task was unexpectedly preempted (e.g., by CFS) and could be
  // holding a critical lock, so we want to schedule it again as soon as
  // possible so it can release the lock. This could improve performance.
  void Enqueue(ShinjukuTask* task, bool back = true);

  // Removes and returns the task at the front of the runqueue.
  ShinjukuTask* Dequeue();

  // Returns (but does not remove) the task at the front of the runqueue.
  ShinjukuTask* Peek();

  // Prints all tasks (includin tasks not running or on the runqueue) managed by
  // the global agent.
  void DumpAllTasks();

  // Updates the task's runtime and performs some consistency checks.
  void UpdateTaskRuntime(ShinjukuTask* task, absl::Duration new_runtime,
                         bool update_elapsed_runtime);

  // Callback when a sched item is updated.
  void SchedParamsCallback(ShinjukuOrchestrator& orch,
                           const ShinjukuSchedParams* sp, Gtid oldgtid);

  // Handles a new process that has at least one of its threads enter the ghOSt
  // scheduling class (e.g., via sched_setscheduler()).
  void HandleNewGtid(ShinjukuTask* task, pid_t tgid);

  // Returns 'true' if a CPU can be scheduled by ghOSt. Returns 'false'
  // otherwise, usually because a higher-priority scheduling class (e.g., CFS)
  // is currently using the CPU.
  bool Available(const Cpu& cpu);

  CpuState* cpu_state_of(const ShinjukuTask* task);

  CpuState* cpu_state(const Cpu& cpu) { return &cpu_states_[cpu.id()]; }

  size_t RunqueueSize() const {
    size_t size = 0;
    for (const auto& [qos, rq] : run_queue_) {
      size += rq.size();
    }
    return size;
  }

  bool RunqueueEmpty() const { return RunqueueSize() == 0; }

  // Returns the highest-QoS runqueue that has at least one task enqueued.
  // Must call this on a non-empty runqueue.
  uint32_t FirstFilledRunqueue() const {
    for (auto it = run_queue_.rbegin(); it != run_queue_.rend(); it++) {
      if (!it->second.empty()) {
        return it->first;
      }
    }
    // A precondition for this method is that the runqueue must be non-empty, so
    // if we get down here, that precondition was not upheld.
    CHECK(false);
    // Return something so that the compiler doesn't complain, but we will never
    // execute this return statement due to the 'CHECK(false)' above.
    return std::numeric_limits<uint32_t>::max();
  }

  CpuState cpu_states_[MAX_CPUS];

  std::atomic<int32_t> global_cpu_;
  LocalChannel global_channel_;
  int num_tasks_ = 0;
  bool in_discovery_ = false;

  // Map from QoS level to runqueue
  // We use an 'std::map' rather than 'absl::flat_hash_map' because we need to
  // iterate on the map in order of QoS level.
  std::map<uint32_t, std::deque<ShinjukuTask*>> run_queue_;
  std::vector<ShinjukuTask*> paused_repeatables_;
  std::vector<ShinjukuTask*> yielding_tasks_;
  absl::flat_hash_map<pid_t, std::shared_ptr<ShinjukuOrchestrator>> orchs_;
  const ShinjukuOrchestrator::SchedCallbackFunc kSchedCallbackFunc =
      absl::bind_front(&ShinjukuScheduler::SchedParamsCallback, this);
  const absl::Duration preemption_time_slice_;
};

// Initializes the task allocator and the Shinjuku scheduler.
std::unique_ptr<ShinjukuScheduler> SingleThreadShinjukuScheduler(
    Enclave* enclave, CpuList cpulist, int32_t global_cpu,
    absl::Duration preemption_time_slice);

// Operates as the Global or Satellite agent depending on input from the
// global_scheduler->GetGlobalCPU callback.
class ShinjukuAgent : public LocalAgent {
 public:
  ShinjukuAgent(Enclave* enclave, Cpu cpu, ShinjukuScheduler* global_scheduler)
      : LocalAgent(enclave, cpu), global_scheduler_(global_scheduler) {}

  void AgentThread() override;
  Scheduler* AgentScheduler() const override { return global_scheduler_; }

 private:
  ShinjukuScheduler* global_scheduler_;
};

class ShinjukuConfig : public AgentConfig {
 public:
  ShinjukuConfig() {}
  ShinjukuConfig(Topology* topology, CpuList cpulist, Cpu global_cpu)
      : AgentConfig(topology, std::move(cpulist)), global_cpu_(global_cpu) {}

  Cpu global_cpu_{Cpu::UninitializedType::kUninitialized};
  absl::Duration preemption_time_slice_;
};

// An global agent scheduler.  It runs a single-threaded Shinjuku scheduler on
// the global_cpu.
template <class EnclaveType>
class FullShinjukuAgent : public FullAgent<EnclaveType> {
 public:
  explicit FullShinjukuAgent(ShinjukuConfig config)
      : FullAgent<EnclaveType>(config) {
    global_scheduler_ = SingleThreadShinjukuScheduler(
        &this->enclave_, *this->enclave_.cpus(), config.global_cpu_.id(),
        config.preemption_time_slice_);
    this->StartAgentTasks();
    this->enclave_.Ready();
  }

  ~FullShinjukuAgent() override {
    // Terminate global agent before satellites to avoid a false negative error
    // from ghost_run(). e.g. when the global agent tries to schedule on a CPU
    // without an active satellite agent.
    int global_cpuid = global_scheduler_->GetGlobalCPUId();

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
    return std::make_unique<ShinjukuAgent>(&this->enclave_, cpu,
                                           global_scheduler_.get());
  }

  void RpcHandler(int64_t req, const AgentRpcArgs& args,
                  AgentRpcResponse& response) override {
    switch (req) {
      case ShinjukuScheduler::kDebugRunqueue:
        global_scheduler_->debug_runqueue_ = true;
        response.response_code = 0;
        return;
      default:
        response.response_code = -1;
        return;
    }
  }

 private:
  std::unique_ptr<ShinjukuScheduler> global_scheduler_;
};

}  // namespace ghost

#endif  // GHOST_SCHEDULERS_SHINJUKU_SHINJUKU_SCHEDULER_H
