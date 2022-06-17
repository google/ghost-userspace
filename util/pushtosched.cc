// Copyright 2022 Google LLC
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

// This is a helper program that moves threads into a specified sched class. The
// thread TIDs are passed to the process via stdin. See `PrintUsage()` for more
// details about using this program.

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "absl/strings/numbers.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "kernel/ghost_uapi.h"
#include "lib/base.h"
#include "lib/logging.h"

namespace {

// For various glibc reasons, this isn't available in glibc/grte. Including
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

const char* kEnclavePath = "/sys/fs/ghost/enclave_1/ctl";

void PrintUsage(absl::string_view program_name, int return_code) {
  absl::FPrintF(stderr, R"(Usage: %s <policy>
To push tasks into ghOSt:
    $ cat /dev/cgroup/cpu/mine/tasks | %s %d
To push tasks into CFS:
    $ cat /dev/cgroup/cpu/your/tasks | %s %d
)",
                program_name, program_name, SCHED_GHOST, program_name,
                SCHED_OTHER);
  exit(return_code);
}

// Adds `tid` to the ghOSt sched class.
int SchedEnterGhost(pid_t tid, int enclave_fd) {
  sched_attr attr = {
      .size = sizeof(sched_attr),
      .sched_policy = SCHED_GHOST,
      .sched_priority = 0,  // GHOST_SCHED_TASK_PRIO
      .sched_runtime = static_cast<uint64_t>(enclave_fd),
  };
  return syscall(__NR_sched_setattr, tid, &attr, /*flags=*/0);
}

// Adds `tid` to the sched class specified by `policy`.
int SchedEnterOther(pid_t tid, int policy) {
  sched_param param = {0};
  return sched_setscheduler(tid, policy, &param);
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 2) {
    PrintUsage(argv[0], 1);
  }

  int policy = -1;
  CHECK(absl::SimpleAtoi(argv[1], &policy));
  int enclave_fd = -1;
  if (policy == SCHED_GHOST) {
    enclave_fd = open(kEnclavePath, O_RDWR);
    if (enclave_fd < 0) {
      absl::FPrintF(stderr, "open(%s): %s\n", kEnclavePath, strerror(errno));
      exit(1);
    }
  }

  absl::FPrintF(stderr, "Setting scheduling policy to %d\n", policy);
  pid_t tid;
  while (fscanf(stdin, "%d\n", &tid) != EOF) {
    absl::FPrintF(stderr, "TID: %d\n", tid);
    if (sched_getscheduler(tid) == policy) {
      absl::FPrintF(stderr, "Already in sched class %d, so skipping\n", policy);
      continue;
    }

    int ret = (policy == SCHED_GHOST) ? SchedEnterGhost(tid, enclave_fd)
                                      : SchedEnterOther(tid, policy);
    // Check that the thread was successfully moved to the sched class.
    if (ret) {
      absl::FPrintF(stderr, "sched_setscheduler(%d) failed: %s\n", tid,
                    strerror(errno));
      exit(1);
    } else {
      int actual = sched_getscheduler(tid);
      if (actual != policy) {
        absl::FPrintF(stderr, "Scheduling policy of %d: want %d, got %d: %s\n",
                      tid, policy, actual, strerror(errno));
      }
    }
  }

  return 0;
}
