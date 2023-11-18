// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

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
// with our open source project, we can use the `verbose` flag internally too
// as an alias.
ABSL_FLAG(int32_t, verbose, 0, "Verbosity level");

// This flag is manually parsed on startup (see CheckVersion()), not within
// absl::ParseCommandLine(). We have it here as an ABSL_FLAG just for
// documentation reasons.
ABSL_FLAG(bool, ghost_version, false, "Print UAPI ghOSt version and quit.");

namespace ghost {

namespace {

Ghost* ghost_helper_ptr = new Ghost;

}  // namespace

LocalStatusWordTable::LocalStatusWordTable(int enclave_fd, int id,
                                           int numa_node) {
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
  header_ = static_cast<ghost_sw_region_header*>(
      mmap(nullptr, map_size_, PROT_READ, MAP_SHARED, fd_, 0));
  CHECK_NE(header_, MAP_FAILED);
  CHECK_LT(0, header_->capacity);
  CHECK_EQ(header_->id, id);
  CHECK_EQ(header_->numa_node, numa_node);

  table_ = reinterpret_cast<ghost_status_word*>(
      reinterpret_cast<intptr_t>(header_) + header_->start);
  CHECK_NE(table_, nullptr);
}

LocalStatusWordTable::~LocalStatusWordTable() {
  CHECK_EQ(munmap(header_, map_size_), 0);
  CHECK_EQ(close(fd_), 0);
}

static ghost_status_word* status_word_from_info(ghost_sw_info* sw_info) {
  StatusWordTable* table = GhostHelper()->GetGlobalStatusWordTable();
  CHECK_EQ(sw_info->id, table->id());
  return table->get(sw_info->index);
}

StatusWord::StatusWord(Gtid gtid, ghost_sw_info sw_info) {
  sw_info_ = sw_info;
  sw_ = status_word_from_info(&sw_info_);
  owner_ = gtid;
}

StatusWord::~StatusWord() {
  // TODO: Consider just making this automatic?
  if (!empty()) {
    GHOST_ERROR("%s leaked status word", owner_.describe());
  }
}

LocalStatusWord::LocalStatusWord(StatusWord::AgentSW) {
  CHECK_EQ(
      GhostHelper()->GetStatusWordInfo(GHOST_AGENT, sched_getcpu(), sw_info_),
      0);
  sw_ = status_word_from_info(&sw_info_);
  owner_ = Gtid::Current();
}

LocalStatusWord& LocalStatusWord::operator=(LocalStatusWord&& move_from) {
  if (&move_from == this) return *this;

  sw_info_ = move_from.sw_info_;
  sw_ = move_from.sw_;
  owner_ = move_from.owner_;
  move_from.sw_ = nullptr;  // reset move_from to empty

  return *this;
}

LocalStatusWord::LocalStatusWord(LocalStatusWord&& move_from) {
  *this = std::move(move_from);
}

void LocalStatusWord::Free() {
  CHECK(!empty());
  CHECK(can_free());

  CHECK_EQ(GhostHelper()->FreeStatusWordInfo(sw_info_), 0);
  sw_ = nullptr;
  owner_ = Gtid(0);
}

// static
bool Ghost::GhostIsMountedAt(const char* path) {
  bool ret = false;
  FILE* mounts = setmntent(GetProc("self/mounts").c_str(), "r");
  CHECK_NE(mounts, nullptr);

  mntent* ent;
  while ((ent = getmntent(mounts))) {
    if (!strcmp(path, ent->mnt_dir) && !strcmp("ghost", ent->mnt_type)) {
      ret = true;
      break;
    }
  }
  endmntent(mounts);
  return ret;
}

// static
void Ghost::MountGhostfs() {
  if (mount("ghost", kGhostfsMount, "ghost", 0, nullptr)) {
    // EBUSY means it is already mounted. Anything else is failure. This CHECK
    // is generally triggered when you forget to compile ghOSt into the kernel,
    // such as by neglecting to set the Linux config option
    // `CONFIG_SCHED_CLASS_GHOST` to `y` in the Linux `.config` file.
    CHECK_EQ(errno, EBUSY);
  }
}

// static
// Returns the ghOSt abi versions supported by the kernel.
int Ghost::GetSupportedVersions(std::vector<uint32_t>& versions) {
  if (!GhostIsMountedAt(kGhostfsMount)) {
    MountGhostfs();
  }
  std::ifstream ver(absl::StrCat(kGhostfsMount, "/version"));
  if (!ver.is_open()) {
    return -1;
  }

  std::string line;
  while (std::getline(ver, line)) {
    uint32_t v;
    if (absl::SimpleAtoi(line, &v)) {
      versions.push_back(v);
    } else {
      return -1;
    }
  }
  return 0;
}

void Ghost::InitCore() {
  Gtid::Current().assign_name("main");
  GhostSignals::Init();

  // Some of the tests don't have agents, but they call InitCore()
  CheckVersion();

  // We make assumptions around MAX_CPUS being a power of 2 in Topology.
  static_assert((MAX_CPUS & (MAX_CPUS - 1)) == 0);
}

int Ghost::SchedTaskEnterGhost(int64_t pid, int dir_fd) {
  if (dir_fd == -1) {
    dir_fd = GhostHelper()->GetGlobalEnclaveDirFd();
  }
  // If the open/close of tasks is a performance problem, we can have the caller
  // open it for us.
  int tasks_fd = openat(dir_fd, "tasks", O_WRONLY);
  if (tasks_fd < 0) {
    return -1;
  }
  std::string pid_s = std::to_string(pid);
  int ret = 0;
  if (write(tasks_fd, pid_s.c_str(), pid_s.length()) != pid_s.length()) {
    ret = -1;
  }
  int old_errno = errno;
  close(tasks_fd);
  errno = old_errno;
  // We used to "Trust but verify" that pid was in ghost.  However, it's
  // possible that the syscall succeeded, but the enclave was immediately
  // destroyed, and our task is back in CFS already.
  return ret;
}

int Ghost::SchedTaskEnterGhost(const Gtid& gtid, int dir_fd) {
  return SchedTaskEnterGhost(gtid.id(), dir_fd);
}

int Ghost::SchedAgentEnterGhost(int ctl_fd, const Cpu& cpu, int queue_fd) {
  std::string cmd = absl::StrCat("become agent ", cpu.id(), " ", queue_fd);
  ssize_t ret = write(ctl_fd, cmd.c_str(), cmd.length());
  if (ret == cmd.length()) {
    CpuList agent_affinity = MachineTopology()->EmptyCpuList();
    // Verify sched_class.
    CHECK_EQ(sched_getscheduler(0), SCHED_GHOST | SCHED_RESET_ON_FORK);

    // Verify cpu affinity.
    CHECK_EQ(SchedGetAffinity(Gtid::Current(), agent_affinity), 0);
    CHECK_EQ(agent_affinity.Size(), 1);
    CHECK(agent_affinity.IsSet(cpu));

    // Verify that we are running on the agent cpu (probably overkill
    // given the affinity check above).
    CHECK_EQ(sched_getcpu(), cpu.id());

    return 0;
  }
  return ret;
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

// Returns the ctlfd and dirfd for some enclave that is accepting tasks.
struct ctl_dir {
  int ctl;
  int dir;
};
static ctl_dir FindActiveEnclave() {
  std::error_code ec;
  auto f =
      std::filesystem::directory_iterator(GhostHelper()->kGhostfsMount, ec);
  auto end = std::filesystem::directory_iterator();
  for (/* f */; !ec && f != end; f.increment(ec)) {
    if (std::regex_match(f->path().filename().string(),
                         std::regex("^enclave_.*"))) {
      std::ifstream status((f->path() / "status").string());
      std::string line;
      while (std::getline(status, line)) {
        if (line == "active yes") {
          int ctl = open((f->path() / "ctl").string().c_str(), O_RDONLY);
          if (ctl >= 0) {
            int dir = open(f->path().string().c_str(), O_PATH);
            CHECK_GE(dir, 0);
            return {ctl, dir};
          }
        }
      }
    }
  }
  return {-1, -1};
}

GhostThread::GhostThread(KernelScheduler ksched, std::function<void()> work,
                         int dir_fd)
    : ksched_(ksched) {
  GhostThread::SetGlobalEnclaveFdsOnce();

  // `dir_fd` must only be set when the scheduler is ghOSt.
  CHECK(ksched == KernelScheduler::kGhost || dir_fd == -1);

  thread_ = std::thread([this, w = std::move(work), dir_fd] {
    tid_ = GetTID();
    gtid_ = Gtid::Current();

    started_.Notify();

    if (ksched_ == KernelScheduler::kGhost) {
      const int ret = GhostHelper()->SchedTaskEnterGhost(/*pid=*/0, dir_fd);
      CHECK_EQ(ret, 0);
    }

    std::move(w)();
  });
  started_.WaitForNotification();
}

GhostThread::~GhostThread() { CHECK(!thread_.joinable()); }

void RemoteThreadTester::Run(std::function<void()> thread_work,
                             std::function<void(GhostThread*)> remote_work) {
  for (int i = 0; i < num_threads_; ++i) {
    threads_.push_back(std::make_unique<GhostThread>(
        GhostThread::KernelScheduler::kGhost,
        [this, thread_work] {
          num_threads_at_barrier_.fetch_add(1, std::memory_order_relaxed);
          start_.WaitForNotification();

          do {
            thread_work();
          } while (!exit_.HasBeenNotified());
        }));
  }

  while (num_threads_at_barrier_.load(std::memory_order_relaxed) <
         num_threads_) {
    absl::SleepFor(absl::Milliseconds(1));
  }
  start_.Notify();

  // Give the ghOSt threads time to wake up and the scheduler a moment to
  // start scheduling them.
  absl::SleepFor(absl::Milliseconds(10));

  for (auto& t : threads_) {
    remote_work(t.get());
  }

  exit_.Notify();

  for (auto& t : threads_) {
    t->Join();
  }
}
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
void GhostThread::SetGlobalEnclaveFdsOnce() {
  static absl::Mutex mtx(absl::kConstInit);
  absl::MutexLock lock(&mtx);
  if (GhostHelper()->GetGlobalEnclaveCtlFd() == -1) {
    CHECK_EQ(GhostHelper()->GetGlobalEnclaveDirFd(), -1);
    ctl_dir cd = FindActiveEnclave();
    if (cd.ctl >= 0) {
      GhostHelper()->SetGlobalEnclaveFds(cd.ctl, cd.dir);
    }
  }
}

Ghost* GhostHelper() {
  CHECK_NE(ghost_helper_ptr, nullptr);
  return ghost_helper_ptr;
}

void UpdateGhostHelper(Ghost* ghost_helper) {
  CHECK_NE(ghost_helper, nullptr);
  if (ghost_helper_ptr) {
    delete ghost_helper_ptr;
  }
  ghost_helper_ptr = ghost_helper;
}

}  // namespace ghost
