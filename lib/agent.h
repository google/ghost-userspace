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

// Encapsulation of ghOSt Agent run-time; including sequence points and the
// agents themselves.
#ifndef GHOST_LIB_AGENT_H_
#define GHOST_LIB_AGENT_H_

// C++ headers
#include <sys/mman.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <thread>

#include "lib/base.h"
#include "lib/enclave.h"
#include "lib/ghost.h"
#include "lib/topology.h"
#include "shared/shmem.h"

namespace ghost {

class Agent;

// Encapsulation for the per-cpu agent threads.
// Implementations should override "AgentThread()".
class Agent {
 public:
  explicit Agent(Enclave* enclave, const Cpu& cpu)
      : enclave_(enclave), cpu_(cpu) {}
  virtual ~Agent();

  // Initiates binding of *this to the constructor passed CPU.  Must call
  // StartComplete.
  // REQUIRES: AgentThread() implementation must call SignalReady().
  void StartBegin();
  // All methods are valid to call when StartComplete returns.
  void StartComplete();
  void Start() {
    StartBegin();
    StartComplete();
  }

  // Signals Finished() and guarantees that the agent will wake to observe it.
  // REQUIRES: May only be called once, and must call TerminateComplete.
  void TerminateBegin();
  // Returns when the thread associated with *this has completed its tear-down.
  // REQUIRES: May only be called once after TerminateBegin.
  void TerminateComplete();
  void Terminate() {
    TerminateBegin();
    TerminateComplete();
  }

  // Returns true iff TerminateBegin() has been called.
  // Agents should test Finished() before each call to Run()
  bool Finished() const { return finished_.HasBeenNotified(); }

  Cpu cpu() const { return cpu_; }

  // Schedule the Agent to run on its CPU.  Can fail only if the CPU is
  // currently unavailable.
  // REQUIRES: StartComplete() has been called.
  bool Ping();

  Gtid gtid() const { return gtid_; }

  // REQUIRES: StartComplete() has been called.
  bool cpu_avail() const { return status_word().cpu_avail(); }
  bool boosted_priority() const { return status_word().boosted_priority(); }
  StatusWord::BarrierToken barrier() const { return status_word().barrier(); }
  Enclave* enclave() const { return enclave_; }

 protected:
  // Used by AgentThread() to signal that any internal, e.g. subclassed,
  // initialization is complete and that StartComplete() can return.
  void SignalReady() { ready_.Notify(); }

  // Must be invoked by AgentThread() to synchronize on enclave readiness.
  void WaitForEnclaveReady() { enclave_ready_.WaitForNotification(); }

  virtual void AgentThread() = 0;
  virtual Scheduler* AgentScheduler() const { return nullptr; }

  const StatusWord& status_word() const { return status_word_; }

 private:
  void WaitForExitNotification() {
    CHECK(Finished());
    do_exit_.WaitForNotification();
  }

  void EnclaveReady() { enclave_ready_.Notify(); }

  Enclave* const enclave_;
  void ThreadBody();

  Gtid gtid_;
  Cpu cpu_;
  Notification ready_, finished_, enclave_ready_, do_exit_;
  StatusWord status_word_;

  std::thread thread_;

  // The purpose of this variable is to ensure 'CheckVersion' runs even if it is
  // not called directly.  Calling 'CheckVersion' automatically on process
  // startup fixes this issue as the static method will fail on the 'CHECK_EQ'
  // and the process will crash if the versions do not match.
  static const bool kVersionCheck;

  friend class Enclave;
};

// A buffer that may be used within the RPC shared memory region to transmit
// abitrary plain-old-data.
//
// DISCLAIMER: The serialization scheme here is only meant to be used for the
// RPC mechanism operating over the shared memory region on a single machine.
// Otherwise, it is not guaranteed that two arbitrary processes will be able
// to serialize/deserialize the data in a consistent manner (for instance, due
// to differences in struct padding, endianness, etc.).
template <size_t BufferBytes = 1024 /* 1 KiB */>
struct AgentRpcBuffer {
  // Converts the input to raw bytes and stores them in the internal data array.
  // Note that T shouldn't contain any pointers, since these pointers will not
  // have meaning for the process on the other side of the shared memory region.
  // See disclaimer attached to the comment for this struct.
  template <class T>
  void Serialize(const T& t) {
    static_assert(std::is_trivially_copyable<T>::value,
                  "Template type needs to be trivially copyable.");
    static_assert(!std::is_pointer<T>::value,
                  "Template type must not be a pointer.");
    static_assert(sizeof(T) <= BufferBytes,
                  "Template type cannot be larger than the buffer.");

    const std::byte* serialized =
        reinterpret_cast<const std::byte*>(&t);
    std::copy_n(serialized, sizeof(T), std::begin(data));
  }

