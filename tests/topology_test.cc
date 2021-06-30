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

#include "lib/topology.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/flat_hash_set.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/strings/numbers.h"

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

// Tests that `num_cpus()` returns number of CPUs in the topology.
TEST(TopologyTest, TopologyNumCpus) {
  EXPECT_THAT(MachineTopology()->all_cpus().Size(),
              Eq(MachineTopology()->num_cpus()));
  Topology* test_topology = TestTopology(absl::GetFlag(FLAGS_test_tmpdir));
  EXPECT_THAT(test_topology->all_cpus().Size(), Eq(test_topology->num_cpus()));
}

// Tests that `ToCpuList` returns an empty list when there are no CPUs.
TEST(TopologyTest, CpuListContentsEmpty) {
  EXPECT_THAT(TestTopology(absl::GetFlag(FLAGS_test_tmpdir))
                  ->ToCpuList(std::vector<int>())
                  .Empty(),
              IsTrue());
}

// Tests that `ToCpuList` returns a correct list of the specified CPUs.
TEST(TopologyTest, CpuListContents) {
  std::vector<int> cpu_ids = std::vector<int>{0, 1, 2};
  CpuList cpu_list =
      TestTopology(absl::GetFlag(FLAGS_test_tmpdir))->ToCpuList(cpu_ids);
  absl::flat_hash_set<int> expected_contents(cpu_ids.begin(), cpu_ids.end());

  for (const Cpu& cpu : cpu_list) {
    EXPECT_THAT(expected_contents.erase(cpu.id()), Eq(1));
  }
  EXPECT_THAT(expected_contents.empty(), IsTrue());
}

// Tests that `ToCpuSet` returns an empty list when there are no CPUs.
TEST(TopologyTest, CpuSetContentEmpty) {
  CpuList cpu_list = TestTopology(absl::GetFlag(FLAGS_test_tmpdir))
                         ->ToCpuList(std::vector<int>());
  cpu_set_t cpu_set = Topology::ToCpuSet(cpu_list);
  EXPECT_THAT(CPU_COUNT(&cpu_set), Eq(0));
}

// Tests that `ToCpuSet` returns a correct list of the specified CPUs.
TEST(TopologyTest, CpuSetContents) {
  std::vector<int> cpu_ids = std::vector<int>{0, 1, 2};
  CpuList cpu_list =
      TestTopology(absl::GetFlag(FLAGS_test_tmpdir))->ToCpuList(cpu_ids);
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
  std::vector<Cpu> expected;
  Topology* test_topology = TestTopology(absl::GetFlag(FLAGS_test_tmpdir));
  for (int i = 0; i < test_topology->num_cpus() / 2; i++) {
    expected.push_back(test_topology->cpu(i));
  }
  EXPECT_THAT(test_topology->all_cores(),
              Eq(test_topology->ToCpuList(expected)));
}

// Tests that each CPU is associated with the correct physical core.
TEST(TopologyTest, CheckCores) {
  Topology* test_topology = TestTopology(absl::GetFlag(FLAGS_test_tmpdir));
  for (int i = 0; i < test_topology->num_cpus() / 2; i++) {
    Cpu expected_core = test_topology->cpu(i);

    Cpu first_cpu = test_topology->cpu(i);
    EXPECT_THAT(test_topology->Core(first_cpu), Eq(expected_core));

    // CPU 0 is co-located with CPU 56, CPU 1 is co-located with CPU 57, ...,
    // CPU 55 is co-located with CPU 111.
    int sibling = i + test_topology->num_cpus() / 2;
    Cpu second_cpu = test_topology->cpu(sibling);
    EXPECT_THAT(test_topology->Core(second_cpu), Eq(expected_core));
  }
}

// Tests that each CPU returns the correct siblings. The CPU's siblings are all
// CPUs co-located on its physical core (including itself).
TEST(TopologyTest, CheckSiblings) {
  Topology* test_topology = TestTopology(absl::GetFlag(FLAGS_test_tmpdir));
  for (int i = 0; i < test_topology->num_cpus() / 2; i++) {
    // CPU 0 is co-located with CPU 56, CPU 1 is co-located with CPU 57, ...,
    // CPU 55 is co-located with CPU 111.
    int sibling = i + test_topology->num_cpus() / 2;
    CpuList expected = test_topology->ToCpuList(std::vector<int>{i, sibling});

    // Check the siblings for CPU `i`.
    CpuList siblings = test_topology->cpu(i).siblings();
    EXPECT_THAT(siblings, Eq(expected));

    // Check the siblings for CPU `sibling`.
    siblings = test_topology->cpu(sibling).siblings();
    EXPECT_THAT(siblings, Eq(expected));
  }
}

