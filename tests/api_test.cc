// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <sys/mman.h>
#include <sys/resource.h>

#include <cstdint>
#include <memory>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/functional/any_invocable.h"
#include "absl/random/random.h"
#include "absl/status/status.h"
#include "lib/agent.h"
#include "lib/ghost.h"
#include "lib/scheduler.h"
#include "schedulers/fifo/per_cpu/fifo_scheduler.h"

namespace ghost {
namespace {

using ::testing::AnyOf;
using ::testing::Eq;
using ::testing::Ge;
using ::testing::Gt;
using ::testing::IsFalse;
using ::testing::IsNull;
using ::testing::IsTrue;
using ::testing::Le;
using ::testing::Lt;
using ::testing::Ne;
using ::testing::NotNull;

// DeadAgent tries to induce a race in stage_commit() such that task_rq_lock
// returns with a task that is already dead. This validates that the kernel
// properly detects this condition and bails out.
//
// Prior to the fix in go/kcl/353858 the kernel would reliably panic in less
// than 1000 iterations of this test in virtme.
class DeadAgent : public LocalAgent {
 public:
  DeadAgent(Enclave* enclave, Cpu this_cpu, Cpu other_cpu, Channel* channel)
      : LocalAgent(enclave, this_cpu),
        other_cpu_(other_cpu),
        channel_(channel) {}

 protected:
  void AgentThread() final {
    // Boilerplate to synchronize startup until both agents are ready.
    SignalReady();
    WaitForEnclaveReady();

    // Satellite agent that simply yields.
    if (!channel_) {
      RunRequest* req = enclave()->GetRunRequest(cpu());
      while (!Finished()) {
        req->LocalYield(status_word().barrier(), 0);
      }
      return;
    }

    // Spinning agent that continuously schedules `task` on `other_cpu_`.
    //
    // The agent is indiscriminate and doesn't track whether a task
    // is blocked or runnable (intentional so we can try to catch
    // the task when it is exiting).
    ASSERT_THAT(channel_, NotNull());

    std::unique_ptr<Task<>> task(nullptr);  // initialized in TASK_NEW handler.
    while (true) {
      while (true) {
        Message msg = Peek(channel_);
        if (msg.empty()) break;

        // Ignore all types other than task messages (e.g. CPU_TICK).
        if (msg.is_task_msg() && msg.type() != MSG_TASK_NEW) {
          ASSERT_THAT(task, NotNull());
          task->Advance(msg.seqnum());
        }

        switch (msg.type()) {
          case MSG_TASK_NEW: {
            const ghost_msg_payload_task_new* payload =
                static_cast<const ghost_msg_payload_task_new*>(msg.payload());
            ASSERT_THAT(task, IsNull());
            task =
                std::make_unique<Task<>>(Gtid(payload->gtid), payload->sw_info);
            task->seqnum = msg.seqnum();
            break;
          }

          case MSG_TASK_DEAD:
            ASSERT_THAT(task, NotNull());
            task = nullptr;
            break;

          default:
            break;
        }
        Consume(channel_, msg);
      }

      BarrierToken agent_barrier = status_word().barrier();
      const bool prio_boost = status_word().boosted_priority();

      if (Finished()) {
        // If the agent is terminating before observing the TASK_DEAD msg
        // then explicitly release ownership of the underlying Task object.
        //
        // In this situation it is possible that the task's status_word is
        // not marked CAN_FREE when the Task::~Task() runs which in turn
        // triggers a CHECK fail.
        //
        // Note that the kernel will move the task to CFS when the enclave
        // is destroyed so there is no risk of stranding the task.
        if (task) {
          task.release();
        }
        break;
      }

      if (prio_boost || !task) {
        // Yield if a higher priority sched_class wants to run or
        // we don't have a task to schedule.
        RunRequest* req = enclave()->GetRunRequest(cpu());
        req->LocalYield(agent_barrier, prio_boost ? RTLA_ON_IDLE : 0);
      } else {
        // Schedule `task` on `other_cpu_`.
        RunRequest* req = enclave()->GetRunRequest(other_cpu_);
        req->Open({.target = task->gtid,
                   .target_barrier = task->seqnum,
                   .agent_barrier = agent_barrier,
                   .commit_flags = COMMIT_AT_TXN_COMMIT});

        // We expect the Submit() to fail most of the time since we don't
        // track whether `task` can be legitimately scheduled but it does
        // maximize opportunities to trigger the race in go/kcl/353858.
        //
        // N.B. We use Submit() rather than Commit() because the latter
        // calls CompleteRunRequest() that CHECK-fails for unexpected
        // errors (for e.g. commit can fail with GHOST_TXN_TARGET_ONCPU
        // which is not expected with a well-behaved agent).
        req->Submit();
        while (!req->committed()) {
          Pause();
        }
      }
    }
  }

 private:
  Cpu other_cpu_;
  Channel* channel_;
};

template <class EnclaveType = LocalEnclave>
class FullDeadAgent final : public FullAgent<EnclaveType> {
 public:
  explicit FullDeadAgent(const AgentConfig& config)
      : FullAgent<EnclaveType>(config),
        sched_cpu_(config.cpus_.Front()),
        satellite_cpu_(config.cpus_.Back()),
        channel_(GHOST_MAX_QUEUE_ELEMS, sched_cpu_.numa_node(),
                 MachineTopology()->ToCpuList({sched_cpu_})) {
    channel_.SetEnclaveDefault();
    // Start an instance of DeadAgent on each cpu.
    this->StartAgentTasks();

    // Unblock all agents and start scheduling.
    this->enclave_.Ready();
  }

  ~FullDeadAgent() final { this->TerminateAgentTasks(); }

  std::unique_ptr<Agent> MakeAgent(const Cpu& cpu) final {
    Cpu other_cpu = satellite_cpu_;
    Channel* channel_ptr = &channel_;
    if (cpu == satellite_cpu_) {
      other_cpu = sched_cpu_;
      channel_ptr = nullptr;  // sentinel value to indicate a satellite cpu.
    }
    return std::make_unique<DeadAgent>(&this->enclave_, cpu, other_cpu,
                                       channel_ptr);
  }

  void RpcHandler(int64_t req, const AgentRpcArgs& args,
                  AgentRpcResponse& response) final {
    response.response_code = -1;
  }

 private:
  Cpu sched_cpu_;         // CPU running the main scheduling loop.
  Cpu satellite_cpu_;     // Satellite agent CPU.
  LocalChannel channel_;  // Channel configured to wakeup `sched_cpu_`.
};

TEST(ApiTest, RunDeadTask) {
  // Skip test if this is a uni-processor system.
  Topology* topology = MachineTopology();
  CpuList all_cpus = topology->all_cpus();
  if (all_cpus.Size() < 2) {
    GTEST_SKIP() << "must be a multiprocessor system";
    return;
  }

  // Pick two CPUs randomly to run the test. An agent spinning on the first
  // CPU continuously schedules the `GhostThread` below on the second CPU.
  std::vector<Cpu> target_cpu_vector;
  std::vector<Cpu> all_cpus_vector = all_cpus.ToVector();
  std::sample(all_cpus_vector.begin(), all_cpus_vector.end(),
              std::back_inserter(target_cpu_vector), 2, absl::BitGen());
  CpuList target_cpus = MachineTopology()->ToCpuList(target_cpu_vector);

  auto ap = AgentProcess<FullDeadAgent<>, AgentConfig>(
      AgentConfig(topology, target_cpus));

  GhostThread t(GhostThread::KernelScheduler::kGhost, [] {
    // Nothing: exit as soon as possible.
  });
  t.Join();

  // When AgentProcess goes out of scope its destructor will trigger
  // the FullDeadAgent destructor that in turn will Terminate() the
  // agents.

  // Since we were a ghOSt client, we were using an enclave.  Now that the test
  // is over, we need to reset so we can get a fresh enclave later.  Note that
  // we used AgentProcess, so the only user of the gbl_enclave_fd_ is us, the
  // client.
  GhostHelper()->CloseGlobalEnclaveFds();
}

class SyncGroupScheduler final : public BasicDispatchScheduler<FifoTask> {
 public:
  explicit SyncGroupScheduler(
      Enclave* enclave, const CpuList& cpulist,
      std::shared_ptr<TaskAllocator<FifoTask>> allocator)
      : BasicDispatchScheduler(enclave, cpulist, std::move(allocator)),
        sched_cpu_(cpulist.Front()),
        channel_(std::make_unique<LocalChannel>(
            GHOST_MAX_QUEUE_ELEMS,
            /*node=*/0, MachineTopology()->ToCpuList({sched_cpu_}))) {}

  Channel& GetDefaultChannel() final { return *channel_; };

  bool Empty(const Cpu& cpu) const {
    // Non-scheduling CPUs only look at what's running on the local cpu.
    if (cpu != sched_cpu_) {
      const CpuState* cs = cpu_state(cpu);
      return !cs->current;
    }

    // The scheduling CPU must look at the runqueue as well as what's running
    // on all cpus.
    for (const Cpu& cpu : *enclave()->cpus()) {
      const CpuState* cs = cpu_state(cpu);
      if (cs->current) {
        return false;
      }
    }

    return rq_.Empty();
  }

