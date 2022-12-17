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

using ::testing::Ge;

class CfsTest : public testing::Test {};

TEST_F(CfsTest, Simple) {
  if (ghost::MachineTopology()->num_cpus() < 5) {
    GTEST_SKIP() << "must have at least 5 cpus";
    return;
  }

  Topology* topology = MachineTopology();

  // The enclave includes CPUs 0, 1 and 2.
  CfsConfig config(topology, topology->ToCpuList(std::vector<int>{0, 1, 2}));

  // Create the CFS agent process.
  auto ap = AgentProcess<FullCfsAgent<LocalEnclave>, CfsConfig>(config);

  std::vector<std::unique_ptr<Notification>> exit;
  for (int i = 0; i < 5; i++) {
    exit.push_back(std::make_unique<Notification>());
  }

  GhostThread t(GhostThread::KernelScheduler::kGhost, [&exit] {
    CHECK_EQ(sched_getcpu(), 0);

    while (!exit[0]->HasBeenNotified()) {}

    // The mask includes CPUs 1 and 3. CPU 3 is outside
    // the enclave, so should be ignored by the agent.
    CHECK_EQ(ghost::GhostHelper()->SchedSetAffinity(
                 ghost::Gtid::Current(),
                 ghost::MachineTopology()->ToCpuList(
                     std::vector<int>{1, 3})),
             0);

    // Thread migration not instant, wait
    while (!exit[1]->HasBeenNotified()) {}

    CHECK_EQ(sched_getcpu(), 1);

    while (!exit[2]->HasBeenNotified()) {}

    // The mask includes CPUs 1 and 4. CPU 4 is outside
    // the enclave, so should be ignored by the agent.
    CHECK_EQ(ghost::GhostHelper()->SchedSetAffinity(
                 ghost::Gtid::Current(),
                 ghost::MachineTopology()->ToCpuList(
                     std::vector<int>{2, 4})),
             0);

    while (!exit[3]->HasBeenNotified()) {}

    CHECK_EQ(sched_getcpu(), 2);

    // Just to observe in top. We can remove this.
    while (!exit[4]->HasBeenNotified()) {}
  });

  for (int i = 0; i < 5; i++) {
    absl::SleepFor(absl::Seconds(1));
    exit[i]->Notify();
  }

  t.Join();

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