// Tests that each CPU returns an expected list of L3 cache siblings.
// e.g. On a 112 CPU system.
// CPUs [0 - 27] share L3 with [56 - 83]
// CPUs [28 - 55] share L3 with [84 - 111]
TEST(TopologyTest, CheckL3Siblings) {
  Topology* test_topology = TestTopology(absl::GetFlag(FLAGS_test_tmpdir));
  int sibling_offset = test_topology->num_cpus() / 2;
  int l3_offset = sibling_offset / 2;

  for (int i = 0; i < sibling_offset; i += l3_offset) {
    CpuList expected = test_topology->EmptyCpuList();
    for (int j = i; j < (i + l3_offset); j++) {
      expected.Set(j);
      expected.Set(j + sibling_offset);
    }

    for (int j = i; j < (i + l3_offset); j++) {
      // Check the L3 siblings for CPU `j`.
      CpuList siblings = test_topology->cpu(j).l3_siblings();
      EXPECT_THAT(siblings, Eq(expected));

      // Check the L3 siblings for sibling of CPU `j`.
      siblings = test_topology->cpu(j + sibling_offset).l3_siblings();
      EXPECT_THAT(siblings, Eq(expected));
    }
  }
}

TEST(TopologyTest, CheckHighestNodeIdx) {
  Topology* test_topology = TestTopology(absl::GetFlag(FLAGS_test_tmpdir));
  EXPECT_THAT(test_topology->highest_node_idx(), Eq(1));
}

// Tests that each CPU is assigned the correct SMT index.
TEST(TopologyTest, CheckSmtIdx) {
  Topology* test_topology = TestTopology(absl::GetFlag(FLAGS_test_tmpdir));

  // We don't have any larger SMT toplogies.
  EXPECT_THAT(test_topology->smt_count(), Le(2));

  for (int i = 0; i < test_topology->num_cpus(); i++) {
    Cpu cpu = test_topology->cpu(i);
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
            (cpu.id() < test_topology->num_cpus() / 2) ? 0 : 1;
        EXPECT_THAT(cpu.smt_idx(), Eq(expected_smt_idx));
      }
    }
    EXPECT_THAT(found, IsTrue());
  }
}

