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

// Encapsulates the implementation of a per-agent scheduler.
#ifndef GHOST_LIB_SCHEDULER_H_
#define GHOST_LIB_SCHEDULER_H_

#include <memory>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "absl/container/flat_hash_map.h"
#include "lib/channel.h"
#include "lib/enclave.h"
#include "lib/ghost.h"

namespace ghost {

// REQUIRES: All Task implementations should derive from Task.
struct Task {
  explicit Task(Gtid gtid, struct ghost_sw_info sw_info)
      : gtid(gtid), status_word(StatusWord(gtid, sw_info)) {}
  virtual ~Task();

  // Provides a light-weight assertion that all events have been processed
  // in-order and no messages were dropped.
  // REQUIRES: Should be invoked for each message associated with *this.
  inline void Advance(uint32_t next_seqnum) {
    CHECK_EQ(seqnum + 1, next_seqnum);  // Assert no missed messages for now.
    seqnum = next_seqnum;
  }

  Gtid gtid;
  StatusWord status_word;
  uint32_t seqnum = -1;
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
    enclave_->AttachScheduler(this);
  }
  virtual ~Scheduler() {}

  // Called once all Enclave participants have been constructed to synchronize
  // late initialization.  See Enclave::Ready() for full details.
  virtual void EnclaveReady() {}

  virtual void DumpState(Cpu cpu, int flags) {}

 protected:
  Enclave* enclave() const { return enclave_; }
  Topology* topology() const { return enclave_->topology(); }
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

  // Returns the Task* associated with <gtid>.
  virtual TaskType* GetTask(Gtid gtid) = 0;
  // Returns the Task* associated with <gtid>.  If no Task exists, then allocate
  // a task and use the given status word info.
  virtual TaskType* GetTask(Gtid gtid, struct ghost_sw_info sw_info) = 0;
  virtual void FreeTask(TaskType* task) = 0;

  typedef std::function<bool(Gtid gtid, const TaskType* task)> TaskCallbackFunc;
  virtual void ForEachTask(TaskCallbackFunc func) = 0;
};

// A Scheduler implementation capable of decoding raw messages (from a Channel),
// associating them with a Task-derived type and dispatching to an appropriate
// scheduling-class method.
template <typename TaskType>
class BasicDispatchScheduler : public Scheduler {
  static_assert(std::is_base_of_v<Task, TaskType>);

 public:
  // REQUIRES: <allocator> must be thread-safe if being used by concurrent
  // schedulers.
  BasicDispatchScheduler(Enclave* enclave, CpuList cpus,
                         std::shared_ptr<TaskAllocator<TaskType>> allocator)
      : Scheduler(enclave, cpus), allocator_(std::move(allocator)) {}
  ~BasicDispatchScheduler() override {}

  virtual void DispatchMessage(const Message& msg);

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

  virtual void CpuTick(const Message& msg) = 0;
  virtual void CpuNotIdle(const Message& msg) = 0;

  TaskAllocator<TaskType>* allocator() const { return allocator_.get(); }

 private:
  std::shared_ptr<TaskAllocator<TaskType>> const allocator_;
};

// A single-threaded (thread-hostile) Task allocator implementation suitable for
// use with global-scheduling models.
template <typename TaskType>
class SingleThreadMallocTaskAllocator : public TaskAllocator<TaskType> {
  static_assert(std::is_base_of_v<Task, TaskType>);

 public:
  TaskType* GetTask(Gtid gtid) override;
  TaskType* GetTask(Gtid gtid, struct ghost_sw_info sw_info) override;
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
  virtual TaskType* AllocTaskImpl(Gtid gtid, struct ghost_sw_info sw_info) {
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
  if (gtid.id() == 0) return nullptr;

  auto it = task_map_.find(gtid.id());
  if (it != task_map_.end()) return it->second;

  return nullptr;
}

template <typename TaskType>
TaskType* SingleThreadMallocTaskAllocator<TaskType>::GetTask(
    Gtid gtid, struct ghost_sw_info sw_info) {
  TaskType* t = SingleThreadMallocTaskAllocator<TaskType>::GetTask(gtid);
  if (t) return t;

  if (gtid.id() == 0) return nullptr;

  TaskType* new_task = AllocTaskImpl(gtid, sw_info);
  auto pair = std::make_pair(gtid.id(), new_task);
  task_map_.insert(pair);
  return pair.second;
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
  TaskType* GetTask(Gtid gtid, struct ghost_sw_info sw_info) override {
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
      default:
        GHOST_ERROR("Unhandled message type %d", msg.type());
        break;
    }
    return;
  }

  // Task messages.
  GHOST_DPRINT(3, stderr, "%s", msg.stringify());

  Gtid gtid = msg.gtid();
  TaskType* task;
  if (ABSL_PREDICT_FALSE(msg.type() == MSG_TASK_NEW)) {
    // Expect to create a new Task for this TaskNew message. If there is already
    // a task present with a matching GTID, something has gone wrong, for
    // example:
    // - GTID collision.
    // - TaskDeparted has not removed the task yet.
    CHECK_EQ(allocator()->GetTask(gtid), nullptr);

    const ghost_msg_payload_task_new* payload =
        static_cast<const ghost_msg_payload_task_new*>(msg.payload());
    task = allocator()->GetTask(gtid, payload->sw_info);
  } else {
    task = allocator()->GetTask(gtid);
  }
  if (ABSL_PREDICT_FALSE(!task)) {
    GHOST_ERROR("Unable to find task for msg %s", msg.stringify());
    return;
  }

  // DEPARTED can be delivered for 'task' regardless of its state. All other
  // messages must be delivered either when it is on a CPU or waking up.
  CHECK(!task->preempted || msg.type() == MSG_TASK_DEPARTED);

  bool update_seqnum = true;

  // Above asserts we've missed no messages, state below should be consistent.
  switch (msg.type()) {
    case MSG_NOP:
      GHOST_ERROR("Saw MSG_NOP! e=%d l=%d\n", msg.empty(), msg.length());
      break;
    case MSG_TASK_NEW:
      TaskNew(task, msg);
      update_seqnum = false;    // `TaskNew()` initializes sequence number.
      break;
    case MSG_TASK_PREEMPT:
      TaskPreempted(task, msg);
      break;
    case MSG_TASK_DEAD:
      TaskDead(task, msg);
      update_seqnum = false;    // `task` pointer may no longer be valid.
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
      update_seqnum = false;    // `task` pointer may no longer be valid.
      break;
    case MSG_TASK_SWITCHTO:
      TaskSwitchto(task, msg);
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
