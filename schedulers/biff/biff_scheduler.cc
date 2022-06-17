// Copyright 2022 Google LLC
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

#include "schedulers/biff/biff_scheduler.h"

#include "absl/strings/str_format.h"
#include "bpf/user/agent.h"

namespace ghost {

BiffScheduler::BiffScheduler(Enclave* enclave, CpuList cpulist,
                             const AgentConfig& config)
    : Scheduler(enclave, std::move(cpulist)),
      unused_channel_(GHOST_MAX_QUEUE_ELEMS, /*node=*/0) {

  bpf_obj_ = biff_bpf__open();
  CHECK_NE(bpf_obj_, nullptr);

  bpf_map__resize(bpf_obj_->maps.cpu_data, libbpf_num_possible_cpus());

  bpf_program__set_types(bpf_obj_->progs.biff_pnt,
                         BPF_PROG_TYPE_GHOST_SCHED, BPF_GHOST_SCHED_PNT);
  bpf_program__set_types(bpf_obj_->progs.biff_msg_send, BPF_PROG_TYPE_GHOST_MSG,
                         BPF_GHOST_MSG_SEND);

  CHECK_EQ(biff_bpf__load(bpf_obj_), 0);

  CHECK_EQ(agent_bpf_register(bpf_obj_->progs.biff_pnt, BPF_GHOST_SCHED_PNT),
           0);
  CHECK_EQ(agent_bpf_register(bpf_obj_->progs.biff_msg_send,
                              BPF_GHOST_MSG_SEND), 0);

  bpf_cpu_data_ = static_cast<struct biff_bpf_cpu_data*>(
      bpf_map__mmap(bpf_obj_->maps.cpu_data));
  CHECK_NE(bpf_cpu_data_, MAP_FAILED);

  bpf_sw_data_ = static_cast<struct biff_bpf_sw_data*>(
      bpf_map__mmap(bpf_obj_->maps.sw_data));
  CHECK_NE(bpf_sw_data_, MAP_FAILED);
}

BiffScheduler::~BiffScheduler() {
  bpf_map__munmap(bpf_obj_->maps.cpu_data, bpf_cpu_data_);
  bpf_map__munmap(bpf_obj_->maps.sw_data, bpf_sw_data_);
  biff_bpf__destroy(bpf_obj_);
}

void BiffScheduler::EnclaveReady() {
  // Biff has no cpu locality, so the remote wakeup is never worth it.
  enclave()->SetWakeOnWakerCpu(true);

  WRITE_ONCE(bpf_obj_->bss->initialized, true);
}

void BiffScheduler::DiscoverTasks() {
  enclave()->DiscoverTasks();
}

void BiffAgentTask::AgentThread() {
  gtid().assign_name("Agent:" + std::to_string(cpu().id()));

  SignalReady();
  WaitForEnclaveReady();

  while (!Finished()) {
    RunRequest* req = enclave()->GetRunRequest(cpu());
    req->LocalYield(status_word().barrier(), /*flags=*/0);
  }
}

}  //  namespace ghost
