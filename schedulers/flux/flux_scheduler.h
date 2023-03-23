// Copyright 2023 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef GHOST_SCHEDULERS_FLUX_FLUX_SCHEDULER_H_
#define GHOST_SCHEDULERS_FLUX_FLUX_SCHEDULER_H_

#include <cstdint>
#include <memory>

#include "third_party/bpf/flux_bpf.h"
#include "lib/agent.h"
#include "lib/scheduler.h"
#include "schedulers/flux/flux_bpf.skel.h"

namespace ghost {

class FluxScheduler : public Scheduler {
 public:
  explicit FluxScheduler(Enclave* enclave, CpuList cpulist,
                         const AgentConfig& config);
  ~FluxScheduler() final;

  void EnclaveReady() final;
  void DiscoverTasks() final;
  Channel& GetDefaultChannel() final { return unused_channel_; };

 private:
  LocalChannel unused_channel_;
  flux_bpf* bpf_obj_;
  flux_cpu* cpu_data_;
  flux_thread* thread_data_;
};

class FluxAgentTask : public LocalAgent {
 public:
  FluxAgentTask(Enclave* enclave, Cpu cpu, FluxScheduler* flux_sched)
      : LocalAgent(enclave, cpu), flux_sched_(flux_sched) {}

  void AgentThread() override;
  Scheduler* AgentScheduler() const override { return flux_sched_; }

 private:
  FluxScheduler* flux_sched_;
};

template <class EnclaveType>
class FullFluxAgent : public FullAgent<EnclaveType> {
 public:
  explicit FullFluxAgent(AgentConfig config)
      : FullAgent<EnclaveType>(config) {
    flux_sched_ = std::make_unique<FluxScheduler>(
        &this->enclave_, *this->enclave_.cpus(), config);
    this->StartAgentTasks();
    this->enclave_.Ready();
  }

  ~FullFluxAgent() override {
    // Turn off the availability messages before fully tearing down.  Once the
    // BPF program is removed, we won't filter the messages anymore, and we'll
    // get a few MSG_CPU_AVAILABLE/BUSY sent to userspace.  That will overflow
    // our channel.  On older kernels, that'd trigger a WARN_ON_ONCE.
    this->enclave_.SetDeliverCpuAvailability(false);
    this->TerminateAgentTasks();
  }

  std::unique_ptr<Agent> MakeAgent(const Cpu& cpu) override {
    return std::make_unique<FluxAgentTask>(&this->enclave_, cpu,
                                           flux_sched_.get());
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
  std::unique_ptr<FluxScheduler> flux_sched_;
};

}  // namespace ghost

#endif  // GHOST_SCHEDULERS_FLUX_FLUX_SCHEDULER_H_