  void Schedule(const Cpu& this_cpu, BarrierToken agent_barrier,
                bool prio_boost, bool finished) {
    RunRequest* req = enclave()->GetRunRequest(this_cpu);

    if (this_cpu != sched_cpu_) {
      req->LocalYield(agent_barrier, /*flags=*/0);
      return;
    }

    // Dequeue any pending messages.
    Message msg;
    while (!(msg = Peek(channel_.get())).empty()) {
      DispatchMessage(msg);
      Consume(channel_.get(), msg);
    }

    // A non-ghost sched_class is runnable so give up the CPU until it is
    // about to idle.
    if (prio_boost) {
      req->LocalYield(agent_barrier, RTLA_ON_IDLE);
      return;
    }

    if (finished && Empty(this_cpu)) return;

    // Populate 'cs->next' for each cpu in the enclave.
    for (const Cpu& cpu : cpus()) {
      CpuState* cs = cpu_state(cpu);
      ASSERT_THAT(cs->next, IsNull());

      if (cs->current) {
        cs->next = cs->current;  // exercise ALLOW_TASK_ONCPU.
      } else {
        cs->next = rq_.Dequeue();
      }

      const int sync_group_owner = this_cpu.id();
      Gtid target = Gtid(GHOST_IDLE_GTID);
      BarrierToken target_barrier = StatusWord::NullBarrierToken();
      RunRequest* req = enclave()->GetRunRequest(cpu);
      if (cs->next) {
        target = cs->next->gtid;
        target_barrier = cs->next->seqnum;
      }

      req->Open({
          .target = target,
          .target_barrier = target_barrier,
          .agent_barrier = agent_barrier,
          .commit_flags = COMMIT_AT_TXN_COMMIT | ALLOW_TASK_ONCPU,
          .run_flags = ELIDE_PREEMPT,
          .sync_group_owner = sync_group_owner,
          .allow_txn_target_on_cpu = true,
      });
      ASSERT_THAT(req->sync_group_owned(), IsTrue());
      ASSERT_THAT(req->sync_group_owner_get(), Eq(sync_group_owner));
    }

    // sync-group commit.
    //
    // N.B. CommitSyncRequests() releases ownership of transactions in
    // the sync_group but that's not an issue for this test since only
    // the agent on 'sched_cpu_' is doing the sync_group commits.
    bool successful = enclave()->CommitSyncRequests(cpus());

    int num_txns_failed = 0;
    int num_txns_succeeded = 0;
    for (const Cpu& cpu : cpus()) {
      CpuState* cs = cpu_state(cpu);

      const RunRequest* req = enclave()->GetRunRequest(cpu);
      ASSERT_THAT(req->sync_group_owned(), IsFalse());
      ASSERT_THAT(req->committed(), IsTrue());
      if (req->succeeded()) {
        num_txns_succeeded++;
      } else {
        num_txns_failed++;
      }

      if (cs->next != cs->current) {
        ASSERT_THAT(cs->next, NotNull());
        ASSERT_THAT(cs->current, IsNull());
        if (successful) {
          TaskGotOnCpu(cs->next, cpu);  // task is oncpu.
        } else {
          rq_.Enqueue(cs->next);  // put task back to the runqueue.

          // This is not intuitive but even though the overall sync_group
          // failed to commit it is possible for 'cs->next' to get oncpu
          // briefly (only to reschedule itself promptly due to a poisoned
          // rendezvous). However this reschedule could go down the preempt
          // path (e.g. ghost->cfs) and generate TASK_PREEMPTED which would
          // run afoul of the CHECK(!task->preempted) in DispatchMessage().
          cs->next->preempted = false;
        }
      } else if (cs->next) {
        // Attempted an idempotent commit on `cpu` (i.e. next == current).
        ASSERT_THAT(cs->next->run_state, Eq(FifoTaskState::kOnCpu));
        if (!successful) {
          TaskGotOffCpu(cs->next, /*blocked=*/false);
          cs->next->prio_boost = true;
          rq_.Enqueue(cs->next);
        }
      }

      cs->next = nullptr;  // reset for next scheduling round.
    }

    // Verify all-or-nothing semantics.
    //
    // If the sync-group commit was successful then all of its component
    // transactions must be successful. In the failure case at least one
    // of its component transactions must have failed.
    if (successful) {
      EXPECT_THAT(num_txns_succeeded, Eq(cpus().Size()));
    } else {
      EXPECT_THAT(num_txns_failed, Gt(0));
    }

    // At the end of the sync_group commit all txns must have completed.
    EXPECT_THAT(num_txns_succeeded + num_txns_failed, Eq(cpus().Size()));
  }

 protected:
  // Task state change callbacks.
  void TaskNew(FifoTask* task, const Message& msg) final {
    const ghost_msg_payload_task_new* payload =
        static_cast<const ghost_msg_payload_task_new*>(msg.payload());

    task->seqnum = msg.seqnum();
    task->run_state = FifoTaskState::kBlocked;
    if (payload->runnable) {
      task->run_state = FifoTaskState::kRunnable;
      task->cpu = sched_cpu_.id();
      rq_.Enqueue(task);
    }
  }

  void TaskRunnable(FifoTask* task, const Message& msg) final {
    const ghost_msg_payload_task_wakeup* payload =
        static_cast<const ghost_msg_payload_task_wakeup*>(msg.payload());

    EXPECT_NE(task->seqnum, msg.seqnum());

    ASSERT_THAT(task->run_state, Eq(FifoTaskState::kBlocked));
    task->run_state = FifoTaskState::kRunnable;
    task->prio_boost = !payload->deferrable;
    rq_.Enqueue(task);
  }

  void TaskYield(FifoTask* task, const Message& msg) final {
    EXPECT_NE(task->seqnum, msg.seqnum());
    TaskGotOffCpu(task, /*blocked=*/false);
    rq_.Enqueue(task);
  }

  void TaskBlocked(FifoTask* task, const Message& msg) final {
    EXPECT_NE(task->seqnum, msg.seqnum());
    TaskGotOffCpu(task, /*blocked=*/true);
  }

  void TaskPreempted(FifoTask* task, const Message& msg) final {
    EXPECT_NE(task->seqnum, msg.seqnum());
    TaskGotOffCpu(task, /*blocked=*/false);
    task->preempted = true;
    task->prio_boost = true;
    rq_.Enqueue(task);
  }

  void TaskDead(FifoTask* task, const Message& msg) final {
    ASSERT_THAT(task->run_state, Eq(FifoTaskState::kBlocked));
    allocator()->FreeTask(task);
  }

  void TaskDeparted(FifoTask* task, const Message& msg) final {
    allocator()->FreeTask(task);
  }

 private:
  struct CpuState final {
    FifoTask* current = nullptr;
    FifoTask* next = nullptr;
  } ABSL_CACHELINE_ALIGNED;

  CpuState* cpu_state(const Cpu& cpu) {
    CHECK_GE(cpu.id(), 0);
    CHECK_LT(cpu.id(), cpu_states_.size());
    return &cpu_states_[cpu.id()];
  }

  const CpuState* cpu_state(const Cpu& cpu) const {
    CHECK_GE(cpu.id(), 0);
    CHECK_LT(cpu.id(), cpu_states_.size());
    return &cpu_states_[cpu.id()];
  }

  CpuState* cpu_state_of(const FifoTask* task) {
    CHECK_GE(task->cpu, 0);
    CHECK_LT(task->cpu, cpu_states_.size());
    return &cpu_states_[task->cpu];
  }

  void TaskGotOffCpu(FifoTask* task, bool blocked) {
    CpuState* cs = cpu_state_of(task);
    if (task->oncpu()) {
      ASSERT_THAT(cs->current, Eq(task));
      cs->current = nullptr;
    } else {
      ASSERT_THAT(task->run_state, Eq(FifoTaskState::kQueued));
      rq_.Erase(task);
    }

    task->run_state =
        blocked ? FifoTaskState::kBlocked : FifoTaskState::kRunnable;
  }

  void TaskGotOnCpu(FifoTask* task, const Cpu& cpu) {
    ASSERT_THAT(task->run_state, Ne(FifoTaskState::kOnCpu));
    CpuState* cs = cpu_state(cpu);
    cs->current = task;

    task->run_state = FifoTaskState::kOnCpu;
    task->cpu = cpu.id();
    task->preempted = task->prio_boost = false;
  }

  FifoRq rq_;
  Cpu sched_cpu_;  // CPU making the scheduling decisions.
  std::array<CpuState, MAX_CPUS> cpu_states_;
  std::unique_ptr<LocalChannel> channel_;
};

template <typename T>
class TestAgent : public LocalAgent {
 public:
  TestAgent(Enclave* enclave, Cpu cpu, T* scheduler)
      : LocalAgent(enclave, cpu), scheduler_(scheduler) {}

 protected:
  void AgentThread() override {
    // Boilerplate to synchronize startup until all agents are ready.
    SignalReady();
    WaitForEnclaveReady();

    while (true) {
      // Order is important: agent_barrier must be evaluated before Finished().
      // (see cl/339780042 for details).
      BarrierToken agent_barrier = status_word().barrier();
      const bool prio_boost = status_word().boosted_priority();
      const bool finished = Finished();

      if (finished && scheduler_->Empty(cpu())) break;

      scheduler_->Schedule(cpu(), agent_barrier, prio_boost, finished);
    }
  }

 private:
  T* scheduler_;
};

template <class EnclaveType>
class SyncGroupAgent final : public FullAgent<EnclaveType> {
 public:
  explicit SyncGroupAgent(AgentConfig config) : FullAgent<EnclaveType>(config) {
    auto allocator =
        std::make_shared<ThreadSafeMallocTaskAllocator<FifoTask>>();
    scheduler_ = std::make_unique<SyncGroupScheduler>(
        &this->enclave_, config.cpus_, std::move(allocator));
    scheduler_->GetDefaultChannel().SetEnclaveDefault();
    this->StartAgentTasks();
    this->enclave_.Ready();
  }

  ~SyncGroupAgent() final { this->TerminateAgentTasks(); }

  std::unique_ptr<Agent> MakeAgent(const Cpu& cpu) final {
    return std::make_unique<TestAgent<SyncGroupScheduler>>(&this->enclave_, cpu,
                                                           scheduler_.get());
  }

  void RpcHandler(int64_t req, const AgentRpcArgs& args,
                  AgentRpcResponse& response) final {
    response.response_code = -1;
  }

 private:
  std::unique_ptr<SyncGroupScheduler> scheduler_;
};

// std:tuple<int,int> contains the test parameters:
// - first field of tuple is number of cpus.
// - second field of tuple is number of threads.
class SyncGroupTest : public testing::TestWithParam<std::tuple<int, int>> {};

TEST_P(SyncGroupTest, Commit) {
  const auto [num_cpus, num_threads] = GetParam();  // test parameters.

  if (MachineTopology()->num_cpus() < num_cpus) {
    GTEST_SKIP() << "must have at least " << num_cpus << " cpus";
    return;
  }

  // Create a 'cpulist' containing 'num_cpus' CPUs.
  const int first_cpu = 0;
  Topology* topology = MachineTopology();
  std::vector<int> cpuvec(num_cpus);
  std::iota(cpuvec.begin(), cpuvec.end(), first_cpu);
  CpuList cpulist = topology->ToCpuList(std::move(cpuvec));

  auto ap = AgentProcess<SyncGroupAgent<LocalEnclave>, AgentConfig>(
      AgentConfig(topology, cpulist));

  // Create application threads.
  std::vector<std::unique_ptr<GhostThread>> threads;
  threads.reserve(num_threads);
  for (int i = 0; i < num_threads; i++) {
    threads.emplace_back(
        new GhostThread(GhostThread::KernelScheduler::kGhost, [] {
          SpinFor(absl::Milliseconds(100));
          for (int j = 0; j < 100; j++) absl::SleepFor(absl::Microseconds(100));
          SpinFor(absl::Milliseconds(1));
          sched_yield();
        }));
  }

  // Wait for all threads to finish.
  for (auto& t : threads) t->Join();

  GhostHelper()->CloseGlobalEnclaveFds();
}

INSTANTIATE_TEST_SUITE_P(
    SyncGroupTestConfig, SyncGroupTest,
    testing::Combine(testing::Values(1, 2, 4, 8),   // num_cpus
                     testing::Values(1, 4, 8, 16))  // num_threads
);

class IdlingAgent : public LocalAgent {
 public:
  IdlingAgent(Enclave* enclave, Cpu cpu, bool schedule)
      : LocalAgent(enclave, cpu), schedule_(schedule) {}

