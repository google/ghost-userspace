#include <sched.h>
#include <stdio.h>

#include <memory>
#include <vector>

#include "absl/flags/parse.h"
#include "lib/ghost.h"
#include "shared/prio_table.h"

// A series of simple tests for the EDF (Earliest Deadline First) scheduler.
// These tests allocate and use a PrioTable.

namespace ghost {
enum { kWcIdle, kWcOneShot, kWcRepeatable, kWcNum };

struct SimpleScopedTime {
  SimpleScopedTime() { start = absl::Now(); }
  ~SimpleScopedTime() {
    printf(" took %0.2f ms\n", absl::ToDoubleMilliseconds(absl::Now() - start));
  }
  absl::Time start;
};

bool sched_item_runnable(const std::unique_ptr<PrioTable> &table_, int sidx) {
  struct sched_item* src = table_->sched_item(sidx);
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
  struct sched_item* si = table_->sched_item(sidx);

  uint32_t seq = si->seqcount.write_begin();
  si->flags &= ~SCHED_ITEM_RUNNABLE;
  si->seqcount.write_end(seq);
  table_->MarkUpdatedIndex(sidx, /* num_retries = */ 3);
}

void mark_sched_item_runnable(const std::unique_ptr<PrioTable> &table_,
                              int sidx) {
  struct sched_item* si = table_->sched_item(sidx);

  uint32_t seq = si->seqcount.write_begin();
  si->flags |= SCHED_ITEM_RUNNABLE;
  si->seqcount.write_end(seq);
  table_->MarkUpdatedIndex(sidx, /* num_retries = */ 3);
}

void update_sched_item(const std::unique_ptr<PrioTable> &table_, uint32_t sidx,
                       uint32_t wcid, uint32_t flags, const Gtid& gtid,
                       absl::Duration d) {
  struct sched_item* si;

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
  struct work_class* wc;

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

void SimpleExp(const std::unique_ptr<PrioTable> &table_, uint32_t start_idx) {
  printf("\nStarting simple worker\n");
  GhostThread t(GhostThread::KernelScheduler::kGhost, [] {
    fprintf(stderr, "hello world!\n");
    absl::SleepFor(absl::Milliseconds(10));
    sched_yield();
    fprintf(stderr, "fantastic nap!\n");
  });
  update_sched_item(table_, start_idx, kWcOneShot, SCHED_ITEM_RUNNABLE,
                    t.gtid(), absl::Milliseconds(100));

  t.Join();
  printf("\nFinished simple worker\n");
}

void SimpleExpMany(const std::unique_ptr<PrioTable> &table_,
                   uint32_t num_threads, uint32_t start_idx) {
  std::vector<std::unique_ptr<GhostThread>> threads;
  threads.reserve(num_threads);

  for (int i = 0; i < num_threads; i++) {
    threads.emplace_back(new GhostThread(
        GhostThread::KernelScheduler::kGhost, [i, start_idx, &table_] {
          absl::SleepFor(absl::Milliseconds(10));
          sched_yield();
          absl::SleepFor(absl::Milliseconds(10));

          // Idle until the main thread makes us runnable again.
          mark_sched_item_idle(table_, start_idx + i);
          while (!sched_item_runnable(table_, start_idx + i)) {
          }
        }));
  }

  for (int i = 0; i < num_threads; i++) {
    auto &t = threads[i];
    update_sched_item(table_, start_idx + i, kWcOneShot, SCHED_ITEM_RUNNABLE,
                      t->gtid(), absl::Milliseconds(100));
  }

  for (int i = 0; i < num_threads; i++) {
    auto &t = threads[i];

    // Wait for thread to idle then let it run again so it can exit.
    while (sched_item_runnable(table_, start_idx + i)) {
    }

    mark_sched_item_runnable(table_, start_idx + i);
    t->Join();
  }

  printf("\nFinished SimpleExpMany worker\n");
}

void SimpleRepeatable(const std::unique_ptr<PrioTable> &table_,
                      uint32_t start_idx) {
  printf("\nStarting simple repeatable\n");
  GhostThread t(GhostThread::KernelScheduler::kGhost, [&] {
    for (int i = 0; i < 10; i++) {
      SpinFor(absl::Milliseconds(10));
      mark_sched_item_idle(table_, start_idx);
      while (!sched_item_runnable(table_, start_idx)) {
      }
    }
  });
  update_sched_item(table_, start_idx, kWcRepeatable, SCHED_ITEM_RUNNABLE,
                    t.gtid(), absl::Milliseconds(100));

  t.Join();
  printf("\nFinished simple repeatable\n");
}

void BusyExpRunFor(const std::unique_ptr<PrioTable> &table_,
                   uint32_t num_threads, absl::Duration d, uint32_t start_idx) {
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

  for (int i = 0; i < num_threads; i++) {
    auto &t = threads[i];
    update_sched_item(table_, start_idx + i, kWcOneShot, SCHED_ITEM_RUNNABLE,
                      t->gtid(), absl::Milliseconds(100));
  }

  for (int i = 0; i < num_threads; i++) {
    auto &t = threads[i];
    t->Join();
  }
  printf("\nFinished BusyExpMany worker\n");
}

void TaskDeparted(const std::unique_ptr<PrioTable> &table_,
                  uint32_t start_idx) {
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
  update_sched_item(table_, start_idx, kWcOneShot, SCHED_ITEM_RUNNABLE,
                    t.gtid(), absl::Milliseconds(100));

  t.Join();
  printf("\nFinished simple worker\n");
}

void TaskDepartedMany(const std::unique_ptr<PrioTable> &table_,
                      uint32_t num_threads, uint32_t start_idx) {
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

  for (int i = 0; i < num_threads; i++) {
    auto &t = threads[i];
    update_sched_item(table_, start_idx + i, kWcOneShot, SCHED_ITEM_RUNNABLE,
                      t->gtid(), absl::Milliseconds(100));
  }

  for (auto &t : threads) t->Join();
}

}  // namespace ghost

ABSL_FLAG(int32_t, spin_threads, 100, "Number of threads for the BusyExp test");
ABSL_FLAG(int32_t, spin_msec, 10, "Milliseconds to spin in the BusyExp test");

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);

  // Pin the main thread to cpu 0 so it doesn't interfere with the
  // global-agent (cpu 1) or satellite agents (cpus > 1).
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(0, &set);
  sched_setaffinity(0, sizeof(set), &set);
  const int kPrioMax = 51200;

  auto table_ = std::make_unique<ghost::PrioTable>(
      kPrioMax, ghost::kWcNum,
      ghost::PrioTable::StreamCapacity::kStreamCapacity19);
  setup_work_classes(table_);
  int start_idx = 0;
  int num_threads = 1;
  {
    printf("SimpleExp\n");
    ghost::SimpleScopedTime time;
    SimpleExp(table_, start_idx);
  }
  start_idx += num_threads;

  num_threads = 1000;
  {
    printf("SimpleExpMany\n");
    ghost::SimpleScopedTime time;
    SimpleExpMany(table_, num_threads, start_idx);
  }
  start_idx += num_threads;

  num_threads = 1;
  {
    printf("SimpleRepeatable\n");
    ghost::SimpleScopedTime time;
    SimpleRepeatable(table_, start_idx);
  }
  start_idx += num_threads;

  num_threads = absl::GetFlag(FLAGS_spin_threads);
  // Give the agent a chance to catch up on the messages from the previous
  // tests.  Otherwise if num_threads is too large, the agent's global channel
  // may overflow.  You can do about 20k with this sleep call, but not without
  // it.  Even with the sleep, 30k is too much.
  sleep(1);

  CHECK_LT(start_idx + num_threads, kPrioMax);

  int spin_ms = absl::GetFlag(FLAGS_spin_msec);

  {
    printf("BusyExp\n");
    ghost::SimpleScopedTime time;
    BusyExpRunFor(table_, num_threads, absl::Milliseconds(spin_ms), start_idx);
  }
  start_idx += num_threads;

  num_threads = 1;
  {
    printf("TaskDeparted\n");
    ghost::SimpleScopedTime time;
    TaskDeparted(table_, start_idx);
  }
  start_idx += num_threads;

  num_threads = 1000;
  {
    printf("TaskDepartedMany\n");
    ghost::SimpleScopedTime time;
    TaskDepartedMany(table_, num_threads, start_idx);
  }
  start_idx += num_threads;

  return 0;
}
