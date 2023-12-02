#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "lib/ghost.h"

int main() {
    std::vector<std::unique_ptr<ghost::GhostThread>> threads;
    int ctr = 0;
    for (int threadId = 0; threadId < 100000; ++threadId) {
        threads.push_back(std::make_unique<ghost::GhostThread>(
            ghost::GhostThread::KernelScheduler::kGhost, [&ctr, threadId]() {
                for (int i = 0; i < 100; ++i) {
                    std::this_thread::sleep_for(
                        std::chrono::microseconds(threadId + 5));
                    ctr = ctr + 1;
                }
            }));
    }
    for (const auto &t : threads) {
        t->Join();
    }
    std::cout << "Result: " << ctr << std::endl;
}
