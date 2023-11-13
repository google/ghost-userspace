#include <unistd.h>

#include <algorithm>
#include <array>
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

constexpr int NUM_THREADS = 1000;
std::array<double, NUM_THREADS> experimentTimes;

template <typename T>
std::function<void()> make_work(int threadId, T duration) {
    return [duration] {
        auto t1 = steady_clock::now();
        while (steady_clock::now() < t1 + duration) {
        }
        auto t2 = steady_clock::now();
        std::chrono::duration<double> diff = t2 - t1;
        experimentTimes[threadId] = diff.count();
    };
}

// return percentile of experimentTimes
// assumes array is sorted
double percentile(double n) {
    double idx = n * (experimentTimes.size() - 1);
    int a = static_cast<int>(idx);
    int b = a + 1;

    if (b >= experimentTimes.size()) {
        return experimentTimes[a];
    }

    double w = idx - a;
    return (1 - w) * experimentTimes[a] + w * experimentTimes[b];
}

int main() {
    std::mutex m;
    std::array<std::unique_ptr<ghost::GhostThread>, NUM_THREADS> threads;

    for (int i = 0; i < NUM_THREADS; ++i) {
        if (rand() % 10 == 1) {
            threads[i] = std::make_unique<ghost::GhostThread>(
                ghost::GhostThread::KernelScheduler::kGhost,
                make_work(std::chrono::milliseconds(10)));
        } else {
            threads[i] = std::make_unique<ghost::GhostThread>(
                ghost::GhostThread::KernelScheduler::kGhost,
                make_work(std::chrono::microseconds(5)));
        }
    }

    for (const auto& t : threads) t->Join();

    std::sort(experimentTimes.begin(), experimentTimes.end());

    printf("Finished running %d threads.\n", NUM_THREADS);
    printf("0th percentile: %.3f\n", percentile(0));
    printf("25th percentile: %.3f\n", percentile(0.25));
    printf("50th percentile: %.3f\n", percentile(0.5));
    printf("75th percentile: %.3f\n", percentile(0.75));
    printf("90th percentile: %.3f\n", percentile(0.9));
    printf("99th percentile: %.3f\n", percentile(0.99));
    printf("100th percentile: %.3f\n", percentile(1));
}
