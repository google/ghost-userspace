// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "lib/topology.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/flat_hash_set.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/strings/numbers.h"
#include "lib/ghost.h"

// These tests check that the `Topology` functions return expected values.

ABSL_FLAG(std::string, test_tmpdir, "/tmp",
          "A temporary file system directory that the test can access");

namespace ghost {
namespace {

using ::testing::Eq;
using ::testing::Ge;
using ::testing::Gt;
using ::testing::IsFalse;
using ::testing::IsTrue;
using ::testing::Le;

// Tests that all CPUs in the topology returned by `MachineTopology()` are
// valid.
TEST(TopologyTest, CpuValid) {
  for (const Cpu& cpu : MachineTopology()->all_cpus()) {
    EXPECT_THAT(cpu.valid(), IsTrue());
  }
}

// Tests that an uninitialized CPU is invalid.
TEST(TopologyTest, UninitializedCpu) {
  Cpu cpu(Cpu::UninitializedType::kUninitialized);
  EXPECT_THAT(cpu.valid(), IsFalse());
}

// Tests that each CPU has a valid NUMA node.
TEST(TopologyTest, CpuNumaNode) {
  for (const Cpu& cpu : MachineTopology()->all_cpus()) {
    EXPECT_THAT(cpu.numa_node(), Ge(0));
  }
}

// Tests that Cpu::ToString() returns the expected CPU ID string for valid CPUs.
TEST(TopologyTest, CpuToString) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  EXPECT_THAT(TestTopology()->cpu(0).ToString(), Eq("0"));
  EXPECT_THAT(TestTopology()->cpu(3).ToString(), Eq("3"));
  EXPECT_THAT(TestTopology()->cpu(19).ToString(), Eq("19"));
}

// Tests that Cpu::ToString() returns "-1" for an uninitialized CPU.
TEST(TopologyTest, UninitializedCpuToString) {
  Cpu cpu(Cpu::UninitializedType::kUninitialized);
  EXPECT_THAT(cpu.ToString(), Eq("-1"));
}

// Tests that `num_cpus()` returns number of CPUs in the topology.
TEST(TopologyTest, TopologyNumCpus) {
  EXPECT_THAT(MachineTopology()->all_cpus().Size(),
              Eq(MachineTopology()->num_cpus()));
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  EXPECT_THAT(TestTopology()->all_cpus().Size(),
              Eq(TestTopology()->num_cpus()));
}

// Tests that the topology contains all CPUs.
TEST(TopologyTest, TopologyAllCpus) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  for (uint32_t i = 0; i < Topology::kNumTestCpus; i++) {
    EXPECT_THAT(TestTopology()->cpu(i).id(), Eq(i));
  }
}

// Tests that `ToCpuList` returns an empty list when there are no CPUs.
TEST(TopologyTest, CpuListContentsEmpty) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  EXPECT_THAT(TestTopology()->ToCpuList(std::vector<int>()).Empty(), IsTrue());
}

// Tests that `ToCpuList` returns a correct list of the specified CPUs.
TEST(TopologyTest, CpuListContents) {
  std::vector<int> cpu_ids = std::vector<int>{0, 1, 2};
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  CpuList cpu_list = TestTopology()->ToCpuList(cpu_ids);
  absl::flat_hash_set<int> expected_contents(cpu_ids.begin(), cpu_ids.end());

  for (const Cpu& cpu : cpu_list) {
    EXPECT_THAT(expected_contents.erase(cpu.id()), Eq(1));
  }
  EXPECT_THAT(expected_contents.empty(), IsTrue());
}

// Tests that `ToCpuSet` returns an empty list when there are no CPUs.
TEST(TopologyTest, CpuSetContentEmpty) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  CpuList cpu_list = TestTopology()->ToCpuList(std::vector<int>());
  cpu_set_t cpu_set = Topology::ToCpuSet(cpu_list);
  EXPECT_THAT(CPU_COUNT(&cpu_set), Eq(0));
}

// Tests that `ToCpuSet` returns a correct list of the specified CPUs.
TEST(TopologyTest, CpuSetContents) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  std::vector<int> cpu_ids = std::vector<int>{0, 1, 2};
  CpuList cpu_list = TestTopology()->ToCpuList(cpu_ids);
  cpu_set_t cpu_set = Topology::ToCpuSet(cpu_list);
  absl::flat_hash_set<int> expected_contents(cpu_ids.begin(), cpu_ids.end());

  for (int i = 0; i < CPU_SETSIZE; i++) {
    if (CPU_ISSET(i, &cpu_set)) {
      EXPECT_THAT(expected_contents.erase(i), Eq(1));
    }
  }
  EXPECT_THAT(expected_contents.empty(), IsTrue());
}

// Tests that `all_cores()` returns all physical cores in the topology.
TEST(TopologyTest, CheckAllCores) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  std::vector<Cpu> expected;
  for (int i = 0; i < TestTopology()->num_cpus() / 2; i++) {
    expected.push_back(TestTopology()->cpu(i));
  }
  EXPECT_THAT(TestTopology()->all_cores(),
              Eq(TestTopology()->ToCpuList(expected)));
}

// Tests that each CPU is associated with the correct physical core.
TEST(TopologyTest, CheckCores) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  for (int i = 0; i < TestTopology()->num_cpus() / 2; i++) {
    Cpu expected_core = TestTopology()->cpu(i);

    Cpu first_cpu = TestTopology()->cpu(i);
    EXPECT_THAT(TestTopology()->Core(first_cpu), Eq(expected_core));

    // CPU 0 is co-located with CPU 56, CPU 1 is co-located with CPU 57, ...,
    // CPU 55 is co-located with CPU 111.
    int sibling = i + TestTopology()->num_cpus() / 2;
    Cpu second_cpu = TestTopology()->cpu(sibling);
    EXPECT_THAT(TestTopology()->Core(second_cpu), Eq(expected_core));
  }
}

// Tests that each CPU returns the correct siblings. The CPU's siblings are all
// CPUs co-located on its physical core (including itself).
TEST(TopologyTest, CheckSiblings) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  for (int i = 0; i < TestTopology()->num_cpus() / 2; i++) {
    // CPU 0 is co-located with CPU 56, CPU 1 is co-located with CPU 57, ...,
    // CPU 55 is co-located with CPU 111.
    int sibling = i + TestTopology()->num_cpus() / 2;
    CpuList expected = TestTopology()->ToCpuList(std::vector<int>{i, sibling});

    // Check the siblings for CPU `i`.
    CpuList siblings = TestTopology()->cpu(i).siblings();
    EXPECT_THAT(siblings, Eq(expected));

    // Check the siblings for CPU `sibling`.
    siblings = TestTopology()->cpu(sibling).siblings();
    EXPECT_THAT(siblings, Eq(expected));
  }
}

