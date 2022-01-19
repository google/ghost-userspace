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

// A currently poorly organized collection of core helpers.
#ifndef GHOST_LIB_GHOST_H_
#define GHOST_LIB_GHOST_H_

#include <sys/ioctl.h>

#include <csignal>
#include <cstdint>
#include <unordered_map>

#include "absl/container/flat_hash_map.h"
#include "absl/flags/declare.h"
#include "absl/flags/flag.h"
#include "kernel/ghost_uapi.h"
#include "lib/base.h"
#include "lib/logging.h"
#include "lib/topology.h"

ABSL_DECLARE_FLAG(int32_t, verbose);

namespace ghost {

static inline int verbose() {
  static int v = absl::GetFlag(FLAGS_verbose);
  return v;
}

static inline void set_verbose(int32_t v) { absl::SetFlag(&FLAGS_verbose, v); }

class StatusWordTable {
 public:
  // Create or attach to a preexisting status word region in the enclave.
  // `enclave_fd` is the fd for the enclave directory, e.g.
  // /sys/fs/ghost/enclave_1/.  `id` is a user-provided identifier for the
  // status word region.  `numa_node` is the NUMA node from which the kernel
  // should allocate the memory for the staus word region.
  StatusWordTable(int enclave_fd, int id, int numa_node);
  ~StatusWordTable();

  int id() const { return header_->id; }
  struct ghost_status_word* get(unsigned int index) {
    CHECK_LT(index, header_->capacity);
    return &table_[index];
  }

  StatusWordTable(const StatusWordTable&) = delete;
  StatusWordTable(StatusWordTable&&) = delete;

  // Runs l on every non-agent, ghost-task status word.
  void ForEachTaskStatusWord(
      const std::function<void(struct ghost_status_word* sw, uint32_t region_id,
                               uint32_t idx)>
          l) {
    for (int i = 0; i < header_->capacity; ++i) {
      struct ghost_status_word* sw = get(i);
      if (!(sw->flags & GHOST_SW_F_INUSE)) {
        continue;
      }
      if (sw->flags & GHOST_SW_TASK_IS_AGENT) {
        continue;
      }
      l(sw, id(), i);
    }
  }

 private:
  int fd_;
  size_t map_size_ = 0;
  struct ghost_sw_region_header* header_ = nullptr;
  struct ghost_status_word* table_ = nullptr;
};

// TODO: Syscall definition needs fixing for any hope of 32-bit compat.
class Ghost {
 public:
  static void InitCore();

  static int Run(const Gtid gtid, const uint32_t agent_barrier,
                 const uint32_t task_barrier, const int cpu, const int flags) {
    return syscall(__NR_ghost_run, gtid.id(), agent_barrier, task_barrier, cpu,
                   flags);
  }

  static int SyncCommit(cpu_set_t* const cpuset) {
    ghost_ioc_commit_txn data = {
        .mask_ptr = cpuset,
        .mask_len = sizeof(cpu_set_t),
        .flags = 0,
    };
    return ioctl(gbl_ctl_fd_, GHOST_IOC_SYNC_GROUP_TXN, &data);
  }

  static int Commit(cpu_set_t* const cpuset) {
    ghost_ioc_commit_txn data = {
        .mask_ptr = cpuset,
        .mask_len = sizeof(cpu_set_t),
        .flags = 0,
    };
    return ioctl(gbl_ctl_fd_, GHOST_IOC_COMMIT_TXN, &data);
  }

  static int Commit(const int cpu) {
    cpu_set_t cpuset;

    CHECK_GE(cpu, 0);
    CHECK_LT(cpu, CPU_SETSIZE);

    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    return Commit(&cpuset);
  }

  static int CreateQueue(const int elems, const int node, const int flags,
                         uint64_t& mapsize) {
    ghost_ioc_create_queue data = {
        .elems = elems,
        .node = node,
        .flags = flags,
    };
    int fd = ioctl(gbl_ctl_fd_, GHOST_IOC_CREATE_QUEUE, &data);
    mapsize = data.mapsize;
    return fd;
  }

  // Configure the set of candidate cpus to wake up when a message is produced
  // into the queue denoted by 'queue_fd'.
  //
  // Returns 0 on success and -1 on failure ('errno' is set on failure).
  static int ConfigQueueWakeup(const int queue_fd, const cpu_set_t& cpuset,
                               const int flags) {
    std::vector<ghost_agent_wakeup> wakeup;

    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
      if (CPU_ISSET(cpu, &cpuset)) {
        wakeup.push_back({
            .cpu = cpu,
            .prio = 0,
        });
      }
    }

