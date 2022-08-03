#include <stdio.h>

#include <latch>
#include <memory>
#include <vector>

#include "lib/base.h"
#include "lib/ghost.h"

// A series of simple tests for ghOSt schedulers.

namespace ghost {
namespace {

struct ScopedTime {
  ScopedTime() { start = absl::Now(); }
  ~ScopedTime() {
    printf(" took %0.2f ms\n", absl::ToDoubleMilliseconds(absl::Now() - start));
  }
  absl::Time start;
};

void SimpleExp() {
  printf("\nStarting simple worker\n");
  GhostThread t(GhostThread::KernelScheduler::kGhost, [] {
    fprintf(stderr, "hello world!\n");
    absl::SleepFor(absl::Milliseconds(10));
    fprintf(stderr, "fantastic nap!\n");
    // Verify that a ghost thread implicitly clones itself in the ghost
    // scheduling class.
    std::thread t2([] {
      CHECK_EQ(sched_getscheduler(/*pid=*/0), SCHED_GHOST);
    });
    t2.join();
  });

  t.Join();
  printf("\nFinished simple worker\n");
}

void SimpleExpMany(int num_threads) {
  std::vector<std::unique_ptr<GhostThread>> threads;

  threads.reserve(num_threads);
  for (int i = 0; i < num_threads; i++) {
    threads.emplace_back(
        new GhostThread(GhostThread::KernelScheduler::kGhost, [] {
          absl::SleepFor(absl::Milliseconds(10));

          // Verify that a ghost thread implicitly clones itself in the ghost
          // scheduling class.
          std::thread t([] {
            CHECK_EQ(sched_getscheduler(/*pid=*/0), SCHED_GHOST);
          });
          t.join();

          absl::SleepFor(absl::Milliseconds(10));
        }));
  }

  for (auto& t : threads) t->Join();
}

void SpinFor(absl::Duration d) {
  while (d > absl::ZeroDuration()) {
    absl::Time a = ghost::MonotonicNow();
    absl::Time b;

    // Try to minimize the contribution of arithmetic/Now() overhead.
    for (int i = 0; i < 150; i++) b = ghost::MonotonicNow();

    absl::Duration t = b - a;

    // Don't count preempted time
    if (t < absl::Microseconds(100)) d -= t;
  }
}

void BusyExpRunFor(int num_threads, absl::Duration d) {
  std::vector<std::unique_ptr<GhostThread>> threads;

  threads.reserve(num_threads);
  for (int i = 0; i < num_threads; i++) {
    threads.emplace_back(
        new GhostThread(GhostThread::KernelScheduler::kGhost, [&] {
          // Time start = Now();
          // while (Now() - start < d) {}
          SpinFor(d);
        }));
  }

  for (auto& t : threads) t->Join();
}

void TaskDeparted() {
  printf("\nStarting simple worker\n");
  GhostThread t(GhostThread::KernelScheduler::kGhost, [] {
    fprintf(stderr, "hello world!\n");
    absl::SleepFor(absl::Milliseconds(10));

    fprintf(stderr, "fantastic nap! departing ghOSt now for CFS...\n");
    const sched_param param{};
    CHECK_EQ(sched_setscheduler(/*pid=*/0, SCHED_OTHER, &param), 0);
    CHECK_EQ(sched_getscheduler(/*pid=*/0), SCHED_OTHER);
    fprintf(stderr, "hello from CFS!\n");
  });

  t.Join();
  printf("\nFinished simple worker\n");
}

void TaskDepartedMany(int num_threads) {
  std::vector<std::unique_ptr<GhostThread>> threads;

  threads.reserve(num_threads);
  for (int i = 0; i < num_threads; i++) {
    threads.emplace_back(
        new GhostThread(GhostThread::KernelScheduler::kGhost, [] {
          absl::SleepFor(absl::Milliseconds(10));

          const sched_param param{};
          CHECK_EQ(sched_setscheduler(/*pid=*/0, SCHED_OTHER, &param), 0);
          CHECK_EQ(sched_getscheduler(/*pid=*/0), SCHED_OTHER);
        }));
  }

  for (auto& t : threads) t->Join();
}

void TaskDepartedManyRace(int num_threads) {
  constexpr size_t kNumIterations = 20;
  for (size_t i = 0; i < kNumIterations; i++) {
    // +1 to account for this main thread.
    std::latch wait(/*expected=*/num_threads + 1);
    Notification exit;
    std::vector<std::unique_ptr<GhostThread>> threads;

    threads.reserve(num_threads);
    for (int j = 0; j < num_threads; j++) {
      threads.emplace_back(
          new GhostThread(GhostThread::KernelScheduler::kGhost, [&wait, &exit] {
            wait.arrive_and_wait();
            // Get the scheduler to open and commit txns frequently.
            while (!exit.HasBeenNotified()) {
              absl::SleepFor(absl::Nanoseconds(1));
            }
          }));
    }

    wait.arrive_and_wait();
    // Give the ghOSt threads time to wake up and the scheduler a moment to
    // start scheduling them.
    absl::SleepFor(absl::Milliseconds(10));
    for (auto& t : threads) {
      const sched_param param{};
      CHECK_EQ(sched_setscheduler(t->tid(), SCHED_OTHER, &param), 0);
      CHECK_EQ(sched_getscheduler(t->tid()), SCHED_OTHER);
    }
    // Wait a little while so that the race is hopefully triggered while we
    // wait.
    absl::SleepFor(absl::Milliseconds(10));
    exit.Notify();
    for (auto& t : threads) {
      t->Join();
    }
  }
}

}  // namespace
}  // namespace ghost

int main() {
  {
    printf("SimpleExp\n");
    ghost::ScopedTime time;
    ghost::SimpleExp();
  }
  {
    printf("SimpleExpMany\n");
    ghost::ScopedTime time;
    ghost::SimpleExpMany(1000);
  }
  {
    printf("BusyExp\n");
    ghost::ScopedTime time;
    ghost::BusyExpRunFor(100, absl::Milliseconds(10));
  }
  {
    printf("TaskDeparted\n");
    ghost::ScopedTime time;
    ghost::TaskDeparted();
  }
  {
    printf("TaskDepartedMany\n");
    ghost::ScopedTime time;
    ghost::TaskDepartedMany(1000);
  }
  {
    printf("TaskDepartedManyRace\n");
    ghost::ScopedTime time;
    ghost::TaskDepartedManyRace(1000);
  }
  return 0;
}
