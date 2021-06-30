/*
 * Copyright 2021 Google LLC
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

// Implements the basic ghOSt IPC abstractions.
#ifndef GHOST_LIB_CHANNEL_H_
#define GHOST_LIB_CHANNEL_H_

#include <cstdint>
#include <string>
#include <vector>

#include "lib/ghost.h"
#include "lib/topology.h"

namespace ghost {

// Encapsulates a single message, read from a Channel.
class Message {
 public:
  Message() : msg_(&kEmpty) {}
  explicit Message(const struct ghost_msg* msg) : msg_(msg) {}

  uint16_t type() const { return msg_->type; }
  uint16_t length() const { return msg_->length; }
  uint32_t seqnum() const { return msg_->seqnum; }
  const void* payload() const {
    return reinterpret_cast<const void*>(msg_->payload);
  }
  bool empty() const { return msg_ == &kEmpty; }

  bool is_cpu_msg() const {
    return type() >= _MSG_CPU_FIRST && type() <= _MSG_CPU_LAST;
  }

  int cpu() const {
    DCHECK(is_cpu_msg());
    return static_cast<const int*>(payload())[0];
  }

  bool is_task_msg() const {
    return type() >= _MSG_TASK_FIRST && type() <= _MSG_TASK_LAST;
  }

  Gtid gtid() const {
    DCHECK(is_task_msg());
    return Gtid(static_cast<const int64_t*>(payload())[0]);
  }

  // TODO: The C/C++ output mixing for convenience is not worth cleaning up
  // until we have Agent-specific logging output (i.e. don't print directly from
  // agents, log to ring buffer and externally propagate).
  absl::string_view describe_type() const;
  std::string stringify() const;

 private:
  const struct ghost_msg* msg_;
  static struct ghost_msg kEmpty;
};

class Channel {
 public:
  // Construct a channel mapping holding up to 'elems' message objects.
  // The optional 'cpu' parameter can be used to configure agent wakeup.
  Channel(int elems, int node,
          CpuList cpulist = MachineTopology()->EmptyCpuList());

  // Signals to ghOSt that the underlying channel mapping may be deallocated.
  ~Channel();

  // Read a single message from the underlying queue. If there are no messages
  // available, returns an empty message, e.g. result.empty() == true.
  Message Peek();
  void Consume(const Message& msg);

  // May be larger than constructor size.
  size_t max_elements() { return header_->nelems; }

  // Associate task (identified by 'gtid') with this channel.
  bool AssociateTask(Gtid gtid, int barrier, int* status = nullptr) const;

  // Set this channel's queue to be the enclave's default queue.  Caller must be
  // an agent of an enclave.  Returns true on success.
  bool SetEnclaveDefault() const;

  int GetFd() const { return fd_; }

  Channel(const Channel&) = delete;
  Channel(Channel&&) = delete;

 private:
  int elems_, node_, fd_;
  size_t map_size_;
  struct ghost_queue_header* header_;
};

inline Message Peek(Channel* f) { return f->Peek(); }
inline void Consume(Channel* f, const Message& msg) { f->Consume(msg); }

}  // namespace ghost

#endif  // GHOST_LIB_CHANNEL_H_
