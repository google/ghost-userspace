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

// A generic topology representation for use by ghOSt agents. As a rule of
// thumb, please strongly prefer use of the topology interfaces (including
// extending them when necessary) over the use of system API calls (e.g.,
// sysconf).  The intent is that non-host topologies can eventually be passed
// and tested (in conjunction with simulated ghOSt API boundaries) for testing.
#ifndef GHOST_LIB_TOPOLOGY_H_
#define GHOST_LIB_TOPOLOGY_H_

#include <sched.h>

#include <filesystem>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "lib/base.h"

// We carry some definitions currently which anchor on this for convenience.
#define MAX_CPUS 256

namespace ghost {

class CpuList;
class Topology;

// This is a CPU in the topology. This class holds information about the CPU
// ID, its physical core, its siblings, and its NUMA node.
//
// CPUs are generally only constructed by the `Topology` class. Example:
// int cpu_id = 0;
// Cpu cpu(&cpus_[cpu_id]);
// CHECK_EQ(cpu.id(), cpu_id);
// int core = cpu.core();
// int numa_node = cpu.numa_node();
// ...
class Cpu {
 public:
  enum class UninitializedType { kUninitialized };
  explicit Cpu(UninitializedType) : rep_(nullptr) {}

  int id() const { return rep_->cpu; }
  int core() const { return rep_->core; }
  int smt_idx() const { return rep_->smt_idx; }
  bool valid() const { return rep_ != nullptr; }
  const CpuList& siblings() const { return *rep_->siblings; }
  const CpuList& l3_siblings() const { return *rep_->l3_siblings; }
  int numa_node() const { return rep_->numa_node; }

  bool operator==(const Cpu& other) const { return id() == other.id(); }
  bool operator!=(const Cpu& other) const { return !(*this == other); }
  bool operator<(const Cpu& other) const { return id() < other.id(); }

  friend std::ostream& operator<<(std::ostream& os, const Cpu& cpu) {
    os << cpu.id();
    return os;
  }

 private:
  // The backing store for the `Cpu` type.
  struct CpuRep {
    int cpu;
    int core;
    int smt_idx;
    std::unique_ptr<CpuList> siblings;
    std::unique_ptr<CpuList> l3_siblings;
    int numa_node;
  };

  explicit Cpu(const CpuRep* rep) : rep_(rep) { CHECK_NE(rep, nullptr); }

  // The backing store for this CPU.
  const CpuRep* rep_;

  friend Topology;
};

// This class implements a bitmap to represent CPUs. Each bit in the bitmap
// corresponds to an index into the global topology backing store of all
// CPUs. It also includes a custom iterator to implement range-based
// for-loops.
// e.g
//  for (const Cpu& cpu : cpus())
//  ..
//  where cpus() returns a CpuList object. The loop will travese the bitmap
//  for each set bit beginning from the LSB. A corresponding Cpu
//  object is returned for each set bit which maps into the global backing
//  store for all Cpus, thus providing immediate access to its siblings, cores
//  etc. Common bitwise operations such as Intersection, Set/Clear, FindNext
//  etc. are implemented with room for further extensions and arch specific
//  optimizations as needed.
class CpuList {
 public:
  explicit CpuList(const Topology& topology) : topology_(&topology) {}

  // Returns true if `this` and `other` have identical bitmaps, false
  // otherwise.
  bool operator==(const CpuList& other) const {
    return std::equal(std::begin(bitmap_), std::end(bitmap_),
                      std::begin(other.bitmap_));
  }

  // Returns the Union of `this` and `other`.
  CpuList& operator+=(const CpuList& other) {
    this->Union(other);
    return *this;
  }

  // Returns the Union of `lhs` and `rhs`.
  friend CpuList operator+(CpuList lhs, const CpuList& rhs) {
    lhs += rhs;
    return lhs;
  }

  // Returns the result of `this.Subtract(other)`.
  CpuList& operator-=(const CpuList& other) {
    this->Subtract(other);
    return *this;
  }

  // Returns the result of `lhs.Subtract(rhs)`.
  friend CpuList operator-(CpuList lhs, const CpuList& rhs) {
    lhs -= rhs;
    return lhs;
  }

