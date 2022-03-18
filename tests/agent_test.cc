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

#include "lib/agent.h"

#include <sched.h>
#include <sys/timerfd.h>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/flat_hash_map.h"
#include "absl/random/random.h"
#include "lib/channel.h"
#include "lib/scheduler.h"

namespace ghost {
namespace {

using ::testing::Eq;
using ::testing::Ge;
using ::testing::Gt;
using ::testing::IsTrue;
using ::testing::Ne;

// A simple agent that just idles.
template <size_t max_notifications = 1>
class SimpleAgent : public Agent {
 public:
  SimpleAgent(Enclave* enclave, Cpu cpu) : Agent(enclave, cpu) {
    // Agent notifies the main thread on idle (up to max_notifications_ times).
    static_assert(max_notifications > 0);
    static_assert(max_notifications < 100);  // let's be reasonable.
  }

  // Wait for agent to idle.
  void WaitForIdle(size_t num = 0) {
    ASSERT_LT(num, idle_.size());
    idle_[num].WaitForNotification();
  }

 protected:
  void AgentThread() override {
    // Boilerplate to synchronize startup until all agents are ready.
    SignalReady();
    WaitForEnclaveReady();

    // Simple scheduling loop that actually doesn't schedule but just does
    // a LocalYield() every time it wakes up. We only expect to be woken up
    // by a Ping() from another agent or the main thread.
    while (!Finished()) {
      StatusWord::BarrierToken agent_barrier = status_word().barrier();
      RunRequest* req = enclave()->GetRunRequest(cpu());

      NotifyIdle();

      req->LocalYield(agent_barrier, /*flags=*/0);
    }
  }

 private:
  std::array<Notification, max_notifications> idle_;
  size_t num_notifications_ = 0;

  // Notify the main thread when agent idles for the first time.
  void NotifyIdle() {
    if (num_notifications_ < idle_.size()) {
      idle_[num_notifications_++].Notify();
    }
  }
};

constexpr int kWaitForIdle = 1;
constexpr int kPingAgents = 2;
constexpr int kRpcSerialize = 3;
constexpr int kRpcDeserializeArgs = 4;

template <size_t MAX_NOTIFICATIONS = 1, class EnclaveType = LocalEnclave>
class FullSimpleAgent : public FullAgent<EnclaveType> {
#define AGENT_AS(agent) \
  agent_down_cast<SimpleAgent<MAX_NOTIFICATIONS>*>((agent).get())

 public:
  // Simple container for testing RPC serialization.
  struct RpcTestData {
    bool operator==(const RpcTestData& rhs) const {
      return a == rhs.a && three == rhs.three && is_true == rhs.is_true &&
             counter == rhs.counter;
    }

    char a = '\0';
    int three = 0;
    bool is_true = false;
    std::array<int, 5> counter;
  };

  static constexpr RpcTestData kRpcTestData = {
      .a = 'a',
      .three = 3,
      .is_true = true,
      .counter = {1, 2, 3, 4, 5},
  };

  explicit FullSimpleAgent(const AgentConfig& config)
      : FullAgent<EnclaveType>(config), channel_(GHOST_MAX_QUEUE_ELEMS, 0) {
    channel_.SetEnclaveDefault();
    // Start an instance of SimpleAgent (above) on each cpu.
    this->StartAgentTasks();

    // Unblock all agents and start scheduling.
    this->enclave_.Ready();
  }

  ~FullSimpleAgent() override { this->TerminateAgentTasks(); }

  std::unique_ptr<Agent> MakeAgent(const Cpu& cpu) override {
    return absl::make_unique<SimpleAgent<MAX_NOTIFICATIONS>>(&this->enclave_,
                                                             cpu);
  }

