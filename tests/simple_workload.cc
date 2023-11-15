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

constexpr int NUM_SHORT_TASKS = 10;
constexpr int NUM_LONG_TASKS = 1;
steady_clock::time_point st_starts[NUM_SHORT_TASKS];
steady_clock::time_point lt_starts[NUM_LONG_TASKS];
steady_clock::time_point st_ends[NUM_SHORT_TASKS];
steady_clock::time_point lt_ends[NUM_LONG_TASKS];

template <typename T>
std::function<void()> make_work(steady_clock::time_point* tp, T duration) {
    return [tp, duration] {
        auto t1 = steady_clock::now();
        while (steady_clock::now() < t1 + duration) {
        }
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

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cout << "Usage: " << argv[0] << " ghost|cfs" << std::endl;
        return 0;
    }
    ghost::GhostThread::KernelScheduler ks_mode;
    if (argv[1][0] == 'g')
        ks_mode = ghost::GhostThread::KernelScheduler::kGhost;
    else if (argv[1][0] == 'c')
        ks_mode = ghost::GhostThread::KernelScheduler::kCfs;
    else {
        std::cout << "invalid scheduler option" << std::endl;
        return 0;
    }

    std::mutex m;
    std::vector<std::unique_ptr<ghost::GhostThread>> threads;

    for (int i = 0; i < NUM_LONG_TASKS; ++i) {
        lt_starts[i] = steady_clock::now();
        threads.push_back(std::make_unique<ghost::GhostThread>(
            ks_mode, make_work(&lt_ends[i], std::chrono::milliseconds(10))));
    }
    std::this_thread::sleep_for(std::chrono::nanoseconds(1));
    for (int i = 0; i < NUM_SHORT_TASKS; ++i) {
        st_starts[i] = steady_clock::now();
        threads.push_back(std::make_unique<ghost::GhostThread>(
            ks_mode, make_work(&st_ends[i], std::chrono::microseconds(5))));
    }

    for (const auto& t : threads) t->Join();

    std::vector<double> st_runtimes(NUM_SHORT_TASKS);
    std::vector<double> lt_runtimes(NUM_LONG_TASKS);

    for (int i = 0; i < NUM_SHORT_TASKS; ++i) {
        std::chrono::duration<double> diff = st_ends[i] - st_starts[i];
        st_runtimes[i] = diff.count() * 1000000;  // measure in us
    }
    for (int i = 0; i < NUM_LONG_TASKS; ++i) {
        std::chrono::duration<double> diff = lt_ends[i] - lt_starts[i];
        lt_runtimes[i] = diff.count() * 1000000;  // measure in us
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
