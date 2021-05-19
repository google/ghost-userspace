/*
 * Copyright 2021 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef GHOST_TESTS_CAPABILITIES_TEST_H_
#define GHOST_TESTS_CAPABILITIES_TEST_H_

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <sys/capability.h>

using ::testing::Eq;
using ::testing::IsFalse;
using ::testing::IsTrue;
using ::testing::NotNull;

// Privileged ghOSt syscalls may only be used by threads with the `CAP_SYS_NICE`
// capability.
constexpr cap_value_t kGhostCapability = CAP_SYS_NICE;

// Sets `is_set` to true if the calling thread has the `CAP_SYS_NICE` capability
// in its effective set. Sets `is_set` to false otherwise.
//
// Note that we pass `is_set` by reference rather than return a boolean so that
// we can use `ASSERT_THAT` and `EXPECT_THAT` macros in this function. These
// macros only work in functions with void return types.
void NiceCapabilitySet(bool& is_set) {
  cap_t current = cap_get_proc();
  ASSERT_THAT(current, NotNull());
  cap_flag_value_t flag_value;
  ASSERT_THAT(
      cap_get_flag(current, kGhostCapability, CAP_EFFECTIVE, &flag_value),
      Eq(0));
  EXPECT_THAT(cap_free(current), Eq(0));
  is_set = (flag_value == CAP_SET);
}

// Asserts that the `CAP_SYS_NICE` capability is set.
void AssertNiceCapabilitySet() {
  bool is_set = false;
  NiceCapabilitySet(is_set);
  ASSERT_THAT(is_set, IsTrue());
}

// Asserts that the `CAP_SYS_NICE` capability is not set.
void AssertNiceCapabilityNotSet() {
  bool is_set = true;
  NiceCapabilitySet(is_set);
  ASSERT_THAT(is_set, IsFalse());
}

// Drops the `CAP_SYS_NICE` capability from the calling thread's effective set.
// Note that the calling thread must already hold the `CAP_SYS_NICE` capability
// when it calls this function.
void DropNiceCapability() {
  AssertNiceCapabilitySet();

  cap_t current = cap_get_proc();
  ASSERT_THAT(current, NotNull());
  const cap_value_t cap_array[] = {kGhostCapability};
  ASSERT_THAT(
      cap_set_flag(current, CAP_EFFECTIVE, /*ncaps=*/1, cap_array, CAP_CLEAR),
      Eq(0));
  ASSERT_THAT(cap_set_proc(current), Eq(0));
  EXPECT_THAT(cap_free(current), Eq(0));

  AssertNiceCapabilityNotSet();
}

#endif  // GHOST_TESTS_CAPABILITIES_TEST_H_