 protected:
  void AgentThread() final {
    // Boilerplate to synchronize startup until all agents are ready.
    SignalReady();
    WaitForEnclaveReady();

    // remote_cpus is all cpus in the enclave except this one.
    CpuList remote_cpus = *enclave()->cpus();
    remote_cpus.Clear(cpu());

    while (!Finished()) {
      BarrierToken agent_barrier = status_word().barrier();

      // Non-scheduling agents just yield until they are terminated.
      if (!schedule_) {
        RunRequest* req = enclave()->GetRunRequest(cpu());
        req->LocalYield(agent_barrier, /*flags=*/0);
        continue;
      }

      for (const Cpu& remote_cpu : remote_cpus) {
        ASSERT_THAT(remote_cpu, Ne(cpu()));

        int run_flags = 0;

        // NEED_CPU_NOT_IDLE enabled on odd numbered cpus.
        if (remote_cpu.id() % 2) {
          run_flags |= NEED_CPU_NOT_IDLE;
        }

        RunRequest* req = enclave()->GetRunRequest(remote_cpu);
        req->Open({
            .target = Gtid(GHOST_IDLE_GTID),
            .agent_barrier = agent_barrier,
            .commit_flags = COMMIT_AT_TXN_COMMIT | ALLOW_TASK_ONCPU,
            .run_flags = run_flags,
        });
      }
      enclave()->CommitRunRequests(remote_cpus);
    }
  }

 private:
  bool schedule_;
};

constexpr int kNeedCpuNotIdle = 1;

template <class EnclaveType>
class FullIdlingAgent final : public FullAgent<EnclaveType> {
 public:
  explicit FullIdlingAgent(const AgentConfig& config)
      : FullAgent<EnclaveType>(config), channel_(GHOST_MAX_QUEUE_ELEMS, 0) {
    channel_.SetEnclaveDefault();
    // Start an instance of IdlingAgent on each cpu.
    this->StartAgentTasks();

    // Unblock all agents and start scheduling.
    this->enclave_.Ready();
  }

  ~FullIdlingAgent() final { this->TerminateAgentTasks(); }

  std::unique_ptr<Agent> MakeAgent(const Cpu& cpu) final {
    const bool schedule = (cpu.id() == 0);  // schedule on the first cpu.
    return std::make_unique<IdlingAgent>(&this->enclave_, cpu, schedule);
  }

  void RpcHandler(int64_t req, const AgentRpcArgs& args,
                  AgentRpcResponse& response) final {
    if (req == kNeedCpuNotIdle) {
      const int num_cpus = this->enclave_.cpus()->Size();
      std::vector<int> not_idle_msg_count(num_cpus);

      Message msg;
      while (!(msg = Peek(&channel_)).empty()) {
        if (msg.type() == MSG_CPU_NOT_IDLE) {
          // Count the number of CPU_NOT_IDLE messages seen on each cpu.
          const ghost_msg_payload_cpu_not_idle* payload =
              static_cast<const ghost_msg_payload_cpu_not_idle*>(msg.payload());
          EXPECT_THAT(payload->cpu, Ge(0));
          EXPECT_THAT(payload->cpu, Lt(num_cpus));
          ++not_idle_msg_count[payload->cpu];
        }
        Consume(&channel_, msg);
      }

      int num_failures = 0;
      for (int cpu = 0; cpu < num_cpus; ++cpu) {
        int msg_count = not_idle_msg_count[cpu];
        if (cpu % 2) {
          // Odd numbered CPUs set NEED_CPU_NOT_IDLE in `run_flags` so
          // we expect at least one CPU_NOT_IDLE message on these CPUs.
          EXPECT_THAT(msg_count, Gt(0));
          if (msg_count <= 0) {
            ++num_failures;
          }
        } else {
          // Even numbered CPUs don't set NEED_CPU_NOT_IDLE in `run_flags`
          // so we don't expect any CPU_NOT_IDLE messages on these CPUs.
          EXPECT_THAT(msg_count, Eq(0));
          if (msg_count != 0) {
            ++num_failures;
          }
        }
      }
      response.response_code = num_failures;
      return;
    } else {
      response.response_code = -1;
      return;
    }
  }

 private:
  LocalChannel channel_;
};

TEST(IdleTest, NeedCpuNotIdle) {
  // cpu0   Spinning agent scheduling on cpus 1 and 2.
  // cpu1   Run GHOST_IDLE_GTID with NEED_CPU_NOT_IDLE.
  // cpu2   Run GHOST_IDLE_GTID.
  constexpr int kNumCpus = 3;

  Topology* topology = MachineTopology();
  CpuList all_cpus = topology->all_cpus();
  if (all_cpus.Size() < kNumCpus) {
    GTEST_SKIP();
    return;
  }

  // Create a `cpulist` containing `kNumCpus`.
  const int first_cpu = 0;
  std::vector<int> cpuvec(kNumCpus);
  std::iota(cpuvec.begin(), cpuvec.end(), first_cpu);
  CpuList cpulist = topology->ToCpuList(std::move(cpuvec));

  auto ap = AgentProcess<FullIdlingAgent<LocalEnclave>, AgentConfig>(
      AgentConfig(topology, cpulist));

  // Create application threads.
  std::vector<std::unique_ptr<GhostThread>> threads;
  for (int cpu = first_cpu + 1; cpu < kNumCpus; cpu++) {
    // Start CFS task on each non-scheduling CPU to induce IDLE->CFS
    // scheduling edges thereby triggering CPU_NOT_IDLE messages if
    // the agent sets NEED_CPU_NOT_IDLE in `run_flags`.
    threads.emplace_back(
        new GhostThread(GhostThread::KernelScheduler::kCfs, [cpu] {
          EXPECT_THAT(GhostHelper()->SchedSetAffinity(
                          Gtid::Current(),
                          MachineTopology()->ToCpuList(std::vector<int>{cpu})),
                      Eq(0));
          SpinFor(absl::Milliseconds(1));
          for (int i = 0; i < 100; i++) absl::SleepFor(absl::Microseconds(10));
          SpinFor(absl::Milliseconds(1));
          sched_yield();
        }));
  }

  // Wait for all threads to finish.
  for (auto& t : threads) t->Join();

  // Verify that the kernel produces MSG_CPU_NOT_IDLE depending on whether
  // NEED_CPU_NOT_IDLE is set in `run_flags`.
  EXPECT_THAT(ap.Rpc(kNeedCpuNotIdle), 0);

  GhostHelper()->CloseGlobalEnclaveFds();
}

struct CoreSchedTask : public Task<> {
  explicit CoreSchedTask(Gtid task_gtid, ghost_sw_info sw_info)
      : Task<>(task_gtid, sw_info) {}
  ~CoreSchedTask() override {}

  enum class RunState {
    kBlocked,
    kRunnable,
    kOnCpu,
  };

  bool blocked() const { return run_state == RunState::kBlocked; }
  bool runnable() const { return run_state == RunState::kRunnable; }
  bool oncpu() const { return run_state == RunState::kOnCpu; }

  enum RunState run_state = RunState::kBlocked;
  int sibling = -1;
};

class CoreScheduler {
 public:
  explicit CoreScheduler(Enclave* enclave)
      : enclave_(enclave),
        siblings_(*enclave->cpus()),
        next_sibling_(-1),
        task_channel_(GHOST_MAX_QUEUE_ELEMS, /*node=*/0, *enclave->cpus()) {
    // Note that 'task_channel_' is configured such that either of
    // siblings can be woken up when a message is produced into it.
    //
    // We verify below that the kernel wakes up the sibling that
    // the task last ran on.
  }

  bool Empty(const Cpu& cpu) const {
    absl::MutexLock lock(&mu_);
    return task_ == nullptr;
  }

  // Admit a new task into the scheduler.
  void TaskNew(uint64_t gtid, bool runnable, ghost_sw_info sw_info,
               uint32_t seqnum) {
    absl::MutexLock lock(&mu_);

    ASSERT_THAT(task_, IsNull());
    ASSERT_THAT(runnable, IsTrue());

    task_ = std::make_unique<CoreSchedTask>(Gtid(gtid), sw_info);
    task_->run_state = CoreSchedTask::RunState::kRunnable;
    task_->seqnum = seqnum;
    task_->sibling = 0;  // arbitrary.

    ASSERT_THAT(task_channel_.AssociateTask(task_->gtid, task_->seqnum,
                                            /*status=*/nullptr),
                IsTrue());

    // A sync_group commit can fail on the _local_ cpu (e.g. due a stale
    // agent barrier). In this case the task_ will run momentarily on the
    // remote cpu before noticing a poisoned rendezvous and rescheduling.
    // We set ALLOW_TASK_ONCPU in 'commit_flags' to account for this case.
    //
    // However this doesn't handle the case where a TASK_NEW is produced
    // but the task hasn't fully gotten offcpu (ALLOW_TASK_ONCPU is only
    // relevant if the task is oncpu on the cpu associated with the txn).
    //
    // We handle this here rather than setting 'allow_txn_target_on_cpu'
    // in the sync_group commit since we don't need it in the common case.
    while (task_->status_word.on_cpu()) {
      // Wait for the task to get offcpu before making it available to
      // the CoreScheduler.
    }

    const Cpu cpu = siblings_[task_->sibling];
    ASSERT_THAT(enclave_->GetAgent(cpu)->Ping(), IsTrue());
  }