  // Converts this `CpuList` to an `std::vector<Cpu>` and returns the vector.
  std::vector<Cpu> ToVector() const {
    std::vector<Cpu> cpus;
    const uint32_t size = Size();
    cpus.reserve(size);

    for (uint32_t i = 0; i < size; i++) {
      cpus.push_back(GetNthCpu(i));
    }
    return cpus;
  }

  // Performs a bitwise AND over two bitmaps and stores the result in the
  // calling object's bitmap.
  // callers bitmap.
  //
  // Example:
  // CpuList a;
  // CpuList b;
  // ...
  // a.Intersection(b);  // Mutates `a`.
  void Intersection(const CpuList& src) {
    std::transform(bitmap_, bitmap_ + kMapSize, src.bitmap_, bitmap_,
                   [](uint64_t a, uint64_t b) { return a & b; });
  }

  // Performs a bitwise OR over two bitmaps and stores the result in the
  // calling object's bitmap.
  //
  // Example:
  // CpuList a;
  // CpuList b;
  // ...
  // a.Union(b);  // Mutates `a`.
  void Union(const CpuList& src) {
    std::transform(bitmap_, bitmap_ + kMapSize, src.bitmap_, bitmap_,
                   [](uint64_t a, uint64_t b) { return a | b; });
  }

  // Performs a bitwise `AND NOT` over two bitmaps, and stores the result in the
  // calling object's bitmap. That is, `a AND ~b`, or the result of subtracting
  // the bits in `b` from `a`.
  //
  // Example:
  // CpuList a;
  // CpuList b;
  // ...
  // a.Subtract(b);  // Mutates `a`.
  void Subtract(const CpuList& src) {
    std::transform(bitmap_, bitmap_ + kMapSize, src.bitmap_, bitmap_,
                   [](uint64_t a, uint64_t b) { return a & ~b; });
  }

  // Sets the bit at index `id`.
  void Set(uint32_t id) {
    DCHECK_GE(id, 0);
    DCHECK_LT(id, MAX_CPUS);

    bitmap_[id / kIntsBits] |= (1ULL << (id % kIntsBits));
  }

  // Sets the bit for CPU `cpu`.
  void Set(const Cpu& cpu) { Set(cpu.id()); }

  // Clears the bit at index `id`.
  void Clear(uint32_t id) {
    DCHECK_GE(id, 0);
    DCHECK_LT(id, MAX_CPUS);

    bitmap_[id / kIntsBits] &= ~(1ULL << (id % kIntsBits));
  }

  // Clears the bit for CPU `cpu`.
  void Clear(const Cpu& cpu) { Clear(cpu.id()); }

  // Returns true if the bit at index `id` is set, returns false otherwise.
  bool IsSet(uint32_t id) const {
    DCHECK_GE(id, 0);
    DCHECK_LT(id, MAX_CPUS);

    return bitmap_[id / kIntsBits] & (1ULL << (id % kIntsBits));
  }

  // Returns true if the bit for CPU `cpu` is set, returns false otherwise.
  bool IsSet(const Cpu& cpu) const { return IsSet(cpu.id()); }

  // Returns the nth CPU set in the bitmap (where `n` is zero-indexed), from
  // low CPU ID to high CPU ID. If fewer than `n + 1` CPUs are set, returns an
  // uninitialized CPU.
  Cpu GetNthCpu(uint32_t n) const;

  Cpu operator[](std::size_t idx) const { return GetNthCpu(idx); }

  Cpu Front() const {
    Cpu front = GetNthCpu(0);
    return front;
  }

  Cpu Back() const {
    Cpu back = GetNthCpu(Size() - 1);
    return back;
  }

  // Returns true if bitmap is all 0s, otherwise returns false.
  // TODO: Bail early if this needs to be efficient.
  bool Empty() const { return CountSetBits() == 0; }

  // Returns number of set bits in the bitmap.
  uint32_t Size() const { return CountSetBits(); }

  const Topology& topology() const { return *topology_; }

  // Custom iterator to implement range based for-loops. Loops over set bits
  // in the bitmap and returns corresponding `Cpu` object.
  //
  // Example:
  // CpuList list = ...;
  // for (const Cpu& cpu : list) {
  //   ...
  // }
  class Iter {
   public:
    // Iterator traits.
    using difference_type = CpuList;
    using value_type = CpuList;
    using pointer = const CpuList*;
    using reference = const CpuList&;
    using iterator_category = std::input_iterator_tag;

