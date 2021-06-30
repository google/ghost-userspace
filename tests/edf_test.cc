// Copyright 2021 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "schedulers/edf/edf_scheduler.h"

namespace ghost {
namespace {

enum class WorkClass { kWcIdle, kWcOneShot, kWcRepeatable, kWcNum };

struct SimpleScopedTime {
  SimpleScopedTime() { start = absl::Now(); }
  ~SimpleScopedTime() {
    printf(" took %0.2f ms\n", absl::ToDoubleMilliseconds(absl::Now() - start));
  }
  absl::Time start;
};

bool SchedItemRunnable(PrioTable* table, int sidx) {
  const struct sched_item* src = table->sched_item(sidx);
  uint32_t begin, flags;
  bool success = false;

  while (!success) {
    begin = src->seqcount.read_begin();
    flags = src->flags;
    success = src->seqcount.read_end(begin);
  }

  return flags & SCHED_ITEM_RUNNABLE;
}

void MarkSchedItemIdle(PrioTable* table, int sidx) {
  struct sched_item* si = table->sched_item(sidx);

  const uint32_t seq = si->seqcount.write_begin();
  si->flags &= ~SCHED_ITEM_RUNNABLE;
  si->seqcount.write_end(seq);
  table->MarkUpdatedIndex(sidx, /* num_retries = */ 3);
}

void MarkSchedItemRunnable(PrioTable* table, int sidx) {
  struct sched_item* si = table->sched_item(sidx);

  const uint32_t seq = si->seqcount.write_begin();
  si->flags |= SCHED_ITEM_RUNNABLE;
  si->seqcount.write_end(seq);
  table->MarkUpdatedIndex(sidx, /* num_retries = */ 3);
}

void UpdateSchedItem(PrioTable* table, uint32_t sidx, uint32_t wcid,
                     uint32_t flags, const Gtid& gtid, absl::Duration d) {
  struct sched_item* si;

  si = table->sched_item(sidx);

  const uint32_t seq = si->seqcount.write_begin();
  si->sid = sidx;
  si->wcid = wcid;
  si->flags = flags;
  si->gpid = gtid.id();
  si->deadline = absl::ToUnixNanos(MonotonicNow() + d);
  si->seqcount.write_end(seq);
  table->MarkUpdatedIndex(sidx, /* num_retries = */ 3);
}

void SetupWorkClasses(PrioTable* table) {
  struct work_class* wc;

  wc = table->work_class(static_cast<int>(WorkClass::kWcIdle));
  wc->id = static_cast<int>(WorkClass::kWcIdle);
  wc->flags = 0;
  wc->exectime = 0;

  wc = table->work_class(static_cast<int>(WorkClass::kWcOneShot));
  wc->id = static_cast<int>(WorkClass::kWcOneShot);
  wc->flags = WORK_CLASS_ONESHOT;
  wc->exectime = absl::ToInt64Nanoseconds(absl::Milliseconds(10));

  wc = table->work_class(static_cast<int>(WorkClass::kWcRepeatable));
  wc->id = static_cast<int>(WorkClass::kWcRepeatable);
  wc->flags = WORK_CLASS_REPEATING;
  wc->exectime = absl::ToInt64Nanoseconds(absl::Milliseconds(10));
  wc->period = absl::ToInt64Nanoseconds(absl::Milliseconds(100));
}

void SpinFor(absl::Duration d) {
  while (d > absl::ZeroDuration()) {
    absl::Time a = absl::Now();
    absl::Time b;

    // Try to minimize the contribution of arithmetic/Now() overhead.
    for (int i = 0; i < 150; ++i) {
      b = absl::Now();
    }

    absl::Duration t = b - a;

    // Don't count preempted time
    if (t < absl::Microseconds(100)) {
      d -= t;
    }
  }
}

class EdfTest : public testing::Test {
 protected:
  // SetUpTestSuite runs once for the entire test suite.  We don't use the usual
  // SetUp and TearDown.  This is because we fork off a single EDF AgentProcess
  // for the entire test suite, and SetUp/TearDown run on each TEST_F
  // invocation.
  static void SetUpTestSuite() {
    constexpr int kGlobalCpu = 1;
    Topology* t = MachineTopology();
    GlobalConfig cfg(t, t->all_cpus(), t->cpu(kGlobalCpu));

    uap_ = new AgentProcess<GlobalEdfAgent<LocalEnclave>, GlobalConfig>(cfg);
  }

  static void TearDownTestSuite() {
    delete uap_;
    uap_ = nullptr;
  }

  static AgentProcess<GlobalEdfAgent<LocalEnclave>, GlobalConfig>* uap_;

  void PostForkSetUp() {
    // Pin the main test thread to cpu 0 so it doesn't interfere with the
    // global-agent (cpu 1) or satellite agents (cpus > 1).
    constexpr int kTestCpu = 0;
    cpu_set_t set;

    CPU_ZERO(&set);
    CPU_SET(kTestCpu, &set);
    sched_setaffinity(0, sizeof(set), &set);

    table_ = absl::make_unique<PrioTable>(
        2000, static_cast<int>(WorkClass::kWcNum),
        PrioTable::StreamCapacity::kStreamCapacity19);
    SetupWorkClasses(table_.get());
  }

