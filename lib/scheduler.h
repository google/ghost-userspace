// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

// Encapsulates the implementation of a per-agent scheduler.
#ifndef GHOST_LIB_SCHEDULER_H_
#define GHOST_LIB_SCHEDULER_H_

#include <atomic>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "absl/container/flat_hash_map.h"
#include "lib/channel.h"
#include "lib/enclave.h"
#include "lib/ghost.h"

namespace ghost {

// A minimal sequence number class implementation that wraps the atomic. There
// are two different usages around this implementation:
//
// For operations where a user does not need any memory guarantees:
//   // Memory writes here may be reordered to after the following line.
//   uint32_t seqnum = task->seqnum;
//
//   task->seqnum = seqnum;
//   // Memory writes here may be reordered to before the above line.
//
// For operations where a user needs memory guarantees, especially in message
// draining loop (i.e., any task state changes before increasing the sequence
// number should be visible to anyone who `load`s the updated sequence number):
//   uint32_t seqnum = ...;
//   // Memory writes here should be visible below the following line.
//   task->seqnum.store(seqnum);
//
//   uint32_t seqnum = task->seqnum.load();
//   // All memory writes before the most recent task->seqnum.store() should be
//   // visible here.
class Seqnum {
 public:
  // Loads the sequence number without memory ordering guarantee.
  operator uint32_t() const {
    return seqnum_.load(std::memory_order_relaxed);
  }

  // Stores the sequence number without memory ordering guarantee.
  Seqnum& operator=(uint32_t seqnum) {
    seqnum_.store(seqnum, std::memory_order_relaxed);
    return *this;
  }

  // Loads the sequence number with acquire semantics.
  uint32_t load() {
    return seqnum_.load(std::memory_order_acquire);
  }

  // Stores the sequence number with release semantics.
  void store(uint32_t seqnum) {
    seqnum_.store(seqnum, std::memory_order_release);
  }

 private:
  std::atomic<uint32_t> seqnum_{0};
};

// REQUIRES: All Task implementations should derive from Task.
template <class StatusWordType = LocalStatusWord>
struct Task {
  Task(Gtid gtid, ghost_sw_info sw_info)
      : gtid(gtid), status_word(StatusWordType(gtid, sw_info)) {}

  virtual ~Task() { status_word.Free(); }

  // Provides a light-weight assertion that all events have been processed
  // in-order and no messages were dropped.
  // REQUIRES: Should be invoked for each message associated with *this.
  inline void Advance(uint32_t next_seqnum) {
    // Note: next_seqnum could be greater than seqnum + 1 if BPF has consumed
    // messages. We only assert that events have not been reordered with this
    // check.
    CHECK_GT(next_seqnum, seqnum);
    seqnum.store(next_seqnum);
  }

  Gtid gtid;
  StatusWordType status_word;
  Seqnum seqnum;
};

// A minimal Scheduler-base class.  Contrary to its name, the majority of this
// skeleton's purpose is synchronization with other system abstractions such as
// our Enclave type.  This is intentional as we want to maximize the flexibility
// of implementations.  See BasicDispatchScheduler below for a convenient
// implementation base.
//
// TODO: We probably want to bring our TaskTypes all the way back to the
// Enclave so that we have an atom to interact with (although the Enclave could
// possibly act on the core Task structure).  Let's see where this goes as we
// write some more tests and schedulers.
class Scheduler {
 public:
  // DumpState flags:
  static constexpr int kDumpStateEmptyRQ = 0x1;
  static constexpr int kDumpAllTasks = 0x2;

  Scheduler(Enclave* enclave, CpuList cpus) : enclave_(enclave), cpus_(cpus) {
    if (enclave_) {
      enclave_->AttachScheduler(this);
    }
  }
  virtual ~Scheduler() {}

  // Called once all Enclave participants have been constructed to synchronize
  // late initialization.  See Enclave::Ready() for full details.
  virtual void EnclaveReady() {}

  // Tells the scheduler to discover new tasks from its enclave
  virtual void DiscoverTasks() {}

  // All schedulers must have some channel that is "default".
  virtual Channel& GetDefaultChannel() = 0;
  // Gets the right channel for the current agent with cpu information. Should
  // be called at the beginning of the agent's thread body. Should return the
  // default channel if the channel for the agent cannot be decided yet or the
  // scheduler would like to postpone this decision.
  virtual Channel& GetAgentChannel(const Cpu& cpu) {
    return GetDefaultChannel();
  }

  // Returns a (const) pointer to this scheduler's topology.
  const Topology* SchedTopology() const { return topology(); }

  virtual void DumpState(const Cpu& cpu, int flags) {}