    explicit Iter(const Topology& topology, const uint64_t* bitmap, size_t id)
        : topology_(&topology), bitmap_(bitmap), id_(id) {
      CHECK_NE(bitmap, nullptr);
      // Initialize the iterator to the first set bit.
      FindNextSetBit();
    }

    bool operator==(const Iter& other) const { return id_ == other.id_; }
    bool operator!=(const Iter& other) const { return !(id_ == other.id_); }

    const Cpu* operator->() const { return &cpu_; }
    Cpu operator*() const { return cpu_; }

    // Pre-increment op.
    Cpu operator++() {
      ++id_;
      FindNextSetBit();
      return cpu_;
    }

    // Post-increment op.
    Cpu operator++(int) {
      // Stash away the old `cpu_` to return.
      Cpu old_cpu = cpu_;
      // Post-increment `id_` and `cpu_`.
      id_++;
      FindNextSetBit();
      return old_cpu;
    }

   private:
    // Find the next set bit in the bitmap (starting the search at the bit at
    // index `id_`) and set `id_` and the corresponding `cpu` object to this
    // bit accordingly.
    void FindNextSetBit();

    // The CPU corresponding to the bit that the iterator is currently
    // pointing at. When the iterator is at the end of the range, `cpu_` is
    // set to an uninitialized CPU.
    Cpu cpu_{Cpu::UninitializedType::kUninitialized};
    // Pointer to the topology used by the `CpuList`.
    const Topology* topology_;
    // Pointer to backing bitmap of CpuList.
    const uint64_t* bitmap_;
    // `id` is maintained as an internal cursor while iterating over range
    // loops.
    size_t id_ = 0;
  };

  Iter begin() const;
  Iter end() const;

  // Returns the bitmap as a hexadecimal string, suitable to be passed to Linux
  // as a cpumask.
  // - Highest cpus to the left.
  // - Leading 0s trimmed. (Optional for Linux, easier for testing)
  // - Comma between every 8 nibbles, counting from the right.
  // - e.g. 1,ffffffff (33 cpus set, cpu ids 0-32)
  std::string CpuMaskStr() const {
    std::string s;
    bool emitted_nibble = false;
    const uint8_t* bitmap_bytes = reinterpret_cast<const uint8_t*>(bitmap_);
    size_t len = kMapSize * sizeof(bitmap_[0]);
    // The bitmap is stored with the lowest bits at the beginning of the map.
    // Print the MSB first, both backwards and the upper nibble first.
    for (int i = len - 1; i >= 0; --i) {
      uint8_t byte = bitmap_bytes[i];
      uint8_t hi = byte >> 4;
      uint8_t lo = byte & 0xf;

      if (hi || emitted_nibble) {
        absl::StrAppend(&s, absl::Hex(hi));
        emitted_nibble = true;
      }
      if (lo || emitted_nibble) {
        absl::StrAppend(&s, absl::Hex(lo));
        emitted_nibble = true;
      }
    }
    // Emit a comma every 8 nibbles, from the right.  So where we start emitting
    // commas is based on the total length.  Note that we never emit a comma
    // first, and if we have 8 nibbles, we do not emit.
    std::string ret;
    int nibble_til_next_comma = s.length() % 8;
    if (!nibble_til_next_comma) {
      nibble_til_next_comma = 8;
    }
    for (const char c : s) {
      if (!nibble_til_next_comma) {
        nibble_til_next_comma = 8;
        absl::StrAppend(&ret, ",");
      }
      --nibble_til_next_comma;
      absl::StrAppend(&ret, std::string(1, c));
    }
    return ret.length() ? ret : std::string("0");
  }

  // Dumps the bitmap in hexadecimal, highest cpus to the left
  friend std::ostream& operator<<(std::ostream& os, const CpuList& clist) {
    os << clist.CpuMaskStr();
    return os;
  }

 private:
  // The number of bits in the `uint64_t` type.
  static constexpr size_t kIntsBits = CHAR_BIT * sizeof(uint64_t);
  // The number of "slots" in the bitmap array `bitmap_`.
  static constexpr size_t kMapSize = MAX_CPUS / kIntsBits;
  // The number of bits in the bitmap.
  static constexpr size_t kNumBits = kIntsBits * kMapSize;