// Tests that each CPU returns an expected list of L3 cache siblings.
// e.g. On a 112 CPU system.
// CPUs [0 - 27] share L3 with [56 - 83]
// CPUs [28 - 55] share L3 with [84 - 111]
TEST(TopologyTest, CheckL3Siblings) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  int sibling_offset = TestTopology()->num_cpus() / 2;
  int l3_offset = sibling_offset / 2;

  for (int i = 0; i < sibling_offset; i += l3_offset) {
    CpuList expected = TestTopology()->EmptyCpuList();
    for (int j = i; j < (i + l3_offset); j++) {
      expected.Set(j);
      expected.Set(j + sibling_offset);
    }

    for (int j = i; j < (i + l3_offset); j++) {
      // Check the L3 siblings for CPU `j`.
      CpuList siblings = TestTopology()->cpu(j).l3_siblings();
      EXPECT_THAT(siblings, Eq(expected));

      // Check the L3 siblings for sibling of CPU `j`.
      siblings = TestTopology()->cpu(j + sibling_offset).l3_siblings();
      EXPECT_THAT(siblings, Eq(expected));
    }
  }
}

// Tests that on a microarchitecture with no L3 cache, each CPU returns an empty
// list of L3 cache siblings.
TEST(TopologyTest, CheckEmptyL3Siblings) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/false);

  for (const Cpu& cpu : TestTopology()->all_cpus()) {
    EXPECT_THAT(cpu.l3_siblings().Empty(), IsTrue());
  }
}

// Tests that the topology returns the highest NUMA node.
TEST(TopologyTest, CheckHighestNodeIdx) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  EXPECT_THAT(TestTopology()->highest_node_idx(), Eq(1));
}

// Tests that each CPU is assigned the correct SMT index.
TEST(TopologyTest, CheckSmtIdx) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);

  // We don't have any larger SMT toplogies.
  EXPECT_THAT(TestTopology()->smt_count(), Le(2));

  for (int i = 0; i < TestTopology()->num_cpus(); i++) {
    Cpu cpu = TestTopology()->cpu(i);
    CpuList siblings = cpu.siblings();
    bool found = false;
    for (int j = 0; j < siblings.Size(); j++) {
      Cpu sibling = siblings.GetNthCpu(j);
      ASSERT_THAT(sibling.valid(), IsTrue());

      // Siblings should be monotonically increasing.
      if (j > 0) {
        EXPECT_THAT(sibling.id(), Gt(siblings.GetNthCpu(j - 1).id()));
      }

      if (sibling.id() == cpu.id()) {
        found = true;
        EXPECT_THAT(cpu.smt_idx(), Eq(j));
        const int expected_smt_idx =
            (cpu.id() < TestTopology()->num_cpus() / 2) ? 0 : 1;
        EXPECT_THAT(cpu.smt_idx(), Eq(expected_smt_idx));
      }
    }
    EXPECT_THAT(found, IsTrue());
  }
}

TEST(TopologyTest, TestConsecutiveSmtNumbering) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true,
                     /*use_consecutive_smt_numbering=*/true);

  EXPECT_EQ(TestTopology()->smt_count(), 2);

  EXPECT_TRUE(TestTopology()->consecutive_smt_numbering());

  for (int i = 0; i < TestTopology()->num_cpus(); i++) {
    const Cpu& cpu = TestTopology()->cpu(i);
    const CpuList& siblings = cpu.siblings();
    bool found = false;
    for (int j = 0; j < siblings.Size(); j++) {
      const Cpu& sibling = siblings.GetNthCpu(j);
      ASSERT_THAT(sibling.valid(), IsTrue());

      if (cpu == sibling) {
        continue;
      }

      found = true;
      EXPECT_EQ(abs(cpu.id() - sibling.id()), 1);
    }
    EXPECT_TRUE(found);
  }
}

// Tests the basic `Set`, `IsSet`, and `Clear` functionality of `CpuList`.
TEST(TopologyTest, BitmapSet) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  CpuList list = TestTopology()->EmptyCpuList();

  std::vector<int> cpus = {5, 20, 87, 94, 100};
  for (int cpu : cpus) {
    list.Set(cpu);
  }
  for (int i = 0; i < MAX_CPUS; i++) {
    bool exists = (std::find(cpus.begin(), cpus.end(), i) != cpus.end());
    EXPECT_THAT(list.IsSet(i), Eq(exists));
  }

  EXPECT_THAT(list.Size(), Eq(cpus.size()));
  for (int i = 0; i < cpus.size(); i++) {
    EXPECT_THAT(list.GetNthCpu(i).id(), Eq(cpus[i]));
  }
  for (int i = 0; i < cpus.size(); i++) {
    list.Clear(cpus[i]);
    if (i < cpus.size() - 1) {
      EXPECT_THAT(list.GetNthCpu(0).id(), Eq(cpus[i + 1]));
    }
  }
  EXPECT_THAT(list.Size(), Eq(0));
}

// Tests the basic `Set`, `IsSet`, and `Clear` functionality of `CpuList` using
// CPUs instead of ints.
TEST(TopologyTest, BitmapSetCpu) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  CpuList list = TestTopology()->EmptyCpuList();

  std::vector<Cpu> cpus = {TestTopology()->cpu(5), TestTopology()->cpu(20),
                           TestTopology()->cpu(87), TestTopology()->cpu(94),
                           TestTopology()->cpu(100)};
  for (const Cpu& cpu : cpus) {
    list.Set(cpu);
  }
  for (const Cpu& cpu : TestTopology()->all_cpus()) {
    bool exists = (std::find(cpus.begin(), cpus.end(), cpu) != cpus.end());
    EXPECT_THAT(list.IsSet(cpu), Eq(exists));
  }

  EXPECT_THAT(list.Size(), Eq(cpus.size()));
  for (int i = 0; i < cpus.size(); i++) {
    EXPECT_THAT(list.GetNthCpu(i), Eq(cpus[i]));
  }
  for (int i = 0; i < cpus.size(); i++) {
    list.Clear(cpus[i]);
    if (i < cpus.size() - 1) {
      EXPECT_THAT(list.GetNthCpu(0), Eq(cpus[i + 1]));
    }
  }
  EXPECT_THAT(list.Size(), Eq(0));
}