 protected:
  Enclave* enclave() const { return enclave_; }
  Topology* topology() const {
    if (enclave_) {
      return enclave_->topology();
    } else {
      return MachineTopology();
    }
  }
  const CpuList& cpus() const { return cpus_; }

 private:
  Enclave* const enclave_;
  CpuList cpus_;
};

// TaskAllocator provides a shared allocator interface that is capable of owning
// the lifetime and providing the location of Task objects, potentially between
// multiple scheduler instances.
//
// Depending on the scheduling model, tasks may be associated with one, or many
// schedulers; the allocator however, will be shared in this case.  In the case
// that parallel schedulers are executing, the allocator
// implementation must be thread-safe.
template <typename TaskType>
class TaskAllocator {
 public:
  virtual ~TaskAllocator() {}

  // Returns the Task* associated with `gtid` if the `gtid` is already known
  // to the allocator and nullptr otherwise.
  virtual TaskType* GetTask(Gtid gtid) = 0;

  // Returns a tuple with the Task* associated with `gtid` and a boolean
  // indicating whether the Task* was a new allocation (i.e. it was not
  // already known to the allocator).
  virtual std::tuple<TaskType*, bool> GetTask(Gtid gtid,
                                              ghost_sw_info sw_info) = 0;

  virtual void FreeTask(TaskType* task) = 0;

  typedef std::function<bool(Gtid gtid, const TaskType* task)> TaskCallbackFunc;
  virtual void ForEachTask(TaskCallbackFunc func) = 0;
};

// A Scheduler implementation capable of decoding raw messages (from a Channel),
// associating them with a Task-derived type and dispatching to an appropriate
// scheduling-class method.
template <typename TaskType>
class BasicDispatchScheduler : public Scheduler {
  static_assert(is_base_of_template_v<Task, TaskType>);

 public:
  // REQUIRES: <allocator> must be thread-safe if being used by concurrent
  // schedulers.
  BasicDispatchScheduler(Enclave* enclave, CpuList cpus,
                         std::shared_ptr<TaskAllocator<TaskType>> allocator)
      : Scheduler(enclave, std::move(cpus)), allocator_(std::move(allocator)) {}
  ~BasicDispatchScheduler() override {}

  virtual void DispatchMessage(const Message& msg);