  void Schedule(const Cpu& agent_cpu, uint32_t agent_barrier, bool prio_boost,
                bool finished) {
    // MutexLock cannot be used because we must drop the lock before
    // calling scheduling functions like LocalYield() that block in
    // the kernel.
    mu_.Lock();

    // At the end of this loop there are no inflight transactions and
    // the mutex serializes with the sibling's scheduling loop.
    for (const Cpu& cpu : siblings_) {
      CpuState* cs = cpu_state(cpu);
      if (!cs->next) continue;

      EXPECT_THAT(cs->current, IsNull());
      EXPECT_THAT(cs->next, Eq(task_.get()));
      EXPECT_THAT(next_sibling_, AnyOf(Eq(0), Eq(1)));

      RunRequest* req = enclave_->GetRunRequest(cpu);
      if (enclave_->CompleteRunRequest(req)) {
        // Transaction committed successfully: promote 'cs->next' to 'current'.
        cs->current = cs->next;
        task_->sibling = next_sibling_;
        task_->run_state = CoreSchedTask::RunState::kOnCpu;
      }

      cs->next = nullptr;
    }

    if (task_) {
      // In the common case sibling 0 kicks things off via TaskNew() and
      // schedules 'task_' on sibling 1 and the NULL_GTID on its own CPU
      // blocking itself. If the sync_group commit succeeds then we expect
      // the next wakeup on sibling 1 when 'task_' schedules for whatever
      // reason (preempt/block/yield). Sibling 1 then returns the favor by
      // scheduling 'task_' on sibling 0 and blocking itself. Assuming the
      // sync_group commit succeeds then the next wakeup is on sibling 0
      // ... and the cycle continues.
      //
      // However there are other events that can wake an agent out of turn:
      // - after delivering a MSG_CPU_TICK.
      // - as a side-effect of CONFIG_QUEUE_WAKEUP.
      // - via set_curr_ghost (e.g. when an oncpu task enters ghost on one
      //   of the cpus in the enclave).
      //
      // Additionally there is an inherent race when the agents are released:
      // we ping sibling 0 in TaskNew() to kick things off with the newly
      // minted 'task_' but there is no guarantee that sibling 1 is won't
      // see it first (we could use Notifiers to ensure that both agents
      // are blocked _before_ calling TaskNew() but that is still not
      // sufficient given the out-of-turn wakeups described above).
      //
      // If we detect that an agent has woken up out of turn then we just
      // block here with the expectation that the real wakeup will get it
      // running again.
      //
      // While this does dilute the premise of the test we validate that
      // the number of bogus_wakeups is less than 5% of the legit_wakeups.
      if (agent_cpu != siblings_[task_->sibling]) {
        mu_.Unlock();
        if (!finished) {
          bogus_wakeups_.fetch_add(1, std::memory_order_relaxed);
        }
        RunRequest* req = enclave_->GetRunRequest(agent_cpu);
        const int run_flags = finished ? RTLA_ON_IDLE : 0;
        req->LocalYield(agent_barrier, run_flags);
        return;
      } else {
        if (!finished) {
          legit_wakeups_.fetch_add(1, std::memory_order_relaxed);
        }
      }
    } else {
      if (finished) {
        mu_.Unlock();
        return;
      }
      no_task_wakeups_.fetch_add(1, std::memory_order_relaxed);
    }

    // We are testing agent wakeups so this test depends on controlling when
    // each agent wakes up. The one wakeup we cannot control is when the main
    // thread pings the agent via Agent::Terminate(). Thus we consume messages
    // only if the agent isn't being terminated or if it is the rightful agent
    // to handle the final TASK_BLOCKED/TASK_DEAD messages (see cl/334728088
    // for a detailed description of the race between pthread_join() and
    // task_dead_ghost()).
    while (task_) {
      Message msg = Peek(&task_channel_);
      if (msg.empty()) break;

      // 'task_channel_' is exclusively for task state change msgs.
      EXPECT_THAT(msg.is_task_msg(), IsTrue());
      EXPECT_THAT(msg.type(), Ne(MSG_TASK_NEW));

      task_->Advance(msg.seqnum());

      Cpu task_cpu = siblings_[task_->sibling];
      CpuState* cs = cpu_state(task_cpu);
      EXPECT_THAT(agent_cpu, Eq(task_cpu));

      // Scheduling is simple: we track whether the task is runnable
      // or blocked. If the task is runnable the agent schedules it
      // on the sibling cpu.
      //
      // We verify that 'agent_cpu' and 'task_cpu' match in all cases.
      switch (msg.type()) {
        case MSG_TASK_WAKEUP:
          EXPECT_THAT(task_->blocked(), IsTrue());
          EXPECT_THAT(cs->current, IsNull());
          task_->run_state = CoreSchedTask::RunState::kRunnable;
          break;

        case MSG_TASK_BLOCKED:
          EXPECT_THAT(task_->oncpu(), IsTrue());
          EXPECT_THAT(cs->current, Eq(task_.get()));
          task_->run_state = CoreSchedTask::RunState::kBlocked;
          cs->current = nullptr;
          break;

        case MSG_TASK_DEAD:
          EXPECT_THAT(task_->blocked(), IsTrue());
          EXPECT_THAT(cs->current, IsNull());
          task_.reset();
          break;

        case MSG_TASK_YIELD:
        case MSG_TASK_PREEMPT:
          EXPECT_THAT(task_->oncpu(), IsTrue());
          EXPECT_THAT(cs->current, Eq(task_.get()));
          task_->run_state = CoreSchedTask::RunState::kRunnable;
          cs->current = nullptr;
          break;

        default:
          EXPECT_THAT(false, IsTrue()) << "unexpected msg: " << msg.stringify();
          break;
      }

      Consume(&task_channel_, msg);
    }

    // Either we don't have a runnable task or a task in another sched_class
    // wants to run on this cpu.
    if (prio_boost || !task_ || !task_->runnable()) {
      // Don't yield CPU indefinitely if we have useful work to do:
      // e.g. schedule a runnable task or terminate.
      const int run_flags =
          (task_ && task_->runnable()) || finished ? RTLA_ON_IDLE : 0;
      mu_.Unlock();
      RunRequest* req = enclave_->GetRunRequest(agent_cpu);
      req->LocalYield(agent_barrier, run_flags);
      return;
    }

    EXPECT_THAT(task_->runnable(), IsTrue());

    // Idle on 'agent_cpu' and schedule task on 'other_cpu'.
    int other_sibling = agent_cpu == siblings_[0] ? (siblings_.Size() - 1) : 0;
    Cpu other_cpu = siblings_[other_sibling];
    CpuState* cs = cpu_state(other_cpu);

    EXPECT_THAT(cs->current, IsNull());

    const int sync_group_owner = agent_cpu.id();
    RunRequest* this_req = enclave_->GetRunRequest(agent_cpu);
    this_req->Open({
        .target = Gtid(GHOST_NULL_GTID),
        .agent_barrier = agent_barrier,
        .commit_flags = COMMIT_AT_TXN_COMMIT,
        .run_flags = finished ? RTLA_ON_IDLE : 0,
        .sync_group_owner = sync_group_owner,
    });

    EXPECT_THAT(this_req->sync_group_owned(), IsTrue());
    EXPECT_THAT(this_req->sync_group_owner_get(), Eq(sync_group_owner));

    cs->next = task_.get();
    next_sibling_ = other_sibling;
    RunRequest* other_req = enclave_->GetRunRequest(other_cpu);

    other_req->Open({
        .target = cs->next->gtid,
        .target_barrier = cs->next->seqnum,
        .commit_flags = COMMIT_AT_TXN_COMMIT | ALLOW_TASK_ONCPU,
        .sync_group_owner = sync_group_owner,
    });
    EXPECT_THAT(other_req->sync_group_owned(), IsTrue());
    EXPECT_THAT(other_req->sync_group_owner_get(), Eq(sync_group_owner));

    mu_.Unlock();

    if (enclave_->CommitSyncRequests(siblings_)) {
      // Commit succeeded and the kernel has already released all transactions
      // in the sync_group on our behalf. Here's why: if this cpu is part of
      // the sync group then the local agent doesn't get an opportunity to
      // release ownership before it schedules.
    } else {
      // The sync_group commit failed and txn ownership was released by the
      // CommitSyncRequests() API after validating the reason for failure.
      //
      // N.B. if the overall sync_group commit failed due to failure of the
      // _local_ transaction then it is possible to observe 'task_' oncpu in
      // the subsequent sync_group commit (this happens because the task
      // must still get oncpu, observe the poisoned rendezvous and then
      // resched to get itself offcpu). We account for this by setting
      // ALLOW_TASK_ONCPU in the remote txn's commit_flags.
    }
  }

  int bogus_wakeups() const { return bogus_wakeups_.load(); }
  int legit_wakeups() const { return legit_wakeups_.load(); }
  int no_task_wakeups() const { return no_task_wakeups_.load(); }

 private:
  struct CpuState {
    CoreSchedTask* current = nullptr;
    CoreSchedTask* next = nullptr;
  } ABSL_CACHELINE_ALIGNED;

  CpuState* cpu_state(const Cpu& cpu) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    CHECK_GE(cpu.id(), 0);
    CHECK_LT(cpu.id(), cpu_states_.size());
    return &cpu_states_[cpu.id()];
  }

  Enclave* enclave_;
  const CpuList& siblings_;

  std::atomic<int> bogus_wakeups_ = 0;
  std::atomic<int> legit_wakeups_ = 0;
  std::atomic<int> no_task_wakeups_ = 0;

  mutable absl::Mutex mu_;
  int next_sibling_ ABSL_GUARDED_BY(mu_);
  LocalChannel task_channel_ ABSL_GUARDED_BY(mu_);
  std::unique_ptr<CoreSchedTask> task_ ABSL_GUARDED_BY(mu_);
  std::array<CpuState, MAX_CPUS> cpu_states_ ABSL_GUARDED_BY(mu_);
};

// This test ensures the version check functionality works properly.
// 'GhostHelper()->GetVersion' should return a version that matches
// 'GHOST_VERSION'.
TEST(ApiTest, CheckVersion) {
  std::vector<uint32_t> kernel_abi_versions;
  ASSERT_THAT(GhostHelper()->GetSupportedVersions(kernel_abi_versions), Eq(0));

  auto iter = std::find(kernel_abi_versions.begin(), kernel_abi_versions.end(),
                        GHOST_VERSION);
  ASSERT_THAT(iter, testing::Ne(kernel_abi_versions.end()));
}

// Bare-bones agent implementation that can schedule exactly one task.
// TODO: Put agent and test thread into separate address spaces to
// avoid potential deadlock.
class TimeAgent : public LocalAgent {
 public:
  TimeAgent(Enclave* enclave, Cpu cpu)
      : LocalAgent(enclave, cpu),
        channel_(GHOST_MAX_QUEUE_ELEMS, kNumaNode,
                 MachineTopology()->ToCpuList({cpu})) {
    channel_.SetEnclaveDefault();
  }

  ~TimeAgent() {
    std::cout << "switch_delays:" << std::endl;
    for (absl::Duration delay : switch_delays_) {
      std::cout << absl::ToInt64Nanoseconds(delay) << " ns" << std::endl;
    }

    std::cout << "commit_delays:" << std::endl;
    for (absl::Duration delay : commit_delays_) {
      std::cout << absl::ToInt64Nanoseconds(delay) << " ns" << std::endl;
    }
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
        Message msg = Peek(&channel_);
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
            task =
                std::make_unique<Task<>>(Gtid(payload->gtid), payload->sw_info);
            task->seqnum = msg.seqnum();
            runnable = payload->runnable;
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

          case MSG_TASK_YIELD: {
            ASSERT_THAT(task, NotNull());
            ASSERT_TRUE(runnable);
            runnable = true;
            ASSERT_THAT(commit_time_, Ne(absl::UnixEpoch()));
            absl::Duration switch_delay =
                task->status_word.switch_time() - commit_time_;
            EXPECT_THAT(switch_delay, Gt(absl::ZeroDuration()));
            if (num_yields_++) {
              EXPECT_THAT(switch_delay, Lt(absl::Microseconds(100)));
            }
            switch_delays_.push_back(switch_delay);
            break;
          }

          case MSG_TASK_DEAD:
            ASSERT_THAT(task, NotNull());
            ASSERT_FALSE(runnable);
            task = nullptr;
            break;

          default:
            // This includes task messages like TASK_PREEMPTED and cpu messages
            // like CPU_TICK that don't influence runnability. We do handle
            // MSG_TASK_YIELD as a special case above since we want to check the
            // context switch time.
            break;
        }
        Consume(&channel_, msg);
      }

