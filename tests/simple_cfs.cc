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

void SimpleLoadBalancing(int num_threads, const CpuList& enclave_cpus) {
  Notification first_stage_done;
  Notification second_stage_done;
  Notification third_stage_done;
  std::atomic<bool> fourth_stage_done{false};

  constexpr uint32_t kMaxCpusForSimpleLoadBalancing = 10;
  std::atomic<uint32_t> num_threads_at_barrier{0};
  std::array<std::atomic<uint32_t>, kMaxCpusForSimpleLoadBalancing>
      first_stage_counters{
          0,
      };
  std::array<std::atomic<uint32_t>, kMaxCpusForSimpleLoadBalancing>
      second_stage_counters{
          0,
      };
  std::array<std::atomic<uint32_t>, kMaxCpusForSimpleLoadBalancing>
      third_stage_counters{
          0,
      };
  std::array<std::atomic<uint32_t>, kMaxCpusForSimpleLoadBalancing>
      fourth_stage_counters{
          0,
      };
  std::array<std::atomic<uint32_t>, kMaxCpusForSimpleLoadBalancing>
      final_stage_counters{
          0,
      };

  uint32_t num_cpus =
      std::min(kMaxCpusForSimpleLoadBalancing, enclave_cpus.Size());
  std::vector<int> cpu_vector;
  for (uint32_t i = 0; i < num_cpus; i++) {
    Cpu cpu = enclave_cpus.GetNthCpu(i);
    cpu_vector.push_back(cpu.id());
  }

  CpuList cpus = MachineTopology()->ToCpuList(cpu_vector);

  std::vector<std::unique_ptr<GhostThread>> threads;
  threads.reserve(num_threads);
  for (int i = 0; i < num_threads; i++) {
    threads.emplace_back(std::make_unique<GhostThread>(
        GhostThread::KernelScheduler::kGhost,
        [&first_stage_counters, &second_stage_counters, &third_stage_counters,
         &fourth_stage_counters, &final_stage_counters, &first_stage_done,
         &second_stage_done, &third_stage_done, &fourth_stage_done, &cpu_vector,
         &cpus, &num_threads_at_barrier] {
          SpinFor(absl::Seconds(1));

          // First stage: threads join ghOSt on any CPU. There is no requirement
          // on which CPU each task should be running on.
          int cpu = sched_getcpu();
          CHECK_GE(cpu, 0);

          std::vector<int>::iterator it =
              std::find(cpu_vector.begin(), cpu_vector.end(), cpu);
          CHECK(it != cpu_vector.end());
          first_stage_counters[it - cpu_vector.begin()].fetch_add(
              1, std::memory_order_relaxed);
          num_threads_at_barrier.fetch_add(1, std::memory_order_relaxed);
          first_stage_done.WaitForNotification();

          // Second stage: threads are migrated to the first CPU via affinity
          // change messages.
          CHECK_EQ(
              GhostHelper()->SchedSetAffinity(
                  Gtid::Current(), MachineTopology()->ToCpuList(
                                       std::vector<int>{cpus.Front().id()})),
              0);

          // This task should be on the first CPU when SchedSetAffinity returns
          // control to userspace.
          cpu = sched_getcpu();
          CHECK_EQ(cpu, cpus.Front().id());
          it = std::find(cpu_vector.begin(), cpu_vector.end(), cpu);
          CHECK(it != cpu_vector.end());
          second_stage_counters[it - cpu_vector.begin()].fetch_add(
              1, std::memory_order_relaxed);
          num_threads_at_barrier.fetch_add(1, std::memory_order_relaxed);
          second_stage_done.WaitForNotification();

          // Third stage: As we haven't sent any task affinity change messages,
          // all the threads should stay on the first CPU and load balancing
          // should not move these tasks to another CPU.
          SpinFor(absl::Seconds(1));
          cpu = sched_getcpu();
          it = std::find(cpu_vector.begin(), cpu_vector.end(), cpu);
          CHECK(it != cpu_vector.end());
          third_stage_counters[it - cpu_vector.begin()].fetch_add(
              1, std::memory_order_relaxed);
          num_threads_at_barrier.fetch_add(1, std::memory_order_relaxed);
          third_stage_done.WaitForNotification();

          // Fourth stage: Now we relax task affinity to all the CPUs and wait
          // for some time for load balancing to happen.
          CHECK_EQ(GhostHelper()->SchedSetAffinity(Gtid::Current(), cpus), 0);
          cpu = sched_getcpu();
          it = std::find(cpu_vector.begin(), cpu_vector.end(), cpu);
          CHECK(it != cpu_vector.end());
          fourth_stage_counters[it - cpu_vector.begin()].fetch_add(
              1, std::memory_order_relaxed);
          num_threads_at_barrier.fetch_add(1, std::memory_order_relaxed);

          while (!fourth_stage_done.load(std::memory_order_relaxed)) {
          }

          cpu = sched_getcpu();
          it = std::find(cpu_vector.begin(), cpu_vector.end(), cpu);
          CHECK(it != cpu_vector.end());
          final_stage_counters[it - cpu_vector.begin()].fetch_add(
              1, std::memory_order_relaxed);
        }));
  }

  while (num_threads_at_barrier.load(std::memory_order_relaxed) < num_threads) {
    absl::SleepFor(absl::Milliseconds(50));
  }
  num_threads_at_barrier.store(0);
  first_stage_done.Notify();
  std::cout << "First stage done." << std::endl;

  while (num_threads_at_barrier.load(std::memory_order_relaxed) < num_threads) {
    absl::SleepFor(absl::Milliseconds(50));
  }
  num_threads_at_barrier.store(0);
  second_stage_done.Notify();
  std::cout << "Second stage done." << std::endl;

  while (num_threads_at_barrier.load(std::memory_order_relaxed) < num_threads) {
    absl::SleepFor(absl::Milliseconds(50));
  }
  num_threads_at_barrier.store(0);
  third_stage_done.Notify();
  std::cout << "Third stage done." << std::endl;

  while (num_threads_at_barrier.load(std::memory_order_relaxed) < num_threads) {
    absl::SleepFor(absl::Milliseconds(50));
  }

  // Forcefully advance `Schedule` loop because the other CPUs' agent thread may
  // not be invoked at all. CpuTick may be turned off due to NO_HZ_IDLE setting
  // on idle CPUs and there is no message to wake them up.
  // TODO: Explicitly disable CpuTicks to ensure we do not rely on
  // CpuTicks for idle load balance.
  // TODO: Upon implementing periodic load balancing, add a case to
  // disable idle load balancing and only test the effect of periodic load
  // balancing.
  std::vector<std::unique_ptr<GhostThread>> ephemeral_threads;
  ephemeral_threads.reserve(cpu_vector.size());
  for (uint32_t j = 0; j < cpu_vector.size(); j++) {
    ephemeral_threads.emplace_back(
        std::make_unique<GhostThread>(GhostThread::KernelScheduler::kGhost,
                                      [] { SpinFor(absl::Milliseconds(10)); }));
  }
  for (std::unique_ptr<GhostThread>& t : ephemeral_threads) {
    t->Join();
  }

  SpinFor(absl::Milliseconds(500));

  fourth_stage_done.store(true, std::memory_order_relaxed);

  for (std::unique_ptr<GhostThread>& t : threads) {
    t->Join();
  }
  std::cout << "Fourth/final stage done." << std::endl;

  std::cout << "cpu\tfirst\tsecond\tthird\tfourth\tfinal" << std::endl;
  for (uint32_t i = 0; i < cpu_vector.size(); i++) {
    std::cout << absl::Substitute(
                     "cpu[$0]\t$1\t$2\t$3\t$4\t$5", cpu_vector[i],
                     first_stage_counters[i].load(std::memory_order_relaxed),
                     second_stage_counters[i].load(std::memory_order_relaxed),
                     third_stage_counters[i].load(std::memory_order_relaxed),
                     fourth_stage_counters[i].load(std::memory_order_relaxed),
                     final_stage_counters[i].load(std::memory_order_relaxed))
              << std::endl;
  }
}

}  // namespace
}  // namespace ghost

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);

  std::unique_ptr<ghost::AgentProcess<ghost::FullCfsAgent<ghost::LocalEnclave>,
                                      ghost::CfsConfig>>
      ap;
  ghost::CpuList cpus = ghost::MachineTopology()->all_cpus();
  if (absl::GetFlag(FLAGS_create_enclave_and_agent)) {
    ghost::Topology* topology = ghost::MachineTopology();
    cpus = topology->ParseCpuStr(absl::GetFlag(FLAGS_ghost_cpus));
    ghost::CfsConfig config(topology, cpus);
    ap = std::make_unique<ghost::AgentProcess<
        ghost::FullCfsAgent<ghost::LocalEnclave>, ghost::CfsConfig>>(config);
  }

  std::cout << "Running SimpleNice ..." << std::endl;
  ghost::SimpleNice();
  std::cout << "SimpleNice Done." << std::endl;

  std::cout << "Running SimpleLoadBalancing ..." << std::endl;
  ghost::SimpleLoadBalancing(10, cpus);
  std::cout << "SimpleLoadBalancing Done." << std::endl;

  return 0;
}