  // Converts the raw bytes in the internal data array to the given type.
  // See disclaimer attached to the comment for this struct.
  template <class T>
  T Deserialize() const {
    static_assert(std::is_trivially_copyable<T>::value,
                  "Template type needs to be trivially copyable.");
    static_assert(!std::is_pointer<T>::value,
                  "Template type must not be a pointer.");
    static_assert(sizeof(T) <= BufferBytes,
                  "Template type cannot be larger than the buffer.");

    T t;
    std::byte* deserialized = reinterpret_cast<std::byte*>(&t);
    std::copy_n(std::begin(data), sizeof(T), deserialized);

    return t;
  }

  // This is a region where arbitrary bytes of data can be written (ie. when the
  // RPC mechanism needs to return more than just a response code). Intended to
  // be used with the Serialize/Deserialize methods. We use a byte array instead
  // of typing this as a templated type, since a given agent might want
  // different RPCs to return different types of responses (all of which must
  // fit within the shared memory region).
  std::array<std::byte, BufferBytes> data;
};

// Encapsulation for any arguments that might need to be passed as part of an
// RPC. These will be included in the shared memory region, which the
// AgentProcess uses to communicate with the main agent thread.
// Since this data is copied to the shared memory region to be consumed by a
// process with a separate address space, only raw data is useful here (ie. no
// pointers).
struct AgentRpcArgs {
  int64_t arg0 = 0;
  int64_t arg1 = 0;

  // This buffer may be used to serialize arbitrary plain-old-data as part of
  // the RPC arguments.
  AgentRpcBuffer<> buffer;
};

// Encapsulates the response for an RPC.
struct AgentRpcResponse {
  // Most RPC functions will only need to return a value via this response_code.
  int64_t response_code = -1;

  // This response buffer may be used to serialize arbitrary plan-old-data as
  // part of the RPC response.
  AgentRpcBuffer<> buffer;
};

// A full Agent entity, not to be confused with individual Agent tasks.
// This is a collection of agent tasks and scheduler, connected to an enclave.
// Derived classes implement specific agents, such as the GlobalEdfAgent.
//
// The agent tasks are all SCHED_GHOST class tasks.  Our caller is the task
// that creates the agents and remains a CFS / SCHED_NORMAL task (or whatever
// it was before).
//
// Most agents operate on a LocalEnclave (i.e. the kernel ABI), but you can
// replace that with any Enclave
template <class ENCLAVE = LocalEnclave>
class FullAgent {
 public:
  explicit FullAgent(AgentConfig config) : enclave_(config) {
    Ghost::InitCore();
  }
  virtual ~FullAgent() {
    // Derived dtors should have called TerminateAgentTasks().
    for (auto& agent : agents_) {
      CHECK(agent->Finished());
    }
  }

  virtual void RpcHandler(int64_t req, const AgentRpcArgs& args,
                          AgentRpcResponse& response) = 0;

  FullAgent(const FullAgent&) = delete;
  FullAgent& operator=(const FullAgent&) = delete;

 protected:
  // Makes an agent of a type specific to a derived FullAgent
  virtual std::unique_ptr<Agent> MakeAgent(const Cpu& cpu) = 0;

