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

using std::chrono::steady_clock;

constexpr int NUM_SHORT_TASKS = 1000;
constexpr int NUM_LONG_TASKS = 100;
steady_clock::time_point st_starts[NUM_SHORT_TASKS];
steady_clock::time_point lt_starts[NUM_LONG_TASKS];
steady_clock::time_point st_ends[NUM_SHORT_TASKS];
steady_clock::time_point lt_ends[NUM_LONG_TASKS];

template <typename T>
std::function<void()> make_work(steady_clock::time_point* tp, T duration) {
    return [runtime, duration] {
        std::this_thread::sleep_for(duration);
        *tp = steady_clock::now();
    };
}

// return percentile of experimentTimes
// assumes array is sorted
double percentile(const std::vector<double>& v, double n) {
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

int main() {
    std::mutex m;
    std::vector<std::unique_ptr<ghost::GhostThread>> threads;

    for (int i = 0; i < NUM_LONG_TASKS; ++i) {
        lt_starts[i] = steady_clock::now();
        threads.push_back(std::make_unique<ghost::GhostThread>(
            ghost::GhostThread::KernelScheduler::kGhost,
            make_work(&lt_ends[i], std::chrono::milliseconds(10))));
    }
    for (int i = 0; i < NUM_SHORT_TASKS; ++i) {
        st_starts[i] = steady_clock::now();
        threads.push_back(std::make_unique<ghost::GhostThread>(
            ghost::GhostThread::KernelScheduler::kGhost,
            make_work(&st_ends[i], std::chrono::microseconds(5))));
    }

    for (const auto& t : threads) t->Join();

    std::vector<double> st_runtimes(NUM_SHORT_TASKS);
    std::vector<double> lt_runtimes(NUM_LONG_TASKS);

    for (int i = 0; i < NUM_SHORT_TASKS; ++i) {
        st_runtimes[i] =
            std::chrono::duration_cast<double>(st_ends[i] - st_starts[i])
                .count() *
            1000000;  // measure in us
    }
    for (int i = 0; i < NUM_LONG_TASKS; ++i) {
        lt_runtimes[i] =
            std::chrono::duration_cast<double>(lt_ends[i] - lt_starts[i])
                .count() *
            1000000;  // measure in us
    }

    std::sort(st_runtimes.begin(), st_runtimes.end());
    std::sort(lt_runtimes.begin(), lt_runtimes.end());

    printf("Finished running.\n");
    printf("== Short task stats ==\n");
    printf("0th percentile: %.3f\n", percentile(st_runtimes, 0));
    printf("25th percentile: %.3f\n", percentile(st_runtimes, 0.25));
    printf("50th percentile: %.3f\n", percentile(st_runtimes, 0.5));
    printf("75th percentile: %.3f\n", percentile(st_runtimes, 0.75));
    printf("90th percentile: %.3f\n", percentile(st_runtimes, 0.9));
    printf("99th percentile: %.3f\n", percentile(st_runtimes, 0.99));
    printf("100th percentile: %.3f\n", percentile(st_runtimes, 1));
    printf("== Long task stats ==\n");
    printf("0th percentile: %.3f\n", percentile(lt_runtimes, 0));
    printf("25th percentile: %.3f\n", percentile(lt_runtimes, 0.25));
    printf("50th percentile: %.3f\n", percentile(lt_runtimes, 0.5));
    printf("75th percentile: %.3f\n", percentile(lt_runtimes, 0.75));
    printf("90th percentile: %.3f\n", percentile(lt_runtimes, 0.9));
    printf("99th percentile: %.3f\n", percentile(lt_runtimes, 0.99));
    printf("100th percentile: %.3f\n", percentile(lt_runtimes, 1));
}
