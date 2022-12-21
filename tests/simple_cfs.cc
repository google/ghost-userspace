// Copyright 2022 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <sys/resource.h>

#include <atomic>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/strings/numbers.h"
#include "absl/strings/substitute.h"
#include "lib/base.h"
#include "lib/ghost.h"
#include "lib/logging.h"
#include "schedulers/cfs/cfs_scheduler.h"

ABSL_FLAG(bool, create_enclave_and_agent, false,
          "If true, spawns an enclave and a CFS agent for experiments. "
          "Otherwise, an enclave and a CFS agent should exist in the system.");
ABSL_FLAG(std::string, ghost_cpus, "1-5",
          "List of cpu IDs to create an enclave with. Effective only if "
          "--create_enclave_and_agent is set.");

namespace ghost {
namespace {

// Spawns three threads with minimum nice value (-20), maximum nice value
// (19) and default nice value (0).
void SimpleNice() {
  Notification start;
  Notification priority_changed;
  std::atomic<int32_t> num_threads_at_barrier{0};

  std::atomic<bool> first_stage{true};
  std::atomic<bool> second_stage{true};

  uint64_t max_first_counter = 0;
  uint64_t max_second_counter = 0;

  uint64_t min_first_counter = 0;
  uint64_t min_second_counter = 0;

  uint64_t default_first_counter = 0;
  uint64_t default_second_counter = 0;

  GhostThread t_max(
      GhostThread::KernelScheduler::kCfs,
      [&first_stage, &second_stage, &max_first_counter, &max_second_counter,
       &start, &priority_changed, &num_threads_at_barrier] {
        CHECK_EQ(setpriority(PRIO_PROCESS, /*pid=*/0, 0), 0);

        GhostHelper()->SchedTaskEnterGhost(/*pid=*/0, -1);
        num_threads_at_barrier.fetch_add(1, std::memory_order_relaxed);
        start.WaitForNotification();

        while (first_stage.load(std::memory_order_relaxed)) {
          max_first_counter++;
        }

        CHECK_EQ(setpriority(PRIO_PROCESS, /*pid=*/0, 19), 0);
        num_threads_at_barrier.fetch_add(1, std::memory_order_relaxed);
        priority_changed.WaitForNotification();

        while (second_stage.load(std::memory_order_relaxed)) {
          max_second_counter++;
        }
      });

  GhostThread t_min(
      GhostThread::KernelScheduler::kCfs,
      [&first_stage, &second_stage, &min_first_counter, &min_second_counter,
       &start, &priority_changed, &num_threads_at_barrier] {
        CHECK_EQ(setpriority(PRIO_PROCESS, /*pid=*/0, 0), 0);

        GhostHelper()->SchedTaskEnterGhost(/*pid=*/0, -1);
        num_threads_at_barrier.fetch_add(1, std::memory_order_relaxed);
        start.WaitForNotification();

        while (first_stage.load(std::memory_order_relaxed)) {
          min_first_counter++;
        }

        CHECK_EQ(setpriority(PRIO_PROCESS, /*pid=*/0, -20), 0);
        num_threads_at_barrier.fetch_add(1, std::memory_order_relaxed);
        priority_changed.WaitForNotification();

        while (second_stage.load(std::memory_order_relaxed)) {
          min_second_counter++;
        }
      });

  GhostThread t_default(
      GhostThread::KernelScheduler::kCfs,
      [&first_stage, &second_stage, &default_first_counter,
       &default_second_counter, &start, &priority_changed,
       &num_threads_at_barrier] {
        CHECK_EQ(setpriority(PRIO_PROCESS, /*pid=*/0, 0), 0);

        GhostHelper()->SchedTaskEnterGhost(/*pid=*/0, -1);
        num_threads_at_barrier.fetch_add(1, std::memory_order_relaxed);
        start.WaitForNotification();

        while (first_stage.load(std::memory_order_relaxed)) {
          default_first_counter++;
        }

        CHECK_EQ(setpriority(PRIO_PROCESS, /*pid=*/0, 0), 0);
        num_threads_at_barrier.fetch_add(1, std::memory_order_relaxed);
        priority_changed.WaitForNotification();

        while (second_stage.load(std::memory_order_relaxed)) {
          default_second_counter++;
        }
      });

  while (num_threads_at_barrier.load(std::memory_order_relaxed) < 3) {
    absl::SleepFor(absl::Milliseconds(1));
  }
  num_threads_at_barrier = 0;
  start.Notify();

  absl::SleepFor(absl::Seconds(10));
  first_stage = false;

  while (num_threads_at_barrier.load(std::memory_order_relaxed) < 3) {
    absl::SleepFor(absl::Milliseconds(1));
  }
  priority_changed.Notify();

  absl::SleepFor(absl::Seconds(10));
  second_stage = false;

  t_max.Join();
  t_min.Join();
  t_default.Join();

  std::cout << absl::Substitute(
      "nicest ghOSt thread:\n"
      "  first stage as nice=0: counter=$0\n"
      "    estimated relative weight to nice(0)=1/$1\n"
      "  second_stage as nice=19: counter=$2\n"
      "    estimated relative weight to nice(0)=1/$3\n"
      "    expected relative weight to nice(0)=1/$4\n",
      max_first_counter,
      static_cast<double>(default_first_counter) / max_first_counter,
      max_second_counter,
      static_cast<double>(default_second_counter) / max_second_counter,
      static_cast<double>(CfsScheduler::kNiceToWeight[20]) /
          CfsScheduler::kNiceToWeight[39]);
  std::cout << absl::Substitute(
      "least nice ghOSt thread:\n"
      "  first stage as nice=0: counter=$0\n"
      "    estimated relative weight to nice(0)=$1\n"
      "  second_stage as nice=-20: counter=$2\n"
      "    estimated relative weight to nice(0)=$3\n"
      "    expected relative weight to nice(0)=$4\n",
      min_first_counter,
      static_cast<double>(min_first_counter) / default_first_counter,
      min_second_counter,
      static_cast<double>(min_second_counter) / default_second_counter,
      static_cast<double>(CfsScheduler::kNiceToWeight[0]) /
          CfsScheduler::kNiceToWeight[20]);
  std::cout << absl::Substitute(
      "default nice ghOSt thread:\n"
      "  first stage as nice=0: counter=$0\n"
      "  second stage as nice=0: counter=$1\n",
      default_first_counter, default_second_counter);
}

}  // namespace
}  // namespace ghost

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);

  std::unique_ptr<ghost::AgentProcess<ghost::FullCfsAgent<ghost::LocalEnclave>,
                                      ghost::CfsConfig>>
      ap;
  if (absl::GetFlag(FLAGS_create_enclave_and_agent)) {
    ghost::Topology* topology = ghost::MachineTopology();
    ghost::CpuList cpus =
        topology->ParseCpuStr(absl::GetFlag(FLAGS_ghost_cpus));
    ghost::CfsConfig config(topology, cpus);
    ap = std::make_unique<ghost::AgentProcess<
        ghost::FullCfsAgent<ghost::LocalEnclave>, ghost::CfsConfig>>(config);
  }

  std::cout << "Running SimpleNice ..." << std::endl;
  ghost::SimpleNice();
  std::cout << "SimpleNice Done." << std::endl;

  return 0;
}