// Tests that the `CpuList::Intersection` method calculates the
// correct intersection of two `CpuList`s.
TEST(TopologyTest, Intersection) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  CpuList list0 =
      TestTopology()->ToCpuList(std::vector<int>{5, 20, 87, 94, 100});
  CpuList list1 = TestTopology()->ToCpuList(
      std::vector<int>{5, 10, 60, 80, 87, 90, 101, 110});

  list0.Intersection(list1);
  EXPECT_THAT(list0, Eq(TestTopology()->ToCpuList(std::vector<int>{5, 87})));
}

// Tests that the `CpuList::Union` method calculates the correct union
// of two `CpuList`s. Also tests the related `+` and `+=` operators.
TEST(TopologyTest, Union) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  const CpuList list0 =
      TestTopology()->ToCpuList(std::vector<int>{5, 20, 87, 94, 100});
  const CpuList list1 = TestTopology()->ToCpuList(
      std::vector<int>{5, 10, 60, 80, 87, 90, 101, 110});
  const CpuList result = TestTopology()->ToCpuList(
      std::vector<int>{5, 10, 20, 60, 80, 87, 90, 94, 100, 101, 110});

  CpuList scratch = list0;
  scratch.Union(list1);
  EXPECT_THAT(scratch, Eq(result));

  scratch = list0;
  scratch += list1;
  EXPECT_THAT(scratch, Eq(result));

  scratch = list0 + list1;
  EXPECT_THAT(scratch, Eq(result));
}

// Tests that the `CpuList::Subtract` method calculates the correct subtraction
// of two `CpuList`s. Also tests the related `-` and `-=` operators.
TEST(TopologyTest, Subtract) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  const CpuList list0 =
      TestTopology()->ToCpuList(std::vector<int>{1, 3, 5, 20, 87, 94, 100});
  const CpuList list1 =
      TestTopology()->ToCpuList(std::vector<int>{0, 5, 10, 60, 94});
  const CpuList result =
      TestTopology()->ToCpuList(std::vector<int>{1, 3, 20, 87, 100});

  CpuList scratch = list0;
  scratch.Subtract(list1);
  EXPECT_THAT(scratch, Eq(result));

  scratch = list0;
  scratch -= list1;
  EXPECT_THAT(scratch, Eq(result));

  scratch = list0 - list1;
  EXPECT_THAT(scratch, Eq(result));
}

// Tests that immediately dereferencing the iterator returned by `begin()` for
// the topology's CPUs returns the first CPU.
TEST(TopologyTest, IteratorBegin) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  EXPECT_THAT(TestTopology()->all_cpus().begin()->id(), Eq(0));
}

// Tests that immediately dereferencing the iterator returned by `end()` for the
// topology's CPUs returns an invalid CPU.
TEST(TopologyTest, IteratorEnd) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  EXPECT_THAT(TestTopology()->all_cpus().end()->valid(), IsFalse());
}

// Tests that the iterator iterates over the `CpuList` correctly with a for
// loop.
TEST(TopologyTest, Iterator) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  std::vector<int> cpus = {5, 20, 87, 94, 100};
  CpuList list = TestTopology()->ToCpuList(cpus);

  int index = 0;
  auto it = list.begin();
  for (; it != list.end(); it++) {
    EXPECT_THAT(it->id(), Eq(cpus[index++]));
  }
  EXPECT_THAT(it, Eq(list.end()));
  // Since `it` == `list.end()`, `it` should point to an invalid CPU.
  EXPECT_THAT(it->valid(), IsFalse());
  // Check that the for loop only iterated once per CPU in `list`.
  EXPECT_THAT(index, Eq(cpus.size()));
}

// Tests that the iterator iterates over the `CpuList` correctly with a
// range-based for loop.
TEST(TopologyTest, IteratorRange) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  std::vector<int> cpus = {5, 20, 87, 94, 100};
  CpuList list = TestTopology()->ToCpuList(cpus);

  int index = 0;
  for (const Cpu& cpu : list) {
    EXPECT_THAT(cpu.id(), Eq(cpus[index++]));
  }
  // Check that the range-based for loop only iterated once per CPU in `list`.
  EXPECT_THAT(index, Eq(cpus.size()));
}

// Tests that the iterator works properly when the first and last CPUs are set
// in the `CpuList` (i.e., tests the boundary cases).
TEST(TopologyTest, IteratorBoundary) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  std::vector<int> cpus = {0, 20, 111};
  CpuList list = TestTopology()->ToCpuList(cpus);

  int index = 0;
  auto it = list.begin();
  for (; it != list.end(); it++) {
    EXPECT_THAT(it->id(), Eq(cpus[index++]));
  }
  EXPECT_THAT(it, Eq(list.end()));
  // Since `it` == `list.end()`, `it` should point to an invalid CPU.
  EXPECT_THAT(it->valid(), IsFalse());
  // Check that the for loop only iterated once per CPU in `list`.
  EXPECT_THAT(index, Eq(cpus.size()));
}

static std::string StripCommas(const std::string s) {
  std::string ret;
  for (const char c : s) {
    if (c != ',') {
      absl::StrAppend(&ret, std::string(1, c));
    }
  }
  return ret;
}

static void TestList(Topology* t, const std::vector<int> cpus,
                     const char* expect) {
  CpuList list = t->ToCpuList(cpus);
  std::string cpumask = list.CpuMaskStr();
  std::string cpumask_nocomma = StripCommas(cpumask);

  for (const int c : cpus) {
    char nibble[2] = {0};
    nibble[0] = cpumask_nocomma[cpumask_nocomma.length() - 1 - c / 4];
    uint32_t nibble_int;
    absl::numbers_internal::safe_strtou32_base(nibble, &nibble_int, 16);
    int which = 1 << (c % 4);
    EXPECT_THAT(nibble_int & which, Eq(which));
  }

  EXPECT_STRCASEEQ(cpumask.c_str(), expect);
}

