// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

// A currently poorly organized collection of core helpers.
#ifndef GHOST_LIB_GHOST_H_
#define GHOST_LIB_GHOST_H_

#include <sys/ioctl.h>
#include <sys/timerfd.h>

#include <csignal>
#include <cstdint>
#include <fstream>
#include <unordered_map>

#include "absl/container/flat_hash_map.h"
#include "absl/flags/declare.h"
#include "absl/flags/flag.h"
#include "absl/strings/str_split.h"
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

typedef uint32_t BarrierToken;

class StatusWordTable {
 public:
  virtual ~StatusWordTable() {}

  int id() const { return header_->id; }
  ghost_status_word* get(unsigned int index) {
    CHECK_LT(index, header_->capacity);
    return &table_[index];
  }

  StatusWordTable(const StatusWordTable&) = delete;
  StatusWordTable(StatusWordTable&&) = delete;

  // Runs l on every non-agent, ghost-task status word.
  void ForEachTaskStatusWord(
      const std::function<void(ghost_status_word* sw, uint32_t region_id,
                               uint32_t idx)>
          l) {
    for (int i = 0; i < header_->capacity; ++i) {
      ghost_status_word* sw = get(i);
      if (!(sw->flags & GHOST_SW_F_INUSE)) {
        continue;
      }
      if (sw->flags & GHOST_SW_TASK_IS_AGENT) {
        continue;
      }
      l(sw, id(), i);
    }
  }

 protected:
  // Empty constructor for subclasses.
  StatusWordTable() {}

  int fd_ = -1;
  size_t map_size_ = 0;
  ghost_sw_region_header* header_ = nullptr;
  ghost_status_word* table_ = nullptr;
};

class LocalStatusWordTable : public StatusWordTable {
 public:
  // Create or attach to a preexisting status word region in the enclave.
  // `enclave_fd` is the fd for the enclave directory, e.g.
  // /sys/fs/ghost/enclave_1/.  `id` is a user-provided identifier for the
  // status word region.  `numa_node` is the NUMA node from which the kernel
  // should allocate the memory for the status word region.
  LocalStatusWordTable(int enclave_fd, int id, int numa_node);
  ~LocalStatusWordTable() final;
};

// TODO: Syscall definition needs fixing for any hope of 32-bit compat.
class Ghost {
 public:
  virtual ~Ghost() {}

  virtual void InitCore();

  virtual int Run(const Gtid& gtid, BarrierToken agent_barrier,
                  BarrierToken task_barrier, const Cpu& cpu, int flags) {
    ghost_ioc_run data = {
        .gtid = gtid.id(),
        .agent_barrier = agent_barrier,
        .task_barrier = task_barrier,
        .run_cpu = cpu.valid() ? cpu.id() : -1,
        .run_flags = flags,
    };
    return ioctl(gbl_ctl_fd_, GHOST_IOC_RUN, &data);
  }

  virtual int SyncCommit(cpu_set_t& cpuset) {
    ghost_ioc_commit_txn data = {
        .mask_ptr = &cpuset,
        .mask_len = sizeof(cpuset),
        .flags = 0,
    };
    return ioctl(gbl_ctl_fd_, GHOST_IOC_SYNC_GROUP_TXN, &data);
  }

  virtual int Commit(cpu_set_t& cpuset) {
    ghost_ioc_commit_txn data = {
        .mask_ptr = &cpuset,
        .mask_len = sizeof(cpuset),
        .flags = 0,
    };
    return ioctl(gbl_ctl_fd_, GHOST_IOC_COMMIT_TXN, &data);
  }

  virtual int Commit(const Cpu& cpu) {
    cpu_set_t cpuset;

    CHECK_GE(cpu.id(), 0);
    CHECK_LT(cpu.id(), CPU_SETSIZE);

    CPU_ZERO(&cpuset);
    CPU_SET(cpu.id(), &cpuset);
    return Commit(cpuset);
  }

  virtual int CreateQueue(int elems, int node, int flags, uint64_t& mapsize) {
    ghost_ioc_create_queue data = {
        .elems = elems,
        .node = node,
        .flags = flags,
    };
    int fd = ioctl(gbl_ctl_fd_, GHOST_IOC_CREATE_QUEUE, &data);
    mapsize = data.mapsize;
    return fd;
  }

  // Remove the associated queue notifier for the provided queue. Use this API
  // when the agent wants to poll on the queue instead of being woken up by the
  // kernel.
  //
  // Returns 0 on success and -1 on failure ('errno' is set on failure).
  virtual int RemoveQueueWakeup(int queue_fd) {
    return ConfigQueueWakeup(queue_fd, MachineTopology()->EmptyCpuList(),
                             /*flags=*/0);
  }