  void RpcHandler(int64_t req, const AgentRpcArgs& args,
                  AgentRpcResponse& response) override {
    int response_code = 0;
    switch (req) {
      case kWaitForIdle:
        // Wait for all agents to enter scheduling loop.
        for (const auto& agent : this->agents_) {
          AGENT_AS(agent)->WaitForIdle();
        }
        break;
      case kPingAgents:
        // Ping each agent kPings number of times.
        for (const auto& agent : this->agents_) {
          for (int n = 0; n < MAX_NOTIFICATIONS; ++n) {
            AGENT_AS(agent)->WaitForIdle(n);               // Idle ...
            EXPECT_THAT(AGENT_AS(agent)->Ping(), IsTrue);  // ... wake up.
          }
        }
        break;
      case kRpcSerialize:
        response.buffer.Serialize<RpcTestData>(kRpcTestData);
        break;
      case kRpcDeserializeArgs:
        response_code = args.buffer.Deserialize<RpcTestData>().three;
        break;
      default:
        response_code = -1;
        break;
    }
    response.response_code = response_code;
  }

 private:
  Channel channel_;
#undef AGENT_AS
};

TEST(AgentTest, DestructorCanFree) {
  // We're using an extra scope here to ensure that the dtor runs before
  // declaring success.
  //
  // agent->Terminate() does not return until status_word() can be freed thereby
  // guaranteeing that ~Agent() will not CHECK-fail when we delete ap.
  {
    auto ap = AgentProcess<FullSimpleAgent<>, AgentConfig>(
        AgentConfig(MachineTopology(), MachineTopology()->all_cpus()));

    ASSERT_EQ(ap.Rpc(kWaitForIdle), 0);
  }
  SUCCEED();
}

TEST(AgentTest, Ping) {
  constexpr int kPings = 10;
  auto ap = AgentProcess<FullSimpleAgent<kPings>, AgentConfig>(
      AgentConfig(MachineTopology(), MachineTopology()->all_cpus()));

  ASSERT_EQ(ap.Rpc(kPingAgents), 0);
}

// Basic test of serialization/deserialization, doing these operations in-place
// rather than via the RPC interface.
TEST(AgentTest, RpcSerializationSimple) {
  struct MyStruct {
    int x, y, z;
  };
  const MyStruct s = {
    .x = 3,
    .y = 5,
    .z = INT_MIN,
  };
  AgentRpcResponse response;
  response.buffer.Serialize<MyStruct>(s);
  MyStruct deserialized = response.buffer.Deserialize<MyStruct>();

  EXPECT_EQ(s.x, deserialized.x);
  EXPECT_EQ(s.y, deserialized.y);
  EXPECT_EQ(s.z, deserialized.z);
}

// Basic test of serialization/deserialization, doing these operations in-place
// rather than via the RPC interface. This test uses the `Serialize<T>()` and
// `Deserialize<T>()` functions with the `size_t size` parameter.
TEST(AgentTest, RpcSerializationSimpleSize) {
  struct MyStruct {
    int x, y, z;
  };
  const MyStruct s = {
      .x = 3,
      .y = 5,
      .z = INT_MIN,
  };
  AgentRpcResponse response;
  response.buffer.Serialize<MyStruct>(s, sizeof(s));

  MyStruct deserialized;
  memset(&deserialized, 0, sizeof(deserialized));
  response.buffer.Deserialize<MyStruct>(deserialized, sizeof(s));

  EXPECT_EQ(s.x, deserialized.x);
  EXPECT_EQ(s.y, deserialized.y);
  EXPECT_EQ(s.z, deserialized.z);
}

// Basic test of vector serialization/deserialization.
TEST(AgentTest, RpcSerializationSimpleVector) {
  struct MyStruct {
    int x, y, z;
  };

  static constexpr int kNumIterations = 10;
  std::vector<MyStruct> to_serialize;
  for (int i = 0; i < kNumIterations; i++) {
    to_serialize.push_back({.x = i, .y = i + 2, .z = INT_MIN + i});
  }
  AgentRpcResponse response;
  response.buffer.SerializeVector<MyStruct>(to_serialize);

  std::vector<MyStruct> deserialized =
      response.buffer.DeserializeVector<MyStruct>(kNumIterations);
  for (int i = 0; i < kNumIterations; i++) {
    EXPECT_EQ(to_serialize[i].x, deserialized[i].x);
    EXPECT_EQ(to_serialize[i].y, deserialized[i].y);
    EXPECT_EQ(to_serialize[i].z, deserialized[i].z);
  }
}

// Tests the serialization mechanism over the RPC interface.
TEST(AgentTest, RpcSerialization) {
  auto ap = AgentProcess<FullSimpleAgent<>, AgentConfig>(
      AgentConfig(MachineTopology(), MachineTopology()->all_cpus()));

  const AgentRpcResponse& response = ap.RpcWithResponse(kRpcSerialize);
  ASSERT_EQ(response.response_code, 0);

  FullSimpleAgent<>::RpcTestData data =
      response.buffer.Deserialize<FullSimpleAgent<>::RpcTestData>();
  EXPECT_EQ(data, FullSimpleAgent<>::kRpcTestData);
}

// Test serialization of an object the same size as the buffer.
TEST(AgentTest, RpcSerializationMaxSize) {
  constexpr size_t kResponseSize = 1024;
  struct LargeStruct {
    std::array<std::byte, kResponseSize> arr;
  };
  AgentRpcBuffer<kResponseSize> response;
  constexpr std::byte val{10};
  const LargeStruct s = {
    .arr = {val},
  };
  response.Serialize<LargeStruct>(s);
  LargeStruct deserialized = response.Deserialize<LargeStruct>();
  EXPECT_EQ(s.arr, deserialized.arr);
}

// Test serialization of RPC arguments.
TEST(AgentTest, RpcArgSerialization) {
  auto ap = AgentProcess<FullSimpleAgent<>, AgentConfig>(
      AgentConfig(MachineTopology(), MachineTopology()->all_cpus()));

  const FullSimpleAgent<>::RpcTestData arg_data =
      FullSimpleAgent<>::kRpcTestData;
  AgentRpcArgs args;
  args.buffer.Serialize<FullSimpleAgent<>::RpcTestData>(arg_data);

  int64_t response = ap.Rpc(kRpcDeserializeArgs, args);
  EXPECT_EQ(response, arg_data.three);
}

TEST(AgentTest, ExitHandler) {
  bool ran = false;

  {
    auto ap = AgentProcess<FullSimpleAgent<>, AgentConfig>(
        AgentConfig(MachineTopology(), MachineTopology()->all_cpus()));

    ap.AddExitHandler([&ran](pid_t, int) {
      ran = true;
      return true;
    });

    ap.KillChild(SIGKILL);

    // Give us a chance to catch it in our SIGCHLD handler.  If not, we'll still
    // catch it when the dtor waits for the child.
    absl::SleepFor(absl::Milliseconds(50));
  }

  ASSERT_TRUE(ran);
  // We killed the agent before it could clean up its enclave.  This will
  // destroy all enclaves, but there should only be the one leftover from this
  // test.
  LocalEnclave::DestroyAllEnclaves();
}

class SpinningAgent : public Agent {
 public:
  SpinningAgent(Enclave* enclave, Cpu cpu) : Agent(enclave, cpu) {}

