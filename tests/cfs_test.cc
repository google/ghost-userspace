// Copyright 2022 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <sched.h>
#include <stdio.h>
#include <sys/resource.h>

#include <atomic>
#include <memory>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "lib/base.h"
#include "lib/ghost.h"
#include "schedulers/cfs/cfs_scheduler.h"

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
  // TASK_DEAD.
  int num_tasks;
  do {
    num_tasks = ap.Rpc(CfsScheduler::kCountAllTasks);
    EXPECT_THAT(num_tasks, Ge(0));
  } while (num_tasks > 0);

  GhostHelper()->CloseGlobalEnclaveFds();
}

TEST_F(CfsTest, KeepsAffinityWhenBecomingRunnableFromBlocked) {
  if (MachineTopology()->num_cpus() < 2) {
    GTEST_SKIP() << "must have at least cpus";
    return;
  }

  Topology* topology = MachineTopology();

  // The enclave has two CPUs.
  CfsConfig config(topology, topology->ToCpuList(std::vector<int>{0, 1}));

  // Create the CFS agent process.
  auto ap = AgentProcess<FullCfsAgent<LocalEnclave>, CfsConfig>(config);

  std::atomic<uint32_t> num_threads_at_barrier{0};
  Notification block;

  // Create 10 threads.
  constexpr uint32_t kNumThreads = 10;
  std::vector<std::unique_ptr<GhostThread>> threads;
  threads.reserve(kNumThreads);

  for (uint32_t i = 0; i < kNumThreads; i++) {
    threads.emplace_back(std::make_unique<GhostThread>(
        GhostThread::KernelScheduler::kGhost,
        [&block, &num_threads_at_barrier] {
          // Migrate all the threads to CPU 1 by setting affinity.
          EXPECT_THAT(GhostHelper()->SchedSetAffinity(
                          Gtid::Current(),
                          MachineTopology()->ToCpuList(std::vector<int>{1})),
                      Eq(0));

          // This task is setting its affinity synchronously and it is
          // guaranteed that this task gets offcpu before returning to user via
          // sched_setaffinity() -> do_set_cpus_allowed() -> set_curr_task() ->
          // _set_curr_task_ghost() -> force_offcpu() kernel path. The task will
          // not get oncpu again until the agent consumes TASK_AFFINITY_CHANGED
          // message.
          EXPECT_THAT(sched_getcpu(), Eq(1));

          num_threads_at_barrier.fetch_add(1, std::memory_order_relaxed);
          block.WaitForNotification();

          SpinFor(absl::Milliseconds(500));

          // This task should still be on CPU 1.
          EXPECT_THAT(sched_getcpu(), Eq(1));
        }));
  }

  while (num_threads_at_barrier.load(std::memory_order_relaxed) < kNumThreads) {
    absl::SleepFor(absl::Milliseconds(10));
  }
  block.Notify();

  for (std::unique_ptr<GhostThread>& t : threads) {
    t->Join();
  }

  // Even though the threads have joined it does not mean they are dead.
  // pthread_join() can return before the dying task has made its way to
  // TASK_DEAD.
  int num_tasks;
  do {
    num_tasks = ap.Rpc(CfsScheduler::kCountAllTasks);
    EXPECT_THAT(num_tasks, Ge(0));
  } while (num_tasks > 0);

  GhostHelper()->CloseGlobalEnclaveFds();
}

TEST_F(CfsTest, TaskChangesAffinityWithIncomingMessages) {
  if (MachineTopology()->num_cpus() < 2) {
    GTEST_SKIP() << "Must have at lease 2 CPUs.";
    return;
  }

  Topology* topology = MachineTopology();

  CfsConfig config(topology, topology->ToCpuList(std::vector<int>{0, 1}));
  auto ap = AgentProcess<FullCfsAgent<LocalEnclave>, CfsConfig>(config);

  std::atomic<bool> finished{false};
  Notification exit;

  GhostThread t(
      GhostThread::KernelScheduler::kGhost, [&finished, &exit] {
        absl::Time end = MonotonicNow() + absl::Seconds(3);
        while (MonotonicNow() < end) {
          // This thread moves between the two CPUs continously.
          int next_cpu = (sched_getcpu() + 1) % 2;
          EXPECT_THAT(GhostHelper()->SchedSetAffinity(
              Gtid::Current(),
              MachineTopology()->ToCpuList(std::vector<int>{next_cpu})),
                      Eq(0));
        }
        finished.store(true, std::memory_order_relaxed);
        exit.WaitForNotification();
      });

  // Send priority changed messages to test whether thread migration can make
  // progress while getting a lot of other messages.
  int i = 0;
  while (!finished.load(std::memory_order_relaxed)) {
    EXPECT_EQ(setpriority(PRIO_PROCESS, t.tid(), i++ % 10), 0);
    absl::SleepFor(absl::Milliseconds(10));
  }

  // If the ghOSt thread `t` exits right after finished.store but before
  // setpriority() in the main thread, the setpriority call will fail with
  // ESRCH. To make sure `t` exits after the main thread stops sending priority
  // changed messages, we put an explicit barrier here with Notification.
  exit.Notify();
  t.Join();

  // Even though the threads have joined it does not mean they are dead.
  // pthread_join() can return before the dying task has made its way to
  // TASK_DEAD.
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
