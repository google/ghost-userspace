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

#include "topology.h"

#include <stdio.h>

#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>

#include "absl/strings/str_format.h"
#include "lib/ghost.h"
#include <numa.h>

namespace ghost {

Cpu CpuList::GetNthCpu(uint32_t n) const {
  for (uint32_t i = 0; i < kMapSize; i++) {
    uint64_t word = bitmap_[i];
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

void CpuList::Iter::FindNextSetBit() {
  uint32_t map_idx = id_ / kIntsBits;
  const size_t bit_offset = id_ & (kIntsBits - 1);
  uint64_t word = 0;

  if (map_idx >= kMapSize) {
    // We are already past the end of the bitmap, so skip to the end to
    // avoid an out-of-bounds access to `bitmap_` below.
    goto end;
  }
  // Reset all LSBs seen so far. Note that since `kIntsBits` is a power of
  // 2, the AND operation below is equivalent to `id_ % kIntsBits`.
  word = bitmap_[map_idx];
  word &= ~0ULL << bit_offset;
  while (map_idx < kMapSize) {
    if (word) {
      id_ = map_idx * kIntsBits + __builtin_ctzll(word);
      cpu_ = topology_->cpu(id_);
      return;
    }
    if (++map_idx < kMapSize) {
      word = bitmap_[map_idx];
    }
  }

end:
  // Since there are no more bits set, then fast forward to the `end` Iter
  // to bail out of range based for-loops.
  id_ = topology_->num_cpus();
  cpu_ = Cpu(Cpu::UninitializedType::kUninitialized);
}

CpuList::Iter CpuList::begin() const {
  return Iter(*topology_, bitmap_, /*id=*/0);
}

CpuList::Iter CpuList::end() const {
  return Iter(*topology_, bitmap_, /*id=*/topology_->num_cpus());
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

static std::vector<std::string> split(const std::string& s, std::string regex) {
  std::vector<std::string> result;
  std::regex split_on(regex);

  std::sregex_token_iterator iter(s.begin(), s.end(), split_on, -1), end;
  for (; iter != end; iter++) result.push_back(*iter);
  return result;
}

int Topology::GetHighestNodeIdx(const std::filesystem::path& path) const {
  std::ifstream f(path);
  CHECK(f.is_open());
  std::string node_possible_str;
  std::getline(f, node_possible_str);
  CHECK(!f.fail());

  int highest_idx = 0;
  for (const auto& node_idx_s : split(node_possible_str, "[,-]"))
    highest_idx = std::max(highest_idx, std::stoi(node_idx_s));

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
  CHECK_EQ(l3_siblings.size(), num_cpus_);
  for (int i = 0; i < num_cpus_; i++) {
    Cpu::CpuRep* rep = &cpus_[i];

    CHECK(siblings.find(i) != siblings.end());
    rep->siblings = std::make_unique<CpuList>(*this);
    *rep->siblings = siblings.find(i)->second;

    CHECK(l3_siblings.find(i) != l3_siblings.end());
    rep->l3_siblings = std::make_unique<CpuList>(*this);
    *rep->l3_siblings = l3_siblings.find(i)->second;

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
  CreateCpuListsForNumaNodes();
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
    const std::filesystem::path& test_directory) const {
  std::filesystem::path topology_test_directory =
      test_directory / topology_test_subpath_;

  for (int i = 0; i < num_cpus_ / 2; i++) {
    // This CPU is co-located on the same physical core as CPU `i + num_cpus_ /
    // 2`. For example, when there are 112 CPUs, CPU 0 is co-located with CPU
    // 56, CPU 1 is co-located with CPU 57, ..., and CPU 55 is co-located with
    // CPU 111.
    int sibling0 = i;
    int sibling1 = i + num_cpus_ / 2;
    std::string kernel_mapping =
        absl::StrFormat("kernel_mapping_iteration_%d", i);

    CreateTestSibling(/*cpu=*/sibling0, topology_test_directory, kernel_mapping,
                      "topology", "thread_siblings");
    CreateTestSibling(/*cpu=*/sibling1, topology_test_directory, kernel_mapping,
                      "topology", "thread_siblings");
  }

  // Construct an L3 siblings topology.
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
      std::string sibling_list = absl::StrFormat(
          "%d-%d, %d-%d", i, i + (l3_offset - 1), i + sibling_offset,
          i + sibling_offset - 1 + l3_offset);
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

Topology::Topology(InitTest, const std::filesystem::path& test_directory)
    : num_cpus_(kNumTestCpus) {
  static_assert(kNumTestCpus <= MAX_CPUS);

  cpus_.resize(num_cpus_);

  // Fill in the CPU IDs first so that they can be accessed out-of-order when
  // filling in 'rep->core' below.
  for (int i = 0; i < num_cpus_; i++) {
    Cpu::CpuRep* rep = &cpus_[i];
    rep->cpu = i;
  }

  const std::filesystem::path siblings_prefix =
      SetUpTestSiblings(test_directory);
  absl::flat_hash_map<int, CpuList> siblings =
      GetAllSiblings(siblings_prefix, "topology/thread_siblings");
  absl::flat_hash_map<int, CpuList> l3_siblings =
      GetAllSiblings(siblings_prefix, "cache/index3/shared_cpu_list");
  CHECK_EQ(l3_siblings.size(), num_cpus_);

  const std::filesystem::path node_possible_path =
      SetupTestNodePossible(test_directory);

  for (int i = 0; i < num_cpus_; i++) {
    Cpu::CpuRep* rep = &cpus_[i];

    CHECK(siblings.find(i) != siblings.end());
    rep->siblings = std::make_unique<CpuList>(*this);
    *rep->siblings = siblings.find(i)->second;
    CHECK(l3_siblings.find(i) != l3_siblings.end());
    rep->l3_siblings = std::make_unique<CpuList>(*this);
    *rep->l3_siblings = l3_siblings.find(i)->second;

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
  CreateCpuListsForNumaNodes();
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

Topology* MachineTopology() {
  static Topology* topology = new Topology(Topology::InitHost{});
  return topology;
}

Topology* TestTopology(const std::filesystem::path& test_directory) {
  static Topology* topology =
      new Topology(Topology::InitTest{}, test_directory);
  return topology;
}

}  // namespace ghost
