// Copyright 2021 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ghost.h"

#include <limits.h>
#include <mntent.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/mount.h>

#include <csignal>
#include <filesystem>
#include <fstream>
#include <limits>
#include <regex>  // NOLINT: no RE2; ghost limits itself to absl
#include <thread>
#include <unordered_map>
#include <vector>

#include "absl/container/flat_hash_map.h"

// The open source Google benchmarks library includes its own verbose flag (`v`)
// and it does not use absl to parse this flag. Thus, to avoid a symbol
// conflict, we need to name our verbose flag differently outside of Google.
//
// All internal Google binaries have a `v` flag by default. To be consistent
// with our open source project, we instead use the `verbose` flag internally,
// too.
ABSL_FLAG(int32_t, verbose, 0, "Verbosity level");

namespace ghost {

StatusWordTable::StatusWordTable(int enclave_fd, int id, int numa_node) {
  int ctl = openat(enclave_fd, "ctl", O_RDWR);
  CHECK_GE(ctl, 0);
  std::string cmd = absl::StrCat("create sw_region ", id, " ", numa_node);
  ssize_t ret = write(ctl, cmd.c_str(), cmd.length());
  CHECK(ret == cmd.length() || errno == EEXIST);
  close(ctl);

  fd_ =
      openat(enclave_fd, absl::StrCat("sw_regions/sw_", id).c_str(), O_RDONLY);
  CHECK_GE(fd_, 0);
  map_size_ = GetFileSize(fd_);
  header_ = static_cast<struct ghost_sw_region_header*>(
      mmap(nullptr, map_size_, PROT_READ, MAP_SHARED, fd_, 0));
  CHECK_NE(header_, MAP_FAILED);
  CHECK_LT(0, header_->capacity);
  CHECK_EQ(header_->id, id);
  CHECK_EQ(header_->numa_node, numa_node);

  table_ = reinterpret_cast<ghost_status_word*>(
      reinterpret_cast<intptr_t>(header_) + header_->start);
  CHECK_NE(table_, nullptr);
}

StatusWordTable::~StatusWordTable() {
  munmap(header_, map_size_);
  close(fd_);
}

static ghost_status_word* status_word_from_info(struct ghost_sw_info* sw_info) {
  StatusWordTable* table = Ghost::GetGlobalStatusWordTable();
  CHECK_EQ(sw_info->id, table->id());
  return table->get(sw_info->index);
}

StatusWord& StatusWord::operator=(StatusWord&& move_from) {
  if (&move_from == this) return *this;

  sw_info_ = move_from.sw_info_;
  sw_ = move_from.sw_;
  owner_ = move_from.owner_;
  move_from.sw_ = nullptr;  // reset move_from to empty

  return *this;
}

StatusWord::StatusWord(StatusWord&& move_from) { *this = std::move(move_from); }

StatusWord::~StatusWord() {
  // TODO: Consider just making this automatic?
  if (!empty()) {
    GHOST_ERROR("%s leaked status word", owner_.describe());
  }
}

StatusWord::StatusWord(AgentSW) {
  CHECK_ZERO(Ghost::GetStatusWordInfo(GHOST_AGENT, GHOST_THIS_CPU, &sw_info_));
  sw_ = status_word_from_info(&sw_info_);
  owner_ = Gtid::Current();
}

StatusWord::StatusWord(Gtid gtid, struct ghost_sw_info sw_info) {
  sw_info_ = sw_info;
  sw_ = status_word_from_info(&sw_info_);
  owner_ = gtid;
}

void StatusWord::Free() {
  CHECK(!empty());
  CHECK(can_free());

  CHECK_EQ(Ghost::FreeStatusWordInfo(&sw_info_), 0);
  sw_ = nullptr;
  owner_ = Gtid(0);
}

// static
bool Ghost::GhostIsMountedAt(const char* path) {
  bool ret = false;
  FILE* mounts = setmntent("/proc/self/mounts", "r");
  CHECK_NE(mounts, nullptr);

  struct mntent* ent;
  while ((ent = getmntent(mounts))) {
    if (!strcmp(Ghost::kGhostfsMount, ent->mnt_dir) &&
        !strcmp("ghost", ent->mnt_type)) {
      ret = true;
      break;
    }
  }
  endmntent(mounts);
  return ret;
}

// static
void Ghost::MountGhostfs() {
  if (mount("ghost", Ghost::kGhostfsMount, "ghost", 0, nullptr)) {
    // EBUSY means it is already mounted. Anything else is failure. This CHECK
    // is generally triggered when you forget to compile ghOSt into the kernel,
    // such as by neglecting to set the Linux config option
    // `CONFIG_SCHED_CLASS_GHOST` to `y` in the Linux `.config` file.
    CHECK_EQ(errno, EBUSY);
  }
}

// Returns the version of ghOSt running in the kernel.
// static
int Ghost::GetVersion(uint64_t& version) {
  if (!GhostIsMountedAt(Ghost::kGhostfsMount)) {
    MountGhostfs();
  }
  std::ifstream ver(absl::StrCat(Ghost::kGhostfsMount, "/version"));
  if (!ver.is_open()) {
    return -1;
  }
  std::string line;
  if (std::getline(ver, line) && absl::SimpleAtoi(line, &version)) {
    return 0;
  } else {
    return -1;
  }
}

// static
int Ghost::gbl_ctl_fd_ = -1;
// static
StatusWordTable* Ghost::gbl_sw_table_;

// static
void Ghost::InitCore() {
  Gtid::Current().assign_name("main");
  GhostSignals::Init();

  // Some of the tests don't have agents, but they call InitCore()
  CheckVersion();

  // We make assumptions around MAX_CPUS being a power of 2 in Topology.
  static_assert((MAX_CPUS & (MAX_CPUS - 1)) == 0);
}

// static
void GhostSignals::Init() {
  std::signal(SIGABRT, SigHand);
  std::signal(SIGFPE, SigHand);
  std::signal(SIGILL, SigHand);
  std::signal(SIGINT, SigHand);
  std::signal(SIGTERM, SigHand);
  std::signal(SIGUSR1, SigHand);
  // Don't handle `SIGCHLD`; it's used by `ForkedProcess`.

  struct sigaction sigsegv_act = {{0}};
  sigsegv_act.sa_sigaction = SigSegvAction;
  sigsegv_act.sa_flags = SA_SIGINFO;

  sigaction(SIGSEGV, &sigsegv_act, NULL);
}

void GhostSignals::IgnoreCommon() {
  std::signal(SIGINT, SigIgnore);
  std::signal(SIGTERM, SigIgnore);
  std::signal(SIGUSR1, SigIgnore);
  // Don't handle `SIGCHLD`; it's used by `ForkedProcess`.
}

// static
absl::flat_hash_map<int, std::vector<std::function<bool(int)>>>
    GhostSignals::handlers_;
// static
void GhostSignals::SigHand(int signum) {
  bool fatal = true;

  for (auto& handler : handlers_[signum]) fatal = handler(signum) && fatal;

  if (fatal) {
    std::cerr << "Fatal signal " << strsignal(signum) << ": " << std::endl;
    Exit(1);
  }
}

void GhostSignals::SigSegvAction(int signum, siginfo_t* info, void* uctx) {
  std::cerr << "PID " << Gtid::Current().tid() << " Fatal segfault at addr "
            << info->si_addr << ": " << std::endl;
  PrintBacktrace(stderr, uctx);
  std::exit(1);
}

void GhostSignals::AddHandler(int signal, std::function<bool(int)> handler) {
  handlers_[signal].push_back(std::move(handler));
}

// For various glibc reasons, this isn't available in glibc/grte.  Including
// uapi/sched.h or sched/types.h will run into conflicts on sched_param.
struct sched_attr {
  uint32_t size;
  uint32_t sched_policy;
  uint64_t sched_flags;
  int32_t sched_nice;
  uint32_t sched_priority;  // overloaded for is/is not an agent
  uint64_t sched_runtime;   // overloaded for enclave ctl fd
  uint64_t sched_deadline;
  uint64_t sched_period;
};
#define SCHED_FLAG_RESET_ON_FORK 0x01

int SchedTaskEnterGhost(pid_t pid, int ctl_fd) {
  struct sched_attr attr = {
      .size = sizeof(sched_attr),
      .sched_policy = SCHED_GHOST,
      .sched_priority = GHOST_SCHED_TASK_PRIO,
  };
  if (ctl_fd == -1) {
    ctl_fd = Ghost::GetGlobalEnclaveCtlFd();
  }
  attr.sched_runtime = ctl_fd;
  const int ret = syscall(__NR_sched_setattr, pid, &attr, /*flags=*/0);
  // We used to "Trust but verify" that pid was in ghost.  However, it's
  // possible that the syscall succeeded, but the enclave was immediately
  // destroyed, and our task is back in CFS already.
  return ret;
}

int SchedAgentEnterGhost(int ctl_fd, int queue_fd) {
  struct sched_attr attr = {
      .size = sizeof(sched_attr),
      .sched_policy = SCHED_GHOST,
      // We don't want to leak ghOSt threads into the agent address space.
      .sched_flags = SCHED_FLAG_RESET_ON_FORK,
      .sched_priority = GHOST_SCHED_AGENT_PRIO,
  };
  attr.sched_runtime = ctl_fd;
  attr.sched_deadline = queue_fd;
  const int ret = syscall(__NR_sched_setattr, 0, &attr, /*flags=*/0);
  if (!ret) {
    CHECK_EQ(sched_getscheduler(0), SCHED_GHOST | SCHED_RESET_ON_FORK);
  }
  return ret;
}

// Returns the ctlfd for some enclave that is accepting tasks.
static int FindActiveEnclave() {
  std::error_code ec;
  auto f = std::filesystem::directory_iterator(Ghost::kGhostfsMount, ec);
  auto end = std::filesystem::directory_iterator();
  for (/* f */; !ec && f != end; f.increment(ec)) {
    if (std::regex_match(f->path().filename().string(),
                         std::regex("^enclave_.*"))) {
      std::ifstream status((f->path() / "status").string());
      std::string line;
      while (std::getline(status, line)) {
        if (line == "active yes") {
          int ctl = open((f->path() / "ctl").string().c_str(), O_RDONLY);
          if (ctl >= 0) return ctl;
        }
      }
    }
  }
  return -1;
}

GhostThread::GhostThread(KernelScheduler ksched, std::function<void()> work)
    : ksched_(ksched) {
  GhostThread::SetGlobalEnclaveCtlFdOnce();

  thread_ = std::thread([this, w = std::move(work)] {
    tid_ = GetTID();
    gtid_ = Gtid::Current();

    // TODO: Consider moving after SchedEnterGhost.
    started_.Notify();

    if (ksched_ == KernelScheduler::kGhost) {
      const int ret = SchedTaskEnterGhost(/*pid=*/0);
      CHECK_EQ(ret, 0);
    }
    std::move(w)();
  });
  started_.WaitForNotification();
}

GhostThread::~GhostThread() { CHECK(!thread_.joinable()); }

// Agents should have already set the global enclave fd before creating agent
// tasks, so this helper is used by clients to find an enclave to join.
//
// This is the only place where there are concurrent writers that set the
// process-wide GlobalEnclaveCtlFd, and there are numerous places with unlocked
// readers.  All readers (agents or client tasks) will go through this helper
// before reading.  Once someone finds an enclave, all tasks will use that FD
// until someone comes along and resets it.
//
// The only resetters are weird test cases, such as agent_test and
// transaction_test, where the same global value gets reused because we run our
// tests from within the same process.
// static
void GhostThread::SetGlobalEnclaveCtlFdOnce() {
  static absl::Mutex mtx(absl::kConstInit);
  absl::MutexLock lock(&mtx);
  if (Ghost::GetGlobalEnclaveCtlFd() == -1) {
    Ghost::SetGlobalEnclaveCtlFd(FindActiveEnclave());
  }
}

}  // namespace ghost