  void DiscoverTasks() override {
    DiscoveryStart();

    // The kernel may concurrently modify the SW as we read it.  Many changes
    // involve incrementing the barrier at some point in the future - in
    // particular during task_woken_ghost and when a task blocks.
    //
    // We may miss out on other state changes - the kernel does not guarantee
    // that the barrier protects the status word, only that the barrier prevents
    // the agent from missing a message.  For instance, switchto involves
    // changing the SW, but there is no corresponding message.  For this reason,
    // we rely on the enclave being "quiescent" (no client tasks).
    enclave()->ForEachTaskStatusWord([this](ghost_status_word* sw,
                                            uint32_t region_id, uint32_t idx) {
      ghost_sw_info swi = {.id = region_id, .index = idx};
      TaskType* task;
      uint32_t sw_barrier;
      uint32_t sw_flags;
      uint64_t sw_gtid;
      uint64_t sw_runtime;
      bool had_estale = false;
      int assoc_status;

    retry:
      // Pairs with the smp_store_release() in the kernel.  (READ_ONCE of the
      // barrier and an smp_rmb()).
      sw_barrier = READ_ONCE(sw->barrier);

      std::atomic_thread_fence(std::memory_order_acquire);

      sw_flags = READ_ONCE(sw->flags);
      sw_runtime = READ_ONCE(sw->runtime);
      sw_gtid = READ_ONCE(sw->gtid);

      // We will "blindly associate" the task with our default queue, where we
      // don't actually make sure we received all of the messages before
      // association.  Any previously sent messages were sent to the old agent's
      // queue, with one exception handled below (EEXIST).
      //
      // The seqnum/barrier has two roles: make sure we didn't miss any messages
      // and make sure we don't *later* handle any messages we shouldn't.
      // Association is used to hand-off a task between agent tasks: whoever
      // controls the queue has the right to muck with the Task.  If we
      // associate to a new queue, the old queue may still have a message, which
      // could violate that rule.
      //
      // In this case, we're OK when it comes to Task ownership.  The only queue
      // *in this new agent* that this task could have been using is the Default
      // queue, which is the one we're (re)associating with.  i.e. we're not
      // swapping queues.
      if (!GetDefaultChannel().AssociateTask(Gtid(sw_gtid), sw_barrier,
                                             &assoc_status)) {
        switch (errno) {
          case ENOENT:
            // The task died or departed.  It could have died and the old agent
            // crashed before it received TASK_DEAD.  It could have departed
            // concurrently with our scan.  When we free the task, it will tell
            // the kernel to free the status_word.
            bool allocated;
            std::tie(task, allocated) =
                allocator()->GetTask(Gtid(sw_gtid), swi);
            // It's a bug if we already created this since we do not allow
            // concurrent discovery and message handling.
            if (!allocated) {
              GHOST_ERROR("Already had task for gtid %lu!", sw_gtid);
            }
            allocator()->FreeTask(task);
            return;
          case ESTALE:
            // Task departed and came back into ghost.
            //
            // The ESTALE is due to a mismatch between the barrier in the
            // task's _live_ status_word compared to the barrier we provided
            // from a status_word associated with an earlier incarnation.
            //
            // Reclaim the orphaned status_word (it is not reachable from
            // the task associated with sw_gtid).
            //
            // TODO: this is relevant only if a task has the same gtid across
            // incarnations (this is the case currently). However if we adopt
            // an approach where each incarnation allocates a new gtid then we
            // can drop this special case.
            if (sw_flags & GHOST_SW_F_CANFREE) {
              CHECK_EQ(GhostHelper()->FreeStatusWordInfo(swi), 0);
              return;
            }

            // We shouldn't have *too many* state changes to the task while we
            // are discovering.  Tasks are not allowed to run while we are in
            // discovery.  This is the "system must be quiescent" requirement.
            //
            // Specifically, we can have a TASK_WAKEUP or TASK_DEPARTED.
            // TASK_DEPARTED would give us ENOENT, handled above.  There can be
            // at most one TASK_WAKEUP (since the task won't run until an agent
            // schedules it), so we can get at most one ESTALE.
            if (had_estale) {
              GHOST_ERROR(
                  "Got repeated ESTALEs from a quiescent reassociation for "
                  "gtid %lu, flags %lu!",
                  sw_gtid, sw_flags);
            }
            had_estale = true;
            goto retry;
          default:
            GHOST_ERROR(
                "Failed reassociation for gtid %lu, errno %d, flags %lu",
                sw_gtid, errno, sw_flags);
        }
      }

      if (assoc_status & (GHOST_ASSOC_SF_ALREADY | GHOST_ASSOC_SF_BRAND_NEW)) {
        // The association succeeded, but we need to handle it specially.  In
        // both of these cases, we will eventually get all of the messages for
        // this task and we do not need to discover it.
        //
        // 1) The task's queue was already set to our default queue.  This
        // means that it arrived after we called SetDefaultQueue, and all of
        // the messages for the task's state (TASK_NEW, etc.) have been
        // delivered to us.
        // 2) Regardless of whether or not the task's queue was set, the kernel
        // has not sent a TASK_NEW yet.  This happens when a third party
        // setsched's a running task into ghost; the kernel waits until the task
        // gets off cpu to send the TASK_NEW.
        //
        // In either event, we skip the task during discovery, and will call
        // TaskNew (and potentially TaskDeparted!) when we handle messages
        // later.
        return;
      }

      // Synthesize a message on the stack from the copied SW state.  All ghost
      // messages must be aligned to the *size* of ghost_msg, not to the
      // alignment of a ghost_msg.  This means the payloads will be aligned to
      // that value (8 currently)
      struct {
        ghost_msg header;
        ghost_msg_payload_task_new payload;
      } synth __attribute__((aligned(sizeof(ghost_msg))));
      // Make sure there's no magic padding between the structs.
      static_assert(sizeof(synth) ==
                    sizeof(ghost_msg) + sizeof(ghost_msg_payload_task_new));

      ghost_msg* gm = &synth.header;
      ghost_msg_payload_task_new* tn = &synth.payload;
      gm->type = MSG_TASK_NEW;
      gm->length = sizeof(synth);
      gm->seqnum = sw_barrier;

      tn->gtid = sw_gtid;
      tn->runtime = sw_runtime;
      tn->runnable = !!(sw_flags & GHOST_SW_TASK_RUNNABLE);
      tn->sw_info = swi;

      Message msg(gm);

      bool allocated;
      std::tie(task, allocated) = allocator()->GetTask(Gtid(sw_gtid), swi);
      if (!allocated) {
        GHOST_ERROR("Already had task for gtid %lu!", sw_gtid);
      }
      TaskNew(task, msg);
      TaskDiscovered(task);
    });

    // All tasks that existed before we started to scan the SW region have been
    // discovered, though there may be new tasks, for which the scheduler will
    // receive msssages on its default queue.
    DiscoveryComplete();
  }

