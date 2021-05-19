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

#include "lib/base.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/synchronization/notification.h"

// Tests `Notification`, `Futex`, and `Gtid`.

namespace ghost {
namespace {

using ::testing::Eq;
using ::testing::Ge;
using ::testing::IsEmpty;
using ::testing::IsTrue;

// Tests that `Notification` is constructed to be "unnotified" and that it
// successfully notifies after a call to `Notify`. This test is single-threaded.
TEST(NotificationTest, NotifyBeforeWait) {
  Notification n;

  EXPECT_FALSE(n.HasBeenNotified());
  n.Notify();
  EXPECT_TRUE(n.HasBeenNotified());
  n.WaitForNotification();
}

// Tests that two threads can notify each other.
TEST(NotificationTest, NotifyAfterWait) {
  Notification n1, n2;

  std::thread thread([&n1, &n2]() {
    n1.WaitForNotification();
    // Deliberately stall so we catch parent in WaitForNotification().
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    n2.Notify();
  });

  // Deliberately stall so we catch child in WaitForNotification().
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  n1.Notify();

  n2.WaitForNotification();
  thread.join();
}

// Tests that `num_threads` threads can wait on a futex with `Futex::Wait` for a
// type `T` and an initial value of `initial_val`. Then tests that all threads
// are woken up when a different thread writes `final_val` to the memory
// location and calls `Futex::Wake` with a count of `num_threads`. We use a
// template for the parameter types so that we can test the futex with an `int`
// type and an `enum class` type.
template <typename T>
void TestFutex(const T initial_val, const T final_val,
               const size_t num_threads) {
  std::atomic<T> val = initial_val;
  std::atomic<size_t> counter = 0;
  // Use `absl::Notification` since this file tests `ghost::Notification`.
  absl::Notification notification;

  std::vector<std::unique_ptr<std::thread>> threads;
  for (size_t i = 0; i < num_threads; i++) {
    threads.push_back(std::make_unique<std::thread>(
        [&val, &counter, num_threads, &notification, initial_val, final_val]() {
          if (++counter == num_threads) {
            notification.Notify();
          }
          Futex::Wait(&val, /*val=*/initial_val);
          EXPECT_THAT(val.load(std::memory_order_relaxed), Eq(final_val));
        }));
  }

  // Wait on a notification so that all threads are running when we update
  // `val`. This makes it more likely that the threads will actually wait on the
  // futex rather than return immediately from the futex wait with `EAGAIN`.
  notification.WaitForNotification();
  ASSERT_THAT(val.load(std::memory_order_relaxed), Eq(initial_val));
  val.store(final_val, std::memory_order_release);
  Futex::Wake(&val, /*count=*/num_threads);

  for (auto& t : threads) {
    t->join();
  }
}

// Tests that one thread can wait on a futex with `Futex::Wait` and is woken up
// when a different thread writes a different value to the memory location and
// calls `Futex::Wake` with a count of 1. The memory location is an int.
TEST(FutexTest, OneThreadInt) {
  TestFutex(/*initial_val=*/0, /*final_val=*/5, /*num_threads=*/1);
}

// Tests that one hundred threads can wait on a futex with `Futex::Wait` and are
// all woken up when a different thread writes a different value to the memory
// location and calls `Futex::Wake` with a count of 100. The memory location is
// an int.
TEST(FutexTest, OneHundredThreadsInt) {
  TestFutex(/*initial_val=*/0, /*final_val=*/5, /*num_threads=*/100);
}

// Tests that one thread can wait on a futex with `Futex::Wait` and is woken up
// when a different thread writes a different value to the memory location and
// calls `Futex::Wake` with a count of 1. The memory location is an enum class
// (and so it has a size equal to the size of int). Thus, this test exercises
// the futex's ability to handle a template.
TEST(FutexTest, OneThreadEnumClass) {
  enum class EnumClass {
    kInitial,
    kFinal,
  };

  TestFutex(/*initial_val=*/EnumClass::kInitial,
            /*final_val=*/EnumClass::kFinal, /*num_threads=*/1);
}

// Tests that one hundred threads can wait on a futex with `Futex::Wait` and are
// all woken up when a different thread writes a different value to the memory
// location and calls `Futex::Wake` with a count of 100. The memory location is
// an enum class (and so it has a size equal to the size of int). Thus, this
// test exercises the futex's ability to handle a template.
TEST(FutexTest, OneHundredThreadsEnumClass) {
  enum class EnumClass {
    kInitial,
    kFinal,
  };

  TestFutex(/*initial_val=*/EnumClass::kInitial,
            /*final_val=*/EnumClass::kFinal, /*num_threads=*/100);
}

// Tests that an atomic with an enum class type is lock free. If this test
// fails, then we know that ghOSt's performance will be adversely affected by
// the compiler since the compiler is inserting locks for atomic operations.
TEST(AtomicTest, LockFree) {
  enum class EnumClass {
    kInitial,
    kFinal,
  };

  std::atomic<EnumClass> val = EnumClass::kInitial;
  EXPECT_THAT(val.is_lock_free(), IsTrue());
}

// Tests that this thread's GTID returns a TID (thread identifier) that matches
// the true TID returned by a system call.
TEST(GtidTest, TidTest) { EXPECT_THAT(Gtid::Current().tid(), Eq(GetTID())); }

// Tests that two different threads have the same TGID (thread group
// identifier).
TEST(GtidTest, TgidThreadTest) {
  pid_t tgid = Gtid::Current().tgid();
  std::thread thread(
      [tgid]() { EXPECT_THAT(Gtid::Current().tgid(), Eq(tgid)); });
  thread.join();
}

// Tests that a GTID can generate a name and also have its name overwritten.
TEST(GtidTest, NameTest) {
  Gtid gtid = Gtid::Current();

  // A name should be automatically generated when one has not previously been
  // assigned.
  EXPECT_THAT(gtid.describe().size(), Ge(1));

  // We should be able to overwrite the name.
  gtid.assign_name("GtidTest");
  EXPECT_THAT(gtid.describe(), Eq("GtidTest"));

  // We should be able to overwrite the name more than once.
  gtid.assign_name("AnotherGtidTest");
  EXPECT_THAT(gtid.describe(), Eq("AnotherGtidTest"));

  // Even an empty name may be assigned to a GTID.
  gtid.assign_name("");
  EXPECT_THAT(gtid.describe(), IsEmpty());
}

}  // namespace
}  // namespace ghost
