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

#include <stdio.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/sysinfo.h>

#include <atomic>
#include <memory>
#include <vector>

#include "absl/random/random.h"
#include "absl/synchronization/barrier.h"
#include "lib/ghost.h"

namespace ghost {
namespace {

absl::Duration GetThreadCpuTime() {
  timespec ts;
  CHECK_EQ(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts), 0);
  return absl::Seconds(ts.tv_sec) + absl::Nanoseconds(ts.tv_nsec);
}

void Spin(absl::Duration duration, absl::Duration start_duration) {
  if (duration <= absl::ZeroDuration()) {
    return;
  }

  while (GetThreadCpuTime() - start_duration < duration) {
    // We are doing synthetic work, so do not issue 'pause' instructions.
  }
}

void doit_cfs(absl::Duration spin_for, absl::Duration sleep_for, int iterations,
              int num_cpus, int overcommit) {
  std::atomic<int64_t> duration = 0;
  std::vector<std::unique_ptr<GhostThread>> threads;

  int num_threads = num_cpus * overcommit;
  threads.reserve(num_threads);

#if 0
  printf("cfs: num_threads = %d, num_cpus = %d, overcommit = %d\n",
         num_threads, num_cpus, overcommit);
#endif

  const struct timespec tv = {
      .tv_sec = sleep_for / absl::Seconds(1),
      .tv_nsec = ToInt64Nanoseconds(sleep_for) % 1'000'000'000L,
  };

  // Set timer_slack to some ridiculously low, non-zero value (otherwise
  // nanosleep can pad the timeout by up to 50 usecs).
  const long timer_slack_ns = 10;
  CHECK_EQ(prctl(PR_SET_TIMERSLACK, timer_slack_ns, 0, 0, 0), 0);

  CHECK_EQ(mlockall(MCL_CURRENT | MCL_FUTURE), 0);

  absl::Barrier* barrier = new absl::Barrier(num_threads);

  for (int i = 0; i < num_threads; i++) {
    threads.emplace_back(new GhostThread(
        GhostThread::KernelScheduler::kCfs,
        [spin_for, iterations, tv, &barrier, &duration] {
          if (barrier->Block()) {
            delete barrier;
          }

          // randomize startup to avoid synchronized sleep and wakeups.
          absl::BitGen rng;
          absl::SleepFor(absl::Milliseconds(absl::Uniform(rng, 0, 10)));

          CHECK_EQ(prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0), timer_slack_ns);
          absl::Time start = absl::Now();
          for (int iter = 0; iter < iterations; iter++) {
            Spin(spin_for, GetThreadCpuTime());
            nanosleep(&tv, NULL);
          }
          std::atomic_fetch_add(&duration,
                                ToInt64Nanoseconds(absl::Now() - start));
        }));
  }

  for (auto& t : threads) t->Join();

  absl::Duration ideal = (spin_for + sleep_for) * iterations * overcommit;

#if 0
  printf("Ideal duration %ld msecs, overcommit = %d\n",
         ToInt64Milliseconds(ideal), overcommit);

  printf("Actual duration %ld msecs across %d cpus (normalized %ld msecs)\n",
         duration / 1'000'000, num_cpus,
         (duration / num_cpus) / 1'000'000);
#endif

  duration = duration / num_cpus;
  printf("%ld\n", (ToInt64Nanoseconds(ideal) * 100) / duration);
}