 protected:
  // Callbacks to IPC messages delivered by the kernel against `task`.
  // Implementations typically will advance the task's state machine and adjust
  // the runqueue(s) as needed.
  virtual void TaskNew(TaskType* task, const Message& msg) = 0;
  virtual void TaskRunnable(TaskType* task, const Message& msg) = 0;
  virtual void TaskDead(TaskType* task, const Message& msg) = 0;
  virtual void TaskDeparted(TaskType* task, const Message& msg) = 0;
  virtual void TaskYield(TaskType* task, const Message& msg) = 0;
  virtual void TaskBlocked(TaskType* task, const Message& msg) = 0;
  virtual void TaskPreempted(TaskType* task, const Message& msg) = 0;
  virtual void TaskSwitchto(TaskType* task, const Message& msg) {}
  virtual void TaskAffinityChanged(TaskType* task, const Message& msg) {}
  virtual void TaskPriorityChanged(TaskType* task, const Message& msg) {}
  virtual void TaskOnCpu(TaskType* task, const Message& msg) {}

  virtual void TaskDiscovered(TaskType* task) {}
  virtual void DiscoveryStart() {}
  virtual void DiscoveryComplete() {}

  virtual void CpuTick(const Message& msg) {}
  virtual void CpuNotIdle(const Message& msg) {}
  virtual void CpuTimerExpired(const Message& msg) {}
  virtual void CpuAvailable(const Message& msg) {}
  virtual void CpuBusy(const Message& msg) {}
  virtual void AgentBlocked(const Message& msg) {}
  virtual void AgentWakeup(const Message& msg) {}

  TaskAllocator<TaskType>* allocator() const { return allocator_.get(); }

 private:
  std::shared_ptr<TaskAllocator<TaskType>> const allocator_;
};

// A single-threaded (thread-hostile) Task allocator implementation suitable for
// use with global-scheduling models.
template <typename TaskType>
class SingleThreadMallocTaskAllocator : public TaskAllocator<TaskType> {
  static_assert(is_base_of_template_v<Task, TaskType>);

 public:
  TaskType* GetTask(Gtid gtid) override;
  std::tuple<TaskType*, bool> GetTask(Gtid gtid,
                                      ghost_sw_info sw_info) override;
  void FreeTask(TaskType* task) override {
    task_map_.erase(task->gtid.id());
    FreeTaskImpl(task);
  }

  void ForEachTask(
      typename TaskAllocator<TaskType>::TaskCallbackFunc func) override {
    for (const auto& [gtid, task] : task_map_) {
      if (!func(Gtid(gtid), task)) break;
    }
  }

 protected:
  // Can be overridden to replace only the allocator.
  virtual TaskType* AllocTaskImpl(Gtid gtid, ghost_sw_info sw_info) {
    return new TaskType(gtid, sw_info);
  }
  virtual void FreeTaskImpl(TaskType* task) { delete task; }