    int ninfo = wakeup.size();

    ghost_ioc_config_queue_wakeup data = {
        .qfd = queue_fd,
        .w = wakeup.data(),
        .ninfo = ninfo,
        .flags = flags,
    };

    return ioctl(gbl_ctl_fd_, GHOST_IOC_CONFIG_QUEUE_WAKEUP, &data);
  }

  static int AssociateQueue(const int queue_fd, const ghost_type type,
                            const uint64_t arg, const int barrier,
                            const int flags, int* status) {
    ghost_msg_src msg_src = {
        .type = type,
        .arg = arg,
    };

    ghost_ioc_assoc_queue data = {
        .fd = queue_fd,
        .src = msg_src,
        .barrier = barrier,
        .flags = flags,
    };

    int err = ioctl(gbl_ctl_fd_, GHOST_IOC_ASSOC_QUEUE, &data);

    if (status != nullptr) {
      *status = data.status;
    }
    return err;
  }

  static int SetDefaultQueue(const int queue_fd) {
    ghost_ioc_set_default_queue data = {
        .fd = queue_fd,
    };

    return ioctl(gbl_ctl_fd_, GHOST_IOC_SET_DEFAULT_QUEUE, &data);
  }

  static int GetStatusWordInfo(const ghost_type type, const uint64_t arg,
                               ghost_sw_info* const info) {
    struct ghost_ioc_sw_get_info data;
    data.request.type = type;
    data.request.arg = arg;
    int err = ioctl(gbl_ctl_fd_, GHOST_IOC_SW_GET_INFO, &data);
    if (err) return err;
    *info = data.response;
    return 0;
  }

  static int FreeStatusWordInfo(ghost_sw_info* const info) {
    return ioctl(gbl_ctl_fd_, GHOST_IOC_SW_FREE, info);
  }

  // This is needed for when a sched item is updated in the shared PrioTable. A
  // write to a sched item indicates that the sched item was updated for a new
  // closure. We want to update the runtime of the task so that we don't bill
  // the new closure for CPU time used by the old closure.
  static int GetTaskRuntime(const Gtid gtid, absl::Duration* const cpu_time) {
    ghost_ioc_get_cpu_time data = {
        .gtid = gtid.id(),
    };
    const int ret = ioctl(gbl_ctl_fd_, GHOST_IOC_GET_CPU_TIME, &data);
    if (ret == 0) {
      *cpu_time = absl::Nanoseconds(data.runtime);
    }
    return ret;
  }

  // Associate a timerfd with agent on 'cpu':
  // - first 3 parameters are identical to timerfd_settime()
  // - cpu: produce CPU_TIMER_EXPIRED msg into 'dst_q' of agent on this cpu. If
  // an uninitialized (ie. invalid) cpu is passed, the timerfd will not produce
  // any msg.
  // - cookie: an opaque value that is reflected back in CPU_TIMER_EXPIRED msg.
  static int TimerFdSettime(
      const int fd, const int flags, itimerspec* const itimerspec,
      const Cpu& cpu = Cpu(Cpu::UninitializedType::kUninitialized),
      const uint64_t cookie = 0) {
    timerfd_ghost timerfd_ghost = {
        .cpu = cpu.valid() ? cpu.id() : -1,
        .flags = cpu.valid() ? TIMERFD_GHOST_ENABLED : 0,
        .cookie = cookie,
    };
    ghost_ioc_timerfd_settime data = {
        .timerfd = fd,
        .flags = flags,
        .in_tmr = itimerspec,
        .out_tmr = NULL,
        .timerfd_ghost = timerfd_ghost,
    };
    return ioctl(gbl_ctl_fd_, GHOST_IOC_TIMERFD_SETTIME, &data);
  }

  static bool GhostIsMountedAt(const char* path);
  static void MountGhostfs();
  // Returns the version of ghOSt running in the kernel.
  static int GetVersion(uint64_t& version);

  // Checks that the userspace ABI version matches the kernel ABI version.
  // This method performs a 'CHECK_EQ' so that the process dies if the versions
  // do not match. This is useful since this method runs on startup when
  // 'kVersionCheck' is initialized in the agent.
  static bool CheckVersion() {
    uint64_t kernel_abi_version;
    CHECK_EQ(GetVersion(kernel_abi_version), 0);
    // The version of ghOSt running in the kernel must match the version of
    // ghOSt that this userspace binary was built for.
    CHECK_EQ(kernel_abi_version, GHOST_VERSION);
    return kernel_abi_version == GHOST_VERSION;
  }

