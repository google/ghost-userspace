#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
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

enum JobType { Short, Long };
struct Job {
    JobType type;
    steady_clock::time_point submitted;
    steady_clock::time_point finished;
};

std::vector<Job> run_experiment(GhostThread::KernelScheduler ks_mode,
                                int reqs_per_sec, int runtime_secs,
                                int num_workers, double proportion_long_jobs) {
    std::cout << "Spawning worker threads..." << std::endl;

    int num_jobs = reqs_per_sec * runtime_secs;
    std::vector<Job> jobs(num_jobs);
    std::atomic<bool> isdead(false);
    std::queue<Job *> work_q;
    std::mutex work_q_m;

    // Spawn worker threads
    std::vector<std::unique_ptr<GhostThread>> worker_threads;
    worker_threads.reserve(num_workers);
    for (int i = 0; i < num_workers; ++i) {
        worker_threads.emplace_back(
            std::make_unique<GhostThread>(ks_mode, [&isdead] {
                while (!isdead) {
                    work_q_m.lock();
                    auto job = work_q.front();
                    work_q.pop();
                    work_q_m.unlock();

                    auto start = steady_clock::now();
                    if (job.type == JobType::Short) {
                        while (std::chrono::duration<double>(
                                   steady_clock::now() - start)
                                   .count() < 1e-6) {
                        }
                    } else if (job.type == JobType::Long) {
                        while (std::chrono::duration<double>(
                                   steady_clock::now() - start)
                                   .count() < 1e-3) {
                        }
                    }
                }
            }));
    }

    // Send requests into work queue
    steady_clock::time_point t1 = steady_clock::now();
    for (int i = 0; i < num_jobs; ++i) {
        work_q_m.lock();
        work_q.push(&jobs[i]);
        work_q_m.unlock();
        double next_scheduled_for = (double)(i + 1) / num_jobs * runtime_secs;
        std::this_thread::sleep_until(
            t1 + std::chrono::duration<double>(next_scheduled_for));
    }

    // Shutdown workers
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::lock_guard lg(work_q_m);
        if (work_q.empty()) {
            isdead = true;
            break;
        }
    }
    for (const auto &t : worker_threads) {
        t->Join();
    }
    std::cout << "Finished running worker threads." << std::endl;

    return jobs;
}

int main(int argc, char *argv[]) {
    if (argc != 6) {
        std::cout << "Usage: " << argv[0]
                  << " ghost|cfs reqs_per_sec runtime_secs num_workers "
                     "proportion_long_jobs"
                  << std::endl;
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
    int num_workers = std::atoi(argv[4]);
    double proportion_long_jobs = std::atof(argv[5]);

    auto jobs = run_experiment(ks_mode, reqs_per_sec, runtime_secs, num_workers,
                               proportion_long_jobs);

    std::vector<double> short_runtimes;
    std::vector<double> long_runtimes;

    for (const auto &job : jobs) {
        if (job.type == JobType::Short) {
            short_runtimes.push_back(
                std::chrono::duration<double>(job.finished - job.submitted)
                    .count() *
                1e6);
        } else if (job.type == JobType::Long) {
            long_runtimes.push_back(
                std::chrono::duration<double>(job.finished - job.submitted)
                    .count() *
                1e6);
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
    printf("75th percentile: %.3f\n", percentile(long_runtimes, 0.75));
    printf("90th percentile: %.3f\n", percentile(long_runtimes, 0.9));
    printf("99th percentile: %.3f\n", percentile(long_runtimes, 0.99));
    printf("100th percentile: %.3f\n", percentile(long_runtimes, 1));
}
