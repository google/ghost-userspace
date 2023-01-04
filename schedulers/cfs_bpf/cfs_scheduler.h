// Copyright 2022 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef GHOST_SCHEDULERS_CFS_BPF_BIFF_SCHEDULER_H_
#define GHOST_SCHEDULERS_CFS_BPF_BIFF_SCHEDULER_H_

#include <cstdint>

#include "third_party/bpf/cfs_bpf.h"
#include "lib/agent.h"
#include "lib/scheduler.h"
#include "schedulers/cfs_bpf/cfs_bpf.skel.h"

namespace ghost {

class CfsScheduler : public Scheduler {
 public:
  explicit CfsScheduler(Enclave* enclave, CpuList cpulist,
                         const AgentConfig& config);
  ~CfsScheduler() final;

  void EnclaveReady() final;
  void DiscoverTasks() final;
  Channel& GetDefaultChannel() final { return unused_channel_; };

 private:
  LocalChannel unused_channel_;
  struct cfs_bpf* bpf_obj_;
  struct cfs_bpf_cpu_data* bpf_cpu_data_;
  struct cfs_bpf_thread* bpf_thread_data_;
};

class CfsAgentTask : public LocalAgent {
 public:
  CfsAgentTask(Enclave* enclave, Cpu cpu, CfsScheduler* cfs_sched)
      : LocalAgent(enclave, cpu), cfs_sched_(cfs_sched) {}

  void AgentThread() override;
  Scheduler* AgentScheduler() const override { return cfs_sched_; }

 private:
  CfsScheduler* cfs_sched_;
};

template <class EnclaveType>
class FullCfsAgent : public FullAgent<EnclaveType> {
 public:
  explicit FullCfsAgent(AgentConfig config)
      : FullAgent<EnclaveType>(config) {
    cfs_sched_ = absl::make_unique<CfsScheduler>(
        &this->enclave_, *this->enclave_.cpus(), config);
    this->StartAgentTasks();
    this->enclave_.Ready();
  }

  ~FullCfsAgent() override {
    this->TerminateAgentTasks();
  }

  std::unique_ptr<Agent> MakeAgent(const Cpu& cpu) override {
    return absl::make_unique<CfsAgentTask>(&this->enclave_, cpu,
                                            cfs_sched_.get());
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
  std::unique_ptr<CfsScheduler> cfs_sched_;
};

}  // namespace ghost

#endif  // GHOST_SCHEDULERS_CFS_BPF_CFS_SCHEDULER_H_