  // Called by derived constructors (but with a special syntax to work
  // around how C++ does dependent name lookup):
  //
  //     this->StartAgentTasks();
  //
  // Writing this out as FullAgent<ENCLAVE>::StartAgentTasks() also works
  // but 'this' is easier to type.
  //
  // Details at https://gcc.gnu.org/onlinedocs/gcc/Name-lookup.html
  //
  // Other member variables and functions in this class also need to
  // be adorned with 'this' when referenced by the derived class.
  void StartAgentTasks() {
    // We split start into StartBegin and StartComplete to speed up
    // initialization.  We create all agent tasks, and they all migrate to their
    // cpus and wait until the old agent (if any) dies.
    for (const Cpu& cpu : *enclave_.cpus()) {
      agents_.push_back(MakeAgent(cpu));
      agents_.back()->StartBegin();
    }
    for (auto& agent : agents_) {
      agent->StartComplete();
    }
  }

  // Called by derived dtors
  void TerminateAgentTasks() {
    enclave_.PrepareToExit();
    // Terminating an agent takes O(100us), much of which is due to the
    // munlock_vma_pages_range in the kernel for the agent's stack.  Start to
    // terminate in parallel so that the agent tasks get off cpu quickly in case
    // of an inplace upgrade.  Then during TerminateComplete we'll join on the
    // threads and munmap their stacks.
    for (auto& agent : agents_) {
      agent->TerminateBegin();
    }
    for (auto& agent : agents_) {
      agent->TerminateComplete();
    }
    // Explicitly destroy all Agents, which is when they are finally detached
    // from the enclave.  Agent threads may call GetAgent on other cpus, so we
    // must join on *all* agent threads to be sure it is safe to detach *any*
    // agent thread.
    agents_.clear();
  }

  ENCLAVE enclave_;
  std::vector<std::unique_ptr<Agent>> agents_;
};

// Helper macro to convert from a base Agent's unique pointer to a derived
// class.  The FullAgent's agents_ vector is of Agent, but our derived classes
// often want their own type.
//
// Use like this: agent_down_cast<T*>(foo);
template <typename To, typename From>
inline To agent_down_cast(From* f) {
  static_assert((std::is_base_of<From, std::remove_pointer_t<To>>::value),
                "target type not derived from source type");
  return static_cast<To>(f);
}

// An AgentProcess is a forked process that runs a FullAgent.  The child runs
// the actual FullAgent.  The parent can communicate with the child via a shared
// memory region.  The primary mechanism for communication is a hand-rolled RPC
// system, built on top of Notifications.
template <class FULL_AGENT, class AGENT_CONFIG>
class AgentProcess {
 public:
  // This helper class is a blob of shared memory for sync between parent and
  // forked child.  It should only be constructed in-place in a shmem region,
  // otherwise the parent and child will have separate copies of the blob.  We
  // could use a bare struct, but the class will auto-construct its members
  // in-place.
  class SharedBlob {
   public:
    explicit SharedBlob() {}
    ~SharedBlob() {
      // Avoid spurious warnings from ~Notification.  We're tearing everything
      // down, and if a parent or child is waiting on a Notification, it should
      // get killed by a signal.
      agent_ready_.Reset();
      kill_agent_.Reset();
      rpc_pending_.Reset();
      rpc_done_.Reset();
    }

    void* operator new(size_t sz) {
      SharedBlob* sb;
      GhostShmem* blob = GhostShmem::GetShmemBlob(sz);

      sb = reinterpret_cast<SharedBlob*>(blob->bytes());
      sb->blob_ = blob;
      return sb;
    }

    void operator delete(void* p) {
      SharedBlob* sb = reinterpret_cast<SharedBlob*>(p);

      delete sb->blob_;
    }

    Notification agent_ready_;  // child to parent
    Notification kill_agent_;   // parent to child

    // Simple RPC channel, passed to FULL_AGENT's Rpc() method.
    // Parent posts request, then notifies rpc_pending_.
    // Child posts response, then notifies rpc_done_.
    int64_t rpc_req_;
    AgentRpcArgs rpc_args_;
    AgentRpcResponse rpc_res_;
    Notification rpc_pending_;  // parent to child
    Notification rpc_done_;     // child_to_parent

   private:
    GhostShmem* blob_;
  };