void doit_ghost(absl::Duration spin_for, absl::Duration sleep_for,
                int iterations, int num_cpus) {
  std::vector<std::unique_ptr<GhostThread>> threads;
  std::vector<absl::Duration> durations;
  int num_threads = num_cpus - 1;  // leave one cpu for global agent.

#if 0
  printf("ghost: num_threads = %d, num_cpus = %d\n", num_threads, num_cpus);
#endif

  threads.reserve(num_threads);
  durations.reserve(num_threads);

  const struct timespec tv = {
      .tv_sec = sleep_for / absl::Seconds(1),
      .tv_nsec = ToInt64Nanoseconds(sleep_for) % 1'000'000'000L,
  };

  // Set timer_slack to some ridiculously low, non-zero value (otherwise
  // nanosleep can pad the timeout by up to 50 usecs).
  const long timer_slack_ns = 10;
  CHECK_EQ(prctl(PR_SET_TIMERSLACK, timer_slack_ns, 0, 0, 0), 0);

  CHECK_EQ(mlockall(MCL_CURRENT | MCL_FUTURE), 0);

  absl::Barrier* barrier = new absl::Barrier(num_threads);

  for (int i = 0; i < num_threads; i++) {
    threads.emplace_back(new GhostThread(
        GhostThread::KernelScheduler::kGhost,
        [spin_for, iterations, i, tv, &barrier, &durations] {
          if (barrier->Block()) {
            delete barrier;
          }

          // randomize startup to avoid synchronized sleep and wakeups.
          absl::BitGen rng;
          absl::SleepFor(absl::Milliseconds(absl::Uniform(rng, 0, 10)));

          CHECK_EQ(prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0), timer_slack_ns);
          absl::Time start = absl::Now();
          for (int iter = 0; iter < iterations; iter++) {
            Spin(spin_for, GetThreadCpuTime());
            nanosleep(&tv, NULL);
          }
          durations[i] = absl::Now() - start;
        }));
  }

  for (auto& t : threads) t->Join();

  absl::Duration ideal = (spin_for + sleep_for) * iterations;

  absl::Duration actual = absl::ZeroDuration();
  for (int i = 0; i < num_threads; i++) {
    actual += durations[i];
  }

  actual /= num_threads;
  printf("%ld\n",
         (ToInt64Nanoseconds(ideal) * 100) / ToInt64Nanoseconds(actual));
}

}  // namespace
}  // namespace ghost

#define DEFAULT_TEST_DURATION 5  // seconds.
#define DEFAULT_OVERCOMMIT 5

static const char* progname;
static int verbose;

static void usage(int rc) {
  fprintf(stderr,
          "Usage: %s [-cv][-d <secs>][-n <num_cpus>]"
          "[-o <overcommit>]\n"
          "  -c: cfs scheduling (default is ghost)\n"
          "  -d: test duration in seconds (default is %d)"
          "  -o: overcommit factor for cfs (default is %d)\n"
          "  -n: num_cpus (default is %d)\n",
          progname, DEFAULT_TEST_DURATION, DEFAULT_OVERCOMMIT, get_nprocs());
  exit(rc);
}

int main(int argc, char* argv[]) {
  bool use_cfs = false;
  int opt, duration = DEFAULT_TEST_DURATION;
  int overcommit = DEFAULT_OVERCOMMIT;
  int num_cpus = get_nprocs();

  progname = basename(argv[0]);

  while ((opt = getopt(argc, argv, "d:n:o:vc")) != -1) {
    switch (opt) {
      case 'c':
        use_cfs = true;
        break;
      case 'o':
        overcommit = strtol(optarg, NULL, 0);
        break;
      case 'd':
        duration = strtol(optarg, NULL, 0);
        break;
      case 'n':
        num_cpus = strtol(optarg, NULL, 0);
        break;
      case 'v':
        verbose++;
        break;
      default:
        usage(1);
    }
  }

  if (duration <= 0 || duration > 30) usage(2);

  std::array<absl::Duration, 6> spin_values = {
      absl::Microseconds(1000), absl::Microseconds(750),
      absl::Microseconds(500),  absl::Microseconds(250),
      absl::Microseconds(100),  absl::Microseconds(50),
  };

  for (absl::Duration spin : spin_values) {
    if (spin >= absl::Milliseconds(1)) {
      printf("%.2f msecs\n", absl::ToDoubleMilliseconds(spin));
    } else {
      printf("%.2f usecs\n", absl::ToDoubleMicroseconds(spin));
    }
    for (absl::Duration sleep = spin; sleep >= spin / 10; sleep -= spin / 10) {
      int iterations = absl::Seconds(duration) / (spin + sleep);
      if (use_cfs)
        ghost::doit_cfs(spin, sleep, iterations, num_cpus, overcommit);
      else
        ghost::doit_ghost(spin, sleep, iterations, num_cpus);
    }
  }
}