TEST(TopologyTest, CpuMaskStr) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);

  TestList(TestTopology(), {1, 2, 3, 4, 5, 20, 87, 94, 100},
           "10,40800000,00000000,0010003e");
  TestList(TestTopology(), {0, 1, 2, 3, 16}, "1000f");
  TestList(TestTopology(), {4, 5, 6, 7, 31, 105},
           "200,00000000,00000000,800000f0");
  TestList(TestTopology(),
           {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16,
            17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32},
           "1,ffffffff");
  TestList(TestTopology(), {0, 5, 10, 15, 16, 21, 26, 31}, "84218421");
  TestList(TestTopology(), {0, 5, 10, 15, 16, 21, 26, 31, 32}, "1,84218421");
}

TEST(TopologyTest, ParseCpuStr) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);

  // tuple[0] is the string to parse. tuple[1] is the expected CpuList.
  std::vector<std::tuple<std::string, CpuList>> test_cases;

  test_cases.emplace_back("", TestTopology()->EmptyCpuList());
  test_cases.emplace_back("2", TestTopology()->ToCpuList(std::vector<int>{2}));
  test_cases.emplace_back(
      "3,5,6,7", TestTopology()->ToCpuList(std::vector<int>{3, 5, 6, 7}));
  test_cases.emplace_back(
      "2-8", TestTopology()->ToCpuList(std::vector<int>{2, 3, 4, 5, 6, 7, 8}));
  test_cases.emplace_back(
      "2-8,11-12",
      TestTopology()->ToCpuList(std::vector<int>{2, 3, 4, 5, 6, 7, 8, 11, 12}));
  test_cases.emplace_back("0-3,6,10-12,14",
                          TestTopology()->ToCpuList(
                              std::vector<int>{0, 1, 2, 3, 6, 10, 11, 12, 14}));
  test_cases.emplace_back("2,3,2",
                          TestTopology()->ToCpuList(std::vector<int>{2, 3}));
  test_cases.emplace_back(
      "0-5,3,4", TestTopology()->ToCpuList(std::vector<int>{0, 1, 2, 3, 4, 5}));
  test_cases.emplace_back("0-5,3-7", TestTopology()->ToCpuList(std::vector<int>{
                                         0, 1, 2, 3, 4, 5, 6, 7}));

  for (const auto& test_case : test_cases) {
    auto& [str, expected_list] = test_case;

    CpuList parsed = TestTopology()->ParseCpuStr(str);
    EXPECT_EQ(parsed, expected_list);
  }
}

TEST(TopologyTest, CpuInNodeTest) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  CpuList cpus_on_node0 = TestTopology()->CpusOnNode(0);
  auto cpus_on_node1 = TestTopology()->CpusOnNode(1);
  cpus_on_node0.Intersection(cpus_on_node1);
  EXPECT_EQ(cpus_on_node0.Size(), 0);
  cpus_on_node0 = TestTopology()->CpusOnNode(0);
  cpus_on_node0.Union(cpus_on_node1);
  EXPECT_EQ(cpus_on_node0.Size(), TestTopology()->all_cpus().Size());
}

// Test conversion of cpu_set_t to CpuList.
TEST(TopologyTest, CpuSetToCpuList) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  CpuList list = TestTopology()->EmptyCpuList();
  int stride = 1;
  for (int i = 0; i < TestTopology()->num_cpus(); i += stride++) {
    list.Set(i);
  }

  const cpu_set_t cpuset = Topology::ToCpuSet(list);
  EXPECT_EQ(list, TestTopology()->ToCpuList(cpuset));
}

// Returns the ID for the core that CPU `cpu` is on.
int GetRawCore(int cpu, bool use_consecutive_smt_numbering) {
  if (use_consecutive_smt_numbering) {
    return cpu / 2;
  }
  return cpu % (Topology::kNumTestCpus / 2);
}

// Returns the NUMA node that CPU `cpu` is on.
int GetRawNumaNode(int cpu) {
  const int quadrant = cpu / (Topology::kNumTestCpus / 4);
  switch (quadrant) {
    case 0:
    case 2:
      return 0;
    case 1:
    case 3:
      return 1;
    default:
      CHECK(false);
      return -1;
  }
}

// Returns the raw format of a topology that is identical to `TestTopology()`.
//
// Repeated from the comments for `UpdateTestTopology()` in lib/topology.h:
// This topology has 112 CPUs, 2 hardware threads per physical core (so there
// are 56 physical cores in total), and 2 NUMA nodes. If
// `use_consecutive_smt_numbering` is false, CPU 0 is co-located with CPU 56 on
// the same physical core, CPU 1 is co-located with CPU 57, ..., and CPU 55 is
// co-located with CPU 111. Otherwise, CPU 0 is co-located with CPU 1, CPU 2 is
// co-located with CPU 3, etc. Lastly, CPUs 0-27 and 56-83 are on NUMA node 0
// and CPUs 28-55 and 84-111 are on NUMA node 1 when we have
// !use_consecutive_smt_numbering. Otherwise, NUMA 0 has CPUs 0-55 and NUMA 1
// has CPUs 56-111.
//
// If `has_l3_cache` is true, an L3 cache is created. All CPUs in a NUMA node
// share the same L3 cache. If `has_l3_cache` is false, then the topology is
// configured as though the microarchitecture does not have an L3 cache.
std::vector<Cpu::Raw> GetRawCustomTopology(
    bool has_l3_cache, bool use_consecutive_smt_numbering = false) {
  std::vector<Cpu::Raw> raw_cpus;

  for (int i = 0; i < Topology::kNumTestCpus; i++) {
    Cpu::Raw raw_cpu;
    raw_cpu.cpu = i;
    raw_cpu.core = GetRawCore(
        /*cpu=*/i,
        /*use_consecutive_smt_numbering=*/use_consecutive_smt_numbering);
    if (use_consecutive_smt_numbering) {
      raw_cpu.smt_idx = i % 2;
    } else {
      raw_cpu.smt_idx = i / (Topology::kNumTestCpus / 2);
    }
    for (int j = 0; j < Topology::kNumTestCpus; j++) {
      // For CPU `j`, we want both `j` and the sibling(s) of `j` to be in the
      // siblings list.
      if (GetRawCore(/*cpu=*/j, /*use_consecutive_smt_numbering=*/
                     use_consecutive_smt_numbering) == raw_cpu.core) {
        raw_cpu.siblings.push_back(j);
      }
    }
    raw_cpu.numa_node = GetRawNumaNode(/*cpu=*/i);

    if (has_l3_cache) {
      for (int j = 0; j < Topology::kNumTestCpus; j++) {
        // For CPU `j`, we want both `j` and the sibling(s) of `j` to be in the
        // siblings list.
        if (GetRawNumaNode(/*cpu=*/j) == raw_cpu.numa_node) {
          raw_cpu.l3_siblings.push_back(j);
        }
      }
    }

    raw_cpus.push_back(raw_cpu);
  }
  return raw_cpus;
}

