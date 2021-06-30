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

#include <cstdint>
#include <string>
#include <vector>

#include "absl/debugging/symbolize.h"
#include "absl/flags/parse.h"
#include "lib/agent.h"
#include "lib/channel.h"
#include "lib/enclave.h"
#include "lib/topology.h"
#include "schedulers/edf/edf_scheduler.h"

ABSL_FLAG(int32_t, firstcpu, 1, "First cpu to start scheduling from.");
ABSL_FLAG(int32_t, globalcpu, -1,
          "Global cpu. If -1, then defaults to <firstcpu>)");
ABSL_FLAG(int32_t, ncpus, 5, "Schedule on <ncpus> starting from <firstcpu>");
ABSL_FLAG(bool, ticks, false, "Generate cpu tick messages");
ABSL_FLAG(std::string, enclave, "", "Connect to preexisting enclave directory");

namespace ghost {

void ParseGlobalConfig(GlobalConfig* config) {
  int firstcpu = absl::GetFlag(FLAGS_firstcpu);
  int globalcpu = absl::GetFlag(FLAGS_globalcpu);
  int ncpus = absl::GetFlag(FLAGS_ncpus);
  int lastcpu = firstcpu + ncpus - 1;

  CHECK_GT(ncpus, 1);
  CHECK_GE(firstcpu, 0);
  CHECK_LT(lastcpu, ghost::MachineTopology()->num_cpus());

  if (globalcpu < 0) {
    CHECK_EQ(globalcpu, -1);
    absl::SetFlag(&FLAGS_globalcpu, firstcpu);
    globalcpu = firstcpu;
  }

  CHECK_GE(globalcpu, firstcpu);
  CHECK_LE(globalcpu, lastcpu);

  std::vector<int> all_cpus_v;
  for (int c = firstcpu; c <= lastcpu; c++) {
    all_cpus_v.push_back(c);
  }

  Topology* topology = MachineTopology();
  config->topology_ = topology;
  config->cpus_ = topology->ToCpuList(std::move(all_cpus_v));
  config->tick_config_ = absl::GetFlag(FLAGS_ticks) ? CpuTickConfig::kAllTicks
                                                    : CpuTickConfig::kNoTicks;

  config->global_cpu_ = topology->cpu(globalcpu);

  std::string enclave = absl::GetFlag(FLAGS_enclave);
  if (!enclave.empty()) {
    int fd = open(enclave.c_str(), O_PATH);
    CHECK_GE(fd, 0);
    config->enclave_fd_ = fd;
  }
}

}  // namespace ghost

int main(int argc, char* argv[]) {
  absl::InitializeSymbolizer(argv[0]);

  // Override default value of the verbose flag while in active development.
  ghost::set_verbose(1);
  absl::ParseCommandLine(argc, argv);

  ghost::GlobalConfig config;
  ghost::ParseGlobalConfig(&config);

  printf("Core map\n");

  int n = 0;
  for (const ghost::Cpu& c : config.topology_->all_cores()) {
    printf("( ");
    for (const ghost::Cpu& s : c.siblings()) printf("%2d ", s.id());
    printf(")%c", ++n % 8 == 0 ? '\n' : '\t');
  }
  printf("\n");

  printf("Initializing...\n");

  // Using new so we can destruct the object before printing Done
  auto uap = new ghost::AgentProcess<ghost::GlobalEdfAgent<ghost::LocalEnclave>,
                                     ghost::GlobalConfig>(config);

  ghost::Ghost::InitCore();

  printf("Initialization complete, ghOSt active.\n");
  // When `stdout` is directed to a terminal, it is newline-buffered. When
  // `stdout` is directed to a non-interactive device (e.g, a Python subprocess
  // pipe), it is fully buffered. Thus, in order for the Python script to read
  // the initialization message as soon as it is passed to `printf`, we need to
  // manually flush `stdout`.
  fflush(stdout);

  ghost::Notification exit;
  static bool first = true;
  ghost::GhostSignals::AddHandler(SIGINT, [&exit](int) {
    if (first) {
      exit.Notify();
      first = false;
      return false;  // We'll exit on subsequent signals.
    }
    return true;
  });
  ghost::GhostSignals::AddHandler(SIGTERM, [&exit](int) {
    if (first) {
      exit.Notify();
      first = false;
      return false;  // We'll exit on subsequent signals.
    }
    return true;
  });

  // TODO: this is racy - uap could be deleted already
  ghost::GhostSignals::AddHandler(SIGUSR1, [uap](int) {
    uap->Rpc(ghost::EdfScheduler::kDebugRunqueue);
    return false;
  });

  exit.WaitForNotification();

  delete uap;

  printf("\nDone!\n");
  return 0;
}
