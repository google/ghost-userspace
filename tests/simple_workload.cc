#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "lib/base.h"
#include "lib/ghost.h"

int main() {
    int NUM_THREADS = 1000;
    std::mutex m;
    std::vector<std::unique_ptr<ghost::GhostThread>> threads;
    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < NUM_THREADS; ++i) {
        if (rand() % 10 == 1) {
            threads.push_back(std::make_unique<ghost::GhostThread>(
                ghost::GhostThread::KernelScheduler::kGhost, []() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }));
        } else {
            threads.push_back(std::make_unique<ghost::GhostThread>(
                ghost::GhostThread::KernelScheduler::kGhost, [] {
                    std::this_thread::sleep_for(std::chrono::microseconds(5))
                }));
        }
    }

    for (const auto& t : threads) t->Join();

    auto end = std::chrono::steady_clock::now();
    std::chrono::duration<double> diff = end - start;
    std::cout << "Finished running threads in " << diff.count() << " seconds"
              << std::endl;
}
