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

#include "experiments/antagonist/orchestrator.h"

#include <iostream>

namespace ghost_test {

std::ostream& operator<<(std::ostream& os,
                         const Orchestrator::Options& options) {
  os << "cpus:";
  for (int i = 0; i < options.cpus.size(); i++) {
    os << " " << options.cpus[i];
  }
  os << std::endl;
  os << "experiment_duration: " << options.experiment_duration << std::endl;
  os << "ghost_qos: " << options.ghost_qos << std::endl;
  os << "num_threads: " << options.num_threads << std::endl;
  os << "print_format: " << (options.print_options.pretty ? "pretty" : "csv")
     << std::endl;
  os << "scheduler: "
     << (options.scheduler == ghost::GhostThread::KernelScheduler::kCfs
             ? "cfs"
             : "ghost")
     << std::endl;
  os << "work_share: " << options.work_share;
  return os;
}

Orchestrator::Orchestrator(Options opts)
    : options_(std::move(opts)),
      run_duration_(opts.num_threads),
      soak_start_(opts.num_threads),
      usage_start_(opts.num_threads),
      thread_triggers_(opts.num_threads),
      thread_pool_(opts.num_threads) {
  CHECK_GE(options_.work_share, 0.0);
  CHECK_LE(options_.work_share, 1.0);
  for (int cpu : options_.cpus) {
    CHECK_NE(cpu, kBackgroundThreadCpu);
  }
}

// C++ requires pure virtual destructors to have a definition.
Orchestrator::~Orchestrator() {}

void Orchestrator::Terminate() {
  const absl::Duration runtime = absl::Now() - start_;
  // Do this check after calculating 'runtime' to avoid inflating 'runtime'.
  CHECK_GT(start_, absl::UnixEpoch());

  for (size_t i = 0; i < options_.num_threads; ++i) {
    thread_pool_.MarkExit(i);
  }
  // No need to mark CFS threads or ghOSt threads as runnable because they are
  // always runnable, unlike in the RocksDB application.
  thread_pool_.Join();

  PrintResults(runtime);
}

void Orchestrator::PrintResults(absl::Duration runtime) const {
  std::cout << "Stats:" << std::endl;
  Print(run_duration_, runtime, options_.print_options);
}

absl::Duration Orchestrator::ThreadUsage() {
  timespec spec;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &spec);
  return absl::Nanoseconds(spec.tv_nsec) + absl::Seconds(spec.tv_sec);
}

void Orchestrator::SoakHelper(uint32_t sid, double soak_share) {
  // For each period, this method spins for a duration of 'kPeriod' *
  // 'soak_share' and then sleeps for the remainder of the period. If we cannot
  // finish the synthetic work before the end of the period (usually because the
  // thread was preempted by the scheduler), finish the synthetic work and
  // immediately move to the next round without sleeping.

  // It does not really matter what we set the period to as long as the period
  // is not too short. If the period is too short (e.g., 1 microsecond), then we
  // will not be able to accurately generate the specified soak share.
  constexpr absl::Duration kPeriod = absl::Microseconds(100);

  if (soak_start_[sid] == absl::UnixEpoch()) {
    soak_start_[sid] = absl::Now();
    usage_start_[sid] = ThreadUsage();
  }
  absl::Duration soak_per_period = soak_share * kPeriod;
  // 'n' is the current round. We subtract one nanosecond since the round number
  // is zero-indexed.
  int n = (absl::Now() - soak_start_[sid] + kPeriod - absl::Nanoseconds(1)) /
          kPeriod;
  absl::Time finish = soak_start_[sid] + (n * kPeriod);

  absl::Duration target_usage = n * soak_per_period;
  absl::Duration usage;
  while ((usage = ThreadUsage() - usage_start_[sid]) < target_usage &&
         !thread_pool_.ShouldExit(sid)) {
  }
  run_duration_[sid] = usage;
  std::this_thread::sleep_until(absl::ToChronoTime(finish));
}

}  // namespace ghost_test
