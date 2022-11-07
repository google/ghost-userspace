// Copyright 2022 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef GHOST_SCHEDULERS_CFS_CFS_SCHEDULER_H_
#define GHOST_SCHEDULERS_CFS_CFS_SCHEDULER_H_

#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <ostream>
#include <set>
#include <vector>

#include "absl/strings/str_format.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "lib/agent.h"
#include "lib/base.h"
#include "lib/scheduler.h"

static const absl::Time start = absl::Now();

namespace ghost {

class CfsTaskState;
std::ostream& operator<<(std::ostream& os, const CfsTaskState& state);

// We could use just "enum class", but embedding an enum (which is implicitly
// convertable to an int) makes a lot of the debugging code simpler. We could do
// some hackery like static_cast<typename
// std::underlying_type<CfsTaskState>::type>(s) to go to an int, but even so,
// enum classes can't have member functions, which makes it very easy to embed
// debugging asserts.
class CfsTaskState {
 public:
  enum State {
    kBlocked = 0,  // Task cannot run
    kRunnable,     // Task can run
    kRunning,      // Task is running (up to preemption by the agent itself)
    kDone,         // Task is dead or departed
    kNumStates,
  };

  explicit CfsTaskState(State state) : state_(state), task_name_("") {}
  explicit CfsTaskState(State state, absl::string_view task_name)
      : state_(state), task_name_(task_name) {}

  // Make sure no one accidentally does something that'll mess up tracking like
  // task.run_state = CfsTaskState::kBlocked.
  CfsTaskState(const CfsTaskState&) = delete;
  CfsTaskState& operator=(const CfsTaskState&) = delete;

  State Get() const { return state_; }
  void Set(State state) {
#ifndef NDEBUG
    // These assertions are expensive, relatively, so don't even try them unless
    // we are in debug mode.
    state_trace_.push_back(state);
    AssertValidTransition(state);
#endif
    state_ = state;
  }

 private:
#ifndef NDEBUG
  void AssertValidTransition(State next);
  const std::map<State, uint64_t>& GetTransitionMap() {
    static const auto* map =
        new std::map<State, uint64_t>{{kBlocked, kToBlocked},
                                      {kRunnable, kToRunnable},
                                      {kRunning, kToRunning},
                                      {kDone, kToDone}};
    return *map;
  }
#endif  // !NDEBUG

  State state_;
  absl::string_view task_name_;

#ifndef NDEBUG
  std::vector<State> state_trace_;
  // State Transition Map. Each kToBlah encodes valid states such that we can
  // transition to blah. To validate that we can go from kFoo to kBar, we check
  // that the correct bit it set. e.g. (kToFoo & (1 << kBar)) == 1 iff kBar ->
  // kFoo is valid.
  constexpr static uint64_t kToBlocked =
      (1 << CfsTaskState::kRunning) + (1 << CfsTaskState::kBlocked);
  constexpr static uint64_t kToRunnable = (1 << kBlocked) + (1 << kRunning);
  constexpr static uint64_t kToRunning = (1 << kRunnable) + (1 << kRunning);
  constexpr static uint64_t kToDone =
      (1 << kRunnable) + (1 << kBlocked) + (1 << kRunning);

#endif
};

struct CpuState;

struct CfsTask : public Task<> {
  explicit CfsTask(Gtid d_task_gtid, ghost_sw_info sw_info)
      : Task<>(d_task_gtid, sw_info), vruntime(absl::ZeroDuration()) {}
  ~CfsTask() override {}

  // std::multiset expects one to pass a strict (< not <=) weak ordering
  // function as a template parameter. Technically, this doesn't have to be
  // inside of the struct, but it seems logical to keep this here.
  static bool Less(CfsTask* a, CfsTask* b) {
    if (a->vruntime == b->vruntime) {
      return (uintptr_t)a < (uintptr_t)b;
    }
    return a->vruntime < b->vruntime;
  }

  CfsTaskState run_state =
      CfsTaskState(CfsTaskState::kBlocked, gtid.describe());
  int cpu = -1;

