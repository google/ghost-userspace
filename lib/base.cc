// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "lib/base.h"

#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <utility>

#include "absl/base/internal/spinlock.h"
#include "absl/base/macros.h"
#include "absl/container/node_hash_map.h"
#include "absl/debugging/stacktrace.h"
#include "absl/debugging/symbolize.h"
#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "absl/time/time.h"
#include "kernel/ghost_uapi.h"
#include "lib/logging.h"

// procfs may have been mounted somewhere other than root (eg. for testing
// purposes).
ABSL_FLAG(std::string, ghost_procfs_prefix, "", "procfs prefix");

ABSL_FLAG(bool, emit_fork_warnings, true,
          "Print info about multiple threads in ForkedProcess");

namespace ghost {

std::string GetProc(const std::string& procfs_path) {
  static std::string procfs_prefix = absl::GetFlag(FLAGS_ghost_procfs_prefix);
  return absl::StrCat(procfs_prefix, "/proc/", procfs_path);
}

Notification::~Notification() {
  CHECK_NE(notified_.load(std::memory_order_relaxed), NotifiedState::kWaiter);
}

void Notification::Notify() {
  NotifiedState v;

  while (true) {
    v = notified_.load(std::memory_order_acquire);

    CHECK(!HasBeenNotified());
    if (notified_.compare_exchange_weak(v, NotifiedState::kNotified,
                                        std::memory_order_release)) {
      break;
    }
  }

  if (v == NotifiedState::kWaiter) {
    Futex::Wake(&notified_, std::numeric_limits<int>::max());
  }
}

void Notification::WaitForNotification() {
  while (true) {
    NotifiedState v = notified_.load(std::memory_order_acquire);
    if (v == NotifiedState::kNotified) {
      return;
    } else if (v == NotifiedState::kNoWaiter) {
      // We only need relaxed here since we're always going to sync via
      // futex or re-acquire load above.
      if (!notified_.compare_exchange_weak(v, NotifiedState::kWaiter,
                                           std::memory_order_relaxed)) {
        continue;
      }
    } else {
      break;  // We'll wait below.
    }
  }
  Futex::Wait(&notified_, NotifiedState::kWaiter);
}

// 64-bit gtids referring to normal tasks always have a positive value:
// (0 | XX bits of actual pid_t | YY bit non-zero seqnum)
// We calculate XX based on the maximum value that a pid may have and
// then derive (YY = 63 - XX).
int ghost_tid_seqnum_bits() {
  static const int num_bits = [] {
    int pid_max_max;
    std::ifstream ifs(GetProc("sys/kernel/pid_max_max"));
    if (!ifs) {
      // We must be running on a kernel that predates 'kernel.pid_max_max'
      // in which case we assume that PID_MAX_LIMIT is 4194304.
      pid_max_max = 4194304;
    } else {
      ifs >> pid_max_max;
    }

    // Paranoia since __builtin_clz() is undefined for the zero value.
    CHECK_GE(pid_max_max, 2);
    const int xx = 32 - __builtin_clz(pid_max_max - 1);
    const int yy = 63 - xx;
    return yy;
  }();

  return num_bits;
}

int64_t GetGtidFromFile(FILE *stream) {
  int64_t gtid;
  if (fscanf(stream, "%ld", &gtid) != 1) return -1;
  return gtid;
}

absl::StatusOr<int64_t> gtid(int64_t pid) {
  FILE* stream = fopen(GetProc(absl::StrCat(pid, "/ghost/gtid")).c_str(), "r");
  if (stream) {
    int64_t gtid = GetGtidFromFile(stream);
    fclose(stream);
    if (gtid < 0) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Unable to extract gtid for %lld", pid));
    }
    return gtid;
  } else {
    return absl::InvalidArgumentError(
        absl::StrFormat("Unable to open proc entry for %lld", pid));
  }
}

int64_t GetTgidFromFile(FILE *stream) {
  int64_t tgid = -1;
  char* line = NULL;
  size_t len = 0;
  while (getline(&line, &len, stream) != -1) {
    std::istringstream iss(line);
    std::string field;
    iss >> field;
    if (iss && field == "Tgid:") {
      iss >> tgid;
      break;
    }
  }

  free(line);
  return tgid;
}

