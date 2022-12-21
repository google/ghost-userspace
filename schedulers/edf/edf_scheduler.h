// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef GHOST_SCHEDULERS_EDF_EDF_SCHEDULER_H_
#define GHOST_SCHEDULERS_EDF_EDF_SCHEDULER_H_

#include <cstdint>
#include <memory>

#include "absl/container/flat_hash_map.h"
#include "absl/functional/bind_front.h"
#include "third_party/bpf/edf.h"
#include "lib/agent.h"
#include "lib/scheduler.h"
#include "schedulers/edf/edf_bpf.skel.h"
#include "schedulers/edf/orchestrator.h"
#include "shared/prio_table.h"

namespace ghost {

class Orchestrator;

struct EdfTask : public Task<> {
  enum class RunState {
    kBlocked = 0,
    kQueued = 1,
    kOnCpu = 2,
    kYielding = 3,
    kPaused = 4,
  };

  explicit EdfTask(Gtid edf_task_gtid, ghost_sw_info sw_info)
      : Task<>(edf_task_gtid, sw_info) {}
  ~EdfTask() override {}

  inline bool paused() const { return run_state == RunState::kPaused; }
  inline bool blocked() const { return run_state == RunState::kBlocked; }
  inline bool queued() const { return run_state == RunState::kQueued; }
  inline bool oncpu() const { return run_state == RunState::kOnCpu; }
  inline bool yielding() const { return run_state == RunState::kYielding; }

  void SetRuntime(absl::Duration runtime, bool update_elapsed_runtime);
  void UpdateRuntime();

  static std::string_view RunStateToString(EdfTask::RunState run_state) {
    switch (run_state) {
      case EdfTask::RunState::kBlocked:
        return "Blocked";
      case EdfTask::RunState::kQueued:
        return "Queued";
      case EdfTask::RunState::kOnCpu:
        return "OnCpu";
      case EdfTask::RunState::kYielding:
        return "Yielding";
      case EdfTask::RunState::kPaused:
        return "Paused";
        // We will get a compile error if a new member is added to the
        // `EdfTask::RunState` enum and a corresponding case is not added here.
    }
    CHECK(false);
    return "Unknown run state";
  }

  friend inline std::ostream& operator<<(std::ostream& os,
                                         EdfTask::RunState run_state) {
    return os << RunStateToString(run_state);
  }

  RunState run_state = RunState::kBlocked;
  int cpu = -1;

  // Position in runqueue.
  int rq_pos = -1;

  // Priority boosting for jumping past regular edf ordering in the runqueue.
  //
  // A task's priority is boosted on a kernel preemption or a !deferrable
  // wakeup - basically when it may be holding locks or other resources
  // that prevent other tasks from making progress.
  bool prio_boost = false;

  // Cumulative runtime in ns.
  absl::Duration runtime = absl::ZeroDuration();
  // Accrued CPU time in ns.
  absl::Duration elapsed_runtime = absl::ZeroDuration();

  // Whether the last execution was preempted or not.
  bool preempted = false;
  void CalculateSchedDeadline();

  // Comparator for min-heap runqueue.
  struct SchedDeadlineGreater {
    // Returns true if 'a' should be ordered after 'b' in the min-heap
    // and false otherwise.
    //
    // A task with boosted priority takes precedence over other usual
    // discriminating factors like QoS or sched_deadline.
    bool operator()(EdfTask* a, EdfTask* b) const {
      if (a->prio_boost != b->prio_boost) {
        return b->prio_boost;
      } else if (a->sp->GetQoS() != b->sp->GetQoS()) {
        // A task in a higher QoS class has preference
        return a->sp->GetQoS() < b->sp->GetQoS();
      } else {
        return a->sched_deadline > b->sched_deadline;
      }
    }
  };

  // Estimated runtime in ns.
  // This value is first set to the estimate in the corresponding sched item's
  // work class, but is later set to a weighted average of observed runtimes
  absl::Duration estimated_runtime = absl::Milliseconds(1);
  // Absolute deadline by which the sched item must finish.
  absl::Time deadline = absl::InfiniteFuture();
  // Absolute deadline by which the sched item must be scheduled.
  absl::Time sched_deadline = absl::InfiniteFuture();

  const struct SchedParams* sp = nullptr;
  bool has_work = false;
  uint32_t wcid = std::numeric_limits<uint32_t>::max();
};

// Config for a global agent scheduler.
class GlobalConfig : public AgentConfig {
 public:
  GlobalConfig() {
    // Set AgentConfig::tick_config_ = kAllTicks to disable the generic BPF
    // program attached to `skip_tick`.  EDF has its own specific BPF programs,
    // controlled by edf_ticks_.
    tick_config_ = CpuTickConfig::kAllTicks;
  }
  GlobalConfig(Topology* topology, CpuList cpus, const Cpu& global_cpu)
      : AgentConfig(topology, std::move(cpus)), global_cpu_(global_cpu) {
    tick_config_ = CpuTickConfig::kAllTicks;
  }

  Cpu global_cpu_{Cpu::UninitializedType::kUninitialized};
  CpuTickConfig edf_ticks_ = CpuTickConfig::kNoTicks;
};

class EdfScheduler : public BasicDispatchScheduler<EdfTask> {
 public:
  explicit EdfScheduler(Enclave* enclave, CpuList cpulist,
                        std::shared_ptr<TaskAllocator<EdfTask>> allocator,
                        const GlobalConfig& config);
  ~EdfScheduler() final;

