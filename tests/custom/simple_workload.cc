#include <sched.h>
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

#include "absl/flags/parse.h"
#include "lib/base.h"
#include "lib/ghost.h"
#include "shared/prio_table.h"

using std::chrono::steady_clock;

using ghost::GhostThread;
using ghost::Gtid;
using ghost::MonotonicNow;
using ghost::PrioTable;
using ghost::sched_item;
using ghost::work_class;

/*
 * PrioTable code adapted from simple_edf.cc
 */

enum { kWcIdle, kWcOneShot, kWcRepeatable, kWcNum };

bool sched_item_runnable(const std::unique_ptr<PrioTable> &table_, int sidx) {
    sched_item *src = table_->sched_item(sidx);
    uint32_t begin, flags;
    bool success;

    do {
        begin = src->seqcount.read_begin();
        flags = src->flags;
        success = src->seqcount.read_end(begin);
    } while (!success);

    return flags & SCHED_ITEM_RUNNABLE;
}

void mark_sched_item_idle(const std::unique_ptr<PrioTable> &table_, int sidx) {
    sched_item *si = table_->sched_item(sidx);

    uint32_t seq = si->seqcount.write_begin();
    si->flags &= ~SCHED_ITEM_RUNNABLE;
    si->seqcount.write_end(seq);
    table_->MarkUpdatedIndex(sidx, /* num_retries = */ 3);
}

void mark_sched_item_runnable(const std::unique_ptr<PrioTable> &table_,
                              int sidx) {
    sched_item *si = table_->sched_item(sidx);

    uint32_t seq = si->seqcount.write_begin();
    si->flags |= SCHED_ITEM_RUNNABLE;
    si->seqcount.write_end(seq);
    table_->MarkUpdatedIndex(sidx, /* num_retries = */ 3);
}

void update_sched_item(const std::unique_ptr<PrioTable> &table_, uint32_t sidx,
                       uint32_t wcid, uint32_t flags, const Gtid &gtid,
                       absl::Duration d) {
    sched_item *si;

    si = table_->sched_item(sidx);

    uint32_t seq = si->seqcount.write_begin();
    si->sid = sidx;
    si->wcid = wcid;
    si->flags = flags;
    si->gpid = gtid.id();
    si->deadline = absl::ToUnixNanos(MonotonicNow() + d);
    si->seqcount.write_end(seq);
    table_->MarkUpdatedIndex(sidx, /* num_retries = */ 3);
}

void setup_work_classes(const std::unique_ptr<PrioTable> &table_) {
    work_class *wc;

    wc = table_->work_class(kWcIdle);
    wc->id = kWcIdle;
    wc->flags = 0;
    wc->exectime = 0;

    wc = table_->work_class(kWcOneShot);
    wc->id = kWcOneShot;
    wc->flags = WORK_CLASS_ONESHOT;
    wc->exectime = absl::ToInt64Nanoseconds(absl::Milliseconds(10));

    wc = table_->work_class(kWcRepeatable);
    wc->id = kWcRepeatable;
    wc->flags = WORK_CLASS_REPEATING;
    wc->exectime = absl::ToInt64Nanoseconds(absl::Milliseconds(10));
    wc->period = absl::ToInt64Nanoseconds(absl::Milliseconds(100));
}

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

std::vector<Job> run_experiment(const std::unique_ptr<PrioTable> &prio_table,
                                GhostThread::KernelScheduler ks_mode,
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
        auto thread = std::make_unique<GhostThread>(
            ks_mode, [i, &isdead, &work_q, &work_q_m] {
                while (!isdead) {
                    Job *job;
                    {
                        std::lock_guard lg(work_q_m);
                        if (work_q.empty()) {
                            continue;
                        }
                        job = work_q.front();
                        work_q.pop();
                    }

                    std::cout << i << std::endl;
                    auto start = steady_clock::now();
                    if (job->type == JobType::Short) {
                        while (std::chrono::duration<double>(
                                   steady_clock::now() - start)
                                   .count() < 1e-6) {
                        }
                    } else if (job->type == JobType::Long) {
                        while (std::chrono::duration<double>(
                                   steady_clock::now() - start)
                                   .count() < 1e-3) {
                        }
                    }
                    job->finished = steady_clock::now();
                }
            });
        update_sched_item(prio_table, 0, kWcOneShot, SCHED_ITEM_RUNNABLE,
                          thread->gtid(), absl::Milliseconds(100));
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
        jobs[i].submitted = steady_clock::now();

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

    // Set up PrioTable
    auto prio_table = std::make_unique<PrioTable>(
        51200, kWcNum, PrioTable::StreamCapacity::kStreamCapacity19);
    setup_work_classes(prio_table);

    auto jobs = run_experiment(prio_table, ks_mode, reqs_per_sec, runtime_secs,
                               num_workers, proportion_long_jobs);

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
