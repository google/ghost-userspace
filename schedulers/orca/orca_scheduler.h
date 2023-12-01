// Copyright 2022 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef GHOST_SCHEDULERS_BIFF_BIFF_SCHEDULER_H_
#define GHOST_SCHEDULERS_BIFF_BIFF_SCHEDULER_H_

#include <cstdint>
#include <memory>

#include "third_party/bpf/biff_bpf.h"
#include "lib/agent.h"
#include "lib/scheduler.h"
#include "schedulers/biff/biff_bpf.skel.h"

namespace ghost {

class BiffScheduler : public Scheduler {
 public:
  explicit BiffScheduler(Enclave* enclave, CpuList cpulist,
                         const AgentConfig& config);
  ~BiffScheduler() final;

  void EnclaveReady() final;
  void DiscoverTasks() final;
  Channel& GetDefaultChannel() final { return unused_channel_; };

 private:
  LocalChannel unused_channel_;
  struct biff_bpf* bpf_obj_;
  struct biff_bpf_cpu_data* bpf_cpu_data_;
  struct biff_bpf_sw_data* bpf_sw_data_;
};

class BiffAgentTask : public LocalAgent {
 public:
  BiffAgentTask(Enclave* enclave, Cpu cpu, BiffScheduler* biff_sched)
      : LocalAgent(enclave, cpu), biff_sched_(biff_sched) {}

  void AgentThread() override;
  Scheduler* AgentScheduler() const override { return biff_sched_; }

 private:
  BiffScheduler* biff_sched_;
};

template <class EnclaveType>
class FullBiffAgent : public FullAgent<EnclaveType> {
 public:
  explicit FullBiffAgent(AgentConfig config)
      : FullAgent<EnclaveType>(config) {
    biff_sched_ = std::make_unique<BiffScheduler>(
        &this->enclave_, *this->enclave_.cpus(), config);
    this->StartAgentTasks();
    this->enclave_.Ready();
  }

  ~FullBiffAgent() override {
    this->enclave_.SetDeliverCpuAvailability(false);
    this->TerminateAgentTasks();
  }

  std::unique_ptr<Agent> MakeAgent(const Cpu& cpu) override {
    return std::make_unique<BiffAgentTask>(&this->enclave_, cpu,
                                           biff_sched_.get());
  }

  void RpcHandler(int64_t req, const AgentRpcArgs& args,
                  AgentRpcResponse& response) override {
    switch (req) {
      default:
        response.response_code = -1;
        return;
    }
  }

 private:
  std::unique_ptr<BiffScheduler> biff_sched_;
};

}  // namespace ghost

#endif  // GHOST_SCHEDULERS_BIFF_BIFF_SCHEDULER_H_
