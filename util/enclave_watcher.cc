/*
 * Copyright 2022 Google LLC
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

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <memory>
#include <string>
#include <vector>

#include "absl/flags/parse.h"
#include "lib/enclave.h"
#include "lib/ghost.h"

ABSL_FLAG(std::string, enclave, "", "path to enclave directory");
ABSL_FLAG(int32_t, agent_pid, -1,
          "Optional PID of agent to kill (default is none)");
ABSL_FLAG(bool, sigkill, false, "send agent_pid a SIGKILL instead of SIGINT");

int main(int argc, char *argv[]) {
  absl::ParseCommandLine(argc, argv);

  std::string enclave = absl::GetFlag(FLAGS_enclave);
  pid_t agent = absl::GetFlag(FLAGS_agent_pid);
  bool sigkill = absl::GetFlag(FLAGS_sigkill);

  if (enclave.empty()) {
    fprintf(stderr,
            "need an enclave path, e.g. --enclave /sys/fs/ghost/enclave_1/\n");
    return 1;
  }
  int dfd = open(enclave.c_str(), O_PATH);
  CHECK_GE(dfd, 0);

  ghost::LocalEnclave::WaitForAgentOnlineValue(dfd, 1);

  absl::Time killed = absl::Now();
  if (agent != -1) {
    // Most agents gracefully shutdown on SIGINT
    kill(agent, sigkill ? SIGKILL : SIGINT);
  }

  // This captures when an enclave goes offline (agent crash/exit)
  ghost::LocalEnclave::WaitForAgentOnlineValue(dfd, 0);
  absl::Time offline = absl::Now();

  // This captures when a new agent takes over the enclave
  ghost::LocalEnclave::WaitForAgentOnlineValue(dfd, 1);
  absl::Time online = absl::Now();

  int nr_tasks = ghost::LocalEnclave::GetNrTasks(dfd);
  int64_t blackout = absl::ToInt64Nanoseconds(online - offline);

  std::cout << "Watcher measured blackout of : " << blackout / 1000000
            << " msec, ~" << blackout / (nr_tasks ?: 1) / 1000
            << " usec per task (" << nr_tasks << " tasks)\n";

  if (agent != -1) {
    std::cout << "Watcher kill-to-agent_offline : "
              << absl::ToInt64Milliseconds(offline - killed) << " msec\n";
  }

  close(dfd);
  return 0;
}
