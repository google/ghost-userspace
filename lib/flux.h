// Copyright 2023 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd
//
// Userspace helpers for schedulers using the flux infrastructure


#ifndef GHOST_LIB_FLUX_H_
#define GHOST_LIB_FLUX_H_

#include "bpf/user/agent.h"

#define FluxSetProgTypes(bpf_obj) ({ \
  bpf_program__set_types(bpf_obj->progs.flux_pnt, \
                         BPF_PROG_TYPE_GHOST_SCHED, BPF_GHOST_SCHED_PNT); \
  bpf_program__set_types(bpf_obj->progs.flux_msg_send, \
                         BPF_PROG_TYPE_GHOST_MSG, BPF_GHOST_MSG_SEND); \
  bpf_program__set_types(bpf_obj->progs.flux_select_rq, \
                         BPF_PROG_TYPE_GHOST_SELECT_RQ, BPF_GHOST_SELECT_RQ); \
})

#define FluxRegisterProgs(bpf_obj) ({ \
  CHECK_EQ(agent_bpf_register(bpf_obj->progs.flux_pnt, BPF_GHOST_SCHED_PNT), \
           0); \
  CHECK_EQ(agent_bpf_register(bpf_obj->progs.flux_msg_send, \
                              BPF_GHOST_MSG_SEND), 0); \
  CHECK_EQ(agent_bpf_register(bpf_obj->progs.flux_select_rq, \
                              BPF_GHOST_SELECT_RQ), 0); \
})

#define FluxSetGlobals(bpf_obj) ({ \
  bpf_obj->rodata->enable_bpf_printd = CapHas(CAP_PERFMON); \
  bpf_obj->rodata->ghost_gtid_seqnum_bits = ghost_tid_seqnum_bits(); \
})

#define FluxCheckMaps(bpf_obj) ({ \
  CHECK_EQ(bpf_map__value_size(bpf_obj->maps.cpu_data), \
           FLUX_MAX_CPUS * sizeof(struct flux_cpu)); \
  CHECK_EQ(bpf_map__value_size(bpf_obj->maps.thread_data), \
           FLUX_MAX_GTIDS * sizeof(struct flux_thread)); \
})

#endif // GHOST_LIB_FLUX_H_
