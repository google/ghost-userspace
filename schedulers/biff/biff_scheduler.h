/*
 * Copyright 2022 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef GHOST_SCHEDULERS_BIFF_BIFF_SCHEDULER_H_
#define GHOST_SCHEDULERS_BIFF_BIFF_SCHEDULER_H_

#include <cstdint>

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
    biff_sched_ = absl::make_unique<BiffScheduler>(
        &this->enclave_, *this->enclave_.cpus(), config);
    this->StartAgentTasks();
    this->enclave_.Ready();
  }

  ~FullBiffAgent() override {
    this->TerminateAgentTasks();
  }

  std::unique_ptr<Agent> MakeAgent(const Cpu& cpu) override {
    return absl::make_unique<BiffAgentTask>(&this->enclave_, cpu,
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
