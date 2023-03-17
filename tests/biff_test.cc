// Copyright 2022 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "schedulers/biff/biff_scheduler.h"

namespace ghost {
namespace {

class BiffTest : public testing::Test {
 protected:
  static void SetUpTestSuite() {
    Topology* t = MachineTopology();
    AgentConfig cfg(t, t->all_cpus());

    uap_ = new AgentProcess<FullBiffAgent<LocalEnclave>, AgentConfig>(cfg);
  }

  static void TearDownTestSuite() {
    delete uap_;
    uap_ = nullptr;
  }

  static AgentProcess<FullBiffAgent<LocalEnclave>, AgentConfig>* uap_;
};

AgentProcess<FullBiffAgent<LocalEnclave>, AgentConfig>* BiffTest::uap_;

TEST_F(BiffTest, Simple) {
  RemoteThreadTester(/*num_threads=*/1).Run(
    [] {
      absl::SleepFor(absl::Milliseconds(10));
      sched_yield();
      absl::SleepFor(absl::Milliseconds(10));
    }
  );
}

TEST_F(BiffTest, SimpleMany) {
  RemoteThreadTester().Run(
    [] {
      absl::SleepFor(absl::Milliseconds(10));
      sched_yield();
      absl::SleepFor(absl::Milliseconds(10));
    }
  );
}

TEST_F(BiffTest, BusyRunFor) {
  RemoteThreadTester(/*num_threads=*/100).Run(
    [] {
      SpinFor(absl::Milliseconds(10));
    }
  );
}

}  // namespace
}  // namespace ghost

int main(int argc, char **argv) {
  testing::InitGoogleMock(&argc, argv);

  return RUN_ALL_TESTS();
}