 protected:
  void AgentThread() override {
    // Boilerplate to synchronize startup until all agents are ready.
    SignalReady();
    WaitForEnclaveReady();

    // Spin so kernel emits MSG_CPU_TICK on the channel associated with
    // this agent.
    while (!Finished()) {
      StatusWord::BarrierToken agent_barrier = status_word().barrier();
      bool prio_boost = status_word().boosted_priority();
      if (prio_boost) {
        RunRequest* req = enclave()->GetRunRequest(cpu());
        req->LocalYield(agent_barrier, RTLA_ON_IDLE);
      }
      asm volatile("pause");
    }
  }
};

class TickConfig : public AgentConfig {
 public:
  TickConfig(Topology* topology, CpuList cpus, int numa_node)
      : AgentConfig(topology, cpus), numa_node_(numa_node) {
        tick_config_ = CpuTickConfig::kAllTicks;
  }

  int numa_node_;
};

// Drain 'channel' and return the number of CPU_TICK messages.
int CountCpuTicks(Channel* channel) {
  Message msg;
  int ticks = 0;
  while (!(msg = Peek(channel)).empty()) {
    if (msg.type() == MSG_CPU_TICK) {
      ticks++;
    }
    Consume(channel, msg);
  }
  return ticks;
}

constexpr int kTickChecker = 3;

template <class EnclaveType = LocalEnclave>
class FullTickAgent : public FullAgent<EnclaveType> {
#define AGENT_AS(agent) agent_down_cast<SpinningAgent*>((agent).get())

