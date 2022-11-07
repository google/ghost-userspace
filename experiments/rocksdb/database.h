// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef GHOST_EXPERIMENTS_ROCKSDB_DATABASE_H_
#define GHOST_EXPERIMENTS_ROCKSDB_DATABASE_H_

#include <filesystem>

#include "lib/base.h"
#include "rocksdb/db.h"

namespace ghost_test {

// This class creates a RocksDB database and moderates access to it. Each entry
// in the database is a number 'x' and its associated value is a string
// 'value0...0x'. For example, '5' is an entry is its associated value is
// 'value0000000000000005'.
//
// Example:
// Database database("/tmp/orch_db");
//
// std::string get_value;
// if (Get(5, get_value)) {
//   ('get_value' is equal to 'value0000000000000005'.)
// } else {
//   (Entry '5' does not exist in the database.)
// }
//
// std::string range_value;
// if (RangeQuery(5, 3, range_value)) {
//   ('range_value' is equal to
//   'value0000000000000005value0000000000000006value0000000000000007').
// } else {
//   (One or more of the entries in the range query do not exist in the
//   database.)
// }
class Database {
 public:
  explicit Database(const std::filesystem::path& path);
  ~Database();

  // Gets the value for key 'entry'. On success, returns true and stores the
  // value at the memory location pointed to by 'value'. On failure, returns
  // false; the value stored at memory location 'value' is undefined.
  bool Get(uint32_t entry, std::string& value) const;
  // Gets the values in the range starting at key 'start_entry' and extending
  // for length 'range_size'. On success, returns true and populates the 'value'
  // string with the values. On failure, returns false; the value stored in the
  // 'value' string is undefined.
  bool RangeQuery(uint32_t start_entry, uint32_t range_size,
                  std::string& value) const;

  // The number of entries in the database.
  static constexpr uint32_t kNumEntries = 1'000'000;

 private:
  // Opens the RocksDB database at 'path' (if it exists) or creates a new
  // RocksDB database at 'path' (if no database exists there yet).
  bool OpenDatabase(const std::filesystem::path& path);

  // Fills the database with 'kNumEntries' key/value pairs. Starts with entry 0
  // and goes up to entry 'kNumEntries - 1'. Generates the keys and values by
  // passing entry nunbers to 'Key()' and 'Value()'.
  bool Fill();

  // Accesses all key/values pairs in the database so that they are added to the
  // RocksDB cache. Since we access key/value pairs randomly in the RocksDB
  // tests, calling this method on test initialization helps ensure that we are
  // doing a fair comparison between ghOSt and CFS rather than making each deal
  // with a unique set of faults.
  void PrepopulateCache() const;

  // Converts the entry number to a formatted string. The formatted string is
  // padded with zeroes in the front until the string length is equal to
  // 'kNumLength'. See the comment below for 'kNumLength' for details.
  static std::string to_string(uint32_t entry) {
    std::string s = std::to_string(entry);
    CHECK(s.size() <= kNumLength);
    return std::string(kNumLength - s.size(), '0') + s;
  }

  // Returns the key string for 'entry'.
  static std::string Key(uint32_t entry) { return "key" + to_string(entry); }

  // Returns the value string for 'entry'.
  static std::string Value(uint32_t entry) {
    return "value" + to_string(entry);
  }

  // The RocksDB database. Note that the test likely wants to store the entire
  // database in memory backed by hugepages. If either of those two cases does
  // not hold, it is impossible to have microsecond-scale tail latencies.
  // Accessing the disk and handling page faults each takes at least 1
  // millisecond, which harms the latency for request that triggers the
  // disk/page fault and harms subsequent requests stuck waiting in the queue.
  // We can store the entire database in memory backed by hugepages by
  // passing a path to a hugepage-backed tmpfs mount to the constructor.
  rocksdb::DB* db_;
  // The length of numbers (in digits) in keys and values must be equal to
  // 'kNumLength'. For example, if 'kNumLength' == 16, then the number 543 would
  // be represented as 'key0000000000000543' and 'value0000000000000543'. This
  // makes iterating through the key/value pairs in a range query easier. If all
  // numbers are not the same length, then 'key543' is followed by 'key5430',
  // even though we would intuitively expect it to be followed by 'key544'.
  // Being able to iterate through keys in this way makes it easier to calculate
  // random ranges we can iterate through without iterating past the last key in
  // the database.
  static constexpr uint32_t kNumLength = 16;
  // The size of the RocksDB cache. Currently set to 1 GiB.
  static constexpr size_t kCacheSize = 1 * 1024 * 1024 * 1024LL;
};

}  // namespace ghost_test

#endif  // GHOST_EXPERIMENTS_ROCKSDB_DATABASE_H_