// Tests that `num_cpus()` returns number of CPUs in the custom topology.
TEST(TopologyTest, CustomTopologyNumCpus) {
  UpdateCustomTopology(GetRawCustomTopology(/*has_l3_cache=*/true));
  EXPECT_THAT(CustomTopology()->all_cpus().Size(),
              Eq(CustomTopology()->num_cpus()));
}

// Tests that the custom topology contains all CPUs.
TEST(TopologyTest, CustomTopologyAllCpus) {
  UpdateCustomTopology(GetRawCustomTopology(/*has_l3_cache=*/true));
  for (uint32_t i = 0; i < Topology::kNumTestCpus; i++) {
    EXPECT_THAT(CustomTopology()->cpu(i).id(), Eq(i));
  }
}

// Tests that `ToCpuList` returns an empty list when there are no CPUs.
TEST(TopologyTest, CustomCpuListContentsEmpty) {
  UpdateCustomTopology(GetRawCustomTopology(/*has_l3_cache=*/true));
  EXPECT_THAT(CustomTopology()->ToCpuList(std::vector<int>()).Empty(),
              IsTrue());
}

// Tests that `ToCpuList` returns a correct list of the specified CPUs.
TEST(TopologyTest, CustomCpuListContents) {
  std::vector<int> cpu_ids = std::vector<int>{0, 1, 2};
  UpdateCustomTopology(GetRawCustomTopology(/*has_l3_cache=*/true));
  CpuList cpu_list = CustomTopology()->ToCpuList(cpu_ids);
  absl::flat_hash_set<int> expected_contents(cpu_ids.begin(), cpu_ids.end());

  for (const Cpu& cpu : cpu_list) {
    EXPECT_THAT(expected_contents.erase(cpu.id()), Eq(1));
  }
  EXPECT_THAT(expected_contents.empty(), IsTrue());
}

// Tests that `ToCpuSet` returns an empty list when there are no CPUs.
TEST(TopologyTest, CustomCpuSetContentEmpty) {
  UpdateCustomTopology(GetRawCustomTopology(/*has_l3_cache=*/true));
  CpuList cpu_list = CustomTopology()->ToCpuList(std::vector<int>());
  cpu_set_t cpu_set = Topology::ToCpuSet(cpu_list);
  EXPECT_THAT(CPU_COUNT(&cpu_set), Eq(0));
}

// Tests that `ToCpuSet` returns a correct list of the specified CPUs.
TEST(TopologyTest, CustomCpuSetContents) {
  UpdateCustomTopology(GetRawCustomTopology(/*has_l3_cache=*/true));
  std::vector<int> cpu_ids = std::vector<int>{0, 1, 2};
  CpuList cpu_list = CustomTopology()->ToCpuList(cpu_ids);
  cpu_set_t cpu_set = Topology::ToCpuSet(cpu_list);
  absl::flat_hash_set<int> expected_contents(cpu_ids.begin(), cpu_ids.end());

  for (int i = 0; i < CPU_SETSIZE; i++) {
    if (CPU_ISSET(i, &cpu_set)) {
      EXPECT_THAT(expected_contents.erase(i), Eq(1));
    }
  }
  EXPECT_THAT(expected_contents.empty(), IsTrue());
}

// Tests that `all_cores()` returns all physical cores in the custom topology.
TEST(TopologyTest, CustomCheckAllCores) {
  UpdateCustomTopology(GetRawCustomTopology(/*has_l3_cache=*/true));
  std::vector<Cpu> expected;
  for (int i = 0; i < CustomTopology()->num_cpus() / 2; i++) {
    expected.push_back(CustomTopology()->cpu(i));
  }
  EXPECT_THAT(CustomTopology()->all_cores(),
              Eq(CustomTopology()->ToCpuList(expected)));
}

// Tests that each CPU is associated with the correct physical core.
TEST(TopologyTest, CustomCheckCores) {
  UpdateCustomTopology(GetRawCustomTopology(/*has_l3_cache=*/true));
  for (int i = 0; i < CustomTopology()->num_cpus() / 2; i++) {
    Cpu expected_core = CustomTopology()->cpu(i);

    Cpu first_cpu = CustomTopology()->cpu(i);
    EXPECT_THAT(CustomTopology()->Core(first_cpu), Eq(expected_core));

    // CPU 0 is co-located with CPU 56, CPU 1 is co-located with CPU 57, ...,
    // CPU 55 is co-located with CPU 111.
    int sibling = i + CustomTopology()->num_cpus() / 2;
    Cpu second_cpu = CustomTopology()->cpu(sibling);
    EXPECT_THAT(CustomTopology()->Core(second_cpu), Eq(expected_core));
  }
}

// Tests that each CPU returns the correct siblings. The CPU's siblings are all
// CPUs co-located on its physical core (including itself).
TEST(TopologyTest, CustomCheckSiblings) {
  UpdateCustomTopology(GetRawCustomTopology(/*has_l3_cache=*/true));
  for (int i = 0; i < CustomTopology()->num_cpus() / 2; i++) {
    // CPU 0 is co-located with CPU 56, CPU 1 is co-located with CPU 57, ...,
    // CPU 55 is co-located with CPU 111.
    int sibling = i + CustomTopology()->num_cpus() / 2;
    CpuList expected =
        CustomTopology()->ToCpuList(std::vector<int>{i, sibling});

    // Check the siblings for CPU `i`.
    CpuList siblings = CustomTopology()->cpu(i).siblings();
    EXPECT_THAT(siblings, Eq(expected));

    // Check the siblings for CPU `sibling`.
    siblings = CustomTopology()->cpu(sibling).siblings();
    EXPECT_THAT(siblings, Eq(expected));
  }
}