      BarrierToken agent_barrier = status_word().barrier();
      const bool prio_boost = status_word().boosted_priority();

      if (Finished() && !task) break;

      RunRequest* req = enclave()->GetRunRequest(cpu());
      if (!task || !runnable || prio_boost) {
        NotifyIdle();
        req->LocalYield(agent_barrier, prio_boost ? RTLA_ON_IDLE : 0);
      } else if (task->status_word.on_cpu()) {
        // 'task' is still oncpu: just loop back and try to schedule it
        // in the next iteration.
      } else {
        absl::Time now = MonotonicNow();
        req->Open({
            .target = task->gtid,
            .target_barrier = task->seqnum,
            .agent_barrier = agent_barrier,
            .commit_flags = COMMIT_AT_TXN_COMMIT,
        });
        req->Commit();

        commit_time_ = req->commit_time();
        absl::Duration commit_delay = commit_time_ - now;
        EXPECT_THAT(commit_delay, Gt(absl::ZeroDuration()));
        if (num_commits_++) {
          EXPECT_THAT(commit_delay, Lt(absl::Microseconds(100)));
        }
        commit_delays_.push_back(commit_delay);
      }
    }
  }

 private:
  // The NUMA node that the channel is on.
  static constexpr int kNumaNode = 0;

  // Notify the main thread when agent idles for the first time.
  void NotifyIdle() {
    if (first_idle_) {
      first_idle_ = false;
      idle_.Notify();
    }
  }

  LocalChannel channel_;

  Notification idle_;
  absl::Time commit_time_;
  bool first_idle_ = true;
  int num_commits_ = 0;
  int num_yields_ = 0;
  std::vector<absl::Duration> switch_delays_;
  std::vector<absl::Duration> commit_delays_;
};

// Tests that the kernel writes plausible commit times to transactions and
// plausible context switch times to task status words.
TEST(ApiTest, KernelTimes) {
  GhostHelper()->InitCore();

  // Arbitrary but safe because there must be at least one CPU.
  constexpr int kCpuNum = 0;
  Topology* topology = MachineTopology();
  auto enclave = std::make_unique<LocalEnclave>(
      AgentConfig(topology, topology->ToCpuList(std::vector<int>{kCpuNum})));
  const Cpu kAgentCpu = topology->cpu(kCpuNum);

  TimeAgent agent(enclave.get(), kAgentCpu);
  agent.Start();
  enclave->Ready();

  agent.WaitForIdle();

  GhostThread t(GhostThread::KernelScheduler::kGhost, [] {
    for (int i = 0; i < 10; i++)
      sched_yield();  // MSG_TASK_YIELD.
  });
  t.Join();

  agent.Terminate();
}

// Tests that `GhostHelper()->SchedGetAffinity()` and
// `GhostHelper()->SchedSetAffinity()` returns/sets the affinity mask for a
// thread.
//
// It is possible for any CPU to be disallowed on a DevRez machine, such as when
// this test is run inside a sys container via cpuset. Thus, to avoid this
// issue, the test first gets its current affinity mask and then removes a CPU
// from that mask rather than affine itself to arbitrary CPUs that may not be
// available.
TEST(ApiTest, SchedGetAffinity) {
  CpuList cpus = MachineTopology()->EmptyCpuList();
  ASSERT_THAT(GhostHelper()->SchedGetAffinity(Gtid::Current(), cpus), Eq(0));
  // This test requires at least 2 CPUs.
  if (cpus.Size() < 2) {
    GTEST_SKIP() << "must have at least 2 cpus";
    return;
  }

  cpus.Clear(cpus.Front());
  ASSERT_THAT(GhostHelper()->SchedSetAffinity(Gtid::Current(), cpus), Eq(0));

  CpuList set_cpus = MachineTopology()->EmptyCpuList();
  ASSERT_THAT(GhostHelper()->SchedGetAffinity(Gtid::Current(), set_cpus),
              Eq(0));
  EXPECT_THAT(set_cpus, Eq(cpus));
}

// SchedAffinityAgent tries to induce a race between latching a task on a cpu
// and then doing sched_setaffinity() to blacklist that cpu. As a result
// of the setaffinity the kernel moves the task off the blacklisted cpu.
// This in turn invalidates the latched_task which prior to the kernel
// fix would just result in task stranding (task is no longer latched
// and kernel did not produce any msg to let the agent know).
class SchedAffinityAgent : public LocalAgent {
 public:
  SchedAffinityAgent(Enclave* enclave, const Cpu& this_cpu, Channel* channel)
      : LocalAgent(enclave, this_cpu), channel_(channel) {}

 protected:
  void AgentThread() final {
    // Boilerplate to synchronize startup until both agents are ready.
    SignalReady();
    WaitForEnclaveReady();

    // Satellite agent that simply yields.
    if (!channel_) {
      RunRequest* req = enclave()->GetRunRequest(cpu());
      while (!Finished()) {
        req->LocalYield(status_word().barrier(), /*flags=*/0);
      }
      return;
    }

    // Spinning agent that continuously schedules `task`.
    ASSERT_THAT(channel_, NotNull());

    bool oncpu = false;
    bool runnable = false;
    std::unique_ptr<Task<>> task(nullptr);  // initialized in TASK_NEW handler.

    CpuList task_cpulist = *enclave()->cpus();
    task_cpulist.Clear(cpu());  // don't schedule on spinning agent's cpu.
    size_t task_cpu_idx = 0;    // schedule task on task_cpulist[task_cpu_idx]

    while (true) {
      const BarrierToken agent_barrier = status_word().barrier();

      while (true) {
        Message msg = Peek(channel_);
        if (msg.empty()) break;

        // Ignore all types other than task messages (e.g. CPU_TICK).
        if (msg.is_task_msg() && msg.type() != MSG_TASK_NEW) {
          ASSERT_THAT(task, NotNull());
          task->Advance(msg.seqnum());
        }

        // Scheduling is simple: we track whether the task is runnable
        // and oncpu. If the task is runnable and not already oncpu the
        // agent schedules it.
        switch (msg.type()) {
          case MSG_TASK_NEW: {
            const ghost_msg_payload_task_new* payload =
                static_cast<const ghost_msg_payload_task_new*>(msg.payload());

            ASSERT_THAT(task, IsNull());
            ASSERT_FALSE(oncpu);
            ASSERT_FALSE(runnable);
            task =
                std::make_unique<Task<>>(Gtid(payload->gtid), payload->sw_info);
            task->seqnum = msg.seqnum();
            runnable = payload->runnable;
            break;
          }

          case MSG_TASK_WAKEUP:
            ASSERT_THAT(task, NotNull());
            ASSERT_FALSE(oncpu);
            ASSERT_FALSE(runnable);
            runnable = true;
            break;

          case MSG_TASK_BLOCKED:
            ASSERT_THAT(task, NotNull());
            ASSERT_TRUE(oncpu);
            ASSERT_TRUE(runnable);
            runnable = false;
            oncpu = false;
            break;

          case MSG_TASK_DEAD:
            ASSERT_THAT(task, NotNull());
            ASSERT_FALSE(runnable);
            ASSERT_FALSE(oncpu);
            task = nullptr;
            break;

          case MSG_TASK_PREEMPT:
          case MSG_TASK_YIELD:
            ASSERT_THAT(task, NotNull());
            ASSERT_TRUE(runnable);
            ASSERT_TRUE(oncpu);
            oncpu = false;
            break;

          default:
            // This includes messages like CPU_TICK that don't influence
            // runnability.
            break;
        }
        Consume(channel_, msg);
      }

      if (Finished() && !task) break;

      if (status_word().boosted_priority()) {
        RunRequest* req = enclave()->GetRunRequest(cpu());
        req->LocalYield(agent_barrier, RTLA_ON_IDLE);
      } else if (runnable && !oncpu) {
        Cpu run_cpu = task_cpulist[task_cpu_idx++ % task_cpulist.Size()];
        int run_flags = 0;

        // Finished() returned 'true' which means that the FullAgent object
        // is being destroyed:
        // ~FullSchedAffinityAgent()->TerminateAgentTasks()->TerminateBegin()
        //
        // This implies that the main thread has made its way beyond t.Join().
        //
        // However this does not mean that the task is dead: pthread_join()
        // can return much before the dying task has made its way to TASK_DEAD
        // (CLONE_CHILD_CLEARTID sync via do_exit()->exit_mm()->mm_release()).
        //
        // At this point we cannot reliably schedule on satellite cpus (agents
        // on those cpus may have observed Finished() and exited). But we must
        // schedule 'task' somewhere so it can proceed to TASK_DEAD.
        //
        // Schedule the task on our local cpu in this situation.
        if (Finished()) {
          run_cpu = cpu();
          run_flags = RTLA_ON_PREEMPT | RTLA_ON_BLOCKED | RTLA_ON_YIELD;
        }

        RunRequest* req = enclave()->GetRunRequest(run_cpu);
        req->Open({
            .target = task->gtid,
            .target_barrier = task->seqnum,
            .agent_barrier = agent_barrier,
            .commit_flags = COMMIT_AT_TXN_COMMIT,
            .run_flags = run_flags,
        });

        // Commit the txn on `run_cpu` and then set the task's affinity to
        // blacklist the same `run_cpu`. By committing the txn synchronously
        // we hope setaffinity to catch it while the task is still latched.
        if (req->Commit()) {
          CpuList new_cpulist = task_cpulist;
          new_cpulist.Clear(run_cpu);
          if (GhostHelper()->SchedSetAffinity(task->gtid, new_cpulist)) {
            ASSERT_THAT(errno, Eq(ESRCH));
          }
          oncpu = true;
        }
      } else {
        // nothing: either task is not runnable or already oncpu.
      }
    }
  }

 private:
  Channel* channel_;
};

template <class EnclaveType = LocalEnclave>
class FullSchedAffinityAgent final : public FullAgent<EnclaveType> {
 public:
  explicit FullSchedAffinityAgent(const AgentConfig& config)
      : FullAgent<EnclaveType>(config),
        sched_cpu_(config.cpus_.Front()),
        channel_(GHOST_MAX_QUEUE_ELEMS, sched_cpu_.numa_node()) {
    channel_.SetEnclaveDefault();
    // Start an instance of SchedAffinityAgent on each cpu.
    this->StartAgentTasks();

    // Unblock all agents and start scheduling.
    this->enclave_.Ready();
  }

  ~FullSchedAffinityAgent() final { this->TerminateAgentTasks(); }

