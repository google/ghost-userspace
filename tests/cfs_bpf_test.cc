// Copyright 2022 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "schedulers/cfs_bpf/cfs_scheduler.h"

namespace ghost {
namespace {

class CfsTest : public testing::Test {
 protected:
  static void SetUpTestSuite() {
    Topology* t = MachineTopology();
    AgentConfig cfg(t, t->all_cpus());

    uap_ = new AgentProcess<FullCfsAgent<LocalEnclave>, AgentConfig>(cfg);
  }

  static void TearDownTestSuite() {
    delete uap_;
    uap_ = nullptr;
  }

  static AgentProcess<FullCfsAgent<LocalEnclave>, AgentConfig>* uap_;
};

AgentProcess<FullCfsAgent<LocalEnclave>, AgentConfig>* CfsTest::uap_;

TEST_F(CfsTest, Simple) {
    GhostThread t(GhostThread::KernelScheduler::kGhost, [] {
      absl::SleepFor(absl::Milliseconds(10));
      sched_yield();
    });

    t.Join();
}

TEST_F(CfsTest, SimpleMany) {
    constexpr int kNumThreads = 1000;
    std::vector<std::unique_ptr<GhostThread>> threads;
    threads.reserve(kNumThreads);

    for (int i = 0; i < kNumThreads; ++i) {
      threads.push_back(
          std::make_unique<GhostThread>
          (GhostThread::KernelScheduler::kGhost, [] {
            absl::SleepFor(absl::Milliseconds(10));
            sched_yield();
            absl::SleepFor(absl::Milliseconds(10));
          }));
    }

    for (std::unique_ptr<GhostThread>& t : threads) {
      t->Join();
    }

}

TEST_F(CfsTest, BusyRunFor) {

    constexpr int kNumThreads = 1000;
    const absl::Duration d = absl::Milliseconds(10);

    std::vector<std::unique_ptr<GhostThread>> threads;
    threads.reserve(kNumThreads);

    for (int i = 0; i < kNumThreads; ++i) {
      threads.push_back(
          std::make_unique<GhostThread>
          (GhostThread::KernelScheduler::kGhost, [&] {
            SpinFor(d);
          }));
    }

    for (std::unique_ptr<GhostThread>& t : threads) {
      t->Join();
    }


}

}  // namespace
}  // namespace ghost

int main(int argc, char **argv) {
  testing::InitGoogleMock(&argc, argv);

  return RUN_ALL_TESTS();
}