// Tests that each CPU returns an expected list of L3 cache siblings.
// e.g. On a 112 CPU system.
// CPUs [0 - 27] share L3 with [56 - 83]
// CPUs [28 - 55] share L3 with [84 - 111]
TEST(TopologyTest, CustomCheckL3Siblings) {
  UpdateCustomTopology(GetRawCustomTopology(/*has_l3_cache=*/true));
  int sibling_offset = CustomTopology()->num_cpus() / 2;
  int l3_offset = sibling_offset / 2;

  for (int i = 0; i < sibling_offset; i += l3_offset) {
    CpuList expected = CustomTopology()->EmptyCpuList();
    for (int j = i; j < (i + l3_offset); j++) {
      expected.Set(j);
      expected.Set(j + sibling_offset);
    }

    for (int j = i; j < (i + l3_offset); j++) {
      // Check the L3 siblings for CPU `j`.
      CpuList siblings = CustomTopology()->cpu(j).l3_siblings();
      EXPECT_THAT(siblings, Eq(expected));

      // Check the L3 siblings for sibling of CPU `j`.
      siblings = CustomTopology()->cpu(j + sibling_offset).l3_siblings();
      EXPECT_THAT(siblings, Eq(expected));
    }
  }
}

// Tests that on a microarchitecture with no L3 cache, each CPU returns an empty
// list of L3 cache siblings.
TEST(TopologyTest, CustomCheckEmptyL3Siblings) {
  UpdateCustomTopology(GetRawCustomTopology(/*has_l3_cache=*/false));

  for (const Cpu& cpu : CustomTopology()->all_cpus()) {
    EXPECT_THAT(cpu.l3_siblings().Empty(), IsTrue());
  }
}

// Tests that the custom topology returns the highest NUMA node.
TEST(TopologyTest, CustomCheckHighestNodeIdx) {
  UpdateCustomTopology(GetRawCustomTopology(/*has_l3_cache=*/true));
  EXPECT_THAT(CustomTopology()->highest_node_idx(), Eq(1));
}

// Tests that each CPU is assigned the correct SMT index.
TEST(TopologyTest, CustomCheckSmtIdx) {
  UpdateCustomTopology(GetRawCustomTopology(/*has_l3_cache=*/true));

  // We don't have any larger SMT toplogies.
  EXPECT_THAT(CustomTopology()->smt_count(), Le(2));

  for (int i = 0; i < CustomTopology()->num_cpus(); i++) {
    Cpu cpu = CustomTopology()->cpu(i);
    CpuList siblings = cpu.siblings();
    bool found = false;
    for (int j = 0; j < siblings.Size(); j++) {
      Cpu sibling = siblings.GetNthCpu(j);
      ASSERT_THAT(sibling.valid(), IsTrue());

      // Siblings should be monotonically increasing.
      if (j > 0) {
        EXPECT_THAT(sibling.id(), Gt(siblings.GetNthCpu(j - 1).id()));
      }

      if (sibling.id() == cpu.id()) {
        found = true;
        EXPECT_THAT(cpu.smt_idx(), Eq(j));
        const int expected_smt_idx =
            (cpu.id() < CustomTopology()->num_cpus() / 2) ? 0 : 1;
        EXPECT_THAT(cpu.smt_idx(), Eq(expected_smt_idx));
      }
    }
    EXPECT_THAT(found, IsTrue());
  }
}

// Test consecutive SMT numbering with the custom topology.
TEST(TopologyTest, CustomConsecutiveSmtNumbering) {
  UpdateCustomTopology(GetRawCustomTopology(
      /*has_l3_cache=*/true, /*use_consecutive_smt_numbering=*/true));

  // We don't have any larger SMT toplogies.
  EXPECT_THAT(CustomTopology()->smt_count(), Le(2));

  for (int i = 0; i < CustomTopology()->num_cpus(); i++) {
    const Cpu& cpu = CustomTopology()->cpu(i);
    const CpuList& siblings = cpu.siblings();
    bool found = false;
    for (int j = 0; j < siblings.Size(); j++) {
      const Cpu& sibling = siblings.GetNthCpu(j);
      ASSERT_THAT(sibling.valid(), IsTrue());
      EXPECT_THAT(cpu.smt_idx(), Eq(cpu.id() % CustomTopology()->smt_count()));
      EXPECT_THAT(cpu.core(), Eq(cpu.id() / CustomTopology()->smt_count()));

      if (cpu == sibling) {
        continue;
      }

      found = true;
      EXPECT_THAT(abs(cpu.id() - sibling.id()), Eq(1));
    }
    EXPECT_THAT(found, IsTrue());
  }
}

// Tests that immediately dereferencing the iterator returned by `begin()` for
// the custom topology's CPUs returns the first CPU.
TEST(TopologyTest, CustomIteratorBegin) {
  UpdateCustomTopology(GetRawCustomTopology(/*has_l3_cache=*/true));
  EXPECT_THAT(CustomTopology()->all_cpus().begin()->id(), Eq(0));
}

// Tests that immediately dereferencing the iterator returned by `end()` for the
// custom topology's CPUs returns an invalid CPU.
TEST(TopologyTest, CustomIteratorEnd) {
  UpdateCustomTopology(GetRawCustomTopology(/*has_l3_cache=*/true));
  EXPECT_THAT(CustomTopology()->all_cpus().end()->valid(), IsFalse());
}

// Tests that the iterator iterates over the `CpuList` correctly with a for
// loop.
TEST(TopologyTest, CustomIterator) {
  UpdateCustomTopology(GetRawCustomTopology(/*has_l3_cache=*/true));
  std::vector<int> cpus = {5, 20, 87, 94, 100};
  CpuList list = CustomTopology()->ToCpuList(cpus);

  int index = 0;
  auto it = list.begin();
  for (; it != list.end(); it++) {
    EXPECT_THAT(it->id(), Eq(cpus[index++]));
  }
  EXPECT_THAT(it, Eq(list.end()));
  // Since `it` == `list.end()`, `it` should point to an invalid CPU.
  EXPECT_THAT(it->valid(), IsFalse());
  // Check that the for loop only iterated once per CPU in `list`.
  EXPECT_THAT(index, Eq(cpus.size()));
}

// Tests that the iterator iterates over the `CpuList` correctly with a
// range-based for loop.
TEST(TopologyTest, CustomIteratorRange) {
  UpdateCustomTopology(GetRawCustomTopology(/*has_l3_cache=*/true));
  std::vector<int> cpus = {5, 20, 87, 94, 100};
  CpuList list = CustomTopology()->ToCpuList(cpus);

  int index = 0;
  for (const Cpu& cpu : list) {
    EXPECT_THAT(cpu.id(), Eq(cpus[index++]));
  }
  // Check that the range-based for loop only iterated once per CPU in `list`.
  EXPECT_THAT(index, Eq(cpus.size()));
}

