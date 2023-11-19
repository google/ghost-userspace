#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "lib/base.h"
#include "lib/ghost.h"
#include "shared/prio_table.h"

using std::chrono::steady_clock;

using ghost::GhostThread;

// Stopwatch answers queries "how much time has elapsed since the stopwatch
// started?" The stopwatch begins upon construction.
class Stopwatch {
  public:
    Stopwatch() : start(steady_clock::now()) {}

    // Get elapsed time in seconds.
    double elapsed() {
        auto now = steady_clock::now();
        std::chrono::duration<double> diff = now - start;
        return diff.count();
    }

  private:
    steady_clock::time_point start;
};

// return percentile of experimentTimes
double percentile(std::vector<double> &v, double n) {
    if (v.empty())
        return 0.0;

    sort(v.begin(), v.end());
    int sz = (int)v.size();

    double idx = n * (sz - 1);
    int a = static_cast<int>(idx);
    int b = a + 1;

    if (b >= sz) {
        return v[a];
    }

    double w = idx - a;
    return (1 - w) * v[a] + w * v[b];
}

struct TaskStats {
    double task_runtime;   // time it takes to run the task
    double actual_runtime; // time it actually took for the task to complete
};

std::vector<TaskStats>
run_experiment(GhostThread::KernelScheduler ks_mode, int reqs_per_sec,
               int runtime_secs,
               std::function<double()> workload_distribution) {
    int total_reqs = reqs_per_sec * runtime_secs;
    std::vector<std::unique_ptr<Stopwatch>> req_timers(total_reqs);
    std::vector<std::unique_ptr<GhostThread>> threads(total_reqs);
    std::vector<TaskStats> stats(total_reqs);

    Stopwatch exp_timer;

    std::cout << "Spawning worker threads..." << std::endl;

    for (int i = 0; i < total_reqs; ++i) {
        double scheduled_for = (double)i / total_reqs * runtime_secs;
        if (exp_timer.elapsed() >= scheduled_for) {
            req_timers[i] = std::make_unique<Stopwatch>();
            threads[i] = std::make_unique<GhostThread>(
                ks_mode,
                [&req_timers, &threads, &stats, &workload_distribution, i]() {
                    double runtime = workload_distribution();
                    while (req_timers[i]->elapsed() < runtime) {
                    }
                    stats[i] = {runtime, req_timers[i]->elapsed()};
                });
        } else {
            --i;
        }
    }

    for (const auto &t : threads) {
        t->Join();
    }

    std::cout << "Finished running " << total_reqs << " worker threads."
              << std::endl;
    return stats;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        std::cout << "Usage: " << argv[0]
                  << " ghost|cfs reqs_per_sec runtime_secs" << std::endl;
        return 0;
    }
    GhostThread::KernelScheduler ks_mode;
    if (argv[1][0] == 'g') {
        ks_mode = GhostThread::KernelScheduler::kGhost;
    } else if (argv[1][0] == 'c') {
        ks_mode = GhostThread::KernelScheduler::kCfs;
    } else {
        std::cout << "invalid scheduler option" << std::endl;
        return 0;
    }
    int reqs_per_sec = std::atoi(argv[2]);
    int runtime_secs = std::atoi(argv[3]);

    auto stats = run_experiment(ks_mode, reqs_per_sec, runtime_secs, [] {
        if (rand() % 1000 < 25) {
            return 0.001;
        } else {
            return 0.000001;
        }
    });

    std::vector<double> short_runtimes;
    std::vector<double> long_runtimes;

    for (const auto &stat : stats) {
        if (stat.task_runtime < 0.0001) {
            short_runtimes.push_back(stat.actual_runtime);
        } else {
            long_runtimes.push_back(stat.actual_runtime);
        }
    }

    printf("Finished running. %d short tasks, %d long tasks ran.\n",
           (int)short_runtimes.size(), (int)long_runtimes.size());
    printf("== Short task stats ==\n");
    printf("0th percentile: %.3f\n", percentile(short_runtimes, 0));
    printf("25th percentile: %.3f\n", percentile(short_runtimes, 0.25));
    printf("50th percentile: %.3f\n", percentile(short_runtimes, 0.5));
    printf("75th percentile: %.3f\n", percentile(short_runtimes, 0.75));
    printf("90th percentile: %.3f\n", percentile(short_runtimes, 0.9));
    printf("99th percentile: %.3f\n", percentile(short_runtimes, 0.99));
    printf("100th percentile: %.3f\n", percentile(short_runtimes, 1));
    printf("== Long task stats ==\n");
    printf("0th percentile: %.3f\n", percentile(long_runtimes, 0));
    printf("25th percentile: %.3f\n", percentile(long_runtimes, 0.25));
    printf("50th percentile: %.3f\n", percentile(long_runtimes, 0.5));
    printf("75th percentdouble percentile(std::vector<double> &v, double n) {
    sort(v.begin(), v.end());
    int sz = (int)v.size();
    double idx = n * (sz - 1);
    int a = static_cast<int>(idx);
    int b = a + 1;

    std::cout 

    if (b >= sz) {
        return v[a];
    }

    double w = idx - a;
    return (1 - w) * v[a] + w * v[b];
}
ile : % .3f\n ", percentile(long_runtimes, 0.75));
      printf("90th percentile: %.3f\n", percentile(long_runtimes, 0.9));
printf("99th percentile: %.3f\n", percentile(long_runtimes, 0.99));
printf("100th percentile: %.3f\n", percentile(long_runtimes, 1));
}