  // Cfs sorts tasks by vruntime, so we need to keep track of how long a task
  // has been running.
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
  void SetMinGranularity(absl::Duration t) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);
  void SetLatency(absl::Duration t) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

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
  absl::Duration MinPreemptionGranularity() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // PickNextTask checks if prev should run again, and if so, returns prev.
  // Otherwise, it picks the task with the smallest vruntime.
  // PickNextTask also is the sync up point for processing state changes to
  // prev. PickNextTask sets the state of its returned task to kOnCpu.
  CfsTask* PickNextTask(CfsTask* prev, TaskAllocator<ghost::CfsTask>* allocator,
                        CpuState* cs) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // Enqueues a new task or a task that is coming from being blocked.
  void EnqueueTask(CfsTask* task) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // Enqueue a task that is transitioning from being on the cpu to off the cpu.
  void PutPrevTask(CfsTask* task) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // Erase 'task' from the runqueue. Task must be on rq.
  void Erase(CfsTask* task) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  size_t Size() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) { return rq_.size(); }

  bool Empty() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) { return Size() == 0; }

  // Needs to be called everytime we touch the rq or update a current task's
  // vruntime.
  void UpdateMinVruntime(CpuState* cs) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // Protects this runqueue and the state of any task assoicated with the rq.
  mutable absl::Mutex mu_;

 private:
  // Inserts a task into the backing runqueue.
  // Preconditons: task->vruntime has been set to a logical value.
  void InsertTaskIntoRq(CfsTask* task) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  absl::Duration min_vruntime_ ABSL_GUARDED_BY(mu_);

  // Unlike in-kernel CFS, we want to have this properties per run-queue instead
  // of system wide.
  absl::Duration min_preemption_granularity_ ABSL_GUARDED_BY(mu_);
  absl::Duration latency_ ABSL_GUARDED_BY(mu_);

  // We use a set as the backing data structure as, according to the
  // C++ standard, it is backed by a red-black tree, which is the backing
  // data structure in CFS in the the kernel. While opaque, using an std::
  // container is easiest way to use a red-black tree short of writing or
  // importing our own.
  std::set<CfsTask*, decltype(&CfsTask::Less)> rq_ ABSL_GUARDED_BY(mu_);
};

struct CpuState {
  // current points to the CfsTask that we most recently picked to run on the
  // cpu. Note, we say most recently picked as a txn could fail leaving us with
  // current pointing to a task that is not currently on cpu.
  CfsTask* current = nullptr;
  // pointer to the kernel ipc queue.
  std::unique_ptr<Channel> channel = nullptr;
  // the run queue responsible from scheduling tasks on this cpu.
  CfsRq run_queue;
  // Should we keep running the current task.
  bool preempt_curr = false;
} ABSL_CACHELINE_ALIGNED;

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
    absl::MutexLock l(&cs->run_queue.mu_);
    bool res = cs->run_queue.Empty();
    return res;
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
  // Empties the channel associated with cpu and dispatches the messages.
  void DrainChannel(const Cpu& cpu);

  // Checks if we should preempt the current task. If so, sets preempt_curr_.
  void CheckPreemptTick(const Cpu& cpu);

  // CfsSchedule looks at the current cpu state and its run_queue, decides what
  // to run next, and then commits a txn. REQUIRES: Called after all messages
  // have been ack'ed otherwise the txn will fail.
  void CfsSchedule(const Cpu& cpu, BarrierToken agent_barrier, bool prio_boost);

  // HandleTaskDone is responsible for remvoing a task from the run queue and
  // freeing it if it is currently !cs->current, otherwise, it defers the
  // freeing to PickNextTask.
  void HandleTaskDone(CfsTask* task, bool from_switchto);

  // Migrate takes task and places it on cpu's run queue.
  void Migrate(CfsTask* task, Cpu cpu, BarrierToken seqnum);
  Cpu SelectTaskRq(CfsTask* task);
  void DumpAllTasks();

  void PingCpu(const Cpu& cpu);

  CpuState* cpu_state(const Cpu& cpu) { return &cpu_states_[cpu.id()]; }

  CpuState* cpu_state_of(const CfsTask* task) {
    CHECK_GE(task->cpu, 0);
    CHECK_LT(task->cpu, MAX_CPUS);
    return &cpu_states_[task->cpu];
  }

  // If called with is_agent_thread = true, then we use the cache'd TLS cpu id
  // as agent threads are local to a single cpu, otherwise, issue a syscall.
  int MyCpu(bool is_agent_thread = true) {
    if (!is_agent_thread) return sched_getcpu();
    // "When thread_local is applied to a variable of block scope the
    // storage-class-specifier static is implied if it does not appear
    // explicitly" - C++ standard.
    // This isn't obvious, so keep the static modifier.
    static thread_local int my_cpu = -1;
    if (my_cpu == -1) {
      my_cpu = sched_getcpu();
    }
    return my_cpu;
  }

  CpuState cpu_states_[MAX_CPUS];
  Channel* default_channel_ = nullptr;

  absl::Duration min_granularity_;
  absl::Duration latency_;

  friend class CfsRq;
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

// TODO: Pull these classes out into different files.
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
