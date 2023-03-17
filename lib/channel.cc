// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "channel.h"

#include <stdio.h>
#include <sys/mman.h>

#include <cstdint>
#include <iostream>

#include "lib/ghost.h"

namespace ghost {

// static
ghost_msg Message::kEmpty = {
    .type = 0,
    .length = 0,
    .seqnum = 0,
};

LocalChannel::LocalChannel(int elems, int node, CpuList cpulist)
    : Channel(), elems_(elems), node_(node) {
  fd_ = GhostHelper()->CreateQueue(elems_, node_, 0, map_size_);
  CHECK_GT(fd_, 0);

  header_ = static_cast<ghost_queue_header*>(
      mmap(nullptr, map_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
  CHECK_NE(header_, MAP_FAILED);
  elems_ = header_->nelems;

  if (!cpulist.Empty()) {
    CHECK_EQ(GhostHelper()->ConfigQueueWakeup(fd_, cpulist, /*flags=*/0), 0);
  }
};

LocalChannel::~LocalChannel() {
  munmap(header_, map_size_);
  close(fd_);
}

bool LocalChannel::AssociateTask(Gtid gtid, int barrier, int* status) const {
  int ret =
      GhostHelper()->AssociateQueue(fd_, GHOST_TASK, gtid.id(), barrier, 0);
  if (status) {
    *status = ret;
  }
  return ret >= 0;
}

void LocalChannel::Consume(const Message& msg) {
  ghost_ring* r = reinterpret_cast<ghost_ring*>(
      reinterpret_cast<char*>(header_) + header_->start);
  const int slot_size = sizeof(ghost_msg);

  uint32_t tail = r->tail.load(std::memory_order_acquire);

  // static_cast<uint32_t> to ensure that msg.length() is properly rounded up.
  // Otherwise if 'msg.length() + slot_size >= 65536' we'll end up only using
  // least significant 16-bits.
  tail += roundup2(static_cast<uint32_t>(msg.length()), slot_size) / slot_size;
  r->tail.store(tail, std::memory_order_release);
}

Message LocalChannel::Peek() const {
  ghost_ring* r = reinterpret_cast<ghost_ring*>(
      reinterpret_cast<char*>(header_) + header_->start);
  uint32_t tidx;

  const uint32_t nelems = header_->nelems;

  const uint32_t tail = r->tail.load(std::memory_order_acquire);
  const uint32_t head = r->head.load(std::memory_order_acquire);

  if (tail == head) return Message();  // Empty message.

  const uint32_t overflow = r->overflow.load(std::memory_order_acquire);
  CHECK_EQ(overflow, 0);

  tidx = tail & (nelems - 1);
  return Message(&r->msgs[tidx]);
}

bool LocalChannel::SetEnclaveDefault() const {
  return GhostHelper()->SetDefaultQueue(fd_) == 0;
}

absl::string_view Message::describe_type() const {
  switch (type()) {
    case MSG_NOP:
      return "MSG_NOP";
    case MSG_TASK_DEAD:
      return "MSG_TASK_DEAD";
    case MSG_TASK_BLOCKED:
      return "MSG_TASK_BLOCKED";
    case MSG_TASK_WAKEUP:
      return "MSG_TASK_WAKEUP";
    case MSG_TASK_NEW:
      return "MSG_TASK_NEW";
    case MSG_TASK_PREEMPT:
      return "MSG_TASK_PREEMPT";
    case MSG_TASK_YIELD:
      return "MSG_TASK_YIELD";
    case MSG_TASK_DEPARTED:
      return "MSG_TASK_DEPARTED";
    case MSG_TASK_SWITCHTO:
      return "MSG_TASK_SWITCHTO";
    case MSG_TASK_AFFINITY_CHANGED:
      return "MSG_TASK_AFFINITY_CHANGED";
    case MSG_TASK_ON_CPU:
      return "MSG_TASK_ON_CPU";
    case MSG_TASK_PRIORITY_CHANGED:
      return "MSG_TASK_PRIORITY_CHANGED";
    case MSG_TASK_LATCH_FAILURE:
      return "MSG_TASK_LATCH_FAILURE";
    case MSG_CPU_TICK:
      return "MSG_CPU_TICK";
    case MSG_CPU_TIMER_EXPIRED:
      return "MSG_CPU_TIMER_EXPIRED";
    case MSG_CPU_NOT_IDLE:
      return "MSG_CPU_NOT_IDLE";
    case MSG_CPU_AVAILABLE:
      return "MSG_CPU_AVAILABLE";
    case MSG_CPU_BUSY:
      return "MSG_CPU_BUSY";
    case MSG_CPU_AGENT_BLOCKED:
      return "MSG_CPU_AGENT_BLOCKED";
    case MSG_CPU_AGENT_WAKEUP:
      return "MSG_CPU_AGENT_WAKEUP";
    default:
      GHOST_ERROR("Unknown message %d", type());
  }
  return "UNKNOWN";
}

std::string Message::stringify() const {
  std::string result = absl::StrCat("M: ", describe_type(), " seq=", seqnum());

  if (is_cpu_msg()) {
    absl::StrAppend(&result, " cpu=", cpu());
    return result;
  }

  if (is_task_msg()) {
    Gtid msg_gtid = gtid();
    absl::StrAppend(&result, " ", msg_gtid.describe());
  }

  switch (type()) {
    case MSG_NOP:
      absl::StrAppend(&result, " l=", length());
      break;

    case MSG_TASK_NEW: {
      const ghost_msg_payload_task_new* new_payload =
          static_cast<const ghost_msg_payload_task_new*>(payload());
      absl::StrAppend(&result, " ",
                      new_payload->runnable ? "runnable" : "blocked");
      break;
    }

    case MSG_TASK_SWITCHTO: {
      const ghost_msg_payload_task_switchto* switchto =
          static_cast<const ghost_msg_payload_task_switchto*>(payload());
      absl::StrAppend(&result, " entering switchto on cpu ", switchto->cpu);
      break;
    }

    case MSG_TASK_BLOCKED: {
      const ghost_msg_payload_task_blocked* blocked =
          static_cast<const ghost_msg_payload_task_blocked*>(payload());
      absl::StrAppend(&result, " on cpu ", blocked->cpu);
      if (blocked->from_switchto) absl::StrAppend(&result, " (from_switchto)");
      break;
    }

    case MSG_TASK_YIELD: {
      const ghost_msg_payload_task_yield* yield =
          static_cast<const ghost_msg_payload_task_yield*>(payload());
      absl::StrAppend(&result, " on cpu ", yield->cpu);
      if (yield->from_switchto) absl::StrAppend(&result, " (from_switchto)");
      break;
    }

    case MSG_TASK_PREEMPT: {
      const ghost_msg_payload_task_preempt* preempt =
          static_cast<const ghost_msg_payload_task_preempt*>(payload());
      absl::StrAppend(&result, " on cpu ", preempt->cpu);
      if (preempt->was_latched) absl::StrAppend(&result, " (was_latched)");
      if (preempt->from_switchto) absl::StrAppend(&result, " (from_switchto)");
      break;
    }

    case MSG_TASK_DEPARTED: {
      const ghost_msg_payload_task_departed* departed =
          static_cast<const ghost_msg_payload_task_departed*>(payload());
      absl::StrAppend(&result, " on cpu ", departed->cpu);
      if (departed->from_switchto) absl::StrAppend(&result, " (from_switchto)");
      break;
    }

    case MSG_TASK_PRIORITY_CHANGED: {
      const ghost_msg_payload_task_priority_changed* priority =
          static_cast<const ghost_msg_payload_task_priority_changed*>(
              payload());
      absl::StrAppend(&result, " to nice ", priority->nice);
      break;
    }

    default:
      break;
  }

  return result;
}

}  // namespace ghost
