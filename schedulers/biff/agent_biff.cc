// Copyright 2022 Google LLC
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
#include "schedulers/biff/biff_scheduler.h"

ABSL_FLAG(std::string, enclave, "", "Connect to preexisting enclave directory");

int main(int argc, char* argv[]) {
  absl::InitializeSymbolizer(argv[0]);
  absl::ParseCommandLine(argc, argv);

  ghost::Topology* t = ghost::MachineTopology();
  ghost::AgentConfig config(t, t->all_cpus());
  std::string enclave = absl::GetFlag(FLAGS_enclave);
  if (!enclave.empty()) {
    int fd = open(enclave.c_str(), O_PATH);
    CHECK_GE(fd, 0);
    config.enclave_fd_ = fd;
  }

  auto uap = new ghost::AgentProcess<ghost::FullBiffAgent<ghost::LocalEnclave>,
                                     ghost::AgentConfig>(config);

  ghost::GhostHelper()->InitCore();

  printf("Initialization complete, ghOSt active.\n");
  fflush(stdout);

  ghost::Notification exit;
  static bool first = true;
  ghost::GhostSignals::AddHandler(SIGINT, [&exit](int) {
    if (first) {
      exit.Notify();
      first = false;
      return false;
    }
    return true;
  });

  exit.WaitForNotification();

  delete uap;

  printf("\nDone!\n");
  return 0;
}