 public:
  explicit FullTickAgent(const TickConfig& config)
      : FullAgent<EnclaveType>(config),
        default_channel_(GHOST_MAX_QUEUE_ELEMS, config.numa_node_),
        agent_channel_(GHOST_MAX_QUEUE_ELEMS, config.numa_node_) {
    default_channel_.SetEnclaveDefault();
    this->StartAgentTasks();
    this->enclave_.Ready();
  }

  ~FullTickAgent() override { this->TerminateAgentTasks(); }

  std::unique_ptr<Agent> MakeAgent(const Cpu& cpu) override {
    return absl::make_unique<SpinningAgent>(&this->enclave_, cpu);
  }

  void RpcHandler(int64_t req, const AgentRpcArgs& args,
                  AgentRpcResponse& response) override {
    switch (req) {
      case kTickChecker: {
        SpinningAgent& agent = *AGENT_AS(this->agents_.front());

        // Sleep for a short duration during which the agent should be spinning.
        // This should induce the kernel into producing MSG_CPU_TICK messages
        // into the channel associated with the agent.
        absl::SleepFor(absl::Milliseconds(50));

        // All messages should be produced into the 'default_channel' so we
        // don't expect any cpu ticks on 'agent_channel'.
        EXPECT_THAT(CountCpuTicks(&agent_channel_), Eq(0));

        // Associate the agent with 'agent_channel' (thereby breaking the
        // implicit association with 'default_channel').
        EXPECT_THAT(agent_channel_.AssociateTask(agent.gtid(), agent.barrier()),
                    IsTrue());

        // There should be at least one MSG_CPU_TICK on the 'default_channel'.
        EXPECT_THAT(CountCpuTicks(&default_channel_), Gt(0));

        // Sleep for a short duration again and verify that the ticks are now
        // routed to the 'agent_channel'.
        absl::SleepFor(absl::Milliseconds(50));
        EXPECT_THAT(CountCpuTicks(&agent_channel_), Gt(0));
        EXPECT_THAT(CountCpuTicks(&default_channel_), Eq(0));
        break;
      }
      default:
        response.response_code = -1;
        return;
    }
    response.response_code = 0;
  }

 private:
  Channel default_channel_;
  Channel agent_channel_;
#undef AGENT_AS
};

TEST(AgentTest, CpuTick) {
  // arbitrary but safe because there must be at least one cpu.
  constexpr int kCpuNum = 0;
  constexpr int kNumaNode = 0;

  auto ap = AgentProcess<FullTickAgent<>, TickConfig>(TickConfig(
      MachineTopology(),
      MachineTopology()->ToCpuList(std::vector<int>{kCpuNum}), kNumaNode));

  ASSERT_EQ(ap.Rpc(kTickChecker), 0);
}

// An agent that dequeues messages and invokes a msg-specific callback.
class CallbackAgent : public Agent {
 public:
  using CallbackMap =
      absl::flat_hash_map<int, std::function<void(Message, Cpu)>>;

  CallbackAgent(Enclave* enclave, Cpu cpu, Channel* channel,
                CallbackMap callbacks)
      : Agent(enclave, cpu), channel_(channel), callbacks_(callbacks) {
    channel_->SetEnclaveDefault();
  }

  void need_cpu_not_idle() { need_cpu_not_idle_ = true; }

 protected:
  void AgentThread() override {
    // Boilerplate to synchronize startup until all agents are ready.
    SignalReady();
    WaitForEnclaveReady();

    while (!Finished()) {
      Message msg;
      while (!(msg = Peek(channel_)).empty()) {
        if (auto iter = callbacks_.find(msg.type()); iter != callbacks_.end()) {
          iter->second(msg, cpu());
        }
        Consume(channel_, msg);
      }

      // Yield until a message on 'channel_' wakes us up.
      RunRequest* req = enclave()->GetRunRequest(cpu());
      const StatusWord::BarrierToken agent_barrier = status_word().barrier();
      const bool prio_boost = status_word().boosted_priority();

      if (prio_boost) {
        req->LocalYield(agent_barrier, RTLA_ON_IDLE);
      } else if (need_cpu_not_idle_) {
        req->Open({
            .target = Gtid(GHOST_IDLE_GTID),
            .agent_barrier = agent_barrier,
            .commit_flags = COMMIT_AT_TXN_COMMIT,
            .run_flags = NEED_CPU_NOT_IDLE,
        });
        req->Commit();
      } else {
        req->LocalYield(agent_barrier, /*flags=*/0);
      }
    }
  }

