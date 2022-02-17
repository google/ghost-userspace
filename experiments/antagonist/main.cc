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

// The Antagonist program. The antagonist tries to consume as many CPU cycles as
// possible by spinning.

#include <csignal>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/time/clock.h"
#include "experiments/antagonist/cfs_orchestrator.h"
#include "experiments/antagonist/ghost_orchestrator.h"
#include "experiments/antagonist/orchestrator.h"
#include "experiments/shared/thread_wait.h"

ABSL_FLAG(std::string, print_format, "pretty",
          "Results print format (\"pretty\" or \"csv\", default: \"pretty\")");
ABSL_FLAG(double, work_share, 1.0,
          "Each thread tries to target this share of the cycles on a CPU. For "
          "example, if 'work_share' is 0.5, each thread tries to target 50%% "
          "of cycles on a CPU. Note that 'work_share' must be greater than or "
          "equal to 0.0 and less than or equal to 1.0. (default: 1.0)");
ABSL_FLAG(size_t, num_threads, 8,
          "The number of worker threads to use (default: 8 threads).");
// It is preferred that the 'cpus' flag be an 'std::vector<int>', but the only
// vector type that Abseil supports is 'std::vector<std::string>>'.
ABSL_FLAG(std::vector<std::string>, cpus,
          std::vector<std::string>({"10", "11", "12", "13", "14", "15", "16",
                                    "17"}),
          "The CPUs to affine threads to. Only threads scheduled by CFS are "
          "affined. (default: CPUs 10-17).");
ABSL_FLAG(absl::Duration, experiment_duration, absl::InfiniteDuration(),
          "The experiment duration (default: infinity).");
ABSL_FLAG(std::string, scheduler, "cfs",
          "The scheduler to use (\"cfs\" for Linux Completely Fair Scheduler "
          "or \"ghost\" for ghOSt, default: \"cfs\")");
ABSL_FLAG(
    uint32_t, ghost_qos, 2,
    "For the ghOSt experiments, this is the QoS (Quality-of-Service) class for "
    "the PrioTable work class that all worker sched items are added to.");

namespace {
// Parses all command line flags.
ghost_test::Orchestrator::Options GetOptions() {
  ghost_test::Orchestrator::Options options;

  std::string print_format = absl::GetFlag(FLAGS_print_format);
  CHECK(print_format == "pretty" || print_format == "csv");
  options.print_options.pretty = (print_format == "pretty");

  options.print_options.os = &std::cout;
  options.work_share = absl::GetFlag(FLAGS_work_share);
  CHECK_GE(options.work_share, 0.0);
  CHECK_LE(options.work_share, 1.0);
  options.num_threads = absl::GetFlag(FLAGS_num_threads);

  for (const std::string& cpu : absl::GetFlag(FLAGS_cpus)) {
    options.cpus.push_back(std::stoi(cpu));
  }

  options.experiment_duration = absl::GetFlag(FLAGS_experiment_duration);
  CHECK_GE(options.experiment_duration, absl::ZeroDuration());

  std::string scheduler = absl::GetFlag(FLAGS_scheduler);
  CHECK(scheduler == "cfs" || scheduler == "ghost");
  options.scheduler = (scheduler == "cfs")
                          ? ghost::GhostThread::KernelScheduler::kCfs
                          : ghost::GhostThread::KernelScheduler::kGhost;

  options.ghost_qos = absl::GetFlag(FLAGS_ghost_qos);

  return options;
}

// Registers signal handlers for SIGINT and SIGALRM (for the timer). When
// receiving either of the signals for the first time, the application stops
// the experiment, prints the results, and exits. After receiving two SIGINTs,
// the program will force exit.
ghost::Notification* RegisterSignalHandlers() {
  // 'exit' must be static so that it can be implicitly captured by the signal
  // handler lambdas. We allocate its memory on the heap as the
  // 'ghost::Notification' type is not trivially destructible.
  static ghost::Notification* exit = new ghost::Notification;
  std::signal(SIGINT, [](int signum) {
    static bool force_exit = false;

    CHECK_EQ(signum, SIGINT);
    if (force_exit) {
      std::cout << "Forcing exit..." << std::endl;
      ghost::Exit(1);
    } else {
      // 'exit' may have already been notified by the timer signal.
      if (!exit->HasBeenNotified()) {
        exit->Notify();
      }
      force_exit = true;
    }
  });
  std::signal(SIGALRM, [](int signum) {
    CHECK_EQ(signum, SIGALRM);
    std::cout << "Timer fired..." << std::endl;
    // 'exit' may have already been notified by SIGINT.
    if (!exit->HasBeenNotified()) {
      exit->Notify();
    }
  });
  return exit;
}

// Sets a timer signal to fire after 'duration'.
void SetTimer(absl::Duration duration) {
  CHECK_GE(duration, absl::ZeroDuration());

  int64_t remainder = absl::ToInt64Microseconds(duration) %
                      absl::ToInt64Microseconds(absl::Seconds(1));
  absl::Duration seconds = duration - absl::Microseconds(remainder);
  itimerval itimer = {.it_interval = {.tv_sec = 0, .tv_usec = 0},
                      .it_value = {.tv_sec = absl::ToInt64Seconds(seconds),
                                   .tv_usec = remainder}};
  CHECK_EQ(setitimer(ITIMER_REAL, &itimer, nullptr), 0);
}

}  // namespace

// Initializes the application, runs the experiment, and exits.
int main(int argc, char* argv[]) {
  ghost::Notification* exit = RegisterSignalHandlers();

  // Affine the main thread to CPU
  // 'ghost_test::Orchestrator::kBackgroundThreadCpu'. Also, since we affine the
  // main thread here before any background threads are spawned (e.g., the
  // Antagonist background threads), the background threads will inherit the
  // main thread's affinity mask when they are spawned. Thus, the background
  // threads will automatically be affined to
  // 'ghost_test::Orchestrator::kBackgroundThreadCpu'.
  CHECK_ZERO(ghost::Ghost::SchedSetAffinity(
      ghost::Gtid::Current(),
      ghost::MachineTopology()->ToCpuList(
          std::vector<int>{ghost_test::Orchestrator::kBackgroundThreadCpu})));

  absl::ParseCommandLine(argc, argv);

  ghost_test::Orchestrator::Options options = GetOptions();
  std::cout << options << std::endl;
  std::cout << std::endl;

  std::cout << "Initializing..." << std::endl;
  std::unique_ptr<ghost_test::Orchestrator> orch;
  switch (options.scheduler) {
    case ghost::GhostThread::KernelScheduler::kCfs:
      // CFS (Linux Completely Fair Scheduler).
      orch = std::make_unique<ghost_test::CfsOrchestrator>(options);
      break;
    case ghost::GhostThread::KernelScheduler::kGhost:
      // ghOSt.
      orch = std::make_unique<ghost_test::GhostOrchestrator>(options);
      break;
  }

  SetTimer(options.experiment_duration);

  exit->WaitForNotification();
  orch->Terminate();

  return 0;
}
