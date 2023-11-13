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
double st_times[NUM_SHORT_TASKS];
double lt_times[NUM_LONG_TASKS];

double now() {
    return std::chrono::duration_cast<double>(steady_clock::now()).count();
}

template <typename T>
std::function<void()> make_work(double* runtime, T duration) {
    return [runtime, duration] {
        std::this_thread::sleep_for(duration);
        double diff = now() - *runtime;
        *runtime = diff * 1000000;  // measure runtime in us
    };
}

// return percentile of experimentTimes
// assumes array is sorted
double percentile(double* arr, int sz, double n) {
    double idx = n * (sz - 1);
    int a = static_cast<int>(idx);
    int b = a + 1;

    if (b >= sz) {
        return arr[a];
    }

    double w = idx - a;
    return (1 - w) * arr[a] + w * arr[b];
}

int main() {
    std::mutex m;
    std::vector<std::unique_ptr<ghost::GhostThread>> threads;

    for (int i = 0; i < NUM_LONG_TASKS; ++i) {
        lt_times[i] = now();
        threads.push_back(std::make_unique<ghost::GhostThread>(
            ghost::GhostThread::KernelScheduler::kGhost,
            make_work(&lt_times[i], std::chrono::milliseconds(10))));
    }
    for (int i = 0; i < NUM_SHORT_TASKS; ++i) {
        st_times[i] = now();
        threads.push_back(std::make_unique<ghost::GhostThread>(
            ghost::GhostThread::KernelScheduler::kGhost,
            make_work(&st_times[i], std::chrono::microseconds(5))));
    }

    for (const auto& t : threads) t->Join();

    std::sort(&st_times[0], &st_times[NUM_SHORT_TASKS]);
    std::sort(&lt_times[0], &lt_times[NUM_LONG_TASKS]);

    printf("Finished running.\n");
    printf("== Short task stats ==\n");
    printf("0th percentile: %.3f\n", percentile(st_times, NUM_SHORT_TASKS, 0));
    printf("25th percentile: %.3f\n",
           percentile(st_times, NUM_SHORT_TASKS, 0.25));
    printf("50th percentile: %.3f\n",
           percentile(st_times, NUM_SHORT_TASKS, 0.5));
    printf("75th percentile: %.3f\n",
           percentile(st_times, NUM_SHORT_TASKS, 0.75));
    printf("90th percentile: %.3f\n",
           percentile(st_times, NUM_SHORT_TASKS, 0.9));
    printf("99th percentile: %.3f\n",
           percentile(st_times, NUM_SHORT_TASKS, 0.99));
    printf("100th percentile: %.3f\n",
           percentile(st_times, NUM_SHORT_TASKS, 1));
    printf("== Long task stats ==\n");
    printf("0th percentile: %.3f\n", percentile(lt_times, NUM_LONG_TASKS, 0));
    printf("25th percentile: %.3f\n",
           percentile(lt_times, NUM_LONG_TASKS, 0.25));
    printf("50th percentile: %.3f\n",
           percentile(lt_times, NUM_LONG_TASKS, 0.5));
    printf("75th percentile: %.3f\n",
           percentile(lt_times, NUM_LONG_TASKS, 0.75));
    printf("90th percentile: %.3f\n",
           percentile(lt_times, NUM_LONG_TASKS, 0.9));
    printf("99th percentile: %.3f\n",
           percentile(lt_times, NUM_LONG_TASKS, 0.99));
    printf("100th percentile: %.3f\n", percentile(lt_times, NUM_LONG_TASKS, 1));
}