  void EnclaveReady() final;
  Channel& GetDefaultChannel() final { return global_channel_; };

  void TaskNew(EdfTask* task, const Message& msg) final;
  void TaskRunnable(EdfTask* task, const Message& msg) final;
  void TaskDead(EdfTask* task, const Message& msg) final;
  void TaskDeparted(EdfTask* task, const Message& msg) final;
  void TaskYield(EdfTask* task, const Message& msg) final;
  void TaskBlocked(EdfTask* task, const Message& msg) final;
  void TaskPreempted(EdfTask* task, const Message& msg) final;

  void DiscoveryStart() final;
  void DiscoveryComplete() final;

  bool Empty() { return num_tasks_ == 0; }

  void UpdateSchedParams();

  void UpdateRunqueue(EdfTask* task);
  void RemoveFromRunqueue(EdfTask* task);
  void UpdateRunqueuePosition(uint32_t pos);
  void CheckRunQueue();

  void GlobalSchedule(const StatusWord& agent_sw, BarrierToken agent_sw_last);

  int32_t GetGlobalCPUId() {
    return global_cpu_.load(std::memory_order_acquire);
  }
  void SetGlobalCPU(const Cpu& cpu) {
    global_cpu_.store(cpu.id(), std::memory_order_release);
  }
  void PickNextGlobalCPU();

  void DumpState(const Cpu& cpu, int flags) final;
  std::atomic<bool> debug_runqueue_ = false;

  static const int kDebugRunqueue = 1;

 private:
  bool PreemptTask(EdfTask* prev, EdfTask* next, BarrierToken agent_barrier);
  void Yield(EdfTask* task);
  void Unyield(EdfTask* task);
  void Enqueue(EdfTask* task);
  EdfTask* Dequeue();
  EdfTask* Peek();
  void DumpAllTasks();

  void UpdateTaskRuntime(EdfTask* task, absl::Duration new_runtime,
                         bool update_elapsed_runtime);
  void SchedParamsCallback(Orchestrator& orch, const SchedParams* sp,
                           Gtid oldgtid);

  void HandleNewGtid(EdfTask* task, pid_t tgid);

  bool Available(const Cpu& cpu);

  struct CpuState {
    EdfTask* current = nullptr;
    EdfTask* next = nullptr;
    const Agent* agent = nullptr;
  } ABSL_CACHELINE_ALIGNED;
  CpuState* cpu_state_of(const EdfTask* task);
  inline CpuState* cpu_state(const Cpu& cpu) { return &cpu_states_[cpu.id()]; }
  CpuState cpu_states_[MAX_CPUS];

  std::atomic<int32_t> global_cpu_;
  LocalChannel global_channel_;
  int num_tasks_ = 0;
  bool in_discovery_ = false;
  // Heapified runqueue
  std::vector<EdfTask*> run_queue_;
  std::vector<EdfTask*> yielding_tasks_;
  absl::flat_hash_map<pid_t, std::unique_ptr<Orchestrator>> orchs_;

  const Orchestrator::SchedCallbackFunc kSchedCallbackFunc =
      absl::bind_front(&EdfScheduler::SchedParamsCallback, this);

  struct edf_bpf* bpf_obj_;
  struct edf_bpf_per_cpu_data* bpf_data_;
};

std::unique_ptr<EdfScheduler> SingleThreadEdfScheduler(Enclave* enclave,
                                                       CpuList cpulist,
                                                       GlobalConfig& config);

// Operates as the Global or Satellite agent depending on input from the
// global_scheduler->GetGlobalCPU callback.
class GlobalSatAgent : public LocalAgent {
 public:
  GlobalSatAgent(Enclave* enclave, Cpu cpu, EdfScheduler* global_scheduler)
      : LocalAgent(enclave, cpu), global_scheduler_(global_scheduler) {}

  void AgentThread() override;
  Scheduler* AgentScheduler() const override { return global_scheduler_; }

 private:
  EdfScheduler* global_scheduler_;
};

// A global agent scheduler.  It runs a single-threaded EDF scheduler on the
// global_cpu.
template <class EnclaveType>
class GlobalEdfAgent : public FullAgent<EnclaveType> {
 public:
  explicit GlobalEdfAgent(GlobalConfig config)
      : FullAgent<EnclaveType>(config) {
    global_scheduler_ = SingleThreadEdfScheduler(
        &this->enclave_, *this->enclave_.cpus(), config);
    this->StartAgentTasks();
    this->enclave_.Ready();
  }

  ~GlobalEdfAgent() override {
    this->enclave_.SetDeliverCpuAvailability(false);
    this->enclave_.SetDeliverAgentRunnability(false);
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
    return std::make_unique<GlobalSatAgent>(&this->enclave_, cpu,
                                            global_scheduler_.get());
  }

  void RpcHandler(int64_t req, const AgentRpcArgs& args,
                  AgentRpcResponse& response) override {
    switch (req) {
      case EdfScheduler::kDebugRunqueue:
        global_scheduler_->debug_runqueue_ = true;
        response.response_code = 0;
        return;
      default:
        response.response_code = -1;
        return;
    }
  }

 private:
  std::unique_ptr<EdfScheduler> global_scheduler_;
};

}  // namespace ghost

#endif  // GHOST_SCHEDULERS_EDF_EDF_SCHEDULER_H_
