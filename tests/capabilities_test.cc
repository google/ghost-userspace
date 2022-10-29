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

#include "tests/capabilities_test.h"

#include <fstream>

#include "kernel/ghost_uapi.h"
#include "lib/agent.h"
#include "lib/channel.h"
#include "lib/ghost.h"

#include "bpf/user/agent.h"
#include "bpf/user/test_bpf.skel.h"

// These tests check that ghOSt properly accepts/rejects syscalls based on the
// capabilities that the calling thread holds.

namespace ghost {
namespace {

// Tests that the `Run` ghOSt ioctl succeeds when the `CAP_SYS_NICE`
// capability is set. We do not want to invoke the scheduler, so we pass
// nonsense values to make the ioctl fail. We know that we have the ability to
// use the ioctl if we fail with error `EINVAL` rather than error `EPERM`.
TEST(CapabilitiesTest, RunNice) {
  AssertNiceCapabilitySet();

  Topology* topology = MachineTopology();
  LocalEnclave enclave(AgentConfig(topology, topology->EmptyCpuList()));

  EXPECT_THAT(Ghost::Run(Gtid::Current(), /*agent_barrier=*/0,
                         /*task_barrier=*/0, /*cpu=*/-1, /*flags=*/0),
              Eq(-1));
  EXPECT_THAT(errno, Eq(EINVAL));
}

// This is a simple agent used to test that a thread with the `CAP_SYS_NICE`
// capability can make another thread an agent.
//
// Example:
// Notification notification;
// CapabilitiesAgent agent(enclave, cpu, &notification);
// agent.Start();
// notification.WaitForNotification();
// agent.Terminate();
class CapabilitiesAgent : public LocalAgent {
 public:
  // Constructs the agent on the specified enclave (`enclave`) and CPU (`cpu`).
  // The agent notifies `notification` in its main thread body so that the test
  // can confirm that the agent actually ran.
  CapabilitiesAgent(Enclave* enclave, Cpu cpu, Notification* notification)
      : LocalAgent(enclave, cpu), notification_(notification) {}

 private:
  // The main thread body checks that it is in the ghOSt scheduling class and
  // then notifies the main test thread that it is ready to finish.
  void AgentThread() final {
    // Boilerplate to synchronize startup until all agents are ready.
    SignalReady();
    WaitForEnclaveReady();

    EXPECT_THAT(sched_getscheduler(/*pid=*/0),
                Eq(SCHED_GHOST | SCHED_RESET_ON_FORK));
    ASSERT_THAT(notification_, NotNull());
    notification_->Notify();

    // This is a scheduling loop that does not actually schedule but rather
    // calls `LocalYield` each time it wakes up. The agent expects to be woken
    // up by a ping from the main test thread on termination.
    RunRequest* req = enclave()->GetRunRequest(cpu());
    while (!Finished()) {
      StatusWord::BarrierToken agent_barrier = status_word().barrier();
      req->LocalYield(agent_barrier, /*flags=*/0);
    }
  }

  // The agent notifies this notification in its main thread body so that the
  // test can confirm that the agent actually ran.
  Notification* notification_;
};

// Tests that this thread can make another thread an agent when the
// `CAP_SYS_NICE` capability is set.
TEST(CapabilitiesTest, AgentNice) {
  // We call enclave->Ready() and that will disable our ability to load bpf
  // programs.  Do this in another process so we can run other gunit tests.
  ForkedProcess fp([]() {
    Ghost::InitCore();

    AssertNiceCapabilitySet();

    // Put the agent on CPU 0. This is an arbitrary choice but is safe because a
    // computer must have at least one CPU.
    constexpr int kAgentCpu = 0;
    Topology* topology = MachineTopology();
    auto enclave = absl::make_unique<LocalEnclave>(
        AgentConfig(topology, topology->ToCpuList(std::vector<int>{kAgentCpu})));
    LocalChannel default_channel(GHOST_MAX_QUEUE_ELEMS, /*node=*/0);
    default_channel.SetEnclaveDefault();

    Notification notification;
    CapabilitiesAgent agent(enclave.get(), topology->cpu(kAgentCpu),
                            &notification);
    agent.Start();
    enclave->Ready();

    // Wait for the notification to be notified. Once it is, the test knows that
    // the agent actually ran.
    notification.WaitForNotification();

    agent.Terminate();

    return 0;
  });

  fp.WaitForChildExit();
}

// Drops the `CAP_SYS_NICE` capability and tests that the `Run` ghOSt ioctl
// fails. We do not want to invoke the scheduler, so we pass nonsense values to
// make the syscall fail. We know that we do not have the ability to use the
// ioctl if we fail with error `EPERM` rather than error `EINVAL`.
TEST(CapabilitiesTest, RunNoNice) {
  GhostThread thread(GhostThread::KernelScheduler::kCfs, []() {
    DropNiceCapability();

    Topology* topology = MachineTopology();
    LocalEnclave enclave(AgentConfig(topology, topology->EmptyCpuList()));

    EXPECT_THAT(Ghost::Run(Gtid::Current(), /*agent_barrier=*/0,
                           /*task_barrier=*/0, /*cpu=*/-1, /*flags=*/0),
                Eq(-1));
    EXPECT_THAT(errno, Eq(EPERM));
  });
  thread.Join();
}

// Drops the `CAP_SYS_NICE` capability and tests that a thread cannot make
// itself an agent. Note that a capability cannot be regained after it is
// dropped, so we spawn a separate thread to run the test. By doing so, we
// ensure that tests run after this one still hold the `CAP_SYS_NICE`
// capability.
TEST(CapabilitiesTest, AgentNoNice) {
  LocalEnclave enclave(AgentConfig{MachineTopology()});
  LocalChannel chan(GHOST_MAX_QUEUE_ELEMS, /*node=*/0);

  // This test deliberately uses an `std::thread` instead of a `GhostThread` to
  // ensure the test only calls `sched_setscheduler` to attempt to move the
  // thread to the ghOSt kernel scheduling class once.
  std::thread thread([&enclave, &chan]() {
    DropNiceCapability();
    // We do not need initialize an enclave, a channel, etc., for the agent
    // since the call below will fail before these are needed.
    EXPECT_THAT(SchedAgentEnterGhost(enclave.GetCtlFd(), chan.GetFd()), Eq(-1));
    EXPECT_THAT(errno, Eq(EPERM));
  });
  thread.join();
}

void TestBpfProgLoad(void) {
  LocalEnclave enclave(AgentConfig{MachineTopology()});

  struct test_bpf* bpf_obj = test_bpf__open();
  CHECK_NE(bpf_obj, nullptr);

  bpf_program__set_types(bpf_obj->progs.test_pnt,
                         BPF_PROG_TYPE_GHOST_SCHED, BPF_GHOST_SCHED_PNT);
  CHECK_EQ(test_bpf__load(bpf_obj), 0);
  CHECK_EQ(agent_bpf_register(bpf_obj->progs.test_pnt, BPF_GHOST_SCHED_PNT),
           0);

  // Normally called from Enclave::Ready();
  enclave.InsertBpfPrograms();

  test_bpf__destroy(bpf_obj);
}

TEST(CapabilitiesTest, BpfProgLoad) {
  TestBpfProgLoad();
}

TEST(CapabilitiesTest, BpfProgLoadTwice) {
  TestBpfProgLoad();
  TestBpfProgLoad();
}

}  // namespace
}  // namespace ghost