// Tests that the iterator works properly when the first and last CPUs are set
// in the `CpuList` (i.e., tests the boundary cases).
TEST(TopologyTest, CustomIteratorBoundary) {
  UpdateCustomTopology(GetRawCustomTopology(/*has_l3_cache=*/true));
  std::vector<int> cpus = {0, 20, 111};
  CpuList list = CustomTopology()->ToCpuList(cpus);

  int index = 0;
  auto it = list.begin();
  for (; it != list.end(); it++) {
    EXPECT_THAT(it->id(), Eq(cpus[index++]));
  }
  EXPECT_THAT(it, Eq(list.end()));
  // Since `it` == `list.end()`, `it` should point to an invalid CPU.
  EXPECT_THAT(it->valid(), IsFalse());
  // Check that the for loop only iterated once per CPU in `list`.
  EXPECT_THAT(index, Eq(cpus.size()));
}

// Tests that `ToVector()` returns a correct list of the topology CPUs.
TEST(TopologyTest, CpuListToVector) {
  const std::vector<int> cpu_ids = std::vector<int>{0, 1, 2};
  UpdateCustomTopology(GetRawCustomTopology(/*has_l3_cache=*/true));

  CpuList cpu_list = CustomTopology()->ToCpuList(cpu_ids);
  std::vector<Cpu> compare = cpu_list.ToVector();
  std::sort(compare.begin(), compare.end());

  EXPECT_THAT(cpu_ids.size(), Eq(compare.size()));
  for (int i = 0; i < cpu_ids.size(); i++) {
    EXPECT_THAT(cpu_ids[i], Eq(compare[i].id()));
  }
}

// Tests that `ToIntVector()` returns a correct list of the topology CPU IDs.
TEST(TopologyTest, CpuListToIntVector) {
  const std::vector<int> cpu_ids = std::vector<int>{0, 1, 2};
  UpdateCustomTopology(GetRawCustomTopology(/*has_l3_cache=*/true));

  CpuList cpu_list = CustomTopology()->ToCpuList(cpu_ids);
  std::vector<int> compare = cpu_list.ToIntVector();
  std::sort(compare.begin(), compare.end());

  // std::vector overrides the `==` operator. Two std::vector's are equal if
  // they are the same length and each element in one vector is equal to the
  // element in the same index in the other vector.
  EXPECT_THAT(cpu_ids, Eq(compare));
}

// Tests that when a `Topology` instance is constructed with a raw topology,
// `Export()` returns the same raw topology.
TEST(TopologyTest, ExportTopology) {
  const std::vector<Cpu::Raw> raw_cpus =
      GetRawCustomTopology(/*has_l3_cache=*/true);

  UpdateCustomTopology(raw_cpus);
  std::vector<Cpu::Raw> compare = CustomTopology()->Export();
  std::sort(compare.begin(), compare.end());

  // std::vector overrides the `==` operator. Two std::vector's are equal if
  // they are the same length and each element in one vector is equal to the
  // element in the same index in the other vector.
  EXPECT_THAT(raw_cpus, Eq(compare));
}

// Tests that when a `Topology` instance is constructed with a raw topology that
// has no L3 cache, `Export()` returns the same raw topology.
TEST(TopologyTest, ExportTopologyWithNoL3Cache) {
  const std::vector<Cpu::Raw> raw_cpus =
      GetRawCustomTopology(/*has_l3_cache=*/false);

  UpdateCustomTopology(raw_cpus);
  std::vector<Cpu::Raw> compare = CustomTopology()->Export();
  std::sort(compare.begin(), compare.end());

  // std::vector overrides the `==` operator. Two std::vector's are equal if
  // they are the same length and each element in one vector is equal to the
  // element in the same index in the other vector.
  EXPECT_THAT(raw_cpus, Eq(compare));
}

// Tests that `Export()` exports the test topology correctly.
TEST(TopologyTest, ExportTestTopology) {
  const std::vector<Cpu::Raw> raw_cpus =
      GetRawCustomTopology(/*has_l3_cache=*/true);

  // The test topology and the custom topology from `GetRawCustomTopology()` are
  // the same.
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  std::vector<Cpu::Raw> compare = TestTopology()->Export();
  std::sort(compare.begin(), compare.end());

  // std::vector overrides the `==` operator. Two std::vector's are equal if
  // they are the same length and each element in one vector is equal to the
  // element in the same index in the other vector.
  EXPECT_THAT(raw_cpus, Eq(compare));
}

// Tests that `Export()` exports the test topology correctly when the
// microarchitecture has no L3 cache.
TEST(TopologyTest, ExportTestTopologyWithNoL3Cache) {
  const std::vector<Cpu::Raw> raw_cpus =
      GetRawCustomTopology(/*has_l3_cache=*/false);

  // The test topology and the custom topology from `GetRawCustomTopology()` are
  // the same.
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/false);
  std::vector<Cpu::Raw> compare = TestTopology()->Export();
  std::sort(compare.begin(), compare.end());

  // std::vector overrides the `==` operator. Two std::vector's are equal if
  // they are the same length and each element in one vector is equal to the
  // element in the same index in the other vector.
  EXPECT_THAT(raw_cpus, Eq(compare));
}

// Tests that CpusOnNode works correctly when we have >2 NUMA nodes.
TEST(TopologyTest, CpusOnNode) {
  constexpr int kNumNodes = 4;
  std::vector<Cpu::Raw> cpus;
  for (int i = 0; i < kNumNodes; i++) {
    Cpu::Raw cpu;
    cpu.cpu = i;
    cpu.core = i;
    cpu.smt_idx = 0;
    cpu.numa_node = i;
    cpu.siblings.push_back(i);
    cpu.l3_siblings.push_back(i);
    cpus.push_back(cpu);
  }

  UpdateCustomTopology(cpus);

  for (int i = 0; i < kNumNodes; i++) {
    CpuList cpus_on_node = CustomTopology()->CpusOnNode(i);
    EXPECT_THAT(cpus_on_node.Size(), Eq(1));
    EXPECT_THAT(cpus_on_node.Front().id(), Eq(i));
  }
}
// Tests the basic `Set`, `IsSet`, and `Clear` functionality of `AtomicCpuMap`.
TEST(TopologyTest, AtomicBitmapSet) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  AtomicCpuMap map = TestTopology()->EmptyAtomicCpuMap();

  std::vector<int> cpus = {5, 20, 87, 94, 100};
  for (int cpu : cpus) {
    map.Set(cpu);
  }
  for (int i = 0; i < MAX_CPUS; i++) {
    bool exists = (std::find(cpus.begin(), cpus.end(), i) != cpus.end());
    EXPECT_THAT(map.IsSet(i), Eq(exists));
  }

  EXPECT_THAT(map.Size(), Eq(cpus.size()));
  for (int i = 0; i < cpus.size(); i++) {
    map.Clear(cpus[i]);
    EXPECT_THAT(map.IsSet(cpus[i]), IsFalse());
  }
  EXPECT_THAT(map.Size(), Eq(0));
}

