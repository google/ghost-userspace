// Copyright 2022 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "schedulers/biff/biff_scheduler.h"

#include "absl/strings/str_format.h"
#include "third_party/bpf/topology.bpf.h"
#include "bpf/user/agent.h"

namespace ghost {

BiffScheduler::BiffScheduler(Enclave* enclave, CpuList cpulist,
                             const AgentConfig& config)
    : Scheduler(enclave, std::move(cpulist)),
      unused_channel_(GHOST_MAX_QUEUE_ELEMS, /*node=*/0) {

  bpf_obj_ = biff_bpf__open();
  CHECK_NE(bpf_obj_, nullptr);

  bpf_program__set_types(bpf_obj_->progs.biff_pnt,
                         BPF_PROG_TYPE_GHOST_SCHED, BPF_GHOST_SCHED_PNT);
  bpf_program__set_types(bpf_obj_->progs.biff_msg_send, BPF_PROG_TYPE_GHOST_MSG,
                         BPF_GHOST_MSG_SEND);
  bpf_program__set_types(bpf_obj_->progs.biff_select_rq,
                         BPF_PROG_TYPE_GHOST_SELECT_RQ, BPF_GHOST_SELECT_RQ);

  bpf_obj_->rodata->enable_bpf_printd = CapHas(CAP_PERFMON);
  SetBpfTopologyVars(bpf_obj_->rodata, MachineTopology());

  CHECK_EQ(biff_bpf__load(bpf_obj_), 0);

  CHECK_EQ(agent_bpf_register(bpf_obj_->progs.biff_pnt, BPF_GHOST_SCHED_PNT),
           0);
  CHECK_EQ(agent_bpf_register(bpf_obj_->progs.biff_msg_send,
                              BPF_GHOST_MSG_SEND), 0);
  CHECK_EQ(agent_bpf_register(bpf_obj_->progs.biff_select_rq,
                              BPF_GHOST_SELECT_RQ), 0);

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
  enclave()->SetDeliverTicks(true);
  enclave()->SetDeliverCpuAvailability(true);
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
