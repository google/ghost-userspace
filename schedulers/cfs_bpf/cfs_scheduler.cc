// Copyright 2022 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "schedulers/cfs_bpf/cfs_scheduler.h"

#include "absl/strings/str_format.h"
#include "bpf/user/agent.h"

namespace ghost {

CfsScheduler::CfsScheduler(Enclave* enclave, CpuList cpulist,
                             const AgentConfig& config)
    : Scheduler(enclave, std::move(cpulist)),
      unused_channel_(1, /*node=*/0) {

  bpf_obj_ = cfs_bpf__open();
  CHECK_NE(bpf_obj_, nullptr);


  bpf_program__set_types(bpf_obj_->progs.cfs_pnt,
                         BPF_PROG_TYPE_GHOST_SCHED, BPF_GHOST_SCHED_PNT);
  bpf_program__set_types(bpf_obj_->progs.cfs_msg_send, BPF_PROG_TYPE_GHOST_MSG,
                         BPF_GHOST_MSG_SEND);

  CHECK_EQ(cfs_bpf__load(bpf_obj_), 0);

  CHECK_EQ(agent_bpf_register(bpf_obj_->progs.cfs_pnt, BPF_GHOST_SCHED_PNT),
           0);
  CHECK_EQ(agent_bpf_register(bpf_obj_->progs.cfs_msg_send,
                              BPF_GHOST_MSG_SEND), 0);

  bpf_cpu_data_ = static_cast<struct cfs_bpf_cpu_data*>(
      bpf_map__mmap(bpf_obj_->maps.cpu_data));
  CHECK_NE(bpf_cpu_data_, MAP_FAILED);

  bpf_thread_data_ = static_cast<struct cfs_bpf_thread*>(
      bpf_map__mmap(bpf_obj_->maps.thread_data));
  CHECK_NE(bpf_thread_data_, MAP_FAILED);

  enclave->SetDeliverCpuAvailability(false);
}

CfsScheduler::~CfsScheduler() {
  bpf_map__munmap(bpf_obj_->maps.cpu_data, bpf_cpu_data_);
  bpf_map__munmap(bpf_obj_->maps.thread_data, bpf_thread_data_);
  cfs_bpf__destroy(bpf_obj_);
}

void CfsScheduler::EnclaveReady() {
  enclave()->SetWakeOnWakerCpu(false);
  enclave()->SetDeliverTicks(true);
  WRITE_ONCE(bpf_obj_->bss->initialized, true);
}

void CfsScheduler::DiscoverTasks() {
  enclave()->DiscoverTasks();
}

void CfsAgentTask::AgentThread() {
  gtid().assign_name("Agent:" + std::to_string(cpu().id()));

  SignalReady();
  WaitForEnclaveReady();

  while (!Finished()) {
    RunRequest* req = enclave()->GetRunRequest(cpu());
    req->LocalYield(status_word().barrier(), /*flags=*/0);
  }
}

}
