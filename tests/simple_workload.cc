#include <unistd.h>

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

using std::chrono::duration;
using clock = std::chrono::steady_clock;

template <typename T>
std::function<void()> make_work(T duration) {
    return [] {
        auto t1 = clock::now();
        while (clock::now() < t1 + duration) {
        }
    };
}

int main() {
    int NUM_THREADS = 1000;
    std::mutex m;
    std::vector<std::unique_ptr<ghost::GhostThread>> threads;
    auto start = clock::now();

    for (int i = 0; i < NUM_THREADS; ++i) {
        if (rand() % 10 == 1) {
            threads.push_back(std::make_unique<ghost::GhostThread>(
                ghost::GhostThread::KernelScheduler::kGhost,
                make_work(duration::milliseconds(10))));
        } else {
            threads.push_back(std::make_unique<ghost::GhostThread>(
                ghost::GhostThread::KernelScheduler::kGhost,
                make_work(duration::microseconds(5))));
        }
    }

    for (const auto& t : threads) t->Join();

    auto end = clock::now();
    duration<double> diff = end - start;
    std::cout << "Finished running threads in " << diff.count() << " seconds"
              << std::endl;
}