  // Returns number of set bits in the bitmap.
  uint32_t CountSetBits() const {
    uint32_t ret = 0;
    for (uint32_t i = 0; i < kMapSize; ++i) {
      ret += absl::popcount(bitmap_[i]);
    }
    return ret;
  }

  const Topology* topology_ = nullptr;
  uint64_t bitmap_[kMapSize] = {0};
};

// This represents the topology of the machine, including the CPUs, cores, and
// NUMA node information. Methods that are passed invalid parameters (e.g.,
// passing -1 to `cpu()`) will crash the program.
//
// Example:
// Get the topology for this machine: Topology* topology = MachineTopology();
// for (const Cpu& cpu : topology->all_cpus()) {
//   int cpu_id = cpu.id();
//   int numa_node = cpu.numa_node();
//   ...
// }
// TODO: Needs other sub-NUMA topology information.
class Topology {
 public:
  // These two functions use the private `Topology` constructor.
  friend Topology* MachineTopology();
  friend Topology* TestTopology(const std::filesystem::path& test_directory);

  CpuList EmptyCpuList() const { return CpuList(*this); }

  // Helper for creating CpuLists from a vector of integer CPU IDs (`cpus`).
  CpuList ToCpuList(const std::vector<int>& cpus) const {
    CpuList result(*this);

    for (int i : cpus) {
      result.Set(i);
    }
    return result;
  }

  // Converts a vector of CPUs (`cpus`) to a `CpuList` type and returns the
  // CPU list.
  CpuList ToCpuList(const std::vector<Cpu>& cpus) const {
    CpuList result(*this);

    for (const Cpu& c : cpus) {
      result.Set(c.id());
    }
    return result;
  }

  // Converts a cpu_set_t (`cpus`) to a `CpuList` type and returns the CPU list.
  CpuList ToCpuList(const cpu_set_t& cpus) const {
    CpuList result(*this);

    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
      if (CPU_ISSET(cpu, &cpus)) {
        result.Set(cpu);
      }
    }
    return result;
  }

  // Converts a CpuList type (`cpu_list`) to a `cpu_set_t` type and returns
  // the CPU set.
  static cpu_set_t ToCpuSet(const CpuList& cpu_list) {
    cpu_set_t set;

    CPU_ZERO(&set);
    for (const Cpu& cpu : cpu_list) {
      CHECK_GE(cpu.id(), 0);
      CHECK_LT(cpu.id(), CPU_SETSIZE);
      CPU_SET(cpu.id(), &set);
    }
    return set;
  }

  // Returns the number of CPUs in this topology.
  uint32_t num_cpus() const { return num_cpus_; }

  // Returns the number of CPUs per physical core.
  uint32_t smt_count() const { return cpus_[0].siblings->Size(); }

  // Returns the number of numa nodes in this topology.
  uint32_t num_numa_nodes() const { return highest_node_idx_ + 1; }

  // Returns the CPU with ID 'cpu'.
  Cpu cpu(int cpu) const {
    DCHECK_GE(cpu, 0);
    DCHECK_LT(cpu, cpus_.size());
    return Cpu(&cpus_[cpu]);
  }

  // Returns all CPUs in this topology.
  const CpuList& all_cpus() const { return all_cpus_; }

  // Returns the physical core that `cpu` is located on.
  Cpu Core(const Cpu& c) const { return cpu(c.core()); }

  // Returns a list of the physical cores that the CPUs in 'cpus' run on.
  // TODO: This wants something more efficient (iterator?)
  CpuList Cores(const CpuList& cpus) const {
    CpuList result(*this);
    // Use a set to track which cores we have seen so far so that we do not
    // include the same core in the `CpuList` more than once.
    absl::flat_hash_set<int> cores;
    for (const Cpu& c : cpus) {
      if (cores.insert(c.core()).second) {
        // The insert succeeded, so we have not seen this core yet.
        result.Set(c.core());
      }
    }
    return result;
  }

  // Returns a list of all of the physical cores in this topology.
  CpuList all_cores() const { return Cores(all_cpus()); }

  int highest_node_idx() const { return highest_node_idx_; }