  // Configure the set of candidate cpus to wake up when a message is produced
  // into the queue denoted by 'queue_fd'.
  //
  // Returns 0 on success and -1 on failure ('errno' is set on failure).
  virtual int ConfigQueueWakeup(int queue_fd, const CpuList& cpulist,
                                int flags) {
    std::vector<ghost_agent_wakeup> wakeup;
    for (const Cpu& cpu : cpulist) {
      wakeup.push_back({
          .cpu = cpu.id(),
          .prio = 0,
      });
    }

    ghost_ioc_config_queue_wakeup data = {
        .qfd = queue_fd,
        .w = wakeup.data(),
        .ninfo = static_cast<int>(wakeup.size()),
        .flags = flags,
    };
    return ioctl(gbl_ctl_fd_, GHOST_IOC_CONFIG_QUEUE_WAKEUP, &data);
  }

  virtual int AssociateQueue(int queue_fd, ghost_type type, uint64_t arg,
                             BarrierToken barrier, int flags) {
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
    return ioctl(gbl_ctl_fd_, GHOST_IOC_ASSOC_QUEUE, &data);
  }

  virtual int SetDefaultQueue(int queue_fd) {
    ghost_ioc_set_default_queue data = {
        .fd = queue_fd,
    };

    return ioctl(gbl_ctl_fd_, GHOST_IOC_SET_DEFAULT_QUEUE, &data);
  }

  virtual int GetStatusWordInfo(ghost_type type, uint64_t arg,
                                ghost_sw_info& info) {
    ghost_ioc_sw_get_info data;
    data.request.type = type;
    data.request.arg = arg;
    int err = ioctl(gbl_ctl_fd_, GHOST_IOC_SW_GET_INFO, &data);
    if (err) {
      return err;
    }

    info = data.response;
    return 0;
  }

  virtual int FreeStatusWordInfo(ghost_sw_info& info) {
    return ioctl(gbl_ctl_fd_, GHOST_IOC_SW_FREE, &info);
  }

  // This is needed for when a sched item is updated in the shared PrioTable. A
  // write to a sched item indicates that the sched item was updated for a new
  // closure. We want to update the runtime of the task so that we don't bill
  // the new closure for CPU time used by the old closure.
  virtual int GetTaskRuntime(const Gtid& gtid, absl::Duration& cpu_time) {
    ghost_ioc_get_cpu_time data = {
        .gtid = gtid.id(),
    };
    int ret = ioctl(gbl_ctl_fd_, GHOST_IOC_GET_CPU_TIME, &data);
    if (ret == 0) {
      cpu_time = absl::Nanoseconds(data.runtime);
    }
    return ret;
  }

  // Creates a timerfd.
  virtual int TimerFdCreate(int clockid, int flags) {
    return timerfd_create(clockid, flags);
  }

  // Associate a timerfd with agent on 'cpu':
  // - first 3 parameters are identical to timerfd_settime()
  // - cpu: produce CPU_TIMER_EXPIRED msg into 'dst_q' of agent on this cpu. If
  // an uninitialized (ie. invalid) cpu is passed, the timerfd will not produce
  // any msg.
  // - type: an opaque value that is reflected back in CPU_TIMER_EXPIRED msg.
  // - cookie: an opaque value that is reflected back in CPU_TIMER_EXPIRED msg.
  virtual int TimerFdSettime(int fd, int flags, itimerspec& itimerspec,
                             const Cpu& cpu, uint64_t type, uint64_t cookie) {
    timerfd_ghost timerfd_ghost = {
        .cpu = cpu.valid() ? cpu.id() : -1,
        .flags = cpu.valid() ? TIMERFD_GHOST_ENABLED : 0,
        .type = type,
        .cookie = cookie,
    };
    ghost_ioc_timerfd_settime data = {
        .timerfd = fd,
        .flags = flags,
        .in_tmr = &itimerspec,
        .out_tmr = nullptr,
        .timerfd_ghost = timerfd_ghost,
    };
    return ioctl(gbl_ctl_fd_, GHOST_IOC_TIMERFD_SETTIME, &data);
  }

  static bool GhostIsMountedAt(const char* path);
  static void MountGhostfs();
  // Returns the ghOSt abi versions supported by the kernel.
  static int GetSupportedVersions(std::vector<uint32_t>& versions);

