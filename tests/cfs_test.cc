// Copyright 2022 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <stdio.h>
#include <sched.h>

#include <memory>
#include <vector>

#include "lib/base.h"
#include "lib/ghost.h"
#include "schedulers/cfs/cfs_scheduler.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace ghost {
namespace {

using ::testing::Eq;
using ::testing::Ge;

class CfsTest : public testing::Test {};

TEST_F(CfsTest, Simple) {
  if (MachineTopology()->num_cpus() < 5) {
    GTEST_SKIP() << "must have at least 5 cpus";
    return;
  }

  Topology* topology = MachineTopology();

  // The enclave includes CPUs 0, 1 and 2.
  CfsConfig config(topology, topology->ToCpuList(std::vector<int>{0, 1, 2}));

  // Create the CFS agent process.
  auto ap = AgentProcess<FullCfsAgent<LocalEnclave>, CfsConfig>(config);

  GhostThread t(GhostThread::KernelScheduler::kGhost, [] {
    EXPECT_THAT(sched_getcpu(), Eq(0));

    // The mask includes CPUs 1 and 3. CPU 3 is outside
    // the enclave, so should be ignored by the agent.
    EXPECT_THAT(GhostHelper()->SchedSetAffinity(
                    Gtid::Current(),
                    MachineTopology()->ToCpuList(std::vector<int>{1, 3})),
                Eq(0));

    int cpu;
    while ((cpu = sched_getcpu()) == 0) {
    }
    EXPECT_THAT(cpu, Eq(1));

    // The mask includes CPUs 1 and 4. CPU 4 is outside
    // the enclave, so should be ignored by the agent.
    EXPECT_THAT(GhostHelper()->SchedSetAffinity(
                    Gtid::Current(),
                    MachineTopology()->ToCpuList(std::vector<int>{2, 4})),
                Eq(0));

    while ((cpu = sched_getcpu()) == 1) {
    }
    EXPECT_THAT(cpu, Eq(2));
  });
  t.Join();

  int num_tasks;
  do {
    num_tasks = ap.Rpc(CfsScheduler::kCountAllTasks);
    EXPECT_THAT(num_tasks, Ge(0));
  } while (num_tasks > 0);

  GhostHelper()->CloseGlobalEnclaveFds();
}

TEST_F(CfsTest, RespectsNewTaskAffinity) {
  if (MachineTopology()->num_cpus() < 2) {
    GTEST_SKIP() << "must have at least 2 cpus";
    return;
  }

  Topology* topology = MachineTopology();

  // The enclave has two CPUs.
  CfsConfig config(topology, topology->ToCpuList(std::vector<int>{0, 1}));

  // Create the CFS agent process.
  auto ap = AgentProcess<FullCfsAgent<LocalEnclave>, CfsConfig>(config);

  // Create 10 threads wanting to be scheduled on a single CPU.
  constexpr uint32_t kNumThreads = 10;
  std::vector<std::unique_ptr<GhostThread>> threads;
  threads.reserve(kNumThreads);

  for (uint32_t i = 0; i < kNumThreads; i++) {
    threads.emplace_back(
        std::make_unique<GhostThread>(GhostThread::KernelScheduler::kCfs, [] {
          // Migrate all the threads to CPU 1 by setting affinity.
          EXPECT_THAT(GhostHelper()->SchedSetAffinity(
                          Gtid::Current(),
                          MachineTopology()->ToCpuList(std::vector<int>{1})),
                      Eq(0));

          EXPECT_THAT(sched_getcpu(), Eq(1));

          EXPECT_THAT(
              GhostHelper()->SchedTaskEnterGhost(/*pid=*/0, /*dir_fd=*/-1),
              Eq(0));
          // Now this thread entered ghOSt and should be on the same CPU 1, even
          // if CPU 0 is idle. Spin for some time to see whether the scheduler
          // migrated this thread against its affinity mask.
          EXPECT_THAT(sched_getcpu(), Eq(1));
          SpinFor(absl::Milliseconds(100));
          EXPECT_THAT(sched_getcpu(), Eq(1));
        }));
  }

  for (std::unique_ptr<GhostThread>& t : threads) {
    t->Join();
  }

  // Even though the threads have joined it does not mean they are dead.
  // pthread_join() can return before the dying task has made its way to
  // TASK_DEAD. In this case, CfsAgent::ValidatePreExitState() triggers
  // a CHECK failure because the runqueue is not empty. We spin here until
  // there are no more tasks remaining.
  int num_tasks;
  do {
    num_tasks = ap.Rpc(CfsScheduler::kCountAllTasks);
    EXPECT_THAT(num_tasks, Ge(0));
  } while (num_tasks > 0);

  GhostHelper()->CloseGlobalEnclaveFds();
}

}  // namespace
}  // namespace ghost

int main(int argc, char **argv) {
  testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