  // Takes as input a cpu string in kernel format (ie. comma-delimited ranges,
  // where each range is either a single cpu, or a range of cpus. For example,
  // "0-3,6,8").
  //
  // Returns a CpuList with the corresponding cpus set.
  CpuList ParseCpuStr(const std::string& str) const;

  const CpuList& CpusOnNode(int node) const {
    CHECK(node <= 1 && node >= 0);
    return cpus_on_node_[node];
  }

 private:
  // These types are used by the free functions below to choose the correct
  // constructor.
  struct InitHost {};
  struct InitTest {};

  // The number of CPUs in the test topology.
  static constexpr uint32_t kNumTestCpus = 112;

  // Constructs a Topology object representing the current machine.
  explicit Topology(InitHost);
  // Constructs a Topology object for the topology tests. See the comments
  // below for the `TestTopology` function for a description of the topology.
  // `test_directory` is a path to scratch space in the file system that the
  // topology can use.
  explicit Topology(InitTest, const std::filesystem::path& test_directory);

  void CreateCpuListsForNumaNodes() {
    for (const Cpu& cpu : all_cpus()) {
      cpus_on_node_[cpu.numa_node()].Set(cpu.id());
    }
  }

  // Returns a map from each CPU to the list of all sibling CPUs.
  // `path_prefix` is the prefix to the CPU information files on sysfs.
  // `path_suffix` steers the sibling search towards L2 or L3 cache siblings.
  absl::flat_hash_map<int, CpuList> GetAllSiblings(
      const std::filesystem::path& path_prefix,
      const std::string path_suffix) const;

  // Gets the largest NUMA node index. Note that this is different from the
  // number of NUMA nodes, if indexing skips some offline or unavailable NUMA
  // nodes.
  int GetHighestNodeIdx(const std::filesystem::path& path) const;

  // Sets up the `thread_siblings` file for CPU `cpu`.
  // `topology_test_directory` is the root of the test scratch which is
  // treated as though it is the Linux sysfs CPU root. `kernel_mapping` is the
  // kernel mapping to write to the file.
  void CreateTestSibling(int cpu,
                         const std::filesystem::path& topology_test_directory,
                         const std::string& kernel_mapping,
                         const std::string& dir_path,
                         const std::string& file_name) const;

  // Initializes the test directory to contain `thread_siblings` files in the
  // same organization and structure as on a normal Linux system in the sysfs
  // CPU root.
  std::filesystem::path SetUpTestSiblings(
      const std::filesystem::path& test_directory) const;

  // Initializes test directory with a node possible file.
  std::filesystem::path SetupTestNodePossible(
      const std::filesystem::path& test_directory) const;

  // Subdir under test directory for test topology info
  const std::filesystem::path topology_test_subpath_ = "topology_test";

  // Subdir for within our topology test directory for system topology info
  const std::filesystem::path topology_test_sys_subpath_ = "sys/devices/system";

  // The number of CPUs in this topology.
  const uint32_t num_cpus_;

  // All of the CPUs in this topology.
  CpuList all_cpus_ = EmptyCpuList();

  // The backing store for all CPUs in this topology.
  std::vector<Cpu::CpuRep> cpus_;

  int highest_node_idx_;

  CpuList cpus_on_node_[2] = {EmptyCpuList(), EmptyCpuList()};
};

// Returns the topology for this machine. The pointer is never null and is
// owned by the `MachineTopology` function. The pointer lives until the
// process dies.
Topology* MachineTopology();

// Returns a topology used by the topology tests. This topology has 112 CPUs,
// 2 hardware threads per physical core (so there are 56 physical cores in
// total), and 2 NUMA nodes. CPU 0 is co-located with CPU 56 on the same
// physical core, CPU 1 is co-located with CPU 57, ..., and CPU 55 is
// co-located with CPU 111. This is how Linux configures CPUs. Lastly, CPUs
// 0-27 and 56-83 are on NUMA node 0 and CPUs 28-55 and 84-111 are on NUMA
// node 1.
//
// `test_directory` is a path to scratch space in the file system that the
// topology can use. The pointer is never null and is owned by the
// `TestTopology` function. The pointer lives until the process dies.
Topology* TestTopology(const std::filesystem::path& test_directory);

}  // namespace ghost

#endif  // GHOST_LIB_TOPOLOGY_H_
