// Copyright 2023 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <sys/resource.h>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "schedulers/flux/flux_scheduler.h"

namespace ghost {
namespace {

class FluxTest : public testing::Test {
 protected:
  static void SetUpTestSuite() {
    Topology* t = MachineTopology();
    AgentConfig cfg(t, t->all_cpus());

    uap_ = new AgentProcess<FullFluxAgent<LocalEnclave>, AgentConfig>(cfg);
  }

  static void TearDownTestSuite() {
    delete uap_;
    uap_ = nullptr;
  }

  static AgentProcess<FullFluxAgent<LocalEnclave>, AgentConfig>* uap_;
};

AgentProcess<FullFluxAgent<LocalEnclave>, AgentConfig>* FluxTest::uap_;

TEST_F(FluxTest, Simple) {
  RemoteThreadTester(/*num_threads=*/1).Run(
    [] {
      absl::SleepFor(absl::Milliseconds(10));
      sched_yield();
      absl::SleepFor(absl::Milliseconds(10));
    }
  );
}

TEST_F(FluxTest, SimpleMany) {
  RemoteThreadTester().Run(
    [] {
      absl::SleepFor(absl::Milliseconds(10));
      sched_yield();
      absl::SleepFor(absl::Milliseconds(10));
    }
  );
}

TEST_F(FluxTest, BusyRunFor) {
  RemoteThreadTester(/*num_threads=*/100).Run(
    [] {
      SpinFor(absl::Milliseconds(10));
    }
  );
}

TEST_F(FluxTest, PrioChangeSelf) {
  RemoteThreadTester().Run(
    [] {
      EXPECT_EQ(setpriority(PRIO_PROCESS, 0, 5), 0);
      absl::SleepFor(absl::Milliseconds(10));
      EXPECT_EQ(setpriority(PRIO_PROCESS, 0, 10), 0);
      sched_yield();
      EXPECT_EQ(setpriority(PRIO_PROCESS, 0, 5), 0);
    }
  );
}

TEST_F(FluxTest, PrioChangeRemote) {
  RemoteThreadTester().Run(
    [] {  // ghost threads
      SpinFor(absl::Milliseconds(5));
      sched_yield();
      absl::SleepFor(absl::Milliseconds(5));
    },
    [](GhostThread* t) {  // remote, per-thread work
      EXPECT_EQ(setpriority(PRIO_PROCESS, t->tid(), 5), 0);
      EXPECT_EQ(setpriority(PRIO_PROCESS, t->tid(), 10), 0);
    }
  );
}

TEST_F(FluxTest, DepartedSelf) {
  RemoteThreadTester().Run(
    [] {  // ghost threads
      absl::SleepFor(absl::Milliseconds(10));
      const sched_param param{};
      EXPECT_EQ(sched_setscheduler(/*pid=*/0, SCHED_OTHER, &param), 0);
      EXPECT_EQ(sched_getscheduler(/*pid=*/0), SCHED_OTHER);
    },
    [](GhostThread* t) {  // remote, per-thread work
    }
  );
}

TEST_F(FluxTest, DepartedRemote) {
  RemoteThreadTester().Run(
    [] {  // ghost threads
      SpinFor(absl::Milliseconds(5));
      sched_yield();
      absl::SleepFor(absl::Milliseconds(5));
    },
    [](GhostThread* t) {  // remote, per-thread work
      const sched_param param{};
      EXPECT_EQ(sched_setscheduler(t->tid(), SCHED_OTHER, &param), 0);
    }
  );
}

// Originally, I thought this was trigging a bug.  Turns out it just takes a
// long time with 1000 threads (~30 seconds on CONFIG=dbg in virtme).
TEST_F(FluxTest, DepartedRemoteShortSleep) {
  RemoteThreadTester(/*num_threads=*/100).Run(
    [] {  // ghost threads
      absl::SleepFor(absl::Nanoseconds(1));
    },
    [](GhostThread* t) {  // remote, per-thread work
      const sched_param param{};
      EXPECT_EQ(sched_setscheduler(t->tid(), SCHED_OTHER, &param), 0);
    }
  );
}

}  // namespace
}  // namespace ghost

int main(int argc, char **argv) {
  testing::InitGoogleMock(&argc, argv);

  return RUN_ALL_TESTS();
}
