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

#include "shared/prio_table.h"

#include "gtest/gtest.h"

namespace {

TEST(PrioTableTest, SimpleEnqueueThenDeqeue) {
  static const int kIdx = 5;
  ghost::PrioTable table(10, 4,
                         ghost::PrioTable::StreamCapacity::kStreamCapacity19);

  ASSERT_EQ(table.NextUpdatedIndex(), ghost::PrioTable::kStreamNoEntries);
  table.MarkUpdatedIndex(kIdx, /* num_retries = */ 0);
  ASSERT_EQ(table.NextUpdatedIndex(), kIdx);
  ASSERT_EQ(table.NextUpdatedIndex(), ghost::PrioTable::kStreamNoEntries);
}

TEST(PrioTableTest, CapacityOverflow) {
  ghost::PrioTable table(10, 4,
                         ghost::PrioTable::StreamCapacity::kStreamCapacity19);

  ASSERT_EQ(table.NextUpdatedIndex(), ghost::PrioTable::kStreamNoEntries);
  for (int i = 0; i < table.hdr()->st_cap + 1; i++) {
    table.MarkUpdatedIndex(/* idx = */ i, /* num_retries = */ 0);
  }
  ASSERT_EQ(table.NextUpdatedIndex(), ghost::PrioTable::kStreamOverflow);
  ASSERT_EQ(table.NextUpdatedIndex(), ghost::PrioTable::kStreamNoEntries);

  // Make sure the stream stops reporting an overflow
  table.MarkUpdatedIndex(/* idx = */ 5, /* num_retries = */ 0);
  ASSERT_EQ(table.NextUpdatedIndex(), 5);
  ASSERT_EQ(table.NextUpdatedIndex(), ghost::PrioTable::kStreamNoEntries);
}

TEST(PrioTableTest, ContentionOverflow) {
  ghost::PrioTable table(10, 4,
                         ghost::PrioTable::StreamCapacity::kStreamCapacity19);

  ASSERT_EQ(table.NextUpdatedIndex(), ghost::PrioTable::kStreamNoEntries);
  table.MarkUpdatedIndex(/* idx = */ 0, /* num_retries = */ 0);
  // 'table.hdr()->st_cap' (i.e., the stream capacity) should be mapped to the
  // same index in the stream as '0', so since 'numRetries' is set to '0' there
  // will be overflow
  table.MarkUpdatedIndex(/* idx = */ table.hdr()->st_cap,
                         /* num_retries = */ 0);
  ASSERT_EQ(table.NextUpdatedIndex(), ghost::PrioTable::kStreamOverflow);
  ASSERT_EQ(table.NextUpdatedIndex(), ghost::PrioTable::kStreamNoEntries);

  // Make sure the stream stops reporting an overflow
  table.MarkUpdatedIndex(/* idx = */ 0, /* num_retries = */ 0);
  ASSERT_EQ(table.NextUpdatedIndex(), 0);
  ASSERT_EQ(table.NextUpdatedIndex(), ghost::PrioTable::kStreamNoEntries);
}

TEST(PrioTableTest, StressThreads) {
  static const int kNumIterations = 1000;
  static const int kNumThreads = 10;
  static const int kIdx = 0;
  static const int kNumRetries = kNumThreads - 1;
  ghost::PrioTable table(10, 4,
                         ghost::PrioTable::StreamCapacity::kStreamCapacity19);
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
      while ((next = table.NextUpdatedIndex()) ==
             ghost::PrioTable::kStreamNoEntries) {
      }
      ASSERT_EQ(next, kIdx);
    }
    for (int i = 0; i < kNumThreads; i++) {
      ASSERT_TRUE(test[i].load(std::memory_order_relaxed));
      test[i].store(false, std::memory_order_relaxed);
    }
  }

  ASSERT_EQ(table.NextUpdatedIndex(), ghost::PrioTable::kStreamNoEntries);
  for (int i = 0; i < kNumThreads; i++) {
    threads[i].join();
  }
}

TEST(PrioTableTest, StressOrdering) {
  static const int kNumIterations = 1000;
  static const int kIdx = 0;
  ghost::PrioTable table(10, 4,
                         ghost::PrioTable::StreamCapacity::kStreamCapacity19);
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
    while ((next = table.NextUpdatedIndex()) ==
           ghost::PrioTable::kStreamNoEntries) {
    }
    ASSERT_EQ(next, kIdx);
    ASSERT_TRUE(test.load(std::memory_order_relaxed));
    test.store(false, std::memory_order_relaxed);
  }

  ASSERT_EQ(table.NextUpdatedIndex(), ghost::PrioTable::kStreamNoEntries);
  thread.join();
}

}  // namespace