  std::unique_ptr<Agent> MakeAgent(const Cpu& cpu) final {
    Channel* channel_ptr = &channel_;
    if (cpu != sched_cpu_) {
      channel_ptr = nullptr;  // sentinel value to indicate a satellite cpu.
    }
    return std::make_unique<SchedAffinityAgent>(&this->enclave_, cpu,
                                                channel_ptr);
  }

  void RpcHandler(int64_t req, const AgentRpcArgs& args,
                  AgentRpcResponse& response) final {
    response.response_code = -1;
  }

 private:
  const Cpu sched_cpu_;   // CPU running the main scheduling loop.
  LocalChannel channel_;  // Channel configured to wakeup `sched_cpu_`.
};

TEST(ApiTest, SchedAffinityRace) {
  // This test requires at least 3 cpus:
  // - one cpu for the spinning agent.
  // - at least two cpus for the ghost application thread (so that the cpuset
  //   passed to sched_setaffinity() is not empty even after clearing one cpu).
  Topology* topology = MachineTopology();
  CpuList agent_cpus = topology->all_cpus();
  if (agent_cpus.Size() < 3) {
    GTEST_SKIP() << "must have at least 3 cpus";
    return;
  }

  auto ap = AgentProcess<FullSchedAffinityAgent<>, AgentConfig>(
      AgentConfig(topology, agent_cpus));

  GhostThread t(GhostThread::KernelScheduler::kGhost, [] {
    // Empirically 10 iterations are more than sufficient to trigger the race
    // between latching a task on a cpu and then calling sched_setaffinity()
    // to blacklist that cpu. Prior to the kernel changes the test reliably
    // hangs on both virtme and real machines.
    for (int i = 0; i < 10; i++) {
      sched_yield();
    }
  });
  t.Join();

  // When AgentProcess goes out of scope its destructor will trigger
  // the FullSchedAffinityAgent destructor that in turn will Terminate()
  // the agents.

  // Since we were a ghOSt client, we were using an enclave.  Now that the test
  // is over, we need to reset so we can get a fresh enclave later.  Note that
  // we used AgentProcess, so the only user of the gbl_enclave_fd_ is us, the
  // client.
  GhostHelper()->CloseGlobalEnclaveFds();
}

// DepartedRaceAgent tries to induce a race in producing MSG_TASK_NEW
// and MSG_TASK_DEPARTED such that TASK_DEPARTED is the first message
// produced for a task. This violates the assumption that TASK_NEW is
// the first message produced and confuses the agent.
//
// The kernel defers producing a TASK_NEW msg when a running task switches
// into ghost until the task schedules (the ghost set_curr_task handler
// forces the issue by setting NEED_RESCHED on the task). The expectation
// is that the task will schedule immediately and TASK_NEW is produced via
// ghost_prepare_task_switch().
//
// In some cases however this assumption is broken. For e.g. when an oncpu
// task is moved to the ghost sched_class while it is in the kernel moving
// itself _out_ of the ghost sched_class.
//
//      Initial conditions: task p is running on cpu-x in the cfs sched_class.
//      cpu-x                               cpu-y
//  T0                                      sched_setscheduler(p, ghost)
//                                          task_rq_lock(p)
//  T1  sched_setscheduler(p, cfs)
//      spinning on task_rq_lock(p)
//      held by cpu-y.
//  T2                                      p->sched_class = ghost_sched_class
//
//  T3                                      p->ghost.new_task = true via
//                                          switched_to_ghost(). MSG_TASK_NEW
//                                          deferred until 'p' gets offcpu.
//
//  T4                                      set_tsk_need_resched(curr) via
//                                          set_curr_task_ghost() to get 'p'
//                                          offcpu.
//
//  T5                                      task_rq_unlock(p) before returning
//                                          from sched_setscheduler().
//
//  T6  ... acquire task_rq_lock(p)
//      p->sched_class = cfs_sched_class
//
//  T7  produce TASK_DEPARTED msg via
//      switched_from_ghost() while the
//      TASK_NEW msg is still deferred.
//
// All current agents treat this is a benign error and just drop the
// TASK_DEPARTED msg (regardless the message reordering is a kernel
// bug that should not be condoned).
//
// It is possible for TASK_NEW and TASK_AFFINITY_CHANGED to be reordered
// in a similar way. We reuse the same test to trigger this reordering.
class DepartedRaceAgent : public LocalAgent {
 public:
  DepartedRaceAgent(Enclave* enclave, const Cpu& this_cpu, Channel* channel)
      : LocalAgent(enclave, this_cpu), channel_(channel) {}

 protected:
  void AgentThread() final {
    // Boilerplate to synchronize startup until both agents are ready.
    SignalReady();
    WaitForEnclaveReady();

    // Satellite agent that simply yields.
    if (!channel_) {
      RunRequest* req = enclave()->GetRunRequest(cpu());
      while (!Finished()) {
        req->LocalYield(status_word().barrier(), /*flags=*/0);
      }
      return;
    }

    FifoRq run_queue;
    int num_tasks = 0;
    auto allocator =
        std::make_unique<SingleThreadMallocTaskAllocator<FifoTask>>();

    CpuList avail_cpus = *enclave()->cpus();
    const Cpu& agent_cpu = cpu();
    avail_cpus.Clear(agent_cpu);  // don't schedule on spinning agent's cpu.

    while (true) {
      while (true) {
        Message msg = Peek(channel_);
        if (msg.empty()) {
          break;
        }

        // CPU_TICK msgs can be produced when the BPF program is not installed.
        // (e.g. during agent startup and shutdown).
        if (!msg.is_task_msg()) {
          Consume(channel_, msg);
          continue;
        }

        Gtid gtid = msg.gtid();
        FifoTask* task = nullptr;

        if (msg.type() == MSG_TASK_NEW) {
          const ghost_msg_payload_task_new* payload =
              static_cast<const ghost_msg_payload_task_new*>(msg.payload());
          bool allocated;
          std::tie(task, allocated) = allocator->GetTask(gtid,
                                                         payload->sw_info);
          ASSERT_THAT(allocated, IsTrue());
        } else {
          task = allocator->GetTask(gtid);
          if (!task) {
            // This is not supposed to happen except that we are inducing the
            // race deliberately here. Prior to the kernel mitigation we got
            // the following TASK_DEPARTED without a preceding TASK_NEW:
            // MSG_TASK_DEPARTED seq=1 B0/2035 on cpu 8
            //                   ^^^^^
            // seq=1 indicates this is the first msg produced for this task.
            absl::FPrintF(stderr, "%s\n", msg.stringify());
          }
          ASSERT_THAT(task, Ne(nullptr));
          task->Advance(msg.seqnum());
        }

        // Scheduling is simple: we track whether the task is runnable
        // and oncpu. If the task is runnable and not already oncpu the
        // agent schedules it.
        switch (msg.type()) {
          case MSG_TASK_NEW: {
            const ghost_msg_payload_task_new* payload =
                static_cast<const ghost_msg_payload_task_new*>(msg.payload());
            task->seqnum = msg.seqnum();
            task->cpu = agent_cpu.id();
            if (payload->runnable) {
              task->run_state = FifoTaskState::kRunnable;
              run_queue.Enqueue(task);
            }
            ASSERT_THAT(++num_tasks, Gt(0));
            break;
          }

          case MSG_TASK_WAKEUP:
            ASSERT_TRUE(task->blocked());
            task->run_state = FifoTaskState::kRunnable;
            run_queue.Enqueue(task);
            break;

          case MSG_TASK_BLOCKED: {
            const ghost_msg_payload_task_blocked* payload =
              static_cast<const ghost_msg_payload_task_blocked*>(msg.payload());
            ASSERT_TRUE(task->oncpu() || payload->from_switchto);
            ASSERT_FALSE(avail_cpus.IsSet(payload->cpu));
            task->run_state = FifoTaskState::kBlocked;
            avail_cpus.Set(payload->cpu);
            break;
          }

          case MSG_TASK_PREEMPT: {
            const ghost_msg_payload_task_preempt* payload =
              static_cast<const ghost_msg_payload_task_preempt*>(msg.payload());
            ASSERT_TRUE(task->oncpu() || payload->from_switchto);
            ASSERT_FALSE(avail_cpus.IsSet(payload->cpu));
            task->run_state = FifoTaskState::kRunnable;
            run_queue.Enqueue(task);
            avail_cpus.Set(payload->cpu);
            break;
          }

          case MSG_TASK_YIELD: {
            const ghost_msg_payload_task_yield* payload =
              static_cast<const ghost_msg_payload_task_yield*>(msg.payload());
            ASSERT_TRUE(task->oncpu() || payload->from_switchto);
            ASSERT_FALSE(avail_cpus.IsSet(payload->cpu));
            task->run_state = FifoTaskState::kRunnable;
            run_queue.Enqueue(task);
            avail_cpus.Set(payload->cpu);
            break;
          }

          case MSG_TASK_DEPARTED: {
            const ghost_msg_payload_task_departed* payload =
             static_cast<const ghost_msg_payload_task_departed*>(msg.payload());
            if (task->oncpu()) {
              ASSERT_FALSE(avail_cpus.IsSet(payload->cpu));
              avail_cpus.Set(payload->cpu);
            } else if (task->queued()) {
              run_queue.Erase(task);
            } else {
              ASSERT_TRUE(task->blocked());
            }
            allocator->FreeTask(task);
            ASSERT_THAT(--num_tasks, Ge(0));
            break;
          }

          case MSG_TASK_DEAD: {
            ASSERT_TRUE(task->blocked());
            allocator->FreeTask(task);
            ASSERT_THAT(--num_tasks, Ge(0));
            break;
          }

          case MSG_TASK_AFFINITY_CHANGED:
            break;

          default:
            ASSERT_FALSE(true);
            break;
        }
        Consume(channel_, msg);
      }

      BarrierToken agent_barrier = status_word().barrier();
      const bool prio_boost = status_word().boosted_priority();

      if (Finished()) {
        break;
      }

      if (prio_boost) {
        RunRequest* req = enclave()->GetRunRequest(agent_cpu);
        req->LocalYield(agent_barrier, RTLA_ON_IDLE);
        continue;
      }

      for (const Cpu& cpu : avail_cpus) {
        ASSERT_THAT(cpu, Ne(agent_cpu));
        FifoTask* task = run_queue.Dequeue();
        if (!task) {
          break;  // no more runnable tasks.
        }

        if (task->status_word.on_cpu()) {
          // 'task' is still oncpu: put it back on the run_queue and try to
          // schedule in the next iteration.
          run_queue.Enqueue(task);
          continue;
        }

        RunRequest* req = enclave()->GetRunRequest(cpu);
        req->Open({
            .target = task->gtid,
            .target_barrier = task->seqnum,
            .agent_barrier = StatusWord::NullBarrierToken(),
            .commit_flags = COMMIT_AT_TXN_COMMIT,
        });

        if (req->Commit()) {
          task->run_state = FifoTaskState::kOnCpu;
          task->cpu = cpu.id();
          avail_cpus.Clear(cpu);
        } else {
          run_queue.Enqueue(task);
        }
      }
    }
  }

