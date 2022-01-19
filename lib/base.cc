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

#include "lib/base.h"

#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <unordered_map>
#include <utility>

#include "absl/base/internal/spinlock.h"
#include "absl/base/macros.h"
#include "absl/container/node_hash_map.h"
#include "absl/debugging/stacktrace.h"
#include "absl/debugging/symbolize.h"
#include "absl/strings/str_format.h"
#include "kernel/ghost_uapi.h"
#include "lib/logging.h"

namespace ghost {

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

pid_t Gtid::tgid() const {
  int64_t tgid;
  // This should ideally be in ":ghost" but that results in a
  // dependency inversion because ":ghost" depends on ":base".
  int rc = syscall(__NR_ghost, GHOST_GTID_LOOKUP, id(), GHOST_GTID_LOOKUP_TGID,
                   /*flags=*/0, &tgid);
  if (rc == -1) return -1;
  return tgid;
}

pid_t Gtid::tid() const { return gtid_raw_ >> GHOST_TID_SEQNUM_BITS; }

int64_t GetGtid() {
  int64_t gtid;

  std::string gtid_path = absl::StrCat("/proc/", GetTID(), "/ghost/gtid");

  std::ifstream ifs(gtid_path);
  if (ifs) {
    ifs >> gtid;
  } else {  // Fallback to syscall.
    int ret = syscall(__NR_ghost, GHOST_BASE_GET_GTID, &gtid);
    if (ABSL_PREDICT_FALSE(ret < 0)) {
      gtid = (int64_t)GetTID() << GHOST_TID_SEQNUM_BITS;
    }
  }

  return gtid;
}

ABSL_CONST_INIT static absl::base_internal::SpinLock gtid_name_map_lock(
    absl::kConstInit, absl::base_internal::SCHEDULE_KERNEL_ONLY);

static absl::node_hash_map<int64_t, std::string>& get_gtid_name_map() {
  static auto map = new absl::node_hash_map<int64_t, std::string>;
  return *map;
}

// Returns the name previously assigned via 'assign_name()' or an
// auto-generated unique name.
static const std::string& get_gtid_name(int64_t key) {
  absl::base_internal::SpinLockHolder lock(&gtid_name_map_lock);
  auto& name_map = get_gtid_name_map();

  if (auto it = name_map.find(key); it == name_map.end()) {
    char b[20];
    int idx = name_map.size();
    absl::SNPrintF(b, sizeof(b), "%c%d/%d", 'A' + (idx % 26), idx / 26,
                   Gtid(key).tid());
    name_map[key] = std::string(b);
  }

  return name_map[key];
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

ForkedProcess::ForkedProcess() {
  pid_t ppid = getpid();
  pid_t p = fork();

  CHECK_GE(p, 0);

  if (p == 0) {
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
