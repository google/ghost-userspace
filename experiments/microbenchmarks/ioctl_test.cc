// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <sys/ioctl.h>

#include "benchmark/benchmark.h"
#include "lib/enclave.h"
#include "lib/ghost.h"
#include "lib/topology.h"

namespace ghost {

void BM_ghost_null_ioctl(benchmark::State& state) {
  GhostHelper()->InitCore();
  Topology* topology = MachineTopology();
  LocalEnclave enclave(AgentConfig(topology, CpuList(*topology)));
  int ctl = GhostHelper()->GetGlobalEnclaveCtlFd();

  for (auto _ : state) {
    CHECK_EQ(ioctl(ctl, GHOST_IOC_NULL), 0);
  }
}
BENCHMARK(BM_ghost_null_ioctl);

void BM_getpid(benchmark::State& state) {
  for (auto _ : state) {
    CHECK_GT(syscall(SYS_getpid), 0);
  }
}
BENCHMARK(BM_getpid);

}  // namespace ghost

int main(int argc, char** argv) {
  ::benchmark::RunSpecifiedBenchmarks();
}
