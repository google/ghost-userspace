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
#include "schedulers/fifo/centralized/fifo_scheduler.h"

namespace ghost {
namespace {

class EnclaveTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { Ghost::InitCore(); }
};

TEST_F(EnclaveTest, TxnRegionFree) {
  Topology* topology = MachineTopology();
  const AgentConfig config(topology, topology->all_cpus());

  // Create an enclave.
  auto enclave = absl::make_unique<LocalEnclave>(config);

  // Destroy the enclave - this should release the transaction region
  // buffers in the destructor.
  enclave.reset(nullptr);

  // Create another enclave with the same set of cpus.
  //
  // The constructor will CHECK fail if it cannot allocate the txn region
  // so if this succeeds the destructor is properly releasing the resource.
  enclave = absl::make_unique<LocalEnclave>(config);

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
  // This test is on the LocalEnclave, which means it applies to actual cpus.
  // Many tests require a certain number of cpus. This test requires at least 8.
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
  // This test is on the LocalEnclave, which means it applies to actual cpus.
  // Many tests require a certain number of cpus. This test requires at least 5.
  if (MachineTopology()->num_cpus() < 5) {
    GTEST_SKIP() << "must have at least 5 cpus";
    return;
  }

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
  CpuList comma_list = MachineTopology()->ToCpuList(
      std::vector<int>{0, 5, 10, 15, 16, 21, 26, 31, 32});
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

void SpinFor(absl::Duration d) {
  while (d > absl::ZeroDuration()) {
    absl::Time a = absl::Now();
    absl::Time b;

    // Try to minimize the contribution of arithmetic/Now() overhead.
    for (int i = 0; i < 150; ++i) {
      b = absl::Now();
    }

    absl::Duration t = b - a;

    // Don't count preempted time
    if (t < absl::Microseconds(100)) {
      d -= t;
    }
  }
}

// One thread destroys the enclave while another thread tries to join it.  The
// race is rare enough that this only failed on a loaded system or one with a
// hacked kernel to exploit the race.
TEST_F(EnclaveTest, DestroyAndSetsched) {
  constexpr int kLoops = 100;
  constexpr int kClientCpu = 0;
  constexpr int kDestroyerCpu = 1;

  // This test is on the LocalEnclave, which means it applies to actual cpus.
  // Many tests require a certain number of cpus. This test requires at least 2.
  if (MachineTopology()->num_cpus() < 2) {
    GTEST_SKIP() << "must have at least 2 cpus";
    return;
  }

  bool client_won = false;
  bool destroyer_won = false;
  bool failed = false;

  // To make the race more likely, both threads run on separate cpus and
  // synchronize via shared memory.  They vary the amount they SpinFor, such
  // that each thread should get a chance to win the race.
  for (int i = 0; i < kLoops; ++i) {
    int ctl_fd = LocalEnclave::MakeNextEnclave();
    CHECK_GE(ctl_fd, 0);

    int enclave_fd = LocalEnclave::GetEnclaveDirectory(ctl_fd);
    CHECK_GE(enclave_fd, 0);
    int client_fd = openat(enclave_fd, "ctl", O_RDONLY);
    CHECK_GE(client_fd, 0);
    close(enclave_fd);

    std::atomic<bool> client_ready = false;
    std::atomic<bool> destroyer_ready = false;

    std::thread client = std::thread([&] {
      cpu_set_t set;
      CPU_ZERO(&set);
      CPU_SET(kClientCpu, &set);
      CHECK_EQ(sched_setaffinity(0, sizeof(set), &set), 0);

      client_ready.store(true, std::memory_order_release);
      while (!destroyer_ready.load(std::memory_order_acquire))
        ;

      SpinFor(absl::Microseconds(kLoops) - absl::Microseconds(i));

      int ret = SchedTaskEnterGhost(/*pid=*/0, client_fd);
      if (ret != 0) {
        switch (errno) {
          case ENXIO:
          case ENOMEM:
            // The client won the race.  We setsched and grabbed the enclave
            // lock before it was dying.
            //
            // The race we are trying to expose is the enclave structure being
            // freed and reused, particularly the page fault on the enclave
            // lock.  ENXIO/ENOMEM happens after that.
            //
            // ENXIO happens on older kernels where we required the enclave had
            // a default queue to join.  ENOMEM happens on newer kernels, where
            // we fail to get a status word.  Both of these cases are artifacts
            // of the test: the enclave doesn't have queue nor a SW region
            client_won = true;
            break;
          case EBADF:
          case EXDEV:
            // The destroyer won the race and the enclave is dying.
            // - EBADF is when the enclave died and was removed from kernfs
            // before we could call kernfs_get_active().
            // - EXDEV is when we got the reference and were able to get the
            // struct ghost_enclave pointer, but it was already is_dying.
            destroyer_won = true;
            break;
          default:
            // We had some unexpected error when entering ghost, possibly
            // unrelated to the test.  errno is not 0, so EXPECT_EQ will fail
            // and print the current errno.
            EXPECT_EQ(errno, 0);
            failed = true;
        }
      }
    });

    std::thread destroyer = std::thread([&] {
      cpu_set_t set;
      CPU_ZERO(&set);
      CPU_SET(kDestroyerCpu, &set);
      CHECK_EQ(sched_setaffinity(0, sizeof(set), &set), 0);

      destroyer_ready.store(true, std::memory_order_release);
      while (!client_ready.load(std::memory_order_acquire))
        ;

      SpinFor(absl::Microseconds(i));

      LocalEnclave::DestroyEnclave(ctl_fd);
    });

    client.join();
    destroyer.join();

    close(client_fd);
    close(ctl_fd);

    ASSERT_FALSE(failed);
  }

  // If the client or destroyer never win, we didn't test the race, but it is
  // not a sign of a bug.
  EXPECT_TRUE(client_won);
  EXPECT_TRUE(destroyer_won);
}

TEST_F(EnclaveTest, GetNrTasks) {
  Topology* topology = MachineTopology();
  auto enclave = absl::make_unique<LocalEnclave>(
      AgentConfig(topology, topology->all_cpus()));

  EXPECT_EQ(enclave->GetNrTasks(), 0);
}

// Tests killing an enclave from ghostfs while an agent is running.  This broke
// in a couple ways: once with a kernfs "active protection" refcount, and again
// when the kernel ABI selection checked pcpu->enclave after it was cleared.
TEST_F(EnclaveTest, KillActiveEnclave) {
  // Anecdotally, this test works best if there are a few agent tasks blocked in
  // the kernel.  Sometimes a couple of the agent tasks exited when the bug was
  // present.
  if (MachineTopology()->num_cpus() < 4) {
    GTEST_SKIP() << "must have at least 4 cpus";
    return;
  }

  int ctl_fd = LocalEnclave::MakeNextEnclave();
  ASSERT_GE(ctl_fd, 0);
  int enclave_fd = LocalEnclave::GetEnclaveDirectory(ctl_fd);
  ASSERT_GE(enclave_fd, 0);
  int cpulist_fd = openat(enclave_fd, "cpulist", O_RDWR);
  ASSERT_GE(cpulist_fd, 0);
  std::string cpus = "0-3";
  ASSERT_EQ(write(cpulist_fd, cpus.c_str(), cpus.length()), cpus.length());
  EXPECT_EQ(close(cpulist_fd), 0);

  FifoConfig config;
  config.topology_ = MachineTopology();
  config.global_cpu_ = config.topology_->cpu(0);
  config.enclave_fd_ = enclave_fd;

  auto ap = new AgentProcess<FullFifoAgent<LocalEnclave>, FifoConfig>(config);
  EXPECT_EQ(close(enclave_fd), 0);

  ap->AddExitHandler([](pid_t child, int status) {
    // When we manually destroy the enclave, the AP will be SIGKILLed.  By
    // handling it (returning true) we prevent the AP parent (us) from exiting.
    return true;
  });

  // Destroys the enclave via ghostfs.  This is not a graceful shutdown.
  LocalEnclave::DestroyEnclave(ctl_fd);
  EXPECT_EQ(close(ctl_fd), 0);

  delete ap;
}

}  // namespace
}  // namespace ghost
