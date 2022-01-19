// Copyright 2021 Google LLC
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

#include "channel.h"

#include <stdio.h>
#include <sys/mman.h>

#include <cstdint>
#include <iostream>

#include "lib/ghost.h"

namespace ghost {

// static
struct ghost_msg Message::kEmpty = {
    .type = 0,
    .length = 0,
    .seqnum = 0,
};

Channel::Channel(int elems, int node, CpuList cpulist)
    : elems_(elems), node_(node) {
  fd_ = Ghost::CreateQueue(elems_, node_, 0, map_size_);
  CHECK_GT(fd_, 0);

  header_ = static_cast<struct ghost_queue_header*>(
      mmap(nullptr, map_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
  CHECK_NE(header_, MAP_FAILED);
  elems_ = header_->nelems;

  if (!cpulist.Empty()) {
    cpu_set_t cpuset = Topology::ToCpuSet(cpulist);
    CHECK_ZERO(Ghost::ConfigQueueWakeup(fd_, cpuset, /*flags=*/0));
  }
};

Channel::~Channel() {
  munmap(header_, map_size_);
  close(fd_);
}

bool Channel::AssociateTask(Gtid gtid, int barrier, int* status) const {
  return Ghost::AssociateQueue(
      fd_, GHOST_TASK, gtid.id(), barrier, 0, status) == 0;
}

void Channel::Consume(const Message& msg) {
  struct ghost_ring* r = reinterpret_cast<struct ghost_ring*>(
      reinterpret_cast<char*>(header_) + header_->start);
  const int slot_size = sizeof(struct ghost_msg);

  uint32_t tail = r->tail.load(std::memory_order_acquire);

  // static_cast<uint32_t> to ensure that msg.length() is properly rounded up.
  // Otherwise if 'msg.length() + slot_size >= 65536' we'll end up only using
  // least significant 16-bits.
  tail += roundup2(static_cast<uint32_t>(msg.length()), slot_size) / slot_size;
  r->tail.store(tail, std::memory_order_release);
}

Message Channel::Peek() {
  struct ghost_ring* r = reinterpret_cast<struct ghost_ring*>(
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

bool Channel::SetEnclaveDefault() const {
  return Ghost::SetDefaultQueue(fd_) == 0;
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
    case MSG_CPU_TICK:
      return "MSG_CPU_TICK";
    case MSG_CPU_NOT_IDLE:
      return "MSG_CPU_NOT_IDLE";
    case MSG_CPU_TIMER_EXPIRED:
      return "MSG_CPU_TIMER_EXPIRED";
    case MSG_TASK_DEPARTED:
      return "MSG_TASK_DEPARTED";
    case MSG_TASK_SWITCHTO:
      return "MSG_TASK_SWITCHTO";
    case MSG_TASK_AFFINITY_CHANGED:
      return "MSG_TASK_AFFINITY_CHANGED";
    case MSG_TASK_LATCHED:
      return "MSG_TASK_LATCHED";
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

  Gtid msg_gtid = gtid();
  absl::StrAppend(&result, " ", msg_gtid.describe());

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

    default:
      break;
  }

  return result;
}

}  // namespace ghost