 private:
  Channel* channel_;  // nullptr for all satellite agents.
                      // non-nullptr for the global spinning agent.
};

template <class EnclaveType = LocalEnclave>
class FullDepartedRaceAgent final : public FullAgent<EnclaveType> {
 public:
  explicit FullDepartedRaceAgent(const AgentConfig& config)
      : FullAgent<EnclaveType>(config),
        sched_cpu_(config.cpus_.Front()),  // arbitrary.
        channel_(GHOST_MAX_QUEUE_ELEMS, sched_cpu_.numa_node()) {
    channel_.SetEnclaveDefault();
    // Start an instance of DepartedRaceAgent on each cpu.
    this->StartAgentTasks();

    // Unblock all agents and start scheduling.
    this->enclave_.Ready();
  }

  ~FullDepartedRaceAgent() final { this->TerminateAgentTasks(); }

  std::unique_ptr<Agent> MakeAgent(const Cpu& cpu) final {
    Channel* channel_ptr = &channel_;
    if (cpu != sched_cpu_) {
      channel_ptr = nullptr;  // sentinel value to indicate a satellite cpu.
    }
    return std::make_unique<DepartedRaceAgent>(&this->enclave_, cpu,
                                               channel_ptr);
  }

  void RpcHandler(int64_t req, const AgentRpcArgs& args,
                  AgentRpcResponse& response) final {
    response.response_code = -1;
  }

 private:
  const Cpu sched_cpu_;   // CPU running the main scheduling loop.
  LocalChannel channel_;  // Channel configured to wakeup `sched_cpu_`.
};

TEST(ApiTest, DepartedRace) {
  // This test requires at least 3 cpus:
  // - one cpu for the spinning agent.
  // - one cpu each for the two threads that are trying to induce the race.
  Topology* topology = MachineTopology();
  const CpuList agent_cpus = topology->all_cpus();
  constexpr int kMinCpus = 3;
  if (agent_cpus.Size() < kMinCpus) {
    GTEST_SKIP() << "must have at least " << kMinCpus << " cpus";
    return;
  }

  auto ap = AgentProcess<FullDepartedRaceAgent<>, AgentConfig>(
      AgentConfig(topology, agent_cpus));

  GhostThread t1(GhostThread::KernelScheduler::kCfs, [agent_cpus] {
    const uint32_t kNumAgentCpus = agent_cpus.Size();
    for (int i = 0; i < 1000; i++) {
      CpuList new_cpulist = agent_cpus;
      new_cpulist.Clear(agent_cpus[i % kNumAgentCpus]);

      // Try to induce a TASK_NEW and TASK_DEPARTED reordering.
      const sched_param param = {0};
      EXPECT_THAT(sched_setscheduler(/*pid=*/0, SCHED_OTHER, &param), Eq(0));

      // Try to induce a TASK_NEW and TASK_AFFINITY_CHANGED reordering.
      EXPECT_THAT(GhostHelper()->SchedSetAffinity(Gtid::Current(), new_cpulist),
                  Eq(0));
    }
  });

  const pid_t t1_tid = t1.tid();
  Notification done;
  GhostThread t2(GhostThread::KernelScheduler::kGhost, [t1_tid, &done] {
    while (!done.HasBeenNotified()) {
      if (GhostHelper()->SchedTaskEnterGhost(t1_tid, /*dir_fd=*/-1) != 0) {
        // EPERM: t1 was already in ghost.
        // ESRCH: t1 had already exited.
        EXPECT_THAT(errno, AnyOf(ESRCH, EPERM));
      }
    }
  });

  t1.Join();        // wait for 't1' to exit.
  done.Notify();    // 't2' is done.
  t2.Join();        // wait for 't2' to exit.

  // When AgentProcess goes out of scope its destructor will trigger
  // the FullDepartedRaceAgent destructor that in turn will Terminate()
  // the agents.

  // Since we were a ghOSt client, we were using an enclave.  Now that the test
  // is over, we need to reset so we can get a fresh enclave later.  Note that
  // we used AgentProcess, so the only user of the gbl_enclave_fd_ is us, the
  // client.
  GhostHelper()->CloseGlobalEnclaveFds();
}

TEST(ApiTest, GhostCloneGhost) {
  // Arbitrary but safe because there must be at least one CPU.
  constexpr int kCpuNum = 0;
  Topology* topology = MachineTopology();

  auto ap = AgentProcess<FullFifoAgent<LocalEnclave>, AgentConfig>(
      AgentConfig(topology, topology->ToCpuList(std::vector<int>{kCpuNum})));

  // Verify that a ghost thread implicitly clones itself in the ghost
  // scheduling class.
  GhostThread t1(GhostThread::KernelScheduler::kGhost, [] {
    EXPECT_THAT(sched_getscheduler(/*pid=*/0), Eq(SCHED_GHOST));
    // A bare std::thread is used here intentionally so nothing is going to
    // move t2 into ghost artificially (compare to GhostThread above).
    std::thread t2([] {
      EXPECT_THAT(sched_getscheduler(/*pid=*/0), Eq(SCHED_GHOST));
    });
    t2.join();
  });
  t1.Join();

  // Even though the threads have joined it does not mean they are dead.
  // pthread_join() can return before the dying task has made its way to
  // TASK_DEAD (CLONE_CHILD_CLEARTID sync via do_exit->exit_mm->mm_release).
  int num_tasks;
  do {
    num_tasks = ap.Rpc(FifoScheduler::kCountAllTasks);
    EXPECT_THAT(num_tasks, Ge(0));
  } while (num_tasks > 0);

  GhostHelper()->CloseGlobalEnclaveFds();
}

// Enter into ghost using a gtid.
TEST(ApiTest, SchedEnterGhostGtid) {
  Topology* topology = MachineTopology();

  auto ap = AgentProcess<FullFifoAgent<LocalEnclave>, AgentConfig>(
      AgentConfig(topology, topology->ToCpuList(std::vector<int>{0})));

  GhostThread t(GhostThread::KernelScheduler::kCfs, [] {
    EXPECT_THAT(sched_getscheduler(/*pid=*/0), Eq(0));
    EXPECT_THAT(
        GhostHelper()->SchedTaskEnterGhost(Gtid::Current(), /*dir_fd=*/-1),
        Eq(0));
    EXPECT_THAT(sched_getscheduler(/*pid=*/0), Eq(SCHED_GHOST));
  });
  t.Join();

  // Wait for all the tasks to die. See the comment in GhostCloneGhost for more
  // information.
  int num_tasks;
  do {
    num_tasks = ap.Rpc(FifoScheduler::kCountAllTasks);
    EXPECT_THAT(num_tasks, Ge(0));
  } while (num_tasks > 0);

  GhostHelper()->CloseGlobalEnclaveFds();
}

TEST(ApiTest, EnteringGhostViaSetSchedulerReturnsBadFdError) {
  // Sets sched_priority to be less than -1 to simulate a regular task trying
  // to enter ghost via sched_setscheduler call, which should fail.
  const sched_param param = { .sched_priority = -2 };
  EXPECT_THAT(sched_setscheduler(/*pid=*/0, SCHED_GHOST, &param), Eq(-1));
  EXPECT_THAT(errno, Eq(EBADF));
}

// Simple container to store ghost message copies.
class GhostMessageStore {
 public:
  GhostMessageStore() = default;

  // Not copyable.
  GhostMessageStore(const GhostMessageStore&) = delete;
  GhostMessageStore& operator=(const GhostMessageStore&) = delete;

  ~GhostMessageStore() {
    for (uint8_t* ptr : ghost_msgs_) {
      delete[] ptr;
    }
  }

  Message Add(const Message& message) {
    uint8_t* data = new uint8_t[message.length()];
    memcpy(data, message.msg(), message.length());
    ghost_msgs_.push_back(data);
    return Message(reinterpret_cast<ghost_msg*>(data));
  }

  Message At(size_t index) const {
    CHECK(index < ghost_msgs_.size());
    return Message(reinterpret_cast<ghost_msg*>(ghost_msgs_[index]));
  }

  size_t Size() const { return ghost_msgs_.size(); }

 private:
  std::vector<uint8_t*> ghost_msgs_;
};

// Bare-bones agent implementation that can schedule exactly one task and
// allows an external callback to verify/inspect messages sent from the
// kernel.
class MessageAgent : public LocalAgent {
 public:
  MessageAgent(Enclave* enclave, Cpu cpu,
               absl::AnyInvocable<void(const Message&) const> on_message)
      : LocalAgent(enclave, cpu),
        channel_(GHOST_MAX_QUEUE_ELEMS, kNumaNode,
                 MachineTopology()->ToCpuList({cpu})),
        on_message_(std::move(on_message)) {
    channel_.SetEnclaveDefault();
  }

  ~MessageAgent() {}