  static void SetGlobalEnclaveCtlFd(int fd) { gbl_ctl_fd_ = fd; }
  static int GetGlobalEnclaveCtlFd() { return gbl_ctl_fd_; }
  static void CloseGlobalEnclaveCtlFd() {
    if (gbl_ctl_fd_ >= 0) close(gbl_ctl_fd_);
    gbl_ctl_fd_ = -1;
  }

  static void SetGlobalStatusWordTable(StatusWordTable* swt) {
    gbl_sw_table_ = swt;
  }
  static StatusWordTable* GetGlobalStatusWordTable() {
    CHECK_NE(gbl_sw_table_, nullptr);
    return gbl_sw_table_;
  }

  // Gets the CPU affinity for the task with the provided Gtid and writes the
  // result to the provided CpuList.
  // Returns 0 on success. Otherwise, returns -1 with errno set.
  static int SchedGetAffinity(const Gtid& gtid, CpuList& cpulist) {
    cpu_set_t allowed_cpus;
    CPU_ZERO(&allowed_cpus);
    if (sched_getaffinity(gtid.tid(), sizeof(allowed_cpus), &allowed_cpus)) {
      return -1;
    }
    DCHECK_LE(CPU_COUNT(&allowed_cpus), MAX_CPUS);
    DCHECK_GT(CPU_COUNT(&allowed_cpus), 0);

    cpulist = cpulist.topology().ToCpuList(allowed_cpus);
    return 0;
  }

  // Sets the CPU affinity for the task with the provided `gtid` from the
  // set of cpus in `cpulist`.
  // Returns 0 on success. Otherwise, returns -1 with errno set.
  static int SchedSetAffinity(const Gtid& gtid, const CpuList& cpulist) {
    cpu_set_t cpuset = Topology::ToCpuSet(cpulist);
    return sched_setaffinity(gtid.tid(), sizeof(cpuset), &cpuset);
  }

  static constexpr const char kGhostfsMount[] = "/sys/fs/ghost";

 private:
  static int gbl_ctl_fd_;
  static StatusWordTable* gbl_sw_table_;
};

class GhostSignals {
 public:
  static void Init();
  static void IgnoreCommon();

  static void AddHandler(int signal, std::function<bool(int)> handler);

 private:
  static void SigHand(int signum);
  static void SigSegvAction(int signum, siginfo_t* info, void* uctx);
  static void SigIgnore(int signum){};

  static absl::flat_hash_map<int, std::vector<std::function<bool(int)>>>
      handlers_;
};

// StatusWord represents an accessor for our kernel shared sequence points.  A
// StatusWord is a movable type representing the mapping for a single thread.
// It releases the word in its destructor.
class StatusWord {
 public:
  typedef uint32_t BarrierToken;

  // Initializes to an empty status word.
  StatusWord() {}
  // Initializes to a known sw.  gtid is only used for debugging.
  StatusWord(Gtid gtid, struct ghost_sw_info sw_info);

  // Takes ownership of the word in "move_from", move_from becomes empty.
  StatusWord(StatusWord&& move_from);
  StatusWord& operator=(StatusWord&&);

  // REQUIRES: *this must be empty.
  ~StatusWord();

  // Signals to ghOSt that the status-word associated with *this is no longer
  // being used and may be potentially freed.  Resets *this to empty().
  // REQUIRES: *this must not be empty().
  void Free();

  bool empty() { return sw_ == nullptr; }

  // Returns a 'Null' barrier token, for call-sites where it is not required.
  static BarrierToken NullBarrierToken() { return 0; }

  // All methods below are invalid on an empty status word.
  BarrierToken barrier() const { return sw_barrier(); }

  // The time at which the task was context-switched onto CPU. We do a relaxed
  // load of `sw_->switch_time` since this should be done after the load to
  // `sw_->flags` to ensure that the oncpu bit is set.
  absl::Time switch_time() const {
    return absl::FromUnixNanos(READ_ONCE(sw_->switch_time));
  }
  uint64_t runtime() const { return sw_runtime(); }

  bool stale(BarrierToken prev) { return prev == barrier(); }

  bool in_use() const { return sw_flags() & GHOST_SW_F_INUSE; }
  bool can_free() const { return sw_flags() & GHOST_SW_F_CANFREE; }
  bool on_cpu() const { return sw_flags() & GHOST_SW_TASK_ONCPU; }
  bool cpu_avail() const { return sw_flags() & GHOST_SW_CPU_AVAIL; }
  bool runnable() const { return sw_flags() & GHOST_SW_TASK_RUNNABLE; }
  bool boosted_priority() const { return sw_flags() & GHOST_SW_BOOST_PRIO; }