 private:
  absl::flat_hash_map<int64_t, TaskType*> task_map_;
};

//------------------------------------------------------------------------------
// Implementation details below this line.
//------------------------------------------------------------------------------

template <typename TaskType>
TaskType* SingleThreadMallocTaskAllocator<TaskType>::GetTask(Gtid gtid) {
  auto it = task_map_.find(gtid.id());
  if (it != task_map_.end()) return it->second;

  return nullptr;
}

template <typename TaskType>
std::tuple<TaskType*, bool> SingleThreadMallocTaskAllocator<TaskType>::GetTask(
    Gtid gtid, ghost_sw_info sw_info) {
  TaskType* t = SingleThreadMallocTaskAllocator<TaskType>::GetTask(gtid);
  if (t) return std::make_tuple(t, false);

  TaskType* new_task = AllocTaskImpl(gtid, sw_info);
  auto pair = std::make_pair(gtid.id(), new_task);
  task_map_.insert(pair);
  return std::make_tuple(pair.second, true);
}

// A thread-safe Task allocator implementation suitable for use with
// per-cpu scheduling models.
template <typename TaskType>
class ThreadSafeMallocTaskAllocator
    : public SingleThreadMallocTaskAllocator<TaskType> {
 public:
  TaskType* GetTask(Gtid gtid) override {
    absl::MutexLock lock(&mu_);
    return Parent::GetTask(gtid);
  }

  std::tuple<TaskType*, bool> GetTask(Gtid gtid,
                                      ghost_sw_info sw_info) override {
    absl::MutexLock lock(&mu_);
    return Parent::GetTask(gtid, sw_info);
  }

  void FreeTask(TaskType* task) override {
    absl::MutexLock lock(&mu_);
    Parent::FreeTask(task);
  }

  void ForEachTask(
      typename TaskAllocator<TaskType>::TaskCallbackFunc func) override {
    absl::MutexLock lock(&mu_);
    Parent::ForEachTask(func);
  }

 private:
  absl::Mutex mu_;
  using Parent = SingleThreadMallocTaskAllocator<TaskType>;
};

template <typename TaskType>
void BasicDispatchScheduler<TaskType>::DispatchMessage(const Message& msg) {
  if (msg.type() == MSG_NOP) return;

  // CPU messages.
  if (msg.is_cpu_msg()) {
    switch (msg.type()) {
      case MSG_CPU_TICK:
        CpuTick(msg);
        // Tick messages can get super noisy, so filter them into a higher
        // verbosity level.
        GHOST_DPRINT(4, stderr, "%s", msg.stringify());
        break;
      case MSG_CPU_NOT_IDLE:
        CpuNotIdle(msg);
        break;
      case MSG_CPU_TIMER_EXPIRED:
        CpuTimerExpired(msg);
        break;
      case MSG_CPU_AVAILABLE:
        CpuAvailable(msg);
        break;
      case MSG_CPU_BUSY:
        CpuBusy(msg);
        break;
      case MSG_CPU_AGENT_BLOCKED:
        AgentBlocked(msg);
        break;
      case MSG_CPU_AGENT_WAKEUP:
        AgentWakeup(msg);
        break;
      default:
        GHOST_ERROR("Unhandled message type %d", msg.type());
        break;
    }
    return;
  }

  // Task messages.
  GHOST_DPRINT(3, stderr, "%s", msg.stringify());

  Gtid gtid = msg.gtid();
  CHECK_NE(gtid.id(), 0);

  TaskType* task;
  if (ABSL_PREDICT_FALSE(msg.type() == MSG_TASK_NEW)) {
    const ghost_msg_payload_task_new* payload =
        static_cast<const ghost_msg_payload_task_new*>(msg.payload());
    bool allocated;
    std::tie(task, allocated) = allocator()->GetTask(gtid, payload->sw_info);
    if (ABSL_PREDICT_FALSE(!allocated)) {
      // This probably cannot happen, though we may have corner cases with
      // in-place upgrades.
      //
      // It could also be the sign that something has gone wrong, for example:
      // - GTID collision.
      // - TaskDeparted has not removed the task yet.
      GHOST_DPRINT(0, stderr, "Already had task for gtid %s!", gtid.describe());
      return;
    }
  } else {
    task = allocator()->GetTask(gtid);
    if (ABSL_PREDICT_FALSE(!task)) {
      // This probably cannot happen, though we may have corner cases with
      // in-place upgrades.
      GHOST_DPRINT(0, stderr, "Unable to find task for msg %s",
                   msg.stringify());
      return;
    }
  }

  bool update_seqnum = true;

  // Above asserts we've missed no messages, state below should be consistent.
  switch (msg.type()) {
    case MSG_NOP:
      GHOST_ERROR("Saw MSG_NOP! e=%d l=%d\n", msg.empty(), msg.length());
      break;
    case MSG_TASK_NEW:
      TaskNew(task, msg);
      update_seqnum = false;  // `TaskNew()` initializes sequence number.
      break;
    case MSG_TASK_PREEMPT:
      TaskPreempted(task, msg);
      break;
    case MSG_TASK_DEAD:
      TaskDead(task, msg);
      update_seqnum = false;  // `task` pointer may no longer be valid.
      break;
    case MSG_TASK_WAKEUP:
      TaskRunnable(task, msg);
      break;
    case MSG_TASK_BLOCKED:
      TaskBlocked(task, msg);
      break;
    case MSG_TASK_YIELD:
      TaskYield(task, msg);
      break;
    case MSG_TASK_DEPARTED:
      TaskDeparted(task, msg);
      update_seqnum = false;  // `task` pointer may no longer be valid.
      break;
    case MSG_TASK_SWITCHTO:
      TaskSwitchto(task, msg);
      break;
    case MSG_TASK_AFFINITY_CHANGED:
      TaskAffinityChanged(task, msg);
      break;
    case MSG_TASK_PRIORITY_CHANGED:
      TaskPriorityChanged(task, msg);
      break;
    case MSG_TASK_ON_CPU:
      TaskOnCpu(task, msg);
      break;
    default:
      GHOST_ERROR("Unhandled message type %d", msg.type());
      break;
  }

  if (ABSL_PREDICT_TRUE(update_seqnum)) {
    task->Advance(msg.seqnum());
  }
}

}  // namespace ghost

#endif  // GHOST_LIB_SCHEDULER_H_
