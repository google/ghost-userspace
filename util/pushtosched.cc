// Copyright 2022 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

// This is a helper program that moves threads into the SCHED_OTHER (CFS) sched
// class. The thread TIDs are passed to the process via stdin. See
// `PrintUsage()` for more details about using this program.

#include <errno.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"

namespace {

void PrintUsage(absl::string_view program_name) {
  absl::FPrintF(stderr, R"(Usage:
To push tasks in a cgroup into CFS:
    $ cat /dev/cgroup/cpu/your/tasks | %s
To push ghOSt tasks into CFS:
    $ cat /sys/fs/ghost/enclave_X/tasks | %s
To push CFS tasks into ghOSt, please write pids directly to enclave's task
file. For example,
    $ cat /dev/cgroup/cpu/your/tasks > /sys/fs/ghost/enclave_X/tasks
)",
                program_name, program_name);
}

// Adds `pid` to the sched class specified by `policy`.
int SchedEnterOther(pid_t pid) {
  sched_param param = {0};
  return sched_setscheduler(pid, SCHED_OTHER, &param);
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 1) {
    PrintUsage(argv[0]);
    return 1;
  }

  absl::FPrintF(stderr, "Moving processes to SCHED_OTHER (CFS).\n");
  pid_t pid;
  while (fscanf(stdin, "%d\n", &pid) != EOF) {
    absl::FPrintF(stderr, "pid: %d\n", pid);
    if (sched_getscheduler(pid) == SCHED_OTHER) {
      absl::FPrintF(
          stderr, "Already in sched class SCHED_OTHER, skipping pid %d\n", pid);
      continue;
    }

    if (SchedEnterOther(pid)) {
      absl::FPrintF(stderr, "sched_setscheduler failed (pid: %d): %s\n", pid,
                    strerror(errno));
    }

    int actual = sched_getscheduler(pid);
    if (actual < 0) {
      absl::FPrintF(stderr, "sched_getscheduler for pid %d failed: %s\n", pid,
                    strerror(errno));
      return 1;
    } else if (actual != SCHED_OTHER) {
      absl::FPrintF(
          stderr,
          "Failed to set sched policy of pid %d: want SCHED_OTHER(%d), "
          "got %d\n",
          pid, SCHED_OTHER, actual);
    }
  }

  return 0;
}
