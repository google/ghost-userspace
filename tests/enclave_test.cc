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

#include "lib/enclave.h"

#include <sys/mman.h>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace ghost {
namespace {

class EnclaveTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { Ghost::InitCore(); }
};

TEST_F(EnclaveTest, TxnRegionFree) {
  Topology* topology = MachineTopology();
  const CpuList& all_cpus = topology->all_cpus();

  // Create an enclave.
  auto enclave = absl::make_unique<LocalEnclave>(topology, all_cpus);

  // Destroy the enclave - this should release the transaction region
  // buffers in the destructor.
  enclave.reset(nullptr);

  // Create another enclave with the same set of cpus.
  //
  // The constructor will CHECK fail if it cannot allocate the txn region
  // so if this succeeds the destructor is properly releasing the resource.
  enclave = absl::make_unique<LocalEnclave>(topology, all_cpus);

  SUCCEED();
}

TEST_F(EnclaveTest, MakeAnyEnclave) {
  int ctl_fd = LocalEnclave::MakeNextEnclave();
  CHECK_GE(ctl_fd, 0);
  LocalEnclave::DestroyEnclave(ctl_fd);
  close(ctl_fd);
}

static void StripSlashN(char* buf, size_t len) {
  char* p = static_cast<char*>(memchr(buf, '\n', len));
  if (p) {
    *p = '\0';
  }
}

// Checks that the cpumask and cpulist files agree with `mask` and `list`.
static void CheckSet(int cpumask_fd, std::string mask, int cpulist_fd,
                     std::string list) {
  char buf[4096];
  ssize_t ret;
  char* p;

  lseek(cpumask_fd, 0, SEEK_SET);
  ret = read(cpumask_fd, buf, sizeof(buf));
  CHECK_GT(ret, 0);
  StripSlashN(buf, ret);

  // Snarf leading zeros and commas; they depend on machine size.
  p = buf;
  while (*p == '0' || *p == ',') {
    ++p;
  }
  EXPECT_STRCASEEQ(p, mask.c_str());

  lseek(cpulist_fd, 0, SEEK_SET);
  ret = read(cpulist_fd, buf, sizeof(buf));
  CHECK_GT(ret, 0);
  StripSlashN(buf, ret);

  EXPECT_STRCASEEQ(buf, list.c_str());
}

// Sets the cpus and tests that they were set, by both cpumask and cpulist.
static void TestSetCpus(int cpumask_fd, std::string mask, int cpulist_fd,
                        std::string list) {
  CHECK_EQ(write(cpumask_fd, mask.c_str(), mask.length()), mask.length());
  CheckSet(cpumask_fd, mask, cpulist_fd, list);

  // Reset in between.
  CHECK_EQ(write(cpulist_fd, "", 0), 0);
  CheckSet(cpumask_fd, "", cpulist_fd, "");

  CHECK_EQ(write(cpulist_fd, list.c_str(), list.length()), list.length());
  CheckSet(cpumask_fd, mask, cpulist_fd, list);
}

TEST_F(EnclaveTest, SetCpus) {
  // These tests are on the LocalEnclave, which means they apply to actual cpus.
  // Many tests require a certain number of cpus.  The tests below require at
  // least 8.
  if (MachineTopology()->num_cpus() < 8) {
    GTEST_SKIP() << "must have at least 8 cpus";
    return;
  }

  int ctl_fd = LocalEnclave::MakeNextEnclave();
  CHECK_GE(ctl_fd, 0);
  int enclave_fd = LocalEnclave::GetEnclaveDirectory(ctl_fd);
  CHECK_GE(enclave_fd, 0);

  int cpumask_fd = openat(enclave_fd, "cpumask", O_RDWR);
  CHECK_GE(cpumask_fd, 0);
  int cpulist_fd = openat(enclave_fd, "cpulist", O_RDWR);
  CHECK_GE(cpulist_fd, 0);

  TestSetCpus(cpumask_fd, "1e", cpulist_fd, "1-4");
  TestSetCpus(cpumask_fd, "21", cpulist_fd, "0,5");
  TestSetCpus(cpumask_fd, "ff", cpulist_fd, "0-7");
  TestSetCpus(cpumask_fd, "4d", cpulist_fd, "0,2-3,6");

  close(cpulist_fd);
  close(cpumask_fd);

  close(enclave_fd);
  LocalEnclave::DestroyEnclave(ctl_fd);
  close(ctl_fd);
}