  // Check /proc/pid/cmdline for the --ghost_version flag and return true if it
  // is present. This function is called by CheckVersion(), so it runs on
  // startup when kVersionCheck is initialized.
  // We check for the flag manually instead of using absl::ParseCommandLine()
  // since we want to output the version and quit before possibly getting a
  // versions mismatch error in CheckVersion().
  static bool GetVersion() {
    std::ifstream cmdline_file;
    cmdline_file.open("/proc/self/cmdline");
    std::string cmdline;
    cmdline_file >> cmdline;
    cmdline_file.close();

    // The command-line stored in /proc/pid/cmdline is NULL char-delimited.
    std::vector<absl::string_view> argv = absl::StrSplit(cmdline, '\0');
    return std::find(argv.begin(), argv.end(), "--ghost_version") != argv.end();
  }

  // Checks that the userspace ABI version matches the kernel ABI version.
  // This method performs a 'CHECK_EQ' so that the process dies if the versions
  // do not match. This is useful since this method runs on startup when
  // 'kVersionCheck' is initialized in the agent.
  static bool CheckVersion() {
    if (GetVersion()) {
      std::cerr << GHOST_VERSION << std::endl;
      exit(0);
    }

    std::vector<uint32_t> versions;
    CHECK_EQ(GetSupportedVersions(versions), 0);

    // The version of ghOSt running in the kernel must match the version of
    // ghOSt that this userspace binary was built for.
    auto iter = std::find(versions.begin(), versions.end(), GHOST_VERSION);
    if (iter == versions.end()) {
      std::cerr << "Fatal error!" << std::endl;
      std::cerr << "Ghost version " << GHOST_VERSION << " not supported"
                << std::endl;
      std::cerr << "Kernel supports versions: ";
      for (uint32_t i : versions) {
        std::cerr << i << ' ';
      }
      std::cerr << std::endl;
      exit(1);
    }

    return iter != versions.end();
  }

  virtual void SetGlobalEnclaveFds(int ctl_fd, int dir_fd) {
    gbl_ctl_fd_ = ctl_fd;
    gbl_dir_fd_ = dir_fd;
  }
  virtual int GetGlobalEnclaveCtlFd() { return gbl_ctl_fd_; }
  virtual int GetGlobalEnclaveDirFd() { return gbl_dir_fd_; }
  virtual void CloseGlobalEnclaveFds() {
    if (gbl_ctl_fd_ >= 0) close(gbl_ctl_fd_);
    gbl_ctl_fd_ = -1;
    if (gbl_dir_fd_ >= 0) close(gbl_dir_fd_);
    gbl_dir_fd_ = -1;
  }

  virtual void SetGlobalStatusWordTable(StatusWordTable* swt) {
    gbl_sw_table_ = swt;
  }
  StatusWordTable* GetGlobalStatusWordTable() {
    CHECK_NE(gbl_sw_table_, nullptr);
    return gbl_sw_table_;
  }

