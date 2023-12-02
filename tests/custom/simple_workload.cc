#include <sched.h>
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

#include "absl/flags/parse.h"
#include "lib/base.h"
#include "lib/ghost.h"

using std::chrono::steady_clock;

using ghost::GhostThread;
using ghost::Gtid;

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
    std::cerr << "Spawning worker threads..." << std::endl;

    int num_jobs = reqs_per_sec * runtime_secs;
    int num_jobs_done = 0;
    std::vector<Job> jobs(num_jobs);
    std::atomic<bool> isdead(false);
    std::queue<Job *> work_q;
    std::mutex work_q_m;

    // Spawn worker threads
    std::vector<std::unique_ptr<GhostThread>> worker_threads;
    worker_threads.reserve(num_workers);
    for (int i = 0; i < num_workers; ++i) {
        auto thread = std::make_unique<GhostThread>(ks_mode, [&] {
            while (!isdead) {
                Job *job;
                {
                    std::lock_guard lg(work_q_m);
                    if (work_q.empty()) {
                        continue;
                    }
                    job = work_q.front();
                    work_q.pop();

                    ++num_jobs_done;
                    if (num_jobs_done % 5000 == 0) {
                        std::cerr << num_jobs_done << std::endl;
                    }
                }

                auto start = steady_clock::now();
                if (job->type == JobType::Short) {
                    while (std::chrono::duration<double>(steady_clock::now() -
                                                         start)
                               .count() < 1e-6) {
                    }
                } else if (job->type == JobType::Long) {
                    while (std::chrono::duration<double>(steady_clock::now() -
                                                         start)
                               .count() < 1e-3) {
                    }
                }
                job->finished = steady_clock::now();
            }
        });

        worker_threads.push_back(std::move(thread));
    }

    // Send requests into work queue
    steady_clock::time_point t1 = steady_clock::now();
    for (int i = 0; i < num_jobs; ++i) {
        if (rand() % 10000 < (int)(proportion_long_jobs * 10000)) {
            jobs[i].type = JobType::Long;
        } else {
            jobs[i].type = JobType::Short;
        }

        work_q_m.lock();
        jobs[i].submitted = steady_clock::now();
        work_q.push(&jobs[i]);
        work_q_m.unlock();
        double next_scheduled_for = (double)(i + 1) / num_jobs * runtime_secs;
        std::this_thread::sleep_until(
            t1 + std::chrono::duration<double>(next_scheduled_for));
    }

    // Shutdown workers
    steady_clock::time_point shutdown_at = steady_clock::now();
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::lock_guard lg(work_q_m);

        // If we haven't shutdown after 10 seconds, then stop the experiment.
        // Mark tail latency of remaining jobs as very large value
        if (std::chrono::duration<double>(steady_clock::now() - shutdown_at)
                .count() > 10.0) {
            std::cout << "Test timed out." << std::endl;
            std::cerr << "Test timed out." << std::endl;
            while (!work_q.empty()) {
                // mark tail latency as 30 seconds (arbitrary large value)
                work_q.front()->finished =
                    shutdown_at + std::chrono::seconds(30);
                work_q.pop();
            }
        }

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
    srand(time(NULL));

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

    std::vector<std::string> pcts = {"0",  "25", "50",   "75",
                                     "90", "99", "99.9", "100"};

    printf("Finished running. %d short tasks, %d long tasks ran.\n",
           (int)short_runtimes.size(), (int)long_runtimes.size());
    printf("== Short task stats ==\n");
    for (const auto &p : pcts) {
        printf("%s percentile: %.3f\n", p.c_str(),
               percentile(short_runtimes, stod(p) / 100.0));
    }
    printf("== Long task stats ==\n");
    for (const auto &p : pcts) {
        printf("%s percentile: %.3f\n", p.c_str(),
               percentile(long_runtimes, stod(p) / 100.0));
    }

    std::vector<std::pair<std::string, std::string>> stats;
    stats.push_back({"num_short", std::to_string(short_runtimes.size())});
    stats.push_back({"num_long", std::to_string(long_runtimes.size())});
    for (const auto &p : pcts) {
        std::ostringstream oss;
        oss << "short_" << p << "_pct";
        stats.push_back({oss.str(), std::to_string(percentile(
                                        short_runtimes, stod(p) / 100.0))});
    }
    for (const auto &p : pcts) {
        std::ostringstream oss;
        oss << "long_" << p << "_pct";
        stats.push_back({oss.str(), std::to_string(percentile(
                                        long_runtimes, stod(p) / 100.0))});
    }

    printf("<csv>\n");

    bool first = true;
    for (const auto &[key, _] : stats) {
        if (!first) {
            printf(", ");
        }
        first = false;
        printf("%s", key.c_str());
    }
    printf("\n");

    first = true;
    for (const auto &[_, value] : stats) {
        if (!first) {
            printf(", ");
        }
        first = false;
        printf("%s", value.c_str());
    }
    printf("\n");

    printf("</csv>\n");
}
