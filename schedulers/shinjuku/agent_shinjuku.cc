// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <cstdint>
#include <string>
#include <vector>

#include "absl/debugging/symbolize.h"
#include "absl/flags/parse.h"
#include "lib/agent.h"
#include "lib/channel.h"
#include "lib/enclave.h"
#include "lib/topology.h"
#include "schedulers/shinjuku/shinjuku_scheduler.h"

ABSL_FLAG(int32_t, firstcpu, 1, "First cpu to start scheduling from.");
ABSL_FLAG(int32_t, globalcpu, -1,
          "Global cpu. If -1, then defaults to <firstcpu>)");
ABSL_FLAG(int32_t, ncpus, 5, "Schedule on <ncpus> starting from <firstcpu>");
ABSL_FLAG(std::string, enclave, "", "Connect to preexisting enclave directory");
ABSL_FLAG(absl::Duration, preemption_time_slice, absl::Microseconds(50),
          "Shinjuku preemption time slice");

namespace ghost {

void ParseShinjukuConfig(ShinjukuConfig* config) {
  int firstcpu = absl::GetFlag(FLAGS_firstcpu);
  int globalcpu = absl::GetFlag(FLAGS_globalcpu);
  int ncpus = absl::GetFlag(FLAGS_ncpus);
  int lastcpu = firstcpu + ncpus - 1;

  CHECK_GT(ncpus, 1);
  CHECK_GE(firstcpu, 0);
  CHECK_LT(lastcpu, MachineTopology()->num_cpus());

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

  ghost::ShinjukuConfig config;
  ghost::ParseShinjukuConfig(&config);

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
  auto uap =
      new ghost::AgentProcess<ghost::FullShinjukuAgent<ghost::LocalEnclave>,
                              ghost::ShinjukuConfig>(config);

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
    uap->Rpc(ghost::ShinjukuScheduler::kDebugRunqueue);
    return false;
  });

  exit.WaitForNotification();

  delete uap;

  printf("Done!\n");
  return 0;
}
