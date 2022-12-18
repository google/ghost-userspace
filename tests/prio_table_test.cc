// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "shared/prio_table.h"

#include "gtest/gtest.h"

namespace ghost {

TEST(PrioTableTest, Owner) {
  PrioTable _client_table(/*num_items=*/10, /*num_classes=*/4,
                          PrioTable::StreamCapacity::kStreamCapacity19);
  EXPECT_EQ(_client_table.Owner(), getpid());

  // Uninitialized PrioTable does not have an owner.
  PrioTable table;
  EXPECT_EQ(table.Owner(), 0);

  // PrioTable does not have an owner after failing to attach.
  EXPECT_EQ(table.Attach(/*remote=*/0), false);
  EXPECT_EQ(table.Owner(), 0);

  // PrioTable must have an expected owner after a successful attach.
  EXPECT_EQ(table.Attach(getpid()), true);
  EXPECT_EQ(table.Owner(), getpid());
}

TEST(PrioTableTest, SimpleEnqueueThenDeqeue) {
  static const int kIdx = 5;
  PrioTable table(10, 4, PrioTable::StreamCapacity::kStreamCapacity19);

  ASSERT_EQ(table.NextUpdatedIndex(), PrioTable::kStreamNoEntries);
  table.MarkUpdatedIndex(kIdx, /* num_retries = */ 0);
  ASSERT_EQ(table.NextUpdatedIndex(), kIdx);
  ASSERT_EQ(table.NextUpdatedIndex(), PrioTable::kStreamNoEntries);
}

TEST(PrioTableTest, CapacityOverflow) {
  PrioTable table(10, 4, PrioTable::StreamCapacity::kStreamCapacity19);

  ASSERT_EQ(table.NextUpdatedIndex(), PrioTable::kStreamNoEntries);
  for (int i = 0; i < table.hdr()->st_cap + 1; i++) {
    table.MarkUpdatedIndex(/* idx = */ i, /* num_retries = */ 0);
  }
  ASSERT_EQ(table.NextUpdatedIndex(), PrioTable::kStreamOverflow);
  ASSERT_EQ(table.NextUpdatedIndex(), PrioTable::kStreamNoEntries);

  // Make sure the stream stops reporting an overflow
  table.MarkUpdatedIndex(/* idx = */ 5, /* num_retries = */ 0);
  ASSERT_EQ(table.NextUpdatedIndex(), 5);
  ASSERT_EQ(table.NextUpdatedIndex(), PrioTable::kStreamNoEntries);
}

TEST(PrioTableTest, ContentionOverflow) {
  PrioTable table(10, 4, PrioTable::StreamCapacity::kStreamCapacity19);

  ASSERT_EQ(table.NextUpdatedIndex(), PrioTable::kStreamNoEntries);
  table.MarkUpdatedIndex(/* idx = */ 0, /* num_retries = */ 0);
  // 'table.hdr()->st_cap' (i.e., the stream capacity) should be mapped to the
  // same index in the stream as '0', so since 'numRetries' is set to '0' there
  // will be overflow
  table.MarkUpdatedIndex(/* idx = */ table.hdr()->st_cap,
                         /* num_retries = */ 0);
  ASSERT_EQ(table.NextUpdatedIndex(), PrioTable::kStreamOverflow);
  ASSERT_EQ(table.NextUpdatedIndex(), PrioTable::kStreamNoEntries);

  // Make sure the stream stops reporting an overflow
  table.MarkUpdatedIndex(/* idx = */ 0, /* num_retries = */ 0);
  ASSERT_EQ(table.NextUpdatedIndex(), 0);
  ASSERT_EQ(table.NextUpdatedIndex(), PrioTable::kStreamNoEntries);
}

TEST(PrioTableTest, StressThreads) {
  static const int kNumIterations = 1000;
  static const int kNumThreads = 10;
  static const int kIdx = 0;
  static const int kNumRetries = kNumThreads - 1;
  PrioTable table(10, 4, PrioTable::StreamCapacity::kStreamCapacity19);
  std::vector<std::thread> threads;
  std::atomic<bool> test[kNumThreads];

  // We don't want to miss out on atomic contention by having overflows, so
  // ensure the number of threads is less than or equal to
  // 'table.hdr()->st_cap', which is the stream capacity
  ASSERT_LE(kNumThreads, table.hdr()->st_cap);

  for (int i = 0; i < kNumThreads; i++) {
    threads.emplace_back(std::thread([&table, i, &test]() {
      for (int j = 0; j < kNumIterations; j++) {
        test[i].store(true, std::memory_order_relaxed);
        table.MarkUpdatedIndex(kIdx, kNumRetries);
        while (test[i].load(std::memory_order_relaxed)) {
        }
      }
    }));
  }

  for (int j = 0; j < kNumIterations; j++) {
    for (int i = 0; i < kNumThreads; i++) {
      int next;
      while ((next = table.NextUpdatedIndex()) == PrioTable::kStreamNoEntries) {
      }
      ASSERT_EQ(next, kIdx);
    }
    for (int i = 0; i < kNumThreads; i++) {
      ASSERT_TRUE(test[i].load(std::memory_order_relaxed));
      test[i].store(false, std::memory_order_relaxed);
    }
  }

  ASSERT_EQ(table.NextUpdatedIndex(), PrioTable::kStreamNoEntries);
  for (int i = 0; i < kNumThreads; i++) {
    threads[i].join();
  }
}

TEST(PrioTableTest, StressOrdering) {
  static const int kNumIterations = 1000;
  static const int kIdx = 0;
  PrioTable table(10, 4, PrioTable::StreamCapacity::kStreamCapacity19);
  std::atomic<bool> test = false;

  std::thread thread([&table, &test]() {
    for (int j = 0; j < kNumIterations; j++) {
      test.store(true, std::memory_order_relaxed);
      table.MarkUpdatedIndex(kIdx, 0);
      while (test.load(std::memory_order_relaxed)) {
      }
    }
  });

  for (int j = 0; j < kNumIterations; j++) {
    int next;
    while ((next = table.NextUpdatedIndex()) == PrioTable::kStreamNoEntries) {
    }
    ASSERT_EQ(next, kIdx);
    ASSERT_TRUE(test.load(std::memory_order_relaxed));
    test.store(false, std::memory_order_relaxed);
  }

  ASSERT_EQ(table.NextUpdatedIndex(), PrioTable::kStreamNoEntries);
  thread.join();
}

}  // namespace ghost
