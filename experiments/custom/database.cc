// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "experiments/rocksdb/database.h"

#include <sstream>

#include "rocksdb/table.h"

namespace ghost_test {

bool Database::OpenDatabase(const std::filesystem::path& path) {
  rocksdb::Options options;
  options.create_if_missing = true;
  options.allow_mmap_reads = true;
  options.allow_mmap_writes = true;
  options.error_if_exists = false;

  rocksdb::BlockBasedTableOptions table_options;
  // Use a ClockCache as the default LRU cache requires locking a per-shard
  // mutex, even on lookups. Using a ClockCache improves lookup throughput as a
  // mutex is only acquired on inserts.
  table_options.block_cache = rocksdb::NewClockCache(kCacheSize, 0);
  CHECK_NE(table_options.block_cache, nullptr);
  options.table_factory.reset(
      rocksdb::NewBlockBasedTableFactory(table_options));

  options.compression = rocksdb::kNoCompression;
  options.OptimizeLevelStyleCompaction();
  rocksdb::Status status = rocksdb::DB::Open(options, path.string(), &db_);
  return status.ok();
}

Database::Database(const std::filesystem::path& path) {
  if (!OpenDatabase(path)) {
    // The database is corrupted.
    CHECK(std::filesystem::exists(path));
    CHECK_GT(std::filesystem::remove_all(path), 0);
    CHECK(OpenDatabase(path));
  }
  CHECK(Fill());
  PrepopulateCache();
}

Database::~Database() { delete db_; }

bool Database::Fill() {
  for (uint32_t i = 0; i < kNumEntries; i++) {
    rocksdb::Status status =
        db_->Put(rocksdb::WriteOptions(), Key(i), Value(i));
    if (!status.ok()) {
      return false;
    }
  }
  return true;
}

void Database::PrepopulateCache() const {
  std::string value;
  for (int i = 0; i < kNumEntries; i++) {
    CHECK(Get(i, value));
  }
}

bool Database::Get(uint32_t entry, std::string& value) const {
  rocksdb::Status status = db_->Get(rocksdb::ReadOptions(), Key(entry), &value);
  if (status.ok()) {
    CHECK_EQ(value, Value(entry));
    return true;
  }
  return false;
}

bool Database::RangeQuery(uint32_t start_entry, uint32_t range_size,
                          std::string& value) const {
  std::stringstream ss;
  std::unique_ptr<rocksdb::Iterator> it(
      db_->NewIterator(rocksdb::ReadOptions()));
  it->Seek(Key(start_entry));

  for (uint32_t i = 0; i < range_size; i++) {
    if (!it->Valid()) {
      return false;
    }
    CHECK_EQ(it->value().ToString(), Value(start_entry + i));
    ss << it->value().ToString();
    if (i < range_size - 1) {
      ss << ",";
    }
    it->Next();
  }
  value = ss.str();
  return true;
}

}  // namespace ghost_test