// Tests that the iterator iterates over the `AtomicCpuMap` correctly with a for
// loop.
TEST(TopologyTest, AtomicIterator) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  AtomicCpuMap map = TestTopology()->EmptyAtomicCpuMap();
  std::vector<int> cpus = {5, 20, 87, 94, 100};
  for (int cpu : cpus) {
    map.Set(cpu);
  }

  int index = 0;
  auto it = map.begin();
  for (; it != map.end(); it++) {
    EXPECT_THAT(it->id(), Eq(cpus[index++]));
  }
  EXPECT_THAT(it, Eq(map.end()));
  // Since `it` == `list.end()`, `it` should point to an invalid CPU.
  EXPECT_THAT(it->valid(), IsFalse());
  // Check that the for loop only iterated once per CPU in `list`.
  EXPECT_THAT(index, Eq(cpus.size()));
}

// Tests that the iterator iterates over the `AtomicCpuMap` correctly with a
// range-based for loop.
TEST(TopologyTest, AtomicIteratorRange) {
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/true);
  AtomicCpuMap map = TestTopology()->EmptyAtomicCpuMap();
  std::vector<int> cpus = {5, 20, 87, 94, 100};
  for (int cpu : cpus) {
    map.Set(cpu);
  }

  int index = 0;
  for (const Cpu& cpu : map) {
    EXPECT_THAT(cpu.id(), Eq(cpus[index++]));
  }
  // Check that the range-based for loop only iterated once per CPU in `list`.
  EXPECT_THAT(index, Eq(cpus.size()));
}

// Tests that multiple threads racing over trying to do an
// AtomicCpuMap::TestAndClear on the same cpu will only be able to clear the cpu
// once.
TEST(TopologyTest, AtomicTestAndClear) {
  AtomicCpuMap map = TestTopology()->EmptyAtomicCpuMap();
  for (const Cpu& cpu : TestTopology()->all_cpus()) {
    map.Set(cpu);
  }
  const Cpu& target_cpu = TestTopology()->all_cpus().GetNthCpu(0);
  map.Clear(target_cpu);
  std::atomic<int> num_clears = 0;
  Notification done;

  std::vector<std::unique_ptr<GhostThread>> threads;
  for (int i = 0; i < 5; i++) {
    threads.emplace_back(
        new GhostThread(GhostThread::KernelScheduler::kCfs,
                        [&map, &target_cpu, &num_clears, &done] {
                          while (true) {
                            if (done.HasBeenNotified()) {
                              return;
                            }
                            if (map.TestAndClear(target_cpu)) {
                              num_clears++;
                            }
                          }
                        }));
  }

  constexpr int n = 50;
  for (int i = 0; i < n; i++) {
    map.Set(target_cpu);
    absl::SleepFor(absl::Microseconds(1));
    while (map.IsSet(target_cpu)) {
    }
  }
  done.Notify();

  // Wait for all threads to finish.
  for (std::unique_ptr<GhostThread>& t : threads) t->Join();

  EXPECT_THAT(num_clears, Eq(n));
}

TEST(TopologyTest, CpuMapEmpty) {
  CpuList l = TestTopology()->EmptyCpuList();

  EXPECT_THAT(l.Empty(), IsTrue());

  l.Set(0);
  EXPECT_THAT(l.Empty(), IsFalse());
  l.Clear(0);
  EXPECT_THAT(l.Empty(), IsTrue());

  l.Set(TestTopology()->all_cpus().Size() - 1);
  EXPECT_THAT(l.Empty(), IsFalse());
}

TEST(TopologyTest, WrappedCpuListTest) {
  uint64_t map[CpuMap::kMapCapacity] = {0};

  WrappedCpuList l(*TestTopology(), map, CpuMap::kMapCapacity);

  for (int slot = 0; slot < l.map_size(); slot++) {
    constexpr int kCpu0Base = 2;
    int cpu0 = kCpu0Base + slot * CpuMap::kIntsBits;
    EXPECT_THAT(l.Empty(), IsTrue());
    l.Set(cpu0);
    EXPECT_THAT(l.Empty(), IsFalse());
    EXPECT_THAT(l.IsSet(cpu0), IsTrue());
    EXPECT_THAT(l.Size(), Eq(1));
    EXPECT_EQ(
        map[slot],
        1ULL << kCpu0Base);  // Make sure that updates are going to our map.

    l.Clear(cpu0);
    EXPECT_THAT(l.Empty(), IsTrue());
    EXPECT_THAT(l.IsSet(cpu0), IsFalse());
    EXPECT_THAT(l.Size(), Eq(0));
    EXPECT_EQ(map[slot], 0);

    // Now test from the other direction; updates directly to our map.
    constexpr int kCpu1Base = 3;
    int cpu1 = kCpu1Base + slot * CpuMap::kIntsBits;
    map[slot] = 1ULL << kCpu1Base;
    EXPECT_THAT(l.Empty(), IsFalse());
    EXPECT_THAT(l.IsSet(cpu1), IsTrue());
    EXPECT_THAT(l.Size(), Eq(1));

    map[slot] = 0;
    EXPECT_THAT(l.Empty(), IsTrue());
    EXPECT_THAT(l.IsSet(cpu1), IsFalse());
    EXPECT_THAT(l.Size(), Eq(0));
  }
  // Rest of the test case assumes the map is empty now.
  ASSERT_THAT(l.Empty(), IsTrue());

  // Comparison between regular CpuList and WrappedCpuList.
  CpuList l2 = TestTopology()->EmptyCpuList();
  int num_cpus = TestTopology()->num_cpus();
  for (int i = 0; i < num_cpus; i += 5) {
    l.Set(i);
    l2.Set(i);
  }
  EXPECT_THAT(l == l2, IsTrue());
  l.Clear(num_cpus - 1);
  l2.Set(num_cpus - 1);
  EXPECT_THAT(l == l2, IsFalse());
}

}  // namespace
}  // namespace ghost

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