 private:
  Channel* channel_;
  CallbackMap callbacks_;
  bool need_cpu_not_idle_ = false;
};

TEST(AgentTest, MsgTimerExpired) {
  Ghost::InitCore();
  Topology* topology = MachineTopology();

  const CpuList agent_cpus = topology->all_cpus();
  ASSERT_THAT(agent_cpus.Size(), Gt(0));

  // Boilerplate so we can create agents.
  auto enclave =
      absl::make_unique<LocalEnclave>(AgentConfig(topology, agent_cpus));

  // Randomly assign one agent as the designated receiver of
  // CPU_TIMER_EXPIRED msg when timer expires.
  absl::BitGen rng;
  Cpu target_cpu = topology->cpu(absl::Uniform(rng, 0u, agent_cpus.Size()));

  const int numa_node = 0;
  Channel default_channel(GHOST_MAX_QUEUE_ELEMS, numa_node);
  default_channel.SetEnclaveDefault();

  // Create a timerfd.
  int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  ASSERT_THAT(fd, Ge(0));

  std::vector<int> msgs(agent_cpus.Size(), 0);   // TIMER_EXPIRED msgs.
  std::vector<int> ticks(agent_cpus.Size(), 0);  // number of timer ticks.

  auto timer_callback = [fd, &msgs, &ticks](Message msg, Cpu cpu) {
    ASSERT_THAT(msg.is_cpu_msg(), IsTrue());
    ASSERT_THAT(msg.type(), Eq(MSG_CPU_TIMER_EXPIRED));

    const ghost_msg_payload_timer* payload =
        static_cast<const ghost_msg_payload_timer*>(msg.payload());
    ASSERT_THAT(payload->cpu, Eq(cpu.id()));
    ASSERT_THAT(payload->type, Eq(fd));
    ASSERT_THAT(payload->cookie, Eq(fd));

    msgs[cpu.id()]++;

    // Got one message but it may have accrued more than one tick
    // if agent execution was delayed (e.g. by hwintr or softirq).
    //
    // read from timerfd to get the number of ticks (this also has
    // the side-effect of rearming periodic timers).
    uint64_t t = 0;
    int nbytes = read(fd, &t, sizeof(t));
    if (nbytes != sizeof(t)) {
      EXPECT_THAT(nbytes, Eq(-1));
      EXPECT_THAT(errno, Eq(EAGAIN));
    } else {
      EXPECT_THAT(t, Ge(1));
      ticks[cpu.id()] += t;
    }
  };

  std::vector<std::unique_ptr<Channel>> channels;
  std::vector<std::unique_ptr<CallbackAgent>> agents;
  for (const Cpu& cpu : agent_cpus) {
    // Associate each agent with its own channel.
    //
    // The channel is configured to wakeup the agent when the kernel produces
    // a message into it.
    auto channel = absl::make_unique<Channel>(
        GHOST_MAX_QUEUE_ELEMS, numa_node, MachineTopology()->ToCpuList({cpu}));
    agents.emplace_back(
        new CallbackAgent(enclave.get(), cpu, channel.get(),
                          {
                              {MSG_CPU_TIMER_EXPIRED, timer_callback},
                          }));
    agents.back()->Start();

    // Associate the agent with 'channel' (thereby breaking the implicit
    // association with 'default_channel').
    Gtid agent_gtid = agents.back()->gtid();
    while (!channel->AssociateTask(agent_gtid, agents.back()->barrier())) {
      // AssociateTask may fail if agent barrier is stale.
      EXPECT_THAT(errno, Eq(ESTALE));
    }
    channels.push_back(std::move(channel));
  }

  // Unblock all agents and start scheduling.
  enclave->Ready();

  // Timer expiring every millisecond.
  const absl::Duration kPeriod = absl::Milliseconds(1);
  struct itimerspec itimerspec = {
      .it_interval = absl::ToTimespec(kPeriod),  // initial expiration.
      .it_value = absl::ToTimespec(kPeriod),     // periodic expiration.
  };

  const uint64_t type = fd;
  const uint64_t cookie = fd;
  ASSERT_THAT(Ghost::TimerFdSettime(fd, /*flags=*/0, &itimerspec, target_cpu,
                                    type, cookie),
              Eq(0));

  // Sleep for 50 msec.
  const absl::Duration kDelay = absl::Milliseconds(50);
  absl::SleepFor(kDelay);

  // Stop timer and disassociate from ghost before terminating agent.
  struct itimerspec itimerzero = {
      .it_interval = {0},
      .it_value = {0},
  };
  ASSERT_THAT(Ghost::TimerFdSettime(fd, /*flags=*/0, &itimerzero), Eq(0));

  // Terminate all agents.
  for (auto& a : agents) a->Terminate();

  for (const Cpu& cpu : agent_cpus) {
    if (cpu == target_cpu) {
      // Each 'msg' accounts for one or more 'ticks'.
      EXPECT_THAT(ticks[cpu.id()], Ge(msgs[cpu.id()]));
      EXPECT_THAT(ticks[cpu.id()], Ge(kDelay / kPeriod));
    } else {
      EXPECT_THAT(ticks[cpu.id()], Eq(0));
      EXPECT_THAT(msgs[cpu.id()], Eq(0));
    }
  }
}

// Verify that NEED_CPU_NOT_IDLE triggers MSG_CPU_NOT_IDLE when a non-idle
// task is scheduled on the cpu.
TEST(AgentTest, MsgCpuNotIdle) {
  Ghost::InitCore();
  Topology* topology = MachineTopology();

  const CpuList agent_cpus = topology->all_cpus();
  ASSERT_THAT(agent_cpus.Size(), Gt(0));

  auto enclave =
      absl::make_unique<LocalEnclave>(AgentConfig(topology, agent_cpus));

  // Randomly assign one agent as the designated receiver of MSG_CPU_NOT_IDLE.
  absl::BitGen rng;
  Cpu target_cpu = topology->cpu(absl::Uniform(rng, 0u, agent_cpus.Size()));

  const int numa_node = 0;
  Channel default_channel(GHOST_MAX_QUEUE_ELEMS, numa_node);
  default_channel.SetEnclaveDefault();

  // Per-cpu counter for the number of CPU_NOT_IDLE msgs.
  std::vector<int> cpu_not_idle_msgs(agent_cpus.Size(), 0);

  // Callback invoked by agent when a CPU_NOT_IDLE msg is received.
  auto callback = [&cpu_not_idle_msgs](Message msg, Cpu cpu) {
    ASSERT_THAT(msg.is_cpu_msg(), IsTrue());
    ASSERT_THAT(msg.type(), Eq(MSG_CPU_NOT_IDLE));

    const ghost_msg_payload_cpu_not_idle* payload =
        static_cast<const ghost_msg_payload_cpu_not_idle*>(msg.payload());
    ASSERT_THAT(payload->cpu, Eq(cpu.id()));
    ASSERT_THAT(payload->next_gtid, Ne(0));

    cpu_not_idle_msgs[cpu.id()]++;
  };

  std::vector<std::unique_ptr<Channel>> channels;
  std::vector<std::unique_ptr<CallbackAgent>> agents;
  for (const Cpu& cpu : agent_cpus) {
    auto channel = absl::make_unique<Channel>(
        GHOST_MAX_QUEUE_ELEMS, numa_node, MachineTopology()->ToCpuList({cpu}));
    agents.emplace_back(new CallbackAgent(enclave.get(), cpu, channel.get(),
                                          {
                                              {MSG_CPU_NOT_IDLE, callback},
                                          }));

    if (cpu == target_cpu) {
      agents.back()->need_cpu_not_idle();
    }

    agents.back()->Start();

    // Associate the agent with 'channel' (thereby breaking the implicit
    // association with 'default_channel').
    Gtid agent_gtid = agents.back()->gtid();
    while (!channel->AssociateTask(agent_gtid, agents.back()->barrier())) {
      // AssociateTask may fail if agent barrier is stale.
      EXPECT_THAT(errno, Eq(ESTALE));
    }
    channels.push_back(std::move(channel));
  }

  // Unblock all agents and start scheduling.
  enclave->Ready();

  // Schedule a CFS task on 'target_cpu' (this will trigger CPU_NOT_IDLE msg).
  std::thread thread([target_cpu] {
    EXPECT_THAT(Ghost::SchedSetAffinity(Gtid::Current(),
                                        MachineTopology()->ToCpuList(
                                            std::vector<int>{target_cpu.id()})),
                Eq(0));
    absl::SleepFor(absl::Milliseconds(100));
  });
  thread.join();

  // Terminate all agents.
  for (auto& a : agents) a->Terminate();

  for (const Cpu& cpu : agent_cpus) {
    if (cpu == target_cpu) {
      EXPECT_THAT(cpu_not_idle_msgs[cpu.id()], Gt(0));
    } else {
      EXPECT_THAT(cpu_not_idle_msgs[cpu.id()], Eq(0));
    }
  }
}

// Agent class to test sched_setscheduler() behavior for ghost agents.
// All tests run before the AgentThread() reaches the main scheduling
// loop so simply starting and terminating the agent is sufficient.
// For e.g.
//   SetSchedAgent agent(enclave, cpu);
//   agent.Start();
//   agent.Terminate();
class SetSchedAgent : public Agent {
 public:
  SetSchedAgent(Enclave* enclave, Cpu cpu) : Agent(enclave, cpu) {}