  // Note the forked child's 'main' thread, which is in CFS, will never leave
  // the constructor.  It will create its own agent tasks.
  explicit AgentProcess(AGENT_CONFIG config) {
    sb_ = absl::make_unique<SharedBlob>();

    agent_proc_ = absl::make_unique<ForkedProcess>();
    if (!agent_proc_->IsChild()) {
      sb_->agent_ready_.WaitForNotification();
      return;
    }

    CHECK_EQ(mlockall(MCL_CURRENT | MCL_FUTURE), 0);

    full_agent_ = absl::make_unique<FULL_AGENT>(config);

    GhostSignals::IgnoreCommon();

    // This spawns another CFS task.  We don't need to join on it, since we (the
    // child from fork) never leave this function.  The rpc_handler thread never
    // dies, at least not until we call _exit below.
    //
    // We could make 'ready' and 'kill' be Rpcs too, but it's simpler to have a
    // thread for the RPCs for our derived class FullAgents and let this thread
    // handle ready/kill for the AgentProcess.
    auto rpc_handler = std::thread([this]() {
      for (;;) {
        sb_->rpc_pending_.WaitForNotification();
        sb_->rpc_pending_.Reset();
        sb_->rpc_res_ = AgentRpcResponse();  // Reset the response.
        full_agent_->RpcHandler(sb_->rpc_req_, sb_->rpc_args_, sb_->rpc_res_);
        sb_->rpc_done_.Notify();
      }
    });

    sb_->agent_ready_.Notify();
    sb_->kill_agent_.WaitForNotification();

    // Explicitly shut down the agent.  We could just exit, but this will run
    // the dtor for the FullAgent, which can check for invariants, make sure
    // all of the client tasks are complete, etc.
    full_agent_ = nullptr;

    // _exit(), and not exit(), so that we don't run any atexit functions, such
    // as those set up by InitGoogle().
    _exit(0);
  }

  ~AgentProcess() {
    sb_->kill_agent_.Notify();
    agent_proc_->WaitForChildExit();
  }

  // Issues the given RPC and returns the RPC response code. This does not
  // return the full response data; RPCs that do not use the full response data
  // don't need to suffer the overhead of copying the full response data.
  //
  // DISCLAIMER: This RPC mechanism is only meant to be used for the shared
  // memory region on a single machine. See AgentRpcBuffer for more details.
  int64_t Rpc(uint64_t req, const AgentRpcArgs& args = AgentRpcArgs()) {
    absl::MutexLock lock(&rpc_mutex_);

    PerformRpc(req, args);
    return sb_->rpc_res_.response_code;
  }

  // Issues the given RPC and returns the full response data. Since this is
  // higher overhead than simply returning the response code, this should only
  // be used by RPCs that actually use the full response data.
  //
  // DISCLAIMER: This RPC mechanism is naturally only meant to be used for the
  // shared memory region on a single machine. See AgentRpcBuffer for more
  // details.
  AgentRpcResponse RpcWithResponse(uint64_t req,
                                   const AgentRpcArgs& args = AgentRpcArgs()) {
    absl::MutexLock lock(&rpc_mutex_);

    PerformRpc(req, args);
    return sb_->rpc_res_;
  }

  void AddExitHandler(std::function<bool(pid_t, int)> handler) {
    agent_proc_->AddExitHandler(handler);
  }

  void KillChild(int signum) { agent_proc_->KillChild(signum); }

  AgentProcess(const AgentProcess&) = delete;
  AgentProcess& operator=(const AgentProcess&) = delete;

 protected:
  // different values in parent and child (based on IsChild())
  std::unique_ptr<ForkedProcess> agent_proc_;

  // only set in child, nullptr in parent
  std::unique_ptr<FULL_AGENT> full_agent_;

  // set in both
  std::unique_ptr<SharedBlob> sb_ ABSL_GUARDED_BY(rpc_mutex_);

 private:
  // Sends the RPC notification.
  void PerformRpc(uint64_t req, const AgentRpcArgs& args)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(rpc_mutex_) {
    CHECK(!agent_proc_->IsChild());

    sb_->rpc_req_ = req;
    sb_->rpc_args_ = args;
    sb_->rpc_pending_.Notify();
    sb_->rpc_done_.WaitForNotification();
    sb_->rpc_done_.Reset();
  }

  // Prevents concurrent use of the shared memory region.
  absl::Mutex rpc_mutex_;
};

}  // namespace ghost

#endif  // GHOST_LIB_AGENT_H_
