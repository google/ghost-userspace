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

#include <sys/ioctl.h>

#include "benchmark/benchmark.h"
#include "lib/enclave.h"
#include "lib/ghost.h"
#include "lib/topology.h"

namespace ghost {

void BM_ghost_null_ioctl(benchmark::State& state) {
  Ghost::InitCore();
  Topology* topology = MachineTopology();
  LocalEnclave enclave(AgentConfig(topology, CpuList(*topology)));
  int ctl = Ghost::GetGlobalEnclaveCtlFd();

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