// Tests the basic `Set`, `IsSet`, and `Clear` functionality of `CpuList`.
TEST(TopologyTest, BitmapSet) {
  Topology* test_topology = TestTopology(absl::GetFlag(FLAGS_test_tmpdir));
  CpuList list = test_topology->EmptyCpuList();

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
  Topology* test_topology = TestTopology(absl::GetFlag(FLAGS_test_tmpdir));
  CpuList list = test_topology->EmptyCpuList();

  std::vector<Cpu> cpus = {test_topology->cpu(5), test_topology->cpu(20),
                           test_topology->cpu(87), test_topology->cpu(94),
                           test_topology->cpu(100)};
  for (const Cpu& cpu : cpus) {
    list.Set(cpu);
  }
  for (const Cpu& cpu : test_topology->all_cpus()) {
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
  Topology* test_topology = TestTopology(absl::GetFlag(FLAGS_test_tmpdir));
  CpuList list0 =
      test_topology->ToCpuList(std::vector<int>{5, 20, 87, 94, 100});
  CpuList list1 = test_topology->ToCpuList(
      std::vector<int>{5, 10, 60, 80, 87, 90, 101, 110});

  list0.Intersection(list1);
  EXPECT_THAT(list0, Eq(test_topology->ToCpuList(std::vector<int>{5, 87})));
}

// Tests that the `CpuList::Union` method calculates the correct union
// of two `CpuList`s. Also tests the related `+` and `+=` operators.
TEST(TopologyTest, Union) {
  Topology* test_topology = TestTopology(absl::GetFlag(FLAGS_test_tmpdir));
  const CpuList list0 =
      test_topology->ToCpuList(std::vector<int>{5, 20, 87, 94, 100});
  const CpuList list1 = test_topology->ToCpuList(
      std::vector<int>{5, 10, 60, 80, 87, 90, 101, 110});
  const CpuList result = test_topology->ToCpuList(
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
  Topology* test_topology = TestTopology(absl::GetFlag(FLAGS_test_tmpdir));
  const CpuList list0 =
      test_topology->ToCpuList(std::vector<int>{1, 3, 5, 20, 87, 94, 100});
  const CpuList list1 =
      test_topology->ToCpuList(std::vector<int>{0, 5, 10, 60, 94});
  const CpuList result = test_topology
                             ->ToCpuList(std::vector<int>{1, 3, 20, 87, 100});

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
  Topology* test_topology = TestTopology(absl::GetFlag(FLAGS_test_tmpdir));
  EXPECT_THAT(test_topology->all_cpus().begin()->id(), Eq(0));
}

// Tests that immediately dereferencing the iterator returned by `end()` for the
// topology's CPUs returns an invalid CPU.
TEST(TopologyTest, IteratorEnd) {
  Topology* test_topology = TestTopology(absl::GetFlag(FLAGS_test_tmpdir));
  EXPECT_THAT(test_topology->all_cpus().end()->valid(), IsFalse());
}

// Tests that the iterator iterates over the `CpuList` correctly with a for
// loop.
TEST(TopologyTest, Iterator) {
  Topology* test_topology = TestTopology(absl::GetFlag(FLAGS_test_tmpdir));
  std::vector<int> cpus = {5, 20, 87, 94, 100};
  CpuList list = test_topology->ToCpuList(cpus);

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
  Topology* test_topology = TestTopology(absl::GetFlag(FLAGS_test_tmpdir));
  std::vector<int> cpus = {5, 20, 87, 94, 100};
  CpuList list = test_topology->ToCpuList(cpus);

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
  Topology* test_topology = TestTopology(absl::GetFlag(FLAGS_test_tmpdir));
  std::vector<int> cpus = {0, 20, 111};
  CpuList list = test_topology->ToCpuList(cpus);

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
  Topology* test_topology = TestTopology(absl::GetFlag(FLAGS_test_tmpdir));

  TestList(test_topology, {1, 2, 3, 4, 5, 20, 87, 94, 100},
           "10,40800000,00000000,0010003e");
  TestList(test_topology, {0, 1, 2, 3, 16}, "1000f");
  TestList(test_topology, {4, 5, 6, 7, 31, 129},
           "2,00000000,00000000,00000000,800000f0");
  TestList(test_topology,
           {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16,
            17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32},
           "1,ffffffff");
  TestList(test_topology, {0, 5, 10, 15, 16, 21, 26, 31}, "84218421");
  TestList(test_topology, {0, 5, 10, 15, 16, 21, 26, 31, 32}, "1,84218421");
}

TEST(TopologyTest, ParseCpuStr) {
  Topology* test_topology = TestTopology(absl::GetFlag(FLAGS_test_tmpdir));

  // tuple[0] is the string to parse. tuple[1] is the expected CpuList.
  std::vector<std::tuple<std::string, CpuList>> test_cases;

  test_cases.emplace_back("", test_topology->EmptyCpuList());
  test_cases.emplace_back("2", test_topology->ToCpuList(std::vector<int>{2}));
  test_cases.emplace_back(
      "3,5,6,7", test_topology->ToCpuList(std::vector<int>{3, 5, 6, 7}));
  test_cases.emplace_back(
      "2-8", test_topology->ToCpuList(std::vector<int>{2, 3, 4, 5, 6, 7, 8}));
  test_cases.emplace_back(
      "2-8,11-12",
      test_topology->ToCpuList(std::vector<int>{2, 3, 4, 5, 6, 7, 8, 11, 12}));
  test_cases.emplace_back("0-3,6,10-12,14",
                          test_topology->ToCpuList(
                              std::vector<int>{0, 1, 2, 3, 6, 10, 11, 12, 14}));
  test_cases.emplace_back("2,3,2",
                          test_topology->ToCpuList(std::vector<int>{2, 3}));
  test_cases.emplace_back(
      "0-5,3,4", test_topology->ToCpuList(std::vector<int>{0, 1, 2, 3, 4, 5}));
  test_cases.emplace_back("0-5,3-7", test_topology->ToCpuList(std::vector<int>{
                                         0, 1, 2, 3, 4, 5, 6, 7}));

  for (const auto& test_case : test_cases) {
    auto& [str, expected_list] = test_case;

    CpuList parsed = test_topology->ParseCpuStr(str);
    EXPECT_EQ(parsed, expected_list);
  }
}

TEST(TopologyTest, CpuInNodeTest) {
  Topology* test_topology = TestTopology(absl::GetFlag(FLAGS_test_tmpdir));
  CpuList cpus_on_node0 = test_topology->CpusOnNode(0);
  auto cpus_on_node1 = test_topology->CpusOnNode(1);
  cpus_on_node0.Intersection(cpus_on_node1);
  EXPECT_EQ(cpus_on_node0.Size(), 0);
  cpus_on_node0 = test_topology->CpusOnNode(0);
  cpus_on_node0.Union(cpus_on_node1);
  EXPECT_EQ(cpus_on_node0.Size(), test_topology->all_cpus().Size());
}

// Test conversion of cpu_set_t to CpuList.
TEST(TopologyTest, CpuSetToCpuList) {
  Topology* test_topology = TestTopology(absl::GetFlag(FLAGS_test_tmpdir));
  CpuList list = test_topology->EmptyCpuList();
  int stride = 1;
  for (int i = 0; i < test_topology->num_cpus(); i += stride++) {
    list.Set(i);
  }

  const cpu_set_t cpuset = Topology::ToCpuSet(list);
  EXPECT_EQ(list, test_topology->ToCpuList(cpuset));
}

}  // namespace
}  // namespace ghost

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