  std::unique_ptr<PrioTable> table_;
};

AgentProcess<GlobalEdfAgent<LocalEnclave>, GlobalConfig>* EdfTest::uap_;

TEST_F(EdfTest, Simple) {
  ForkedProcess fp([this]() {
    PostForkSetUp();

    GhostThread t(GhostThread::KernelScheduler::kGhost, [] {
      absl::SleepFor(absl::Milliseconds(10));
      sched_yield();
    });
    UpdateSchedItem(table_.get(), 0, static_cast<int>(WorkClass::kWcOneShot),
                    SCHED_ITEM_RUNNABLE, t.gtid(), absl::Milliseconds(100));

    t.Join();
    return 0;
  });

  ASSERT_EQ(fp.WaitForChildExit(), 0);
}

// If we keep the tests all in the same process, then this is a good check
TEST_F(EdfTest, SimpleAgain) {
  ForkedProcess fp([this]() {
    PostForkSetUp();

    GhostThread t(GhostThread::KernelScheduler::kGhost, [] {
      absl::SleepFor(absl::Milliseconds(10));
      sched_yield();
    });
    UpdateSchedItem(table_.get(), 0, static_cast<int>(WorkClass::kWcOneShot),
                    SCHED_ITEM_RUNNABLE, t.gtid(), absl::Milliseconds(100));

    t.Join();
    return 0;
  });

  ASSERT_EQ(fp.WaitForChildExit(), 0);
}

TEST_F(EdfTest, SimpleMany) {
  ForkedProcess fp([this]() {
    PostForkSetUp();

    constexpr int kNumThreads = 1000;
    std::vector<std::unique_ptr<GhostThread>> threads;
    threads.reserve(kNumThreads);

    for (int i = 0; i < kNumThreads; ++i) {
      threads.emplace_back(
          new GhostThread(GhostThread::KernelScheduler::kGhost, [i, this] {
            absl::SleepFor(absl::Milliseconds(10));
            sched_yield();
            absl::SleepFor(absl::Milliseconds(10));

            // Idle until the main thread makes us runnable again.
            MarkSchedItemIdle(table_.get(), i);
            while (!SchedItemRunnable(table_.get(), i)) {
            }
          }));
    }

    for (int i = 0; i < kNumThreads; ++i) {
      auto& t = threads[i];
      UpdateSchedItem(table_.get(), i, static_cast<int>(WorkClass::kWcOneShot),
                      SCHED_ITEM_RUNNABLE, t->gtid(), absl::Milliseconds(100));
    }

    for (int i = 0; i < kNumThreads; ++i) {
      auto& t = threads[i];

      // Wait for thread to idle then let it run again so it can exit.
      while (SchedItemRunnable(table_.get(), i)) {
      }

      MarkSchedItemRunnable(table_.get(), i);
      t->Join();
    }
    return 0;
  });
  ASSERT_EQ(fp.WaitForChildExit(), 0);
}

TEST_F(EdfTest, SimpleRepeatable) {
  ForkedProcess fp([this]() {
    PostForkSetUp();

    constexpr int kNumLoops = 10;
    const absl::Duration d = absl::Milliseconds(10);

    GhostThread t(GhostThread::KernelScheduler::kGhost, [&] {
      for (int i = 0; i < kNumLoops; ++i) {
        SpinFor(d);
        MarkSchedItemIdle(table_.get(), 0);
        while (!SchedItemRunnable(table_.get(), 0)) {
        }
      }
    });
    UpdateSchedItem(table_.get(), 0, static_cast<int>(WorkClass::kWcRepeatable),
                    SCHED_ITEM_RUNNABLE, t.gtid(), absl::Milliseconds(100));

    t.Join();

    return 0;
  });
  ASSERT_EQ(fp.WaitForChildExit(), 0);
}

TEST_F(EdfTest, BusyRunFor) {
  ForkedProcess fp([this]() {
    PostForkSetUp();

    constexpr int kNumThreads = 100;
    const absl::Duration d = absl::Milliseconds(10);

    std::vector<std::unique_ptr<GhostThread>> threads;
    threads.reserve(kNumThreads);

    for (int i = 0; i < kNumThreads; ++i) {
      threads.emplace_back(new GhostThread(GhostThread::KernelScheduler::kGhost,
                                           [&] { SpinFor(d); }));
    }

    for (int i = 0; i < kNumThreads; ++i) {
      auto& t = threads[i];
      UpdateSchedItem(table_.get(), i, static_cast<int>(WorkClass::kWcOneShot),
                      SCHED_ITEM_RUNNABLE, t->gtid(), absl::Milliseconds(100));
    }

    for (int i = 0; i < kNumThreads; ++i) {
      auto& t = threads[i];
      t->Join();
    }

    return 0;
  });
  ASSERT_EQ(fp.WaitForChildExit(), 0);
}

}  // namespace
}  // namespace ghost

int main(int argc, char **argv) {
  testing::InitGoogleMock(&argc, argv);

  if (ghost::MachineTopology()->num_cpus() < 2) {
    GTEST_MESSAGE_("", ::testing::TestPartResult::kSkip)
        << "must have at least 2 cpus";
    return 0;
  }

  return RUN_ALL_TESTS();
}
