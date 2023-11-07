// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "topology.h"

#include <stdio.h>

#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>

#include "absl/strings/numbers.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
#include <numa.h>

namespace ghost {

namespace {

bool CheckConsecutiveSmtNumbering(const Topology& topology) {
  const CpuList& siblings = topology.cpu(0).siblings();
  if (siblings.Size() != 2) {
    CHECK_EQ(siblings.Size(), 1);
    return false;
  }

  int sibling_offset = abs(siblings[0].id() - siblings[1].id());
  if (sibling_offset == 1) {
    return true;
  }

  // BPF topology helpers assume a standard core offset if we don't have
  // consecutive siblings.
  CHECK_EQ(sibling_offset, topology.all_cores().Size());
  return false;
}

}  // namespace

CpuMap::CpuMap(const Topology& topology)
    : topology_(&topology),
      map_size_((topology.num_cpus() + kIntsBits - 1) / kIntsBits) {
  static_assert(MAX_CPUS % kIntsBits == 0);
  CHECK_LE(map_size_, kMapCapacity);
}

Cpu CpuList::GetNthCpu(uint32_t n) const {
  for (uint32_t i = 0; i < map_size_; i++) {
    uint64_t word = GetMapConst()[i];
    int count = absl::popcount(word);
    // `n` is zero-indexed, so if `count` == `n`, then the `n`th bit set is
    // not in this word.
    if (count <= n) {
      n -= count;
    } else {
      // The `n`th set bit is in this word.

      // Clears the least significant `n` bits in `word`.
      for (uint32_t j = 0; j < n; j++) {
        word &= word - 1;
      }
      return topology_->cpu(i * kIntsBits + __builtin_ctzll(word));
    }
  }
  return Cpu(Cpu::UninitializedType::kUninitialized);
}

void CpuMap::Iter::FindNextSetBit() {
  uint32_t map_idx = id_ / kIntsBits;
  const size_t bit_offset = id_ & (kIntsBits - 1);
  uint64_t word = 0;

  if (map_idx >= map_->map_size_) {
    // We are already past the end of the bitmap, so skip to the end to
    // avoid an out-of-bounds access to `bitmap_` below.
    goto end;
  }
  // Reset all LSBs seen so far. Note that since `kIntsBits` is a power of
  // 2, the AND operation below is equivalent to `id_ % kIntsBits`.
  word = map_->GetNthMap(map_idx);
  word &= ~0ULL << bit_offset;
  while (map_idx < map_->map_size_) {
    if (word) {
      id_ = map_idx * kIntsBits + __builtin_ctzll(word);
      cpu_ = map_->topology().cpu(id_);
      return;
    }
    if (++map_idx < map_->map_size_) {
      word = map_->GetNthMap(map_idx);
    }
  }

end:
  // Since there are no more bits set, then fast forward to the `end` Iter
  // to bail out of range based for-loops.
  id_ = map_->topology().num_cpus();
  cpu_ = Cpu(Cpu::UninitializedType::kUninitialized);
}

CpuMap::Iter CpuMap::begin() const {
  return Iter(this, /*id=*/0);
}

CpuMap::Iter CpuMap::end() const {
  return Iter(this, /*id=*/topology_->num_cpus());
}

absl::flat_hash_map<int, CpuList> Topology::GetAllSiblings(
    const std::filesystem::path& path_prefix,
    const std::string path_suffix) const {
  absl::flat_hash_map<std::string, std::vector<int>> sibling_map;

  for (uint32_t cpu = 0; cpu < num_cpus_; cpu++) {
    std::filesystem::path path =
        path_prefix / absl::StrFormat("cpu%d", cpu) / path_suffix;
    std::ifstream stream(path);
    std::string sibling_list;
    std::getline(stream, sibling_list);
    if (sibling_list.empty()) {
      // We should detect that there are no siblings on the first iteration.
      CHECK_EQ(cpu, 0);
      break;
    }

    CHECK(!sibling_list.empty());
    // All siblings share the same sibling list as exported by the kernel in
    // sysfs. The siblings list may be exported in hexadecimal notation,
    // range list or a comma separated list depending on the `path_suffix` file.
    // e.g. if CPUs 0 and 56 are siblings, then the `thread_siblings` file
    // displays `0000,00000000,01000000,00000001` and the `thread_siblings_list`
    // file displays `0,56`. We can group all siblings together by using the
    // contents from `path_suffix` file as a key into the `sibling_map`.
    sibling_map[sibling_list].push_back(cpu);
  }

  absl::flat_hash_map<int, CpuList> siblings;
  for (const auto& [sibling_list, cpus] : sibling_map) {
    for (int cpu : cpus) {
      CHECK(siblings.insert({cpu, ToCpuList(cpus)}).second);
    }
  }
  return siblings;
}

int Topology::GetHighestNodeIdx(const std::filesystem::path& path) const {
  std::ifstream f(path);
  CHECK(f.is_open());
  std::string node_possible_str;
  std::getline(f, node_possible_str);
  CHECK(!f.fail());

  int highest_idx = 0;
  for (absl::string_view node_idx_s :
       absl::StrSplit(node_possible_str, absl::ByAnyChar(",-"))) {
    int node_idx;
    CHECK(absl::SimpleAtoi(node_idx_s, &node_idx));
    highest_idx = std::max(highest_idx, node_idx);
  }

  return highest_idx;
}

Topology::Topology(InitHost) : num_cpus_(std::thread::hardware_concurrency()) {
  // Consumers assume MAX_CPUS invariant.
  CHECK_LE(num_cpus_, MAX_CPUS);

  cpus_.resize(num_cpus_);

  // Fill in the CPU IDs first so that they can be accessed out-of-order when
  // filling in the siblings and the core below.
  for (int i = 0; i < num_cpus_; i++) {
    Cpu::CpuRep* rep = &cpus_[i];
    rep->cpu = i;
  }

  absl::flat_hash_map<int, CpuList> siblings =
      GetAllSiblings("/sys/devices/system/cpu", "topology/thread_siblings");
  CHECK_EQ(siblings.size(), num_cpus_);

  absl::flat_hash_map<int, CpuList> l3_siblings =
      GetAllSiblings("/sys/devices/system/cpu", "cache/index3/shared_cpu_list");
  // Not all microarchitectures have an L3 cache, so only do the CHECK below if
  // L3 siblings are found.
  if (!l3_siblings.empty()) {
    CHECK_EQ(l3_siblings.size(), num_cpus_);
  }
  for (int i = 0; i < num_cpus_; i++) {
    Cpu::CpuRep* rep = &cpus_[i];

    CHECK(siblings.find(i) != siblings.end());
    rep->siblings = std::make_unique<CpuList>(*this);
    *rep->siblings = siblings.find(i)->second;

    // As mentioned above, not all microarchitectures have an L3 cache, so there
    // may be no L3 siblings. Start by initializing `rep->l3_siblings` to an
    // empty CPU list and only fill in the list if there are L3 siblings.
    rep->l3_siblings = std::make_unique<CpuList>(*this);
    if (!l3_siblings.empty()) {
      CHECK(l3_siblings.find(i) != l3_siblings.end());
      *rep->l3_siblings = l3_siblings.find(i)->second;
    }

    rep->core = rep->siblings->begin()->id();
    rep->numa_node = numa_node_of_cpu(rep->cpu);
    CHECK_GE(rep->numa_node, 0);
    // smt_idx requires mapping a rep->cpu to a Core local index.
    // Range-based for loop like (const Cpu& c : rep->siblings) won't work
    // since it internally requires an initialized MachineTopology() which
    // is not yet set up here.
    int idx = -1;
    for (int c = 0; c < MAX_CPUS; ++c) {
      if (rep->siblings->IsSet(c)) {
        idx++;
        if (c == rep->cpu) break;
      }
    }
    CHECK_GE(idx, 0);
    rep->smt_idx = idx;
    all_cpus_.Set(i);
  }

  // Calculate the node with highest idx. This can in turn be used to tell how
  // many nodes are in the system (ignoring hotplug).
  highest_node_idx_ = GetHighestNodeIdx("/sys/devices/system/node/possible");
  CheckSiblings();
  CreateCpuListsForNumaNodes(highest_node_idx_ + 1);
  consecutive_smt_numbering_ = CheckConsecutiveSmtNumbering(*this);
}

void Topology::CreateTestSibling(
    int cpu, const std::filesystem::path& topology_test_directory,
    const std::string& kernel_mapping, const std::string& dir_path,
    const std::string& file_name) const {
  std::filesystem::path path =
      topology_test_directory / absl::StrFormat("cpu%d", cpu) / dir_path;
  // Create the directory if it does not yet exist.
  CHECK(std::filesystem::create_directories(path) ||
        (std::filesystem::exists(path) && std::filesystem::is_directory(path)));
  path /= file_name;
  // Overwrite the file.
  std::ofstream sibling_file(path, std::ofstream::trunc);
  sibling_file << kernel_mapping;
}

std::filesystem::path Topology::SetUpTestSiblings(
    const std::filesystem::path& test_directory, bool has_l3_cache,
    bool use_consecutive_smt_numbering) const {
  std::filesystem::path topology_test_directory =
      test_directory / topology_test_subpath_;

  for (int i = 0; i < num_cpus_ / 2; i++) {
    int sibling0;
    int sibling1;

    if (use_consecutive_smt_numbering) {
      // SMT offset is 1, so CPU 0 pairs with CPU 1, CPU 2 pairs with CPU 3,
      // etc.
      sibling0 = i * 2;
      sibling1 = sibling0 + 1;
    } else {
      // This CPU is co-located on the same physical core as CPU
      // `i + num_cpus_ / 2`. For example, when there are 112 CPUs, CPU 0 is
      // co-located with CPU 56, CPU 1 is co-located with CPU 57, ..., and CPU
      // 55 is co-located with CPU 111.
      sibling0 = i;
      sibling1 = i + num_cpus_ / 2;
    }
    std::string kernel_mapping =
        absl::StrFormat("kernel_mapping_iteration_%d", i);

    CreateTestSibling(/*cpu=*/sibling0, topology_test_directory, kernel_mapping,
                      "topology", "thread_siblings");
    CreateTestSibling(/*cpu=*/sibling1, topology_test_directory, kernel_mapping,
                      "topology", "thread_siblings");
  }

  // Construct an L3 siblings topology if `has_l3_cache` is true. Otherwise,
  // create empty files.
  // e.g. on a 112 CPU system.
  // CPUs [0 - 27] share L3 with [56 - 83]
  // CPUs [28 - 55] share L3 with [84 - 111]
  //
  // The sibling_list will look like:
  // Node 0:
  // c0 l3 siblings = 0-27,56-83
  // [..]
  // c27 l3 siblings = 0-27,56-83
  // c56 l3 siblings = 0-27,56-83
  // [..]
  // c83 l3 siblings = 0-27,56-83
  //
  // Node 1:
  // c28 l3 siblings = 28-55,84-111
  // [..]
  // c55 l3 siblings = 28-55,84-111
  // c84 l3 siblings = 28-55,84-111
  // [..]
  // c111 l3 siblings = 28-55,84-111
  int sibling_offset = num_cpus_ / 2;
  int l3_offset = sibling_offset / 2;

  for (int i = 0; i < sibling_offset; i += l3_offset) {
    for (int j = i; j < (i + l3_offset); j++) {
      std::string sibling_list;
      if (has_l3_cache) {
        sibling_list = absl::StrFormat("%d-%d, %d-%d", i, i + (l3_offset - 1),
                                       i + sibling_offset,
                                       i + sibling_offset - 1 + l3_offset);
      } else {
        // `sibling_list` is an empty string.
      }
      CreateTestSibling(j, topology_test_directory, sibling_list,
                        "cache/index3", "shared_cpu_list");
      CreateTestSibling(j + sibling_offset, topology_test_directory,
                        sibling_list, "cache/index3", "shared_cpu_list");
    }
  }

  return topology_test_directory;
}

std::filesystem::path Topology::SetupTestNodePossible(
    const std::filesystem::path& test_directory) const {
  std::filesystem::path topology_test_directory =
      test_directory / topology_test_subpath_;
  std::filesystem::path path =
      topology_test_directory / topology_test_sys_subpath_ / "node";
  CHECK(std::filesystem::create_directories(path) ||
        (std::filesystem::exists(path) && std::filesystem::is_directory(path)));
  path /= "possible";
  std::ofstream sibling_file(path, std::ofstream::trunc);
  // Show 2 NUMA nodes
  sibling_file << "0-1";

  return path;
}

Topology::Topology(InitTest, const std::filesystem::path& test_directory,
                   bool has_l3_cache, bool use_consecutive_smt_numbering)
    : num_cpus_(kNumTestCpus) {
  static_assert(kNumTestCpus <= MAX_CPUS);

  cpus_.resize(num_cpus_);

  // Fill in the CPU IDs first so that they can be accessed out-of-order when
  // filling in 'rep->core' below.
  for (int i = 0; i < num_cpus_; i++) {
    Cpu::CpuRep* rep = &cpus_[i];
    rep->cpu = i;
  }

  const std::filesystem::path siblings_prefix = SetUpTestSiblings(
      test_directory, has_l3_cache, use_consecutive_smt_numbering);
  absl::flat_hash_map<int, CpuList> siblings =
      GetAllSiblings(siblings_prefix, "topology/thread_siblings");
  CHECK_EQ(siblings.size(), num_cpus_);
  absl::flat_hash_map<int, CpuList> l3_siblings =
      GetAllSiblings(siblings_prefix, "cache/index3/shared_cpu_list");
  // Not all microarchitectures have an L3 cache, so vary the CHECK based on
  // whether there is an L3 cache.
  if (has_l3_cache) {
    CHECK_EQ(l3_siblings.size(), num_cpus_);
  } else {
    CHECK(l3_siblings.empty());
  }

  const std::filesystem::path node_possible_path =
      SetupTestNodePossible(test_directory);

  for (int i = 0; i < num_cpus_; i++) {
    Cpu::CpuRep* rep = &cpus_[i];

    CHECK(siblings.find(i) != siblings.end());
    rep->siblings = std::make_unique<CpuList>(*this);
    *rep->siblings = siblings.find(i)->second;

    rep->l3_siblings = std::make_unique<CpuList>(*this);
    if (!l3_siblings.empty()) {
      CHECK(l3_siblings.find(i) != l3_siblings.end());
      *rep->l3_siblings = l3_siblings.find(i)->second;
    }

    rep->core = rep->siblings->begin()->id();
    // Physical cores 0-27 are on NUMA node 0 and physical cores 28-55 are on
    // NUMA node 1.
    rep->numa_node = rep->core < (num_cpus_ / 4) ? 0 : 1;
    // smt_idx requires mapping a rep->cpu to a Core local index.
    // Range-based for loop like (const Cpu& c : rep->siblings) won't work
    // since it internally requires an initialized MachineTopology() which
    // is not yet set up here.
    int idx = -1;
    for (int c = 0; c < MAX_CPUS; ++c) {
      if (rep->siblings->IsSet(c)) {
        idx++;
        if (c == rep->cpu) break;
      }
    }

    CHECK_GE(idx, 0);
    rep->smt_idx = idx;
    all_cpus_.Set(i);
  }

  highest_node_idx_ = GetHighestNodeIdx(node_possible_path);
  CheckSiblings();
  CreateCpuListsForNumaNodes(highest_node_idx_ + 1);
  consecutive_smt_numbering_ = CheckConsecutiveSmtNumbering(*this);
  CHECK_EQ(consecutive_smt_numbering_, use_consecutive_smt_numbering);
}

namespace {

void CheckCustomTopology(const std::vector<Cpu::Raw>& cpus) {
  CHECK(!cpus.empty());

  int next = 0;
  for (const Cpu::Raw& cpu : cpus) {
    // Check that `cpus` contains all CPUs in 0, 1, 2, ..., N. `cpus` is already
    // sorted when this function is called.
    CHECK_EQ(cpu.cpu, next++);

    // Check that all siblings are valid CPUs.
    for (int s : cpu.siblings) {
      CHECK_GE(s, 0);
      CHECK_LT(s, cpus.size());
    }
    for (int s : cpu.l3_siblings) {
      CHECK_GE(s, 0);
      CHECK_LT(s, cpus.size());
    }
  }
}

}  // namespace

void Topology::CheckSiblings() const {
  for (const Cpu& cpu : all_cpus()) {
    CHECK(cpu.siblings().IsSet(cpu));
    for (const Cpu& sibling : cpu.siblings()) {
      CHECK_EQ(cpu.siblings(), sibling.siblings());
    }

    for (const Cpu& sibling : cpu.l3_siblings()) {
      CHECK_EQ(cpu.l3_siblings(), sibling.l3_siblings());
    }
  }
}

Topology::Topology(InitCustom, std::vector<Cpu::Raw> raw_cpus)
    : num_cpus_(raw_cpus.size()) {
  // Consumers assume MAX_CPUS invariant.
  CHECK_LE(num_cpus_, MAX_CPUS);

  std::sort(raw_cpus.begin(), raw_cpus.end());
  CheckCustomTopology(raw_cpus);

  cpus_.resize(num_cpus_);

  // Fill in the CPU IDs first so that they can be accessed out-of-order when
  // filling in `rep->core` below.
  for (int i = 0; i < num_cpus_; i++) {
    Cpu::CpuRep* rep = &cpus_[i];
    rep->cpu = i;
  }

  for (int i = 0; i < num_cpus_; i++) {
    const Cpu::Raw& raw_cpu = raw_cpus[i];
    Cpu::CpuRep* rep = &cpus_[i];

    rep->core = raw_cpu.core;
    rep->smt_idx = raw_cpu.smt_idx;
    rep->numa_node = raw_cpu.numa_node;
    highest_node_idx_ = std::max(rep->numa_node, highest_node_idx_);

    rep->siblings = std::make_unique<CpuList>(*this);
    *rep->siblings = ToCpuList(raw_cpu.siblings);
    rep->l3_siblings = std::make_unique<CpuList>(*this);
    *rep->l3_siblings = ToCpuList(raw_cpu.l3_siblings);

    all_cpus_.Set(i);
  }

  CheckSiblings();
  CreateCpuListsForNumaNodes(highest_node_idx_ + 1);
  consecutive_smt_numbering_ = CheckConsecutiveSmtNumbering(*this);
}

CpuList Topology::ParseCpuStr(const std::string& str) const {
  std::vector<std::string> cpu_ranges;
  std::stringstream stream(str);
  std::string cpu_range;
  while (std::getline(stream, cpu_range, ',')) {
    cpu_ranges.push_back(cpu_range);
  }

  CpuList cpus = EmptyCpuList();
  for (const std::string& field : cpu_ranges) {
    if (field == "\n") {
      continue;
    }

    int first, last;
    if (absl::SimpleAtoi(field, &first)) {
      // Single cpu.
      last = first;
    } else {
      // A range.
      std::regex re("(\\d+)-(\\d+)\n?");
      std::cmatch matches;
      CHECK(std::regex_match(field.c_str(), matches, re));
      CHECK_EQ(matches.size(), 3);
      CHECK(absl::SimpleAtoi(matches[1].str(), &first));
      CHECK(absl::SimpleAtoi(matches[2].str(), &last));
    }

    CHECK_GE(first, 0);
    CHECK_GE(last, first);
    CHECK_LT(last, num_cpus());
    while (first <= last) {
      cpus.Set(first++);
    }
  }
  return cpus;
}

std::vector<Cpu::Raw> Topology::Export() const {
  std::vector<Cpu::Raw> to_export;

  for (const Cpu& cpu : all_cpus()) {
    to_export.push_back({.cpu = cpu.id(),
                         .core = cpu.core(),
                         .smt_idx = cpu.smt_idx(),
                         .siblings = cpu.siblings().ToIntVector(),
                         .l3_siblings = cpu.l3_siblings().ToIntVector(),
                         .numa_node = cpu.numa_node()});
  }
  return to_export;
}

Topology* MachineTopology() {
  static Topology* topology = new Topology(Topology::InitHost{});
  return topology;
}

namespace {

Topology* test_topology = nullptr;
Topology* custom_topology = nullptr;

}  // namespace

void UpdateTestTopology(const std::filesystem::path& test_directory,
                        bool has_l3_cache, bool use_consecutive_smt_numbering) {
  if (test_topology) {
    delete test_topology;
  }
  test_topology = new Topology(Topology::InitTest{}, test_directory,
                               has_l3_cache, use_consecutive_smt_numbering);
}

Topology* TestTopology() {
  // Make sure `UpdateTestTopology()` was already called.
  CHECK_NE(test_topology, nullptr);
  return test_topology;
}

void UpdateCustomTopology(const std::vector<Cpu::Raw>& cpus) {
  if (custom_topology) {
    delete custom_topology;
  }
  custom_topology = new Topology(Topology::InitCustom{}, cpus);
}

Topology* CustomTopology() {
  // Make sure `UpdateCustomTopology()` was already called.
  CHECK_NE(custom_topology, nullptr);
  return custom_topology;
}

}  // namespace ghost