pid_t Gtid::tgid() const {
  int64_t pid = tid(), tgid = -1, gtid;
  int statusfd = -1, gtidfd = -1;
  FILE *status_stream = NULL, *gtid_stream = NULL;

  int dirfd = open(GetProc(std::to_string(pid)).c_str(), O_RDONLY);
  if (dirfd < 0) {
    goto done;
  }

  statusfd = openat(dirfd, "status", O_RDONLY);
  if (statusfd < 0) {
    goto done;
  }

  gtidfd = openat(dirfd, "ghost/gtid", O_RDONLY);
  if (gtidfd < 0) {
    goto done;
  }

  status_stream = fdopen(statusfd, "r");
  if (!status_stream) {
    goto done;
  }
  statusfd = -1;  // 'status_stream' now owns the underlying fd.

  gtid_stream = fdopen(gtidfd, "r");
  if (!gtid_stream) {
    goto done;
  }
  gtidfd = -1;  // 'gtid_stream' now owns the underlying fd.

  tgid = GetTgidFromFile(status_stream);
  gtid = GetGtidFromFile(gtid_stream);

  if (gtid != id()) {  // Check for pid recycling.
    tgid = -1;
  }

done:
  if (gtid_stream) {
    CHECK_LT(gtidfd, 0);
    fclose(gtid_stream);
  }

  if (status_stream) {
    CHECK_LT(statusfd, 0);
    fclose(status_stream);
  }

  if (gtidfd >= 0) {
    close(gtidfd);
  }

  if (statusfd >= 0) {
    close(statusfd);
  }

  if (dirfd >= 0) {
    close(dirfd);
  }

  return tgid;
}

pid_t Gtid::tid() const { return gtid_raw_ >> ghost_tid_seqnum_bits(); }

absl::StatusOr<int64_t> GetGtid() { return gtid(GetTID()); }

ABSL_CONST_INIT static absl::base_internal::SpinLock gtid_name_map_lock(
    absl::kConstInit, absl::base_internal::SCHEDULE_KERNEL_ONLY);

static absl::node_hash_map<int64_t, std::string>& get_gtid_name_map() {
  static auto map = new absl::node_hash_map<int64_t, std::string>;
  return *map;
}

// Returns the name previously assigned via 'assign_name()' or an
// auto-generated unique name.
static const std::string& get_gtid_name(int64_t gtid) {
  absl::base_internal::SpinLockHolder lock(&gtid_name_map_lock);
  auto& name_map = get_gtid_name_map();

  if (auto it = name_map.find(gtid); it == name_map.end()) {
    int idx = name_map.size();
    name_map[gtid] = absl::StrFormat("%c%d/%d/%lld", 'A' + (idx % 26), idx / 26,
                                     Gtid(gtid).tid(), gtid);
  }

  return name_map[gtid];
}

void Gtid::assign_name(std::string name) const {
  CHECK_NE(id(), 0);  // Note: This is useful for catching uninitialized gtids.

  absl::base_internal::SpinLockHolder lock(&gtid_name_map_lock);
  auto& name_map = get_gtid_name_map();
  name_map[id()] = std::move(name);
}

absl::string_view Gtid::describe() const {
  int64_t gtid = id();

  // Describe special encodings.
  if (gtid <= 0) {
    if (gtid == GHOST_NULL_GTID) return "<empty>";

    if (gtid == GHOST_AGENT_GTID) return "<agent>";

    return "<unknown>";
  }

  return get_gtid_name(gtid);
}

absl::StatusOr<Gtid> Gtid::FromTid(int64_t tid) {
  absl::StatusOr<int64_t> gtid_or = gtid(tid);
  if (!gtid_or.ok()) {
    return gtid_or.status();
  }
  return Gtid(gtid_or.value());
}

static std::string DecodeAddr(void* addr) {
  char tmp[1024];
  const char* symbol = "(unknown)";
  if (absl::Symbolize(addr, tmp, sizeof(tmp))) {
    symbol = tmp;
  }
  return std::string(symbol);
}

void PrintBacktrace(FILE* f, void* uctx) {
  constexpr int kMaxDepth = 20;
  void* array[kMaxDepth];
  size_t size;

  size = absl::GetStackTraceWithContext(array, ABSL_ARRAYSIZE(array), 1, uctx,
                                        /*min_dropped_frames=*/nullptr);
  for (int i = 0; i < size; i++) {
    absl::FPrintF(f, "[%d] %p : %s\n", i, array[i], DecodeAddr(array[i]));
  }
}

bool CapHas(cap_value_t cap) {
  cap_t caps = cap_get_proc();
  if (!caps) {
    return false;
  }
  cap_flag_value_t set_or_clr;
  int err = cap_get_flag(caps, cap, CAP_EFFECTIVE, &set_or_clr);
  cap_free(caps);
  if (err) {
    return false;
  }
  return set_or_clr == CAP_SET;
}

void Exit(int code) {
  if (code != 0) {
    std::cerr << "PID " << Gtid::Current().tid() << " Backtrace:" << std::endl;
    PrintBacktrace(stderr);
  }
  std::exit(code);
}

size_t GetFileSize(int fd) {
  struct stat stat_buf;
  CHECK_EQ(fstat(fd, &stat_buf), 0);
  return stat_buf.st_size;
}

void SpinFor(absl::Duration remaining) {
  while (remaining > absl::ZeroDuration()) {
    // We use MonotonicNow instead of absl::Now(), since the latter can acquire
    // a lock and sleep.
    absl::Time start = MonotonicNow();
    absl::Duration delta;

    for (int i = 0; i < 100; ++i) {
      delta = MonotonicNow() - start;

      // If clock_gettime is slow, we don't want to mistake that for a
      // preemption.
      if (delta > absl::Microseconds(10)) {
        break;
      }
    }

    // Don't count preempted time; if we were off cpu, the large delta
    // represents time we were waiting, not running.
    if (delta < absl::Microseconds(100)) {
      remaining -= delta;
    }
  }
}

