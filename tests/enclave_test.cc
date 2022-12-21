// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "lib/enclave.h"

#include <sys/mman.h>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "schedulers/fifo/centralized/fifo_scheduler.h"

ABSL_FLAG(std::string, test_tmpdir, "/tmp",
          "A temporary file system directory that the test can access");

namespace ghost {
namespace {

class EnclaveTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { GhostHelper()->InitCore(); }
};

bool WriteEnclaveCpus(int fd, const std::string& data) {
  constexpr int kMaxRetries = 10;
  int retries = 0;

  do {
    int ret = write(fd, data.c_str(), data.length());
    if (ret == data.length()) {
      return true;
    }
    // go/kcl/466292 always defers enclave destruction to a CFS task.
    // This sometimes causes failures in unit tests because enclave
    // destruction from a prior test races with enclave creation in
    // the next one.
    //
    // Fix this by retrying a finite number of times as long as the
    // reason for failure matches the race described above (EBUSY).
    CHECK_EQ(ret, -1);
    if (errno != EBUSY) {
      break;
    }
    absl::SleepFor(absl::Milliseconds(50));
  } while (++retries < kMaxRetries);

  return false;
}

TEST_F(EnclaveTest, TxnRegionFree) {
  Topology* topology = MachineTopology();
  const AgentConfig config(topology, topology->all_cpus());

  // Create an enclave.
  auto enclave = std::make_unique<LocalEnclave>(config);

  // Destroy the enclave - this should release the transaction region
  // buffers in the destructor.
  enclave.reset(nullptr);

  // Create another enclave with the same set of cpus.
  //
  // The constructor will CHECK fail if it cannot allocate the txn region
  // so if this succeeds the destructor is properly releasing the resource.
  enclave = std::make_unique<LocalEnclave>(config);

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
  CHECK(WriteEnclaveCpus(cpumask_fd, mask));
  CheckSet(cpumask_fd, mask, cpulist_fd, list);

  // Reset in between.
  CHECK(WriteEnclaveCpus(cpulist_fd, ""));
  CheckSet(cpumask_fd, "", cpulist_fd, "");

  CHECK(WriteEnclaveCpus(cpulist_fd, list));
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

  CHECK(!WriteEnclaveCpus(cpumask_fd, "hello"));
  CHECK(!WriteEnclaveCpus(cpulist_fd, "-2"));
  CHECK(!WriteEnclaveCpus(cpulist_fd, "9999999999"));

  close(cpulist_fd);
  close(cpumask_fd);

  close(enclave_fd);
  LocalEnclave::DestroyEnclave(ctl_fd);
  close(ctl_fd);
}

static void AddCpu(int enclave_fd, std::string cpu) {
  int cpulist_fd = openat(enclave_fd, "cpulist", O_RDWR);
  CHECK_GE(cpulist_fd, 0);
  CHECK(WriteEnclaveCpus(cpulist_fd, cpu));
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
  UpdateTestTopology(absl::GetFlag(FLAGS_test_tmpdir), /*has_l3_cache=*/false);
  CpuList comma_list = TestTopology()->ToCpuList(
      std::vector<int>{0, 5, 10, 15, 16, 21, 26, 31, 32});
  std::string comma_str = comma_list.CpuMaskStr();

  // If we have more than 32 cpus, setting the cpumask should succeed.
  if (MachineTopology()->num_cpus() > comma_list.Back().id()) {
    EXPECT_TRUE(WriteEnclaveCpus(cpumask_fd, comma_str));
  } else if (!WriteEnclaveCpus(cpumask_fd, comma_str)) {
    // If WriteEnclaveCpus fails with 32 or less CPUs, the test case should
    // fail with EOVERFLOW indicating the given CPU mask contains a mask bit
    // set for a CPU whose ID is larger than the number of possible CPUs on
    // this machine. WriteEnclaveCpus may succeed if NR_CPUS and nmaskbits
    // are fixed to some large enough numbers - in that case, we do
    // not have to check anything.
    EXPECT_EQ(errno, EOVERFLOW);
  }

  close(cpumask_fd);
  close(enclave_fd);

  LocalEnclave::DestroyEnclave(ctl_fd);
  close(ctl_fd);
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

  // To make the race more likely, both threads run on separate cpus and
  // synchronize via shared memory.  They vary the amount they SpinFor, such
  // that each thread should get a chance to win the race.
  for (int i = 0; i < kLoops; ++i) {
    int ctl_fd = LocalEnclave::MakeNextEnclave();
    CHECK_GE(ctl_fd, 0);

    int enclave_fd = LocalEnclave::GetEnclaveDirectory(ctl_fd);
    CHECK_GE(enclave_fd, 0);

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

      int ret = GhostHelper()->SchedTaskEnterGhost(/*pid=*/0, enclave_fd);
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
          case ENODEV:
          case ENOENT:
            // The destroyer won the race and the enclave is dying.
            // - EBADF is when the enclave died and was removed from kernfs
            // before we could call kernfs_get_active().
            // - EXDEV is when we got the reference and were able to get the
            // struct ghost_enclave pointer, but it was already is_dying.
            // - ENODEV is when the kernfs_get_active() fails to get an active
            // ref
            // - ENOENT is potentially when the openat("tasks") fails.
            destroyer_won = true;
            break;
          default:
            // The above list of errors is not exhaustive.  Ignore other errors.
            // We have kLoops attempts, so skipping a trial won't matter.
            break;
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

    close(enclave_fd);
    close(ctl_fd);
  }

  // If the client or destroyer never win, we didn't test the race, but it is
  // not a sign of a bug.
  EXPECT_TRUE(client_won);
  EXPECT_TRUE(destroyer_won);
}

TEST_F(EnclaveTest, GetNrTasks) {
  Topology* topology = MachineTopology();
  auto enclave = std::make_unique<LocalEnclave>(
      AgentConfig(topology, topology->all_cpus()));

  EXPECT_EQ(enclave->GetNrTasks(), 0);
}

TEST_F(EnclaveTest, SetDeliverAgentRunnability) {
  Topology* topology = MachineTopology();
  auto enclave = std::make_unique<LocalEnclave>(
      AgentConfig(topology, topology->all_cpus()));

  std::string val =
    LocalEnclave::ReadEnclaveTunable(enclave->GetDirFd(),
                                     "deliver_agent_runnability");
  EXPECT_EQ(val, "0");

  enclave->SetDeliverAgentRunnability(true);

  val =
    LocalEnclave::ReadEnclaveTunable(enclave->GetDirFd(),
                                     "deliver_agent_runnability");
  EXPECT_EQ(val, "1");
}

TEST_F(EnclaveTest, SetDeliverCpuAvailability) {
  Topology* topology = MachineTopology();
  auto enclave = std::make_unique<LocalEnclave>(
      AgentConfig(topology, topology->all_cpus()));

  std::string val =
    LocalEnclave::ReadEnclaveTunable(enclave->GetDirFd(),
                                     "deliver_cpu_availability");
  EXPECT_EQ(val, "0");

  enclave->SetDeliverCpuAvailability(true);

  val =
    LocalEnclave::ReadEnclaveTunable(enclave->GetDirFd(),
                                     "deliver_cpu_availability");
  EXPECT_EQ(val, "1");
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
  ASSERT_TRUE(WriteEnclaveCpus(cpulist_fd, cpus));
  EXPECT_EQ(close(cpulist_fd), 0);

  FifoConfig config;
  config.topology_ = MachineTopology();
  config.enclave_fd_ = enclave_fd;
  config.global_cpu_ = config.topology_->cpu(0);

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
