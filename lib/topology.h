// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

// A generic topology representation for use by ghOSt agents. As a rule of
// thumb, please strongly prefer use of the topology interfaces (including
// extending them when necessary) over the use of system API calls (e.g.,
// sysconf). The intent is that non-host topologies can eventually be passed and
// tested (in conjunction with simulated ghOSt API boundaries).
#ifndef GHOST_LIB_TOPOLOGY_H_
#define GHOST_LIB_TOPOLOGY_H_

#include <sched.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "lib/base.h"

// We carry some definitions currently which anchor on this for convenience.
#define MAX_CPUS 512

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
  struct Raw {
    int cpu;
    int core;
    int smt_idx;
    std::vector<int> siblings;
    std::vector<int> l3_siblings;
    int numa_node;
    // If any additional fields are added to this struct, then update the
    // implementation of the override of the `==` operator below.

    // std::vector overrides the `==` operator. Two std::vector's are equal if
    // they are the same length and each element in one vector is equal to the
    // element in the same index in the other vector.
    bool operator==(const Raw& other) const {
      return cpu == other.cpu && core == other.core &&
             smt_idx == other.smt_idx && siblings == other.siblings &&
             l3_siblings == other.l3_siblings && numa_node == other.numa_node;
    }
    bool operator!=(const Raw& other) const { return !(*this == other); }
    bool operator<(const Raw& other) const { return cpu < other.cpu; }
  };

  enum class UninitializedType { kUninitialized };
  explicit Cpu(UninitializedType) : rep_(nullptr) {}

  int id() const { return rep_->cpu; }
  int core() const { return rep_->core; }
  int smt_idx() const { return rep_->smt_idx; }
  bool valid() const { return rep_ != nullptr; }
  const CpuList& siblings() const { return *rep_->siblings; }
  // Returns the L3 siblings for this CPU. If the returned CpuList is empty,
  // then the microarchitecture does not have an L3 cache.
  const CpuList& l3_siblings() const { return *rep_->l3_siblings; }
  int numa_node() const { return rep_->numa_node; }

  bool operator==(const Cpu& other) const { return id() == other.id(); }
  bool operator!=(const Cpu& other) const { return !(*this == other); }
  bool operator<(const Cpu& other) const { return id() < other.id(); }

  // Returns the CPU ID if the CPU is initialized. Otherwise, if the CPU is
  // uninitialized, returns -1.
  std::string ToString() const {
    if (valid()) {
      return std::to_string(id());
    }
    return std::to_string(-1);
  }

  friend std::ostream& operator<<(std::ostream& os, const Cpu& cpu) {
    return os << cpu.ToString();
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

// This class represents an interface for a generic bitmap type structure that
// can be used to represent cpus.
//
// Derived classes are expected to maintain a bitmap of the form
// $TYPE bitmap_[kMapCapacity]
// where $TYPE is a 64-bit type.
//
// It also includes a custom iterator to implement range-based for-loops.
// e.g
//  for (const Cpu& cpu : cpus())
//  ..
class CpuMap {
 public:
  // The number of bits in the `uint64_t` type.
  static constexpr size_t kIntsBits = CHAR_BIT * sizeof(uint64_t);
  // The number of "slots" allocated to the bitmap.
  static constexpr size_t kMapCapacity = MAX_CPUS / kIntsBits;

  explicit CpuMap(const Topology& topology);

  virtual ~CpuMap() = default;

  // Custom iterator to implement range based for-loops. Loops over the set of
  // cpus and returns the corresponding `Cpu` object.
  //
  // Example:
  // for (const Cpu& cpu : map) {
  //   ...
  // }
  class Iter {
   public:
    // Iterator traits.
    using difference_type = CpuMap;
    using value_type = CpuMap;
    using pointer = const CpuMap*;
    using reference = const CpuMap&;
    using iterator_category = std::input_iterator_tag;

    explicit Iter(const CpuMap* map, size_t id) : map_(map), id_(id) {
      CHECK_NE(map, nullptr);
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

    // Pointer to backing CpuMap.
    const CpuMap* map_;

    // `id` is maintained as an internal cursor while iterating over range
    // loops.
    size_t id_ = 0;
  };
  Iter begin() const;
  Iter end() const;

  // Sets the bit at index `id`.
  virtual void Set(uint32_t id) = 0;
  void Set(const Cpu& cpu) { Set(cpu.id()); }

  // Clears the bit at index `id`.
  virtual void Clear(uint32_t id) = 0;
  void Clear(const Cpu& cpu) { Clear(cpu.id()); }

  // Returns true if the bit at index `id` is set, returns false otherwise.
  virtual bool IsSet(uint32_t id) const = 0;
  bool IsSet(const Cpu& cpu) const { return IsSet(cpu.id()); }

  const Topology& topology() const { return *topology_; }

  // Returns true if bitmap is all 0s, otherwise returns false.
  bool Empty() const {
    for (size_t i = 0; i < map_size_; ++i) {
      if (GetNthMap(i)) {
        return false;
      }
    }
    return true;
  }

  // Returns number of set bits in the bitmap.
  uint32_t Size() const { return CountSetCpus(); }

  size_t map_size() const { return map_size_; }

 protected:
  // Returns number of set cpus in the map.
  uint32_t CountSetCpus() const {
    uint32_t ret = 0;
    for (uint32_t i = 0; i < map_size_; ++i) {
      ret += absl::popcount(GetNthMap(i));
    }
    return ret;
  }

  // Returns the 64-bit value of the nth slot in the bitmap.
  virtual uint64_t GetNthMap(int n) const = 0;

  // The underlying topology.
  const Topology* topology_ = nullptr;

  // The actual number of used "slots" in the bitmap. It is never larger than
  // kMapCapacity. Not marked const to allow assignment.
  size_t map_size_;
};

// This class extends the generic bitmap class with a simple uint64_t backed
// bitmap. Common bitwise operations such as Intersection, Set/Clear, FindNext
// etc. are implemented with room for further extensions and arch specific
// optimizations as needed.
class CpuList : public CpuMap {
 public:
  explicit CpuList(const Topology& topology) : CpuMap(topology) {}

  // Returns true if `this` and `other` have identical bitmaps, false
  // otherwise.
  bool operator==(const CpuList& other) const {
    const uint64_t* bitmap = GetMapConst();
    const uint64_t* compare = other.GetMapConst();
    return !memcmp(bitmap, compare, sizeof(*bitmap) * map_size_);
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
      Cpu cpu = GetNthCpu(i);
      // If the list is modified concurrently, there might be fewer than `size`
      // cpus left in the mask.
      if (!cpu.valid()) {
        break;
      }
      cpus.push_back(cpu);
    }
    return cpus;
  }

  // Converts this `CpuList` to an `std::vector<int>` and returns the vector.
  std::vector<int> ToIntVector() const {
    std::vector<int> cpus;
    const uint32_t size = Size();
    cpus.reserve(size);

    for (uint32_t i = 0; i < size; i++) {
      // If the list is modified concurrently, there might be fewer than `size`
      // cpus left in the mask.
      Cpu cpu = GetNthCpu(i);
      if (!cpu.valid()) {
        break;
      }
      cpus.push_back(cpu.id());
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
    uint64_t* bitmap = GetMap();
    std::transform(bitmap, bitmap + map_size_, src.GetMapConst(), bitmap,
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
    uint64_t* bitmap = GetMap();
    std::transform(bitmap, bitmap + map_size_, src.GetMapConst(), bitmap,
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
    uint64_t* bitmap = GetMap();
    std::transform(bitmap, bitmap + map_size_, src.GetMapConst(), bitmap,
                   [](uint64_t a, uint64_t b) { return a & ~b; });
  }

  // Sets the bit at index `id`.
  void Set(uint32_t id) override {
    DCHECK_GE(id, 0);
    DCHECK_LT(id, MAX_CPUS);

    GetMap()[id / kIntsBits] |= (1ULL << (id % kIntsBits));
  }
  using CpuMap::Set;

  // Clears the bit at index `id`.
  void Clear(uint32_t id) override {
    DCHECK_GE(id, 0);
    DCHECK_LT(id, MAX_CPUS);

    GetMap()[id / kIntsBits] &= ~(1ULL << (id % kIntsBits));
  }
  using CpuMap::Clear;

  // Returns true if the bit at index `id` is set, returns false otherwise.
  bool IsSet(uint32_t id) const override {
    DCHECK_GE(id, 0);
    DCHECK_LT(id, MAX_CPUS);

    return GetMapConst()[id / kIntsBits] & (1ULL << (id % kIntsBits));
  }
  using CpuMap::IsSet;

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

  // Returns the bitmap as a hexadecimal string, suitable to be passed to Linux
  // as a cpumask.
  // - Highest cpus to the left.
  // - Leading 0s trimmed. (Optional for Linux, easier for testing)
  // - Comma between every 8 nibbles, counting from the right.
  // - e.g. 1,ffffffff (33 cpus set, cpu ids 0-32)
  std::string CpuMaskStr() const {
    std::string s;
    bool emitted_nibble = false;
    const uint8_t* bitmap_bytes =
        reinterpret_cast<const uint8_t*>(GetMapConst());
    size_t len = map_size_ * sizeof(*GetMapConst());
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
    return os << clist.CpuMaskStr();
  }

 private:
  virtual uint64_t* GetMap() {
    return bitmap_internal_;
  }

  virtual const uint64_t* GetMapConst() const {
    return bitmap_internal_;
  }

  uint64_t GetNthMap(int n) const override {
    DCHECK_GE(n, 0);
    DCHECK_LT(n, kMapCapacity);
    return GetMapConst()[n];
  }

  // Only the above helpers should use this directly, since derived classes
  // may override them.
  uint64_t bitmap_internal_[kMapCapacity] = {0};
};

// A CpuList derived class that wraps a pointer to a remote bitmap, rather than
// use class-local storage.
class WrappedCpuList : public CpuList {
 public:
  explicit WrappedCpuList(const Topology& topology, uint64_t* map, size_t slots)
      : CpuList(topology), bitmap_ptr_(map) {
    CHECK_EQ(slots, kMapCapacity);
  }

 private:
  uint64_t* GetMap() override { return bitmap_ptr_; }

  const uint64_t* GetMapConst() const override {
    return bitmap_ptr_;
  }

  uint64_t* const bitmap_ptr_ = nullptr;
};

// An atomic implementation of a CpuMap. Useful for cases where it would be
// undesirable to use a mutex to protect a CpuList.
//
// Like CpuMap, this works with range based iterators.
class AtomicCpuMap : public CpuMap {
 public:
  explicit AtomicCpuMap(const Topology& topology) : CpuMap(topology) {}

  // Sets the bit at index `id`.
  void Set(uint32_t id) override {
    DCHECK_GE(id, 0);
    DCHECK_LT(id, MAX_CPUS);

    bitmap_[id / kIntsBits].fetch_or(1ULL << (id % kIntsBits),
                                     std::memory_order_release);
  }
  // Inheritance would otherwise hide the overloaded function that takes in a
  // Cpu argument (here and below).
  using CpuMap::Set;

  // Clears the bit at index `id`.
  void Clear(uint32_t id) override {
    DCHECK_GE(id, 0);
    DCHECK_LT(id, MAX_CPUS);

    bitmap_[id / kIntsBits].fetch_and(~(1ULL << (id % kIntsBits)),
                                      std::memory_order_release);
  }
  using CpuMap::Clear;

  // Returns true if the bit at index `id` is set, returns false otherwise.
  bool IsSet(uint32_t id) const override {
    DCHECK_GE(id, 0);
    DCHECK_LT(id, MAX_CPUS);

    return GetNthMap(id / kIntsBits) & (1ULL << (id % kIntsBits));
  }
  using CpuMap::IsSet;

  // Clears the given bit in the mask. Returns true if bit was previously set,
  // false otherwise.
  bool TestAndClear(uint32_t id) {
    DCHECK_GE(id, 0);
    DCHECK_LT(id, MAX_CPUS);

    const uint64_t mask = ~(1ULL << (id % kIntsBits));
    uint64_t old =
        bitmap_[id / kIntsBits].fetch_and(mask, std::memory_order_release);
    return old & ~mask;
  }
  bool TestAndClear(const Cpu& cpu) { return TestAndClear(cpu.id()); }

 private:
  uint64_t GetNthMap(int n) const override {
    DCHECK_GE(n, 0);
    DCHECK_LT(n, kMapCapacity);
    return bitmap_[n].load(std::memory_order_acquire);
  }

  std::atomic_uint64_t bitmap_[kMapCapacity] = {{0}};
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
  friend void UpdateTestTopology(const std::filesystem::path& test_directory,
                                 bool has_l3_cache,
                                 bool use_consecutive_smt_numbering);
  friend void UpdateCustomTopology(const std::vector<Cpu::Raw>& cpus);
  friend Topology* CustomTopology();

  // The number of CPUs in the test topology.
  static constexpr uint32_t kNumTestCpus = 112;

  // Export the topology into a raw format.
  std::vector<Cpu::Raw> Export() const;

  CpuList EmptyCpuList() const { return CpuList(*this); }
  AtomicCpuMap EmptyAtomicCpuMap() const { return AtomicCpuMap(*this); }

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

  // Returns the number of CCXs in this topology.
  // TODO: compute this
  uint32_t num_ccxs() const { return 0; }

  // Returns the number of numa nodes in this topology.
  uint32_t num_numa_nodes() const { return highest_node_idx_ + 1; }

  // Returns true if the system uses consecutive SMT numbering.
  bool consecutive_smt_numbering() const { return consecutive_smt_numbering_; }

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
    CHECK_GE(node, 0);
    CHECK_LT(node, cpus_on_node_.size());
    return cpus_on_node_[node];
  }

 private:
  // These types are used by the free functions below to choose the correct
  // constructor.
  struct InitHost {};
  struct InitTest {};
  struct InitCustom {};

  // Constructs a Topology object representing the current machine.
  explicit Topology(InitHost);
  // Constructs a Topology object for the topology tests. See the comments
  // below for the `TestTopology` function for a description of the topology.
  // `test_directory` is a path to scratch space in the file system that the
  // topology can use.
  //
  // If `has_l3_cache` is true, creates an L3 cache. Otherwise, does not create
  // an L3 cache.
  Topology(InitTest, const std::filesystem::path& test_directory,
           bool has_l3_cache, bool use_consecutive_smt_numbering);

  Topology(InitCustom, std::vector<Cpu::Raw> cpus);

  void CreateCpuListsForNumaNodes(int num_possible_nodes) {
    cpus_on_node_.resize(num_possible_nodes, EmptyCpuList());
    for (const Cpu& cpu : all_cpus()) {
      int node = cpu.numa_node();
      CHECK_GE(node, 0);
      CHECK_LT(node, num_possible_nodes);
      cpus_on_node_[node].Set(cpu.id());
    }
  }

  // Returns a map from each CPU to the list of all sibling CPUs.
  // `path_prefix` is the prefix to the CPU information files on sysfs.
  // `path_suffix` steers the sibling search towards L2 or L3 cache siblings.
  //
  // This method will return an empty map if there are no siblings. For example,
  // some microarchitectures do not have an L3 cache, so each CPU has no L3
  // siblings.
  absl::flat_hash_map<int, CpuList> GetAllSiblings(
      const std::filesystem::path& path_prefix,
      const std::string path_suffix) const;

  // Check that all siblings are consistent. In other words, if CPUs x and y are
  // siblings, then their siblings lists should be identical.
  void CheckSiblings() const;

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
  //
  // If `has_l3_cache` is true, creates an L3 cache. Otherwise, does not create
  // an L3 cache.
  //
  // If `use_consecutive_smt_numbering` is true, this will use an SMT offset of
  // 1.
  std::filesystem::path SetUpTestSiblings(
      const std::filesystem::path& test_directory, bool has_l3_cache,
      bool use_consecutive_smt_numbering) const;

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

  int highest_node_idx_ = -1;

  bool consecutive_smt_numbering_ = false;

  std::vector<CpuList> cpus_on_node_;
};

// Returns the topology for this machine. The pointer is never null and is
// owned by the `MachineTopology` function. The pointer lives until the
// process dies.
Topology* MachineTopology();

// Creates a topology used by the topology tests and replaces the current test
// topology if one exists. This topology has 112 CPUs, 2 hardware threads per
// physical core (so there are 56 physical cores in total), and 2 NUMA nodes.
// If `use_consecutive_smt_numbering` is false, CPU 0 is co-located with CPU 56
// on the same physical core, CPU 1 is co-located with CPU 57, ..., and CPU 55
// is co-located with CPU 111. Otherwise, CPU 0 is co-located with CPU 1, CPU 2
// with CPU 3, etc. Lastly, CPUs 0-27 and 56-83 are on NUMA node 0 and CPUs
// 28-55 and 84-111 are on NUMA node 1 when we have
// !use_consecutive_smt_numbering. Otherwise, NUMA 0 has CPUs 0-55 and NUMA 1
// has CPUs 56-111.
//
// If `has_l3_cache` is true, an L3 cache is created. All CPUs in a NUMA node
// share the same L3 cache. If `has_l3_cache` is false, then the topology is
// configured as though the microarchitecture does not have an L3 cache.
//
// `test_directory` is a path to scratch space in the file system that the
// topology can use.
//
// If `use_consecutive_smt_numbering` is true, the SMT offset will be set to 1.
void UpdateTestTopology(const std::filesystem::path& test_directory,
                        bool has_l3_cache,
                        bool use_consecutive_smt_numbering = false);

// Returns the test topology described above. The pointer is never null and is
// owned by the `TestTopology` function. The pointer lives until the process
// dies or `UpdateTestTopology()` is called again.
Topology* TestTopology();

// Creates a custom topology from `cpus`.
void UpdateCustomTopology(const std::vector<Cpu::Raw>& cpus);

// Returns the custom topology. The pointer is never null and is owned by the
// `CustomTopology` function. The pointer lives until the process dies or
// `UpdateCustomTopology()` is called again.
Topology* CustomTopology();

}  // namespace ghost

#endif  // GHOST_LIB_TOPOLOGY_H_