  uint32_t id() { return sw_info_.id; }

  Gtid owner() const { return owner_; }

  StatusWord(const StatusWord&) = delete;
  StatusWord& operator=(const StatusWord&) = delete;

 private:
  struct AgentSW {};
  explicit StatusWord(AgentSW);

  Gtid owner_;  // Debug only, remove at some point.
  struct ghost_sw_info sw_info_;
  struct ghost_status_word* sw_ = nullptr;

  uint32_t sw_barrier() const {
    std::atomic<uint32_t>* barrier =
        reinterpret_cast<std::atomic<uint32_t>*>(&sw_->barrier);
    return barrier->load(std::memory_order_acquire);
  }

  uint64_t sw_runtime() const {
    std::atomic<uint64_t>* runtime =
        reinterpret_cast<std::atomic<uint64_t>*>(&sw_->runtime);
    return runtime->load(std::memory_order_relaxed);
  }

  uint32_t sw_flags() const {
    std::atomic<uint32_t>* flags =
        reinterpret_cast<std::atomic<uint32_t>*>(&sw_->flags);
    return flags->load(std::memory_order_acquire);
  }

  friend class Agent;  // For AgentSW constructor.
};

class PeriodicEdge {
 public:
  explicit PeriodicEdge(absl::Duration d)
      : duration_(d), last_(MonotonicNow()) {}
  bool Edge() {
    absl::Time now = MonotonicNow();
    if (now - last_ > duration_) {
      last_ = now;
      return true;
    }
    return false;
  }

 private:
  absl::Duration duration_;
  absl::Time last_;
};

// Simple (non-agent) thread class capable of advertising its GTID and running
// on either CFS (Linux Completely Fair Scheduler) or ghOSt.
//
// Example:
// GhostThread cfs_thread(GhostThread::KernelScheduler::kCfs, []() {
//   std::cout << "Hello!" << std::endl;
// }
// CHECK(cfs_thread.Joinable());
// cfs_thread.Join();
//
// The same code works for a thread that runs in the ghOSt scheduling class,
// though you should pass `kGhost` to the constructor instead of `kCfs`.
class GhostThread {
 public:
  // The kernel scheduling class to run the thread in.
  enum class KernelScheduler {
    // Linux Completely Fair Scheduler.
    kCfs,
    // ghOSt.
    kGhost,
  };

  explicit GhostThread(KernelScheduler ksched, std::function<void()> work);
  explicit GhostThread(const GhostThread&) = delete;
  GhostThread& operator=(const GhostThread&) = delete;
  ~GhostThread();

  friend std::ostream& operator<<(
      std::ostream& os, const GhostThread::KernelScheduler& kernel_scheduler) {
    switch (kernel_scheduler) {
      case GhostThread::KernelScheduler::kCfs:
        os << "CFS";
        break;
      case GhostThread::KernelScheduler::kGhost:
        os << "ghOSt";
        break;
      default:
        GHOST_ERROR("`kernel_scheduler` has non-enumerator value %d.",
                    static_cast<int>(kernel_scheduler));
        break;
    }
    return os;
  }

  // Joins the thread.
  void Join() {
    CHECK(Joinable());
    thread_.join();
  }

  // Returns true if the thread is joinable. Returns false otherwise (likely
  // because the thread has already been joined).
  bool Joinable() const { return thread_.joinable(); }

  // Returns this thread's TID (thread identifier).
  int tid() { return tid_; }

  // Returns this thread GTID (Google thread identifier).
  Gtid gtid() { return gtid_; }

  // Used by client processes who don't care which enclave they are in.
  static void SetGlobalEnclaveCtlFdOnce();

 private:
  // The thread's TID (thread identifier).
  int tid_;

  // The thread's GTID (Google thread identifier).
  Gtid gtid_;

  // The kernel scheduling class the thread is running in.
  KernelScheduler ksched_;

  // This notification is notified once the thread has started running.
  Notification started_;

  // The thread.
  std::thread thread_;
};

// Moves the thread with PID `pid` to the ghOSt scheduling class, using the
// enclave ctl_fd.  If ctl_fd is -1, this will use the enclave ctl_fd previously
// set with SetGlobalEnclaveCtlFd().
int SchedTaskEnterGhost(pid_t pid, int ctl_fd = -1);
// Makes the calling thread an agent.  Note that the calling thread must have
// the `CAP_SYS_NICE` capability to make itself an agent.
int SchedAgentEnterGhost(int ctl_fd, int queue_fd);

}  // namespace ghost

#endif  // GHOST_LIB_GHOST_H_