 protected:
  void AgentThread() override {
    // Boilerplate to synchronize startup until all agents are ready.
    SignalReady();
    WaitForEnclaveReady();

    constexpr int my_pid = 0;
    constexpr sched_param param = {0};

    // Kernel ensures 'reset_on_fork' is set for an agent.
    EXPECT_THAT(sched_getscheduler(my_pid),
                Eq(SCHED_GHOST | SCHED_RESET_ON_FORK));

    // Try to clear 'reset_on_fork' which should fail.
    EXPECT_THAT(sched_setscheduler(my_pid, SCHED_GHOST, &param), Eq(-1));
    EXPECT_THAT(errno, Eq(EPERM));
    EXPECT_THAT(sched_getscheduler(my_pid),
                Eq(SCHED_GHOST | SCHED_RESET_ON_FORK));

    // Try to move the agent out of the ghost sched_class.
    EXPECT_THAT(sched_setscheduler(my_pid, SCHED_OTHER, &param), Eq(-1));
    EXPECT_THAT(errno, Eq(EPERM));
    EXPECT_THAT(sched_getscheduler(my_pid),
                Eq(SCHED_GHOST | SCHED_RESET_ON_FORK));

    // Scheduling loop that actually doesn't schedule but just does a
    // LocalYield() every time it wakes up. We expect to be woken up by
    // a Ping() from the main thread on termination.
    RunRequest* req = enclave()->GetRunRequest(cpu());
    while (!Finished()) {
      StatusWord::BarrierToken agent_barrier = status_word().barrier();
      req->LocalYield(agent_barrier, /*flags=*/0);
    }
  }
};

// Test to validate sched_setscheduler() behavior for ghost agents.
TEST(AgentTest, SetSched) {
  Ghost::InitCore();
  // arbitrary but safe since there must be one cpu.
  constexpr int agent_cpu = 0;
  Topology* topology = MachineTopology();
  auto enclave = absl::make_unique<LocalEnclave>(
      AgentConfig(topology, topology->ToCpuList(std::vector<int>{agent_cpu})));

  Channel default_channel(GHOST_MAX_QUEUE_ELEMS, /*node=*/0);
  default_channel.SetEnclaveDefault();

  SetSchedAgent agent(enclave.get(), topology->cpu(agent_cpu));
  agent.Start();
  enclave->Ready();

  agent.Terminate();
}

}  // namespace
}  // namespace ghost
