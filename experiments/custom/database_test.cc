// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "experiments/rocksdb/database.h"

#include <filesystem>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"

// These tests check that the database returns expected values for entries that
// exist and returns failures for entries that do not exist.

ABSL_FLAG(std::string, test_tmpdir, "/tmp",
          "A temporary file system directory that the test can access");

namespace ghost_test {
namespace {

using ::testing::Eq;
using ::testing::IsFalse;
using ::testing::IsTrue;

// Returns the path to where the database should be created. The location at
// this path is in the test tmp directory.
std::filesystem::path GetDatabasePath() {
  return std::filesystem::path(absl::GetFlag(FLAGS_test_tmpdir)) / "orch_db";
}

// Tests that the database returns an error when we attempt to get the value for
// an entry that does not exist.
TEST(DatabaseTest, GetNoEntry) {
  Database database(GetDatabasePath());
  std::string get_value;
  EXPECT_THAT(database.Get(/*entry=*/Database::kNumEntries, get_value),
              IsFalse());
}

// Tests that the database returns the expected value for an entry that exists.
TEST(DatabaseTest, Get) {
  Database database(GetDatabasePath());
  std::string get_value;
  EXPECT_THAT(database.Get(/*entry=*/5, get_value), IsTrue());
  EXPECT_THAT(get_value, Eq("value0000000000000005"));
}

// Tests that the database returns an error when we attempt to get the values
// for entries in a range such that the first entry does not exist.
TEST(DatabaseTest, RangeNoStartEntry) {
  Database database(GetDatabasePath());
  std::string range_value;
  EXPECT_THAT(database.RangeQuery(/*start_entry=*/Database::kNumEntries,
                                  /*range_size=*/10, range_value),
              IsFalse());
}

// Tests that the database returns an error when we attempt to get the values
// for entries in a range such that the first few entries exist but a middle
// entry does not.
TEST(DatabaseTest, RangeNoMiddleEntry) {
  constexpr uint32_t kRangeSize = 10;
  Database database(GetDatabasePath());
  std::string range_value;
  EXPECT_THAT(database.RangeQuery(
                  /*start_entry=*/Database::kNumEntries - (kRangeSize / 2),
                  kRangeSize, range_value),
              IsFalse());
}

// Tests that the database returns an error when we attempt to get the values
// for entries in a range such that all entries exist except for the last one.
TEST(DatabaseTest, RangeNoEndEntry) {
  constexpr uint32_t kRangeSize = 10;
  Database database(GetDatabasePath());
  std::string range_value;
  EXPECT_THAT(database.RangeQuery(
                  /*start_entry=*/Database::kNumEntries - kRangeSize + 1,
                  kRangeSize, range_value),
              IsFalse());
}

// Tests that the database returns the expected value for a range query that
// accesses a single valid entry.
TEST(DatabaseTest, RangeSizeOne) {
  Database database(GetDatabasePath());
  std::string range_value;
  EXPECT_THAT(
      database.RangeQuery(/*start_entry=*/5, /*range_size=*/1, range_value),
      IsTrue());
  EXPECT_THAT(range_value, Eq("value0000000000000005"));
}

// Tests that the database returns the expected value for a range query that
// accesses multiple entries. All entries are valid.
TEST(DatabaseTest, Range) {
  Database database(GetDatabasePath());
  std::string range_value;
  EXPECT_THAT(
      database.RangeQuery(/*start_entry=*/5, /*range_size=*/10, range_value),
      IsTrue());

  std::string expected =
      "value0000000000000005,value0000000000000006,value0000000000000007,"
      "value0000000000000008,value0000000000000009,value0000000000000010,"
      "value0000000000000011,value0000000000000012,value0000000000000013,"
      "value0000000000000014";
  EXPECT_THAT(range_value, Eq(expected));
}

}  // namespace
}  // namespace ghost_test

int main(int argc, char **argv) {
  absl::ParseCommandLine(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
