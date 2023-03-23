// Copyright 2023 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <sched.h>

#include "schedulers/flux/flux_scheduler.h"

#include "absl/strings/str_format.h"
#include "bpf/user/agent.h"

namespace ghost {

// We only have one scheduler of each type, so id-to-type is pretty basic.
int IdToType(int id)
{
  switch (id) {
  case FLUX_SCHED_NONE:
    return FLUX_SCHED_TYPE_NONE;
  case FLUX_SCHED_ROCI:
    return FLUX_SCHED_TYPE_ROCI;
  case FLUX_SCHED_BIFF:
    return FLUX_SCHED_TYPE_BIFF;
  case FLUX_SCHED_IDLE:
    return FLUX_SCHED_TYPE_IDLE;
  default:
    return FLUX_SCHED_TYPE_NONE;
  }
}

FluxScheduler::FluxScheduler(Enclave* enclave, CpuList cpulist,
                             const AgentConfig& config)
    : Scheduler(enclave, std::move(cpulist)),
      unused_channel_(1, /*node=*/0) {

  bpf_obj_ = flux_bpf__open();
  CHECK_NE(bpf_obj_, nullptr);

  bpf_program__set_types(bpf_obj_->progs.flux_pnt,
                         BPF_PROG_TYPE_GHOST_SCHED, BPF_GHOST_SCHED_PNT);
  bpf_program__set_types(bpf_obj_->progs.flux_msg_send, BPF_PROG_TYPE_GHOST_MSG,
                         BPF_GHOST_MSG_SEND);
  bpf_program__set_types(bpf_obj_->progs.flux_select_rq,
                         BPF_PROG_TYPE_GHOST_SELECT_RQ, BPF_GHOST_SELECT_RQ);

  bpf_obj_->rodata->enable_bpf_printd = CapHas(CAP_PERFMON);

  CHECK_EQ(flux_bpf__load(bpf_obj_), 0);

  CHECK_EQ(agent_bpf_register(bpf_obj_->progs.flux_pnt, BPF_GHOST_SCHED_PNT),
           0);
  CHECK_EQ(agent_bpf_register(bpf_obj_->progs.flux_msg_send,
                              BPF_GHOST_MSG_SEND), 0);
  CHECK_EQ(agent_bpf_register(bpf_obj_->progs.flux_select_rq,
                              BPF_GHOST_SELECT_RQ), 0);

  cpu_data_ = static_cast<flux_cpu*>(
      bpf_map__mmap(bpf_obj_->maps.cpu_data));
  CHECK_NE(cpu_data_, MAP_FAILED);
  for (int i = 0; i < FLUX_MAX_CPUS; ++i) {
    cpu_data_[i].f.id = i;
  }

  struct flux_sched s;
  memset(&s, 0, sizeof(struct flux_sched));
  for (int i = 0; i < FLUX_NR_SCHEDS; i++) {
    s.f.id = i;
    s.f.type = IdToType(i);
    CHECK_EQ(bpf_map_update_elem(bpf_map__fd(bpf_obj_->maps.schedulers),
                                 &i, &s, BPF_ANY), 0);
  }

  thread_data_ = static_cast<flux_thread*>(
      bpf_map__mmap(bpf_obj_->maps.thread_data));
  CHECK_NE(thread_data_, MAP_FAILED);
}

FluxScheduler::~FluxScheduler() {
  bpf_map__munmap(bpf_obj_->maps.cpu_data, cpu_data_);
  bpf_map__munmap(bpf_obj_->maps.thread_data, thread_data_);
  flux_bpf__destroy(bpf_obj_);
}

void FluxScheduler::EnclaveReady() {
  enclave()->SetDeliverTicks(true);
  enclave()->SetDeliverCpuAvailability(true);
  // We learn about cpu availability via a message.  Some cpus may currently be
  // available and idle, but will not generate a message until CFS runs on them.
  // Poke each cpu to speed up the process.
  //
  // Running a CFS task on a cpu will eventually result in an
  // unavailable->available edge when that cpu runs out of CFS tasks, and that
  // edge will generate a MSG_CPU_AVAILABLE.
  std::thread thread([this] {
    for (const Cpu& cpu : *enclave()->cpus()) {
      // Ignore errors.  It's possible the agent is in a cgroup that doesn't
      // include all of the enclave cpus.  The cpus we skip will eventually run
      // a CFS task, just not right away.
      (void) GhostHelper()->SchedSetAffinity(Gtid::Current(),
                                MachineTopology()->ToCpuList({cpu}));
    }
  });
  thread.join();

  WRITE_ONCE(bpf_obj_->bss->user_initialized, true);
}

void FluxScheduler::DiscoverTasks() {
  enclave()->DiscoverTasks();
}

void FluxAgentTask::AgentThread() {
  gtid().assign_name("Agent:" + std::to_string(cpu().id()));

  SignalReady();
  WaitForEnclaveReady();

  while (!Finished()) {
    RunRequest* req = enclave()->GetRunRequest(cpu());
    req->LocalYield(status_word().barrier(), /*flags=*/0);
  }
}

}  //  namespace ghost
