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

#include "lib/channel.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "lib/agent.h"
#include "lib/scheduler.h"

namespace ghost {
namespace {

using ::testing::IsNull;
using ::testing::NotNull;

// Bare-bones agent implementation that can schedule exactly one task.
//
// The agent listens on 'default_channel_' for the TASK_NEW message
// and then switches to 'task_channel_' for subsequent task state
// transitions.
//
// 'task_new_callback_' is executed when a new task is discovered.
class TestAgent : public Agent {
 public:
  TestAgent(Enclave* enclave, Cpu cpu, Channel* default_channel,
            Channel* task_channel, std::function<void(Task<>*)> callback)
      : Agent(enclave, cpu),
        default_channel_(default_channel),
        task_channel_(task_channel),
        task_new_callback_(callback) {
    default_channel_->SetEnclaveDefault();
  }

  // Wait for agent to idle.
  void WaitForIdle() { idle_.WaitForNotification(); }

 protected:
  void AgentThread() override {
    // Boilerplate to synchronize startup until all agents are ready
    // (mostly redundant since we only have a single agent in the test).
    SignalReady();
    WaitForEnclaveReady();

    std::unique_ptr<Task<>> task(nullptr);
    bool runnable = false;
    while (true) {
      while (true) {
        Channel* channel = task ? task_channel_ : default_channel_;
        Message msg = Peek(channel);
        if (msg.empty()) break;

        // Ignore all types other than task messages (e.g. CPU_TICK).
        if (msg.is_task_msg() && msg.type() != MSG_TASK_NEW) {
          ASSERT_THAT(task, NotNull());
          task->Advance(msg.seqnum());
        }

        // Scheduling is simple: we track whether the task is runnable
        // or blocked. If the task is runnable the agent switches to it
        // and idles otherwise.
        switch (msg.type()) {
          case MSG_TASK_NEW: {
            const ghost_msg_payload_task_new* payload =
                static_cast<const ghost_msg_payload_task_new*>(msg.payload());

            ASSERT_THAT(task, IsNull());
            task = absl::make_unique<Task<>>(Gtid(payload->gtid),
                                             payload->sw_info);
            task->seqnum = msg.seqnum();
            runnable = payload->runnable;

            // Invoke the test-specific callback for new task.
            task_new_callback_(task.get());
            break;
          }

          case MSG_TASK_WAKEUP:
            ASSERT_THAT(task, NotNull());
            ASSERT_FALSE(runnable);
            runnable = true;
            break;

          case MSG_TASK_BLOCKED:
            ASSERT_THAT(task, NotNull());
            ASSERT_TRUE(runnable);
            runnable = false;
            break;

          case MSG_TASK_DEAD:
            ASSERT_THAT(task, NotNull());
            ASSERT_FALSE(runnable);
            task = nullptr;
            break;

          default:
            // This includes task messages like TASK_YIELD or TASK_PREEMPTED
            // and cpu messages like CPU_TICK that don't influence runnability.
            break;
        }
        Consume(channel, msg);
      }

      StatusWord::BarrierToken agent_barrier = status_word().barrier();
      const bool prio_boost = status_word().boosted_priority();

      if (Finished() && !task) break;

      RunRequest* req = enclave()->GetRunRequest(cpu());
      if (!task || !runnable || prio_boost) {
        NotifyIdle();
        req->LocalYield(agent_barrier, prio_boost ? RTLA_ON_IDLE : 0);
      } else {
        req->Open({
            .target = task->gtid,
            .target_barrier = task->seqnum,
            .agent_barrier = agent_barrier,
            .commit_flags = COMMIT_AT_TXN_COMMIT,
        });
        req->Commit();
      }
    }
  }

 private:
  // Notify the main thread when agent idles for the first time.
  void NotifyIdle() {
    if (first_idle_) {
      first_idle_ = false;
      idle_.Notify();
    }
  }

  Channel* default_channel_;
  Channel* task_channel_;
  std::function<void(Task<>*)> task_new_callback_;

  Notification idle_;
  bool first_idle_ = true;
};

TEST(ChannelTest, Wakeup) {
  Ghost::InitCore();

  // arbitrary but safe because there must be at least one cpu.
  const int cpu_num = 0;
  Topology* topology = MachineTopology();
  auto enclave = absl::make_unique<LocalEnclave>(
      AgentConfig(topology, topology->ToCpuList(std::vector<int>{cpu_num})));
  Cpu agent_cpu = topology->cpu(cpu_num);

  // Configure 'chan0' to wakeup agent.
  const int numa_node = 0;
  Channel chan0(GHOST_MAX_QUEUE_ELEMS, numa_node,
                topology->ToCpuList({agent_cpu}));
  TestAgent agent(enclave.get(), agent_cpu, &chan0, &chan0, [](Task<>*) {});
  agent.Start();
  enclave->Ready();

  // Wait for agent to idle before launching the GhostThread. This verifies
  // that the MSG_TASK_NEW actually wakes up the agent.
  agent.WaitForIdle();

  GhostThread t(GhostThread::KernelScheduler::kGhost, [] {
    absl::SleepFor(absl::Milliseconds(10));  // MSG_TASK_BLOCKED.
    sched_yield();                           // MSG_TASK_YIELD.
  });
  t.Join();

  agent.Terminate();
}

TEST(ChannelTest, Associate) {
  Ghost::InitCore();

  // arbitrary but safe because there must be at least one cpu.
  const int cpu_num = 0;
  Topology* topology = MachineTopology();
  auto enclave = absl::make_unique<LocalEnclave>(
      AgentConfig(topology, topology->ToCpuList(std::vector<int>{cpu_num})));
  Cpu agent_cpu = topology->cpu(cpu_num);

  // Configure both 'chan0' and 'chan1' to wakeup agent.
  const int numa_node = 0;
  Channel chan0(GHOST_MAX_QUEUE_ELEMS, numa_node,
                topology->ToCpuList({agent_cpu}));
  Channel chan1(GHOST_MAX_QUEUE_ELEMS, numa_node,
                topology->ToCpuList({agent_cpu}));

  // The new task is associated with the default channel (chan0).
  // We change the association to chan1 in the task_new callback.
  TestAgent agent(enclave.get(), agent_cpu, &chan0, &chan1,
                  [&chan1](Task<>* task) {
                    ASSERT_TRUE(chan1.AssociateTask(task->gtid, task->seqnum));
                  });
  agent.Start();
  enclave->Ready();

  agent.WaitForIdle();

  // TASK_NEW is produced into chan0 but all subsequent messages go to chan1.
  GhostThread t(GhostThread::KernelScheduler::kGhost, [] {
    sched_yield();
    absl::SleepFor(absl::Milliseconds(10));
    sched_yield();
  });

  t.Join();
  agent.Terminate();
}

// Number of elements in the channel must match what was requested.
TEST(ChannelTest, MaxElements) {
  // Need an enclave to make a channel.
  Topology* topology = MachineTopology();
  auto enclave = absl::make_unique<LocalEnclave>(
      AgentConfig(topology, topology->all_cpus()));
  for (int elems = GHOST_MAX_QUEUE_ELEMS; elems > 0; elems >>= 1) {
    Channel chan(elems, /*node=*/0);
    EXPECT_EQ(chan.max_elements(), elems);
  }
}

}  // namespace
}  // namespace ghost
