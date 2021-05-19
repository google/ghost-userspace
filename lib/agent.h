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
#include <functional>
#include <thread>

#include "lib/base.h"
#include "lib/enclave.h"
#include "lib/ghost.h"
#include "lib/topology.h"
#include "shared/shmem.h"

namespace {
// eBPF is excluded in open source for now.
inline void bpf_init() {}
}  // namespace

namespace ghost {

class Agent;

// Encapsulation for the per-cpu agent threads.
// Implementations should override "AgentThread()".
class Agent {
 public:
  explicit Agent(Enclave* enclave, const Cpu& cpu)
      : enclave_(enclave), cpu_(cpu) {
  }
  virtual ~Agent();

  // Initiates binding of *this to the constructor passed CPU.  All methods are
  // valid to call on return.
  // REQUIRES: AgentThread() implementation must call SignalReady().
  void Start();

  // Signals Finished() and guarantees that the agent will wake to observe it.
  // Returns when the thread associated with *this has completed its tear-down.
  // REQUIRES: May only be called once.
  void Terminate();

  // Returns true iff Terminate() has been called.
  // Agents should test Finished() before each call to Run()
  inline bool Finished() { return finished_.HasBeenNotified(); }

  Cpu cpu() const { return cpu_; }

  // Schedule the Agent to run on its CPU.  Can fail only if the CPU is
  // currently unavailable.
  // REQUIRES: Start() has been called.
  bool Ping();

  Gtid gtid() const { return gtid_; }

  // REQUIRES: Start() has been called.
  bool cpu_avail() const { return status_word().cpu_avail(); }
  bool boosted_priority() const { return status_word().boosted_priority(); }
  StatusWord::BarrierToken barrier() const { return status_word().barrier(); }
  Enclave* enclave() const { return enclave_; }

 protected:
  // Used by AgentThread() to signal that any internal, e.g. subclassed,
  // initialization is complete and that Start() can return.
  void SignalReady() { ready_.Notify(); }

  // Optionally invoked by AgentThread() to synchronize on enclave readiness.
  void WaitForEnclaveReady() { enclave_ready_.WaitForNotification(); }

  virtual void AgentThread() = 0;

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

// Contains the configuration for an Agent, including the topology, the list of
// cpus, etc.  Pass this to FullAgent.
class AgentConfig {
 public:
  Topology* topology_;
  // If enclave_fd_ is set, then cpus_ is ignored.
  CpuList cpus_;
  int enclave_fd_ = -1;
  bool use_bpf_ = false;

  explicit AgentConfig(Topology* topology = nullptr,
                       CpuList cpus = MachineTopology()->EmptyCpuList())
      : topology_(topology), cpus_(cpus) {}
  virtual ~AgentConfig() {}
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
  ENCLAVE MakeEnclave(AgentConfig config) {
    if (config.enclave_fd_ == -1) {
      return ENCLAVE(config.topology_, config.cpus_);
    }
    return ENCLAVE(config.topology_, config.enclave_fd_);
  }
  explicit FullAgent(AgentConfig config)
      : config_(config), enclave_(MakeEnclave(config)) {
    Ghost::InitCore();
    if (config.use_bpf_) {
      bpf_init();
    }
  }
  virtual ~FullAgent() {
    // Derived dtors should have called TerminateAgentTasks().
    for (auto& agent : agents_) {
      CHECK(agent->Finished());
    }
  }

  virtual int64_t RpcHandler(int64_t req, const AgentRpcArgs& args) = 0;

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
    for (auto cpu : *enclave_.cpus()) {
      agents_.push_back(MakeAgent(cpu));
      agents_.back()->Start();
    }
  }

  // Called by derived dtors
  void TerminateAgentTasks() {
    for (auto& agent : agents_) {
      agent->Terminate();
    }
  }

  const AgentConfig config_;
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
    int64_t rpc_res_;
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

    GhostSignals::IgnoreAll();

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
        sb_->rpc_res_ = full_agent_->RpcHandler(sb_->rpc_req_, sb_->rpc_args_);
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

  uint64_t Rpc(uint64_t req, const AgentRpcArgs& args = AgentRpcArgs()) {
    // Need to prevent concurrent use of the shared memory region.
    static absl::Mutex mtx(absl::kConstInit);
    absl::MutexLock lock(&mtx);

    CHECK(!agent_proc_->IsChild());

    sb_->rpc_req_ = req;
    sb_->rpc_args_ = args;
    sb_->rpc_pending_.Notify();
    sb_->rpc_done_.WaitForNotification();
    sb_->rpc_done_.Reset();
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
  std::unique_ptr<SharedBlob> sb_;
};

}  // namespace ghost

#endif  // GHOST_LIB_AGENT_H_
