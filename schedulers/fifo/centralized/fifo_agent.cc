// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <sys/mman.h>

#include <cstdint>
#include <string>
#include <vector>

#include "absl/debugging/symbolize.h"
#include "absl/flags/parse.h"
#include "lib/agent.h"
#include "lib/channel.h"
#include "lib/enclave.h"
#include "lib/topology.h"
#include "schedulers/fifo/centralized/fifo_scheduler.h"

ABSL_FLAG(std::string, ghost_cpus, "1-5", "cpulist");
ABSL_FLAG(int32_t, globalcpu, -1,
          "Global cpu. If -1, then defaults to the first cpu in <cpus>");
ABSL_FLAG(absl::Duration, preemption_time_slice, absl::InfiniteDuration(),
          "A task is preempted after running for this time slice (default = "
          "infinite time slice)");
ABSL_FLAG(std::string, enclave, "", "Connect to preexisting enclave directory");

namespace ghost {

void ParseFifoConfig(FifoConfig* config) {
  CpuList ghost_cpus =
      MachineTopology()->ParseCpuStr(absl::GetFlag(FLAGS_ghost_cpus));
  // One CPU for the spinning global agent and at least one other for running
  // scheduled ghOSt tasks.
  CHECK_GE(ghost_cpus.Size(), 2);

  int globalcpu = absl::GetFlag(FLAGS_globalcpu);
  if (globalcpu < 0) {
    CHECK_EQ(globalcpu, -1);
    globalcpu = ghost_cpus.Front().id();
    absl::SetFlag(&FLAGS_globalcpu, globalcpu);
  }
  CHECK(ghost_cpus.IsSet(globalcpu));

  Topology* topology = MachineTopology();
  config->topology_ = topology;
  config->cpus_ = ghost_cpus;
  config->global_cpu_ = topology->cpu(globalcpu);
  config->preemption_time_slice_ = absl::GetFlag(FLAGS_preemption_time_slice);

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

  absl::ParseCommandLine(argc, argv);

  ghost::FifoConfig config;
  ghost::ParseFifoConfig(&config);

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
  auto uap = new ghost::AgentProcess<ghost::FullFifoAgent<ghost::LocalEnclave>,
                                     ghost::FifoConfig>(config);

  ghost::GhostHelper()->InitCore();

  printf("Initialization complete, ghOSt active.\n");

  // When `stdout` is directed to a terminal, it is newline-buffered. When
  // `stdout` is directed to a non-interactive device (e.g, a Python subprocess
  // pipe), it is fully buffered. Thus, in order for the Python script to read
  // the initialization message as soon as it is passed to `printf`, we need to
  // manually flush `stdout`.
  fflush(stdout);

  ghost::Notification exit;
  ghost::GhostSignals::AddHandler(SIGINT, [&exit](int) {
    static bool first = true;  // We only modify the first SIGINT.

    if (first) {
      exit.Notify();
      first = false;
      return false;  // We'll exit on subsequent SIGTERMs.
    }
    return true;
  });

  // TODO: this is racy - uap could be deleted already
  ghost::GhostSignals::AddHandler(SIGUSR1, [uap](int) {
    uap->Rpc(ghost::FifoScheduler::kDebugRunqueue);
    return false;
  });

  exit.WaitForNotification();

  delete uap;

  printf("Done!\n");
  return 0;
}