 protected:
  void AgentThread() override {
    // Boilerplate to synchronize startup until all agents are ready
    // (mostly redundant since we only have a single agent in the test).
    SignalReady();
    WaitForEnclaveReady();

    std::unique_ptr<Task<>> task;
    bool runnable = false;
    while (true) {
      while (true) {
        Message msg = Peek(&channel_);
        if (msg.empty()) break;

        on_message_(msg);

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
            task =
                std::make_unique<Task<>>(Gtid(payload->gtid), payload->sw_info);
            task->seqnum = msg.seqnum();
            runnable = payload->runnable;
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
            // This includes task messages like TASK_PREEMPTED and cpu messages
            // like CPU_TICK that don't influence runnability.
            break;
        }
        Consume(&channel_, msg);
      }

      BarrierToken agent_barrier = status_word().barrier();
      const bool prio_boost = status_word().boosted_priority();

      if (Finished() && !task) break;

      RunRequest* req = enclave()->GetRunRequest(cpu());
      if (!task || !runnable || prio_boost) {
        req->LocalYield(agent_barrier, prio_boost ? RTLA_ON_IDLE : 0);
      } else if (task->status_word.on_cpu()) {
        // 'task' is still oncpu: just loop back and try to schedule it
        // in the next iteration.
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
  // The NUMA node that the channel is on.
  static constexpr int kNumaNode = 0;

  LocalChannel channel_;
  absl::AnyInvocable<void(const Message&) const> on_message_;
};

template <class EnclaveType>
class FullMessageAgent final : public FullAgent<EnclaveType> {
 public:
  explicit FullMessageAgent(const AgentConfig& config)
      : FullAgent<EnclaveType>(config) {
    this->StartAgentTasks();
    this->enclave_.Ready();
  }

  ~FullMessageAgent() final { this->TerminateAgentTasks(); }

  std::unique_ptr<Agent> MakeAgent(const Cpu& cpu) final {
    return std::make_unique<MessageAgent>(
        &this->enclave_, cpu, [this](const Message& message) {
          absl::MutexLock lock(&this->mu_);
          this->message_store_.Add(message);

          if (message.type() == MSG_TASK_DEAD) {
            task_dead_message_count++;
          }
        });
  }

  static constexpr int64_t kErrorUnknownReq = -1;
  static constexpr int64_t kErrorInvalidIndex = -2;
  static constexpr int64_t kErrorIndexOutOfRange = -3;

  static constexpr int64_t kCountAllMessages = 1;
  static constexpr int64_t kCountDeadTaskMessages = 2;
  static constexpr int64_t kMessageAt = 3;

  void RpcHandler(int64_t req, const AgentRpcArgs& args,
                  AgentRpcResponse& response) final {
    if (req == kCountAllMessages) {
      absl::MutexLock lock(&mu_);
      response.response_code = message_store_.Size();
      return;
    }

    if (req == kCountDeadTaskMessages) {
      absl::MutexLock lock(&mu_);
      response.response_code = task_dead_message_count;
      return;
    }

    if (req == kMessageAt) {
      if (args.arg0 < 0) {
        response.response_code = kErrorInvalidIndex;
        return;
      }

      Message message;
      size_t index = static_cast<size_t>(args.arg0);
      {
        absl::MutexLock lock(&mu_);
        if (index >= message_store_.Size()) {
          response.response_code = kErrorIndexOutOfRange;
          return;
        }
        message = message_store_.At(index);
      }

      ASSERT_EQ(response.buffer.Serialize(*message.msg(), message.length()),
                absl::OkStatus());
      response.response_code = 0;
      return;
    }

    response.response_code = kErrorUnknownReq;
  }

 private:
  absl::Mutex mu_;
  int64_t task_dead_message_count ABSL_GUARDED_BY(mu_) = 0;
  GhostMessageStore message_store_ ABSL_GUARDED_BY(mu_);
};

TEST(ApiTest, GhostTaskPriorityViaGetSetPriority) {
  GhostHelper()->InitCore();

  Topology* topology = MachineTopology();

  auto ap = AgentProcess<FullMessageAgent<LocalEnclave>, AgentConfig>(
      AgentConfig(topology, topology->ToCpuList(std::vector<int>{0})));

  GhostThread t(GhostThread::KernelScheduler::kCfs, [] {
    EXPECT_THAT(setpriority(PRIO_PROCESS, /*pid=*/0, 1), Eq(0));

    GhostHelper()->SchedTaskEnterGhost(/*pid=*/0, -1);

    // At this point, the ghOSt agent should receive TASK_NEW message with
    // nice = 1.

    // Clear errno before calling getpriority because it can also return
    // a negative number on success.
    errno = 0;
    EXPECT_THAT(getpriority(PRIO_PROCESS, /*pid=*/0), Eq(1));
    EXPECT_THAT(errno, Eq(0));

    EXPECT_THAT(setpriority(PRIO_PROCESS, /*pid=*/0, 2), Eq(0));

    // At this point, the ghOSt agent should receive TASK_PRIORITY_CHANGED
    // message with nice = 2.

    errno = 0;
    EXPECT_THAT(getpriority(PRIO_PROCESS, /*pid=*/0), Eq(2));
    EXPECT_THAT(errno, Eq(0));
  });

  Gtid gtid = t.gtid();
  t.Join();

  // Wait until task dead message is received.
  int num_dead_task_messages = 0;
  do {
    num_dead_task_messages =
        ap.Rpc(FullMessageAgent<LocalEnclave>::kCountDeadTaskMessages);
    EXPECT_THAT(num_dead_task_messages, Ge(0));
  } while (num_dead_task_messages < 1);

  // Agent now received all the messages related to the thread under test. The
  // agent should have received the messages in the following order.
  // 1. TASK_NEW message with nice = 1.
  //    Then, zero or more messages other than TASK_NEW and
  //    TASK_PRIORITY_CHANGED messages.
  // 2. TASK_PRIORITY_CHANGED message with nice = 2.
  //    Then, zero or more messages other than TASK_NEW and
  //    TASK_PRIORITY_CHANGED messages.
  // 3. TASK_DEAD message.
  int64_t index = 0;
  int64_t response_code = 0;
  int32_t state = 0;
  do {
    auto args = std::make_unique<AgentRpcArgs>();
    args->arg0 = index;
    std::unique_ptr<AgentRpcResponse> resp =
        ap.RpcWithResponse(FullMessageAgent<LocalEnclave>::kMessageAt, *args);

    response_code = resp->response_code;
    if (!response_code) {
      Message message(reinterpret_cast<ghost_msg*>(resp->buffer.data.data()));
      if (message.type() == MSG_TASK_NEW) {
        EXPECT_THAT(state, Eq(0));
        EXPECT_THAT(message.gtid(), Eq(gtid));

        const ghost_msg_payload_task_new* payload_new =
            static_cast<const ghost_msg_payload_task_new*>(message.payload());
        EXPECT_THAT(payload_new->nice, Eq(1));
        state++;
      } else if (message.type() == MSG_TASK_PRIORITY_CHANGED) {
        EXPECT_THAT(state, Eq(1));
        EXPECT_THAT(message.gtid(), Eq(gtid));

        const ghost_msg_payload_task_priority_changed* payload_priority =
            static_cast<const ghost_msg_payload_task_priority_changed*>(
                message.payload());
        EXPECT_THAT(payload_priority->nice, Eq(2));
        state++;
      } else if (message.type() == MSG_TASK_DEAD) {
        EXPECT_THAT(state, Eq(2));
        EXPECT_THAT(message.gtid(), Eq(gtid));
        state++;
      }
    }
    index++;
  } while (!response_code);
  EXPECT_THAT(response_code,
              Eq(FullMessageAgent<LocalEnclave>::kErrorIndexOutOfRange));
  EXPECT_THAT(state, Eq(3));

  GhostHelper()->CloseGlobalEnclaveFds();
}

TEST(ApiTest, GhostTaskPriorityViaSchedGetSetAttr) {
  GhostHelper()->InitCore();

  Topology* topology = MachineTopology();

  auto ap = AgentProcess<FullMessageAgent<LocalEnclave>, AgentConfig>(
      AgentConfig(topology, topology->ToCpuList(std::vector<int>{0})));

  GhostThread t(GhostThread::KernelScheduler::kCfs, [] {
    // Need to define `sched_attr` and call `sched_{get|set}attr` via syscall
    // because they are not supported by glibc.
    struct {
      uint32_t size;

      uint32_t sched_policy;
      uint64_t sched_flags;

      // SCHED_NORMAL, SCHED_BATCH
      int32_t sched_nice;

      // SCHED_FIFO, SCHED_RR
      uint32_t sched_priority;

      // SCHED_DEADLINE
      uint64_t sched_runtime;
      uint64_t sched_deadline;
      uint64_t sched_period;
    } attr;

    EXPECT_THAT(syscall(SYS_sched_getattr, 0, &attr, sizeof(attr), 0), Eq(0));

    attr.sched_nice = 1;
    EXPECT_THAT(syscall(SYS_sched_setattr, 0, &attr, 0), Eq(0));

    GhostHelper()->SchedTaskEnterGhost(/*pid=*/0, -1);

    // At this point, the ghOSt agent should receive TASK_NEW message with
    // nice = 1.

    EXPECT_THAT(syscall(SYS_sched_getattr, 0, &attr, sizeof(attr), 0), Eq(0));
    EXPECT_THAT(attr.sched_policy, Eq(SCHED_GHOST));
    EXPECT_THAT(attr.sched_nice, Eq(1));

    attr.sched_nice = 2;
    EXPECT_THAT(syscall(SYS_sched_setattr, 0, &attr, 0), Eq(0));

    // At this point, the ghOSt agent should receive TASK_PRIORITY_CHANGED
    // message with nice = 2.

    EXPECT_THAT(syscall(SYS_sched_getattr, 0, &attr, sizeof(attr), 0), Eq(0));
    EXPECT_THAT(attr.sched_policy, Eq(SCHED_GHOST));
    EXPECT_THAT(attr.sched_nice, Eq(2));
  });

  Gtid gtid = t.gtid();
  t.Join();

  // Wait until task dead message is received.
  int num_dead_task_messages = 0;
  do {
    num_dead_task_messages =
        ap.Rpc(FullMessageAgent<LocalEnclave>::kCountDeadTaskMessages);
    EXPECT_THAT(num_dead_task_messages, Ge(0));
  } while (num_dead_task_messages < 1);

  // Agent now received all the messages related to the thread under test. The
  // agent should have received the messages in the following order.
  // 1. TASK_NEW message with nice = 1.
  //    Then, zero or more messages other than TASK_NEW and
  //    TASK_PRIORITY_CHANGED messages.
  // 2. TASK_PRIORITY_CHANGED message with nice = 2.
  //    Then, zero or more messages other than TASK_NEW and
  //    TASK_PRIORITY_CHANGED messages.
  // 3. TASK_DEAD message.
  int64_t index = 0;
  int64_t response_code = 0;
  int32_t state = 0;
  do {
    auto args = std::make_unique<AgentRpcArgs>();
    args->arg0 = index;
    std::unique_ptr<AgentRpcResponse> resp =
        ap.RpcWithResponse(FullMessageAgent<LocalEnclave>::kMessageAt, *args);

    response_code = resp->response_code;
    if (!response_code) {
      Message message(reinterpret_cast<ghost_msg*>(resp->buffer.data.data()));
      if (message.type() == MSG_TASK_NEW) {
        EXPECT_THAT(state, Eq(0));
        EXPECT_THAT(message.gtid(), Eq(gtid));

        const ghost_msg_payload_task_new* payload_new =
            static_cast<const ghost_msg_payload_task_new*>(message.payload());
        EXPECT_THAT(payload_new->nice, Eq(1));
        state++;
      } else if (message.type() == MSG_TASK_PRIORITY_CHANGED) {
        EXPECT_THAT(state, Eq(1));
        EXPECT_THAT(message.gtid(), Eq(gtid));

        const ghost_msg_payload_task_priority_changed* payload_priority =
            static_cast<const ghost_msg_payload_task_priority_changed*>(
                message.payload());
        EXPECT_THAT(payload_priority->nice, Eq(2));
        state++;
      } else if (message.type() == MSG_TASK_DEAD) {
        EXPECT_THAT(state, Eq(2));
        EXPECT_THAT(message.gtid(), Eq(gtid));
        state++;
      }
    }
    index++;
  } while (!response_code);
  EXPECT_THAT(response_code,
              Eq(FullMessageAgent<LocalEnclave>::kErrorIndexOutOfRange));
  EXPECT_THAT(state, Eq(3));

  GhostHelper()->CloseGlobalEnclaveFds();
}

}  // namespace
}  // namespace ghost
