// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

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
  explicit Message(const ghost_msg* msg) : msg_(msg) {}

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

  const ghost_msg* msg() const { return msg_; }

  bool operator==(const Message& other) const {
    // Note that this is just checking that both messages have the same `msg_`
    // pointer. This is *not* checking the `ghost_msg` structs themselves.
    return msg_ == other.msg_;
  }

  friend std::ostream& operator<<(std::ostream& os, const Message& msg) {
    return os << msg.stringify();
  }

 private:
  const ghost_msg* msg_;
  static ghost_msg kEmpty;
};

class Channel {
 public:
  virtual ~Channel() {}

  // Read a single message from the underlying queue. If there are no messages
  // available, returns an empty message, e.g. result.empty() == true.
  virtual Message Peek() const = 0;
  virtual void Consume(const Message& msg) = 0;

  // May be larger than constructor size.
  virtual size_t max_elements() const = 0;

  // Associate task (identified by 'gtid') with this channel.
  virtual bool AssociateTask(Gtid gtid, int barrier, int* status) const = 0;

  // Set this channel's queue to be the enclave's default queue.  Caller must be
  // an agent of an enclave.  Returns true on success.
  virtual bool SetEnclaveDefault() const = 0;

  virtual int GetFd() const = 0;
};

// Ensure that `Channel` remains an abstract base class. Some methods, such as
// `Scheduler::GetDefaultChannel()`, have a return type of `Channel&` rather
// than `Channel`. We want to ensure that `Channel` remains an abstract base
// class so that if code does something like `Channel channel =
// scheduler.GetDefaultChannel()` rather than `Channel& channel =
// scheduler.GetDefaultChannel()`, a compile error is generated as an instance
// of an abstract base class cannot be constructed.
//
// If `Channel` were no longer an abstract base class, then `Channel channel =
// scheduler.GetDefaultChannel()` would compile, which is bad because if a
// subclass reference were returned (such as `LocalChannel`), the subclass
// instance would be copied and then sliced to fit into `Channel channel`
// (assuming the subclass did not delete its copy constructor).
static_assert(std::is_abstract<Channel>::value);

// Encapsulates a shared memory IPC queue created by the kernel.
class LocalChannel : public Channel {
 public:
  // Construct a channel mapping holding up to `elems` message objects.
  // The optional `cpulist` parameter can be used to configure agent wakeup.
  LocalChannel(int elems, int node,
               CpuList cpulist = MachineTopology()->EmptyCpuList());

  // Signals to ghOSt that the underlying channel mapping may be deallocated.
  ~LocalChannel();

  Message Peek() const override;
  void Consume(const Message& msg) override;

  // May be larger than constructor size.
  size_t max_elements() const override { return header_->nelems; }

  // Associate task (identified by 'gtid') with this channel.
  bool AssociateTask(Gtid gtid, int barrier, int* status) const override;

  // Set this channel's queue to be the enclave's default queue.  Caller must be
  // an agent of an enclave.  Returns true on success.
  bool SetEnclaveDefault() const override;

  int GetFd() const override { return fd_; }

  LocalChannel(const LocalChannel&) = delete;
  LocalChannel(LocalChannel&&) = delete;

 private:
  int elems_, node_, fd_;
  size_t map_size_;
  ghost_queue_header* header_;
};

inline Message Peek(Channel* f) { return f->Peek(); }
inline void Consume(Channel* f, const Message& msg) { f->Consume(msg); }

}  // namespace ghost

#endif  // GHOST_LIB_CHANNEL_H_