TEST_F(EnclaveTest, BadCpus) {
  int ctl_fd = LocalEnclave::MakeNextEnclave();
  CHECK_GE(ctl_fd, 0);
  int enclave_fd = LocalEnclave::GetEnclaveDirectory(ctl_fd);
  CHECK_GE(enclave_fd, 0);

  int cpumask_fd = openat(enclave_fd, "cpumask", O_RDWR);
  CHECK_GE(cpumask_fd, 0);
  int cpulist_fd = openat(enclave_fd, "cpulist", O_RDWR);
  CHECK_GE(cpulist_fd, 0);

  CHECK_EQ(write(cpumask_fd, "hello", 5), -1);
  CHECK_EQ(write(cpulist_fd, "-2", 2), -1);
  CHECK_EQ(write(cpulist_fd, "9999999999", 10), -1);

  close(cpulist_fd);
  close(cpumask_fd);

  close(enclave_fd);
  LocalEnclave::DestroyEnclave(ctl_fd);
  close(ctl_fd);
}

static void AddCpu(int enclave_fd, std::string cpu) {
  int cpulist_fd = openat(enclave_fd, "cpulist", O_RDWR);
  CHECK_GE(cpulist_fd, 0);
  CHECK_EQ(write(cpulist_fd, cpu.c_str(), cpu.length()), cpu.length());
  close(cpulist_fd);
}

static void MakeEnclaveAndLeakCpu1() {
  int ctl_fd = LocalEnclave::MakeNextEnclave();
  CHECK_GE(ctl_fd, 0);
  int enclave_fd = LocalEnclave::GetEnclaveDirectory(ctl_fd);
  CHECK_GE(enclave_fd, 0);
  AddCpu(enclave_fd, "1");
  close(enclave_fd);
  LocalEnclave::DestroyEnclave(ctl_fd);
  close(ctl_fd);
}

// When we destroy an enclave, the kernel should free its cpu.  We can test that
// by trying to add that cpu to another enclave.
TEST_F(EnclaveTest, CleanupLeakedCpus) {
  MakeEnclaveAndLeakCpu1();
  MakeEnclaveAndLeakCpu1();
}

static void CheckCpuMmap(void* mmap_addr, unsigned int cpuid) {
  struct ghost_cpu_data* lcd =
      &(reinterpret_cast<struct ghost_cpu_data*>(mmap_addr)[cpuid]);
  struct ghost_txn* txn = &lcd->txn;
  CHECK_EQ(txn->cpu, cpuid);
  CHECK_EQ(txn->version, GHOST_TXN_VERSION);
  // Can't use CHECK_EQ on these, since they are std::atomics and it'll trigger
  // a copy constructor.
  CHECK(txn->u.sync_group_owner == -1);
  CHECK(txn->state == GHOST_TXN_COMPLETE);
}

TEST_F(EnclaveTest, MmapTxn) {
  int ctl_fd = LocalEnclave::MakeNextEnclave();
  // TODO: Change all of these CHECKs to ASSERT/EXPECTS
  CHECK_GE(ctl_fd, 0);
  int enclave_fd = LocalEnclave::GetEnclaveDirectory(ctl_fd);
  CHECK_GE(enclave_fd, 0);

  int data_fd = LocalEnclave::GetCpuDataRegion(enclave_fd);
  CHECK_GE(data_fd, 0);
  void* addr = mmap(/*addr=*/nullptr, GetFileSize(data_fd),
                    PROT_READ | PROT_WRITE, MAP_SHARED, data_fd, /*offset=*/0);
  CHECK_NE(addr, MAP_FAILED);

  AddCpu(enclave_fd, "1");
  CheckCpuMmap(addr, 1);
  AddCpu(enclave_fd, "4");
  CheckCpuMmap(addr, 4);

  munmap(addr, GetFileSize(data_fd));
  close(data_fd);

  close(enclave_fd);
  LocalEnclave::DestroyEnclave(ctl_fd);
  close(ctl_fd);
}

TEST_F(EnclaveTest, CpuListComma) {
  int ctl_fd = LocalEnclave::MakeNextEnclave();
  CHECK_GE(ctl_fd, 0);
  int enclave_fd = LocalEnclave::GetEnclaveDirectory(ctl_fd);
  CHECK_GE(enclave_fd, 0);

  int cpumask_fd = openat(enclave_fd, "cpumask", O_RDWR);
  CHECK_GE(cpumask_fd, 0);

  // Linux's cpumask requires a comma between every 32 bit chunk (8 hex
  // nibbles).  We want to test having at least one comma. (1,84218421)
  CpuList comma_list =
      MachineTopology()->ToCpuList({0, 5, 10, 15, 16, 21, 26, 31, 32});
  std::string comma_str = comma_list.CpuMaskStr();

  ssize_t ret = write(cpumask_fd, comma_str.c_str(), comma_str.length());
  if (ret < 0) {
    // We can legitimately fail if we're on a machine with too few cpus, but
    // EOVERFLOW means our cpumask wasn't parsed by the kernel.
    CHECK_NE(errno, EOVERFLOW);
  } else {
    CHECK_EQ(ret, comma_str.length());
  }

  close(cpumask_fd);

  close(enclave_fd);
  LocalEnclave::DestroyEnclave(ctl_fd);
  close(ctl_fd);
}

}  // namespace
}  // namespace ghost