absl::Mutex ForkedProcess::mu_(absl::kConstInit);

// static
bool ForkedProcess::HandleAbnormalExit(pid_t child, int wait_status) {
  bool handled = false;
  absl::MutexLock lock(&ForkedProcess::mu_);
  absl::flat_hash_map<pid_t, ForkedProcess*>& children =
      ForkedProcess::GetAllChildren();

  auto fpl = children.find(child);
  if (fpl == children.end()) {
    return false;
  }
  ForkedProcess* fp = fpl->second;

  // We want to run all of the handlers.  Any of them can abort the exit.
  for (const auto& handler : fp->exit_handlers_) {
    handled |= handler(child, wait_status);
  }
  return handled;
}

// static
void ForkedProcess::HandleIfAbnormalExit(pid_t child, int wait_status) {
  int exit_status;

  if (WIFEXITED(wait_status)) {
    exit_status = WEXITSTATUS(wait_status);
  } else if (WIFSIGNALED(wait_status)) {
    exit_status = -1;
  } else {
    exit_status = 0;
  }
  if (exit_status && !HandleAbnormalExit(child, wait_status)) {
    exit(exit_status);
    CHECK(false);
  }
}

// static
void ForkedProcess::HandleSigchild(int signum) {
  int wstatus;
  pid_t child = waitpid(/*pid=*/-1, &wstatus, WNOHANG);

  if (child > 0) {
    HandleIfAbnormalExit(child, wstatus);
  }
}

void CheckForMultiThreaded(void) {
  std::error_code ec;
  auto f = std::filesystem::directory_iterator("/proc/self/task/", ec);
  auto end = std::filesystem::directory_iterator();
  std::string me = std::to_string(GetTID());
  for ( ; !ec && f != end; f.increment(ec)) {
    std::string tid = f->path().filename();
    if (tid == me) {
      continue;
    }
    std::ifstream ifs_comm((f->path() / "comm").string());
    std::string comm;
    std::getline(ifs_comm, comm);
    absl::FPrintF(stderr, "Fork danger!  Found extra task %s %s\n", tid, comm);
  }
}

ForkedProcess::ForkedProcess(int stderr_fd) {
  pid_t ppid = getpid();
  pid_t p;

  // Any extra threads that exist when making a ForkedProcess are potentially a
  // risk: they will not be forked, and your application may depend on them.
  if (absl::GetFlag(FLAGS_emit_fork_warnings)) {
    CheckForMultiThreaded();
  }

  p = fork();
  CHECK_GE(p, 0);

  if (p == 0) {
    CHECK_EQ(dup2(stderr_fd, 2), 2);

    // Drop our parent's children. No need to lock, since we're single threaded.
    ForkedProcess::GetAllChildren().clear();

    prctl(PR_SET_PDEATHSIG, SIGKILL);
    // In case parent already died and we were reaped.
    if (getppid() != ppid) {
      exit(1);
      CHECK(false);
    }
    // Disconnect us from our parent's process group so we do not get their
    // signals.
    setpgid(/*pid=*/0, /*pgid=*/0);
  } else {
    // Normally, parents can wait on their children directly.  However, the
    // parent might be in a busy loop or otherwise held up.  If the child dies
    // in those scenarios, the signal will allow the parent to handle it.
    signal(SIGCHLD, ForkedProcess::HandleSigchild);
    {
      absl::MutexLock lock(&mu_);
      ForkedProcess::GetAllChildren()[p] = this;
    }
  }
  child_ = p;
}

ForkedProcess::ForkedProcess(std::function<int()> lambda) : ForkedProcess() {
  if (IsChild()) {
    _exit(lambda());
  }
}

ForkedProcess::~ForkedProcess() {
  if (child_) {
    absl::MutexLock lock(&mu_);
    GetAllChildren().erase(child_);
  }
}

int ForkedProcess::WaitForChildExit() {
  int wstatus;

  if (waitpid(child_, &wstatus, /*options=*/0) != child_) {
    // Some error, but it likely is benign.  The child could have already
    // exited and been handled.
    return -1;
  }
  HandleIfAbnormalExit(child_, wstatus);
  if (WIFEXITED(wstatus)) {
    return WEXITSTATUS(wstatus);
  }
  // Child has exited, either abnormally or properly.  It's possible that they
  // were killed with a signal, but our user installed their own handler for it,
  // such that the parent didn't exit too.  In this case, we don't know why the
  // child has exited, but they are gone.
  return -1;
}

void ForkedProcess::AddExitHandler(std::function<bool(pid_t, int)> handler) {
  exit_handlers_.push_back(handler);
}

void ForkedProcess::KillChild(int signum) { kill(child_, signum); }

}  // namespace ghost