  // Gets the CPU affinity for the task with the provided Gtid and writes the
  // result to the provided CpuList.
  // Returns 0 on success. Otherwise, returns -1 with errno set.
  virtual int SchedGetAffinity(const Gtid& gtid, CpuList& cpulist) {
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
  virtual int SchedSetAffinity(const Gtid& gtid, const CpuList& cpulist) {
    cpu_set_t cpuset = Topology::ToCpuSet(cpulist);
    return sched_setaffinity(gtid.tid(), sizeof(cpuset), &cpuset);
  }

  // Returns the sched class that the task `gtid` is in.
  virtual int SchedGetScheduler(const Gtid& gtid) {
    return sched_getscheduler(gtid.tid());
  }

  // Moves the specified thread to the ghOSt scheduling class, using the enclave
  // dir_fd.  `pid` may be a pid_t or a raw gtid. If dir_fd is -1, this will use
  // the enclave dir_fd previously set with SetGlobalEnclaveFds().
  virtual int SchedTaskEnterGhost(int64_t pid, int dir_fd);
  virtual int SchedTaskEnterGhost(const Gtid& gtid, int dir_fd);
  // Makes calling thread the ghost agent on `cpu`.  Note that the calling
  // thread must have the `CAP_SYS_NICE` capability to make itself an agent.
  virtual int SchedAgentEnterGhost(int ctl_fd, const Cpu& cpu, int queue_fd);

  static constexpr char kGhostfsMount[] = "/sys/fs/ghost";

 private:
  int gbl_ctl_fd_ = -1;
  int gbl_dir_fd_ = -1;
  StatusWordTable* gbl_sw_table_;
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
  struct AgentSW {};

  // REQUIRES: *this must be empty.
  virtual ~StatusWord();

  // Signals to ghOSt that the status-word associated with *this is no longer
  // being used and may be potentially freed.  Resets *this to empty().
  // REQUIRES: *this must not be empty().
  virtual void Free() = 0;

  bool empty() const { return sw_ == nullptr; }

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

  bool stale(BarrierToken prev) const { return prev == barrier(); }

  bool in_use() const { return sw_flags() & GHOST_SW_F_INUSE; }
  virtual bool can_free() const { return sw_flags() & GHOST_SW_F_CANFREE; }
  bool on_cpu() const { return sw_flags() & GHOST_SW_TASK_ONCPU; }
  bool cpu_avail() const { return sw_flags() & GHOST_SW_CPU_AVAIL; }
  bool runnable() const { return sw_flags() & GHOST_SW_TASK_RUNNABLE; }
  virtual bool boosted_priority() const {
    return sw_flags() & GHOST_SW_BOOST_PRIO;
  }

  uint32_t id() const { return sw_info_.id; }

  Gtid owner() const { return owner_; }

  ghost_sw_info sw_info() const { return sw_info_; }
  const ghost_status_word* sw() const { return sw_; }

 protected:
  // Initializes to an empty status word.
  StatusWord() {}
  StatusWord(Gtid gtid, ghost_sw_info sw_info);

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

  Gtid owner_;  // Debug only, remove at some point.
  ghost_sw_info sw_info_;
  ghost_status_word* sw_ = nullptr;
};

class LocalStatusWord : public StatusWord {
 public:
  // Initializes to an empty status word.
  LocalStatusWord() {}
  // Initializes to a known sw.  gtid is only used for debugging.
  LocalStatusWord(Gtid gtid, ghost_sw_info sw_info)
      : StatusWord(gtid, sw_info) {}
  // Initializes an agent status word.
  explicit LocalStatusWord(StatusWord::AgentSW);

  // Takes ownership of the word in "move_from", move_from becomes empty.
  LocalStatusWord(LocalStatusWord&& move_from);
  LocalStatusWord& operator=(LocalStatusWord&&);

  LocalStatusWord(const LocalStatusWord&) = delete;
  LocalStatusWord& operator=(const LocalStatusWord&) = delete;

  void Free() override;
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

  explicit GhostThread(KernelScheduler ksched, std::function<void()> work,
                       int dir_fd = -1);
  explicit GhostThread(const GhostThread&) = delete;
  GhostThread& operator=(const GhostThread&) = delete;
  ~GhostThread();

  friend std::ostream& operator<<(std::ostream& os,
                                  GhostThread::KernelScheduler scheduler) {
    switch (scheduler) {
      case GhostThread::KernelScheduler::kCfs:
        return os << "CFS";
      case GhostThread::KernelScheduler::kGhost:
        return os << "ghOSt";
    }
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
  static void SetGlobalEnclaveFdsOnce();

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

// Test helper class that launches threads and performs operations on them while
// they do their work.
//
// You don't have to do anything in remote_work.  In that case, this class just
// helps you spawn and join on num_threads.
class RemoteThreadTester {
 public:
  RemoteThreadTester(int num_threads = 1000) : num_threads_(num_threads) {}
  ~RemoteThreadTester() {};
  RemoteThreadTester& operator=(const RemoteThreadTester&) = delete;

  // Spawns threads, each of which does `thread_work()` in a loop at least once.
  //
  // Once the threads are up, all threads are notified to start their work loop.
  // Then we run `remote_work(GhostThread*)` on each thread.
  // After all remote_work is done, the threads are notified to exit.
  // Once they are joined, Run() returns.
  void Run(std::function<void()> thread_work,
           std::function<void(GhostThread*)> remote_work =
                                             [](GhostThread* t) {});

 private:
  int num_threads_;
  std::atomic<int> num_threads_at_barrier_;
  Notification start_;
  Notification exit_;
  std::vector<std::unique_ptr<GhostThread>> threads_;
};

// Returns the Ghost helper instance for this machine. The pointer is never null
// and is owned by the  `GhostHelper` function. The pointer lives until the
// process dies.
Ghost* GhostHelper();

// Frees the current `GhostHelper()` pointer and updates it to `ghost_helper`.
// This is useful for providing custom implementations of Ghost helper
// functions.
//
// This function is not thread-safe. You should generally only call it once when
// the process starts.
void UpdateGhostHelper(Ghost* ghost_helper);

}  // namespace ghost

#endif  // GHOST_LIB_GHOST_H_
