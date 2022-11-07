// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

// A skeleton implementation mapping our current scheduler objects onto a ghost
// shmem region.  No interaction with the mapped data is currently implemented.
#ifndef GHOST_SHARED_PRIO_TABLE_H
#define GHOST_SHARED_PRIO_TABLE_H

#include <atomic>

#include "shared/shmem.h"

namespace ghost {

// TODO: Move to ghost.h?
struct seqcount {
  std::atomic<uint32_t> seqnum;

  uint32_t write_begin();
  std::pair<bool, uint32_t> try_write_begin();
  void write_end(uint32_t begin);

  uint32_t read_begin() const;
  bool read_end(uint32_t begin) const;

  constexpr static int kLocked = 1;
};
typedef struct seqcount seqcount_t;

// TODO: Replace with internal types.
struct ghost_shmem_hdr {
  uint16_t version;
  uint16_t hdrlen;
  uint32_t maplen;
  uint32_t si_num; /* number of elements in 'sched_item[]' array */
  uint32_t si_off; /* offset of 'sched_item[0]' from start of hdr */
  uint32_t wc_num; /* number of elements in 'work_class[]' array */
  uint32_t wc_off; /* offset of 'work_class[0]' from start of hdr */
  uint32_t st_cap; /* capacity of the stream */
  uint32_t st_off; /* offset of stream from start of hdr */
} ABSL_CACHELINE_ALIGNED;

struct sched_item {
  uint32_t sid;   /* unique identifier for schedulable */
  uint32_t wcid;  /* unique identifier for work class */
  uint64_t gpid;  /* unique identifier for thread */
  uint32_t flags; /* schedulable attributes */
  seqcount_t seqcount;
  uint64_t deadline; /* deadline in ns (relative to the Unix epoch) */
} ABSL_CACHELINE_ALIGNED;

/* sched_item.flags */
// If we ever have more than one flag, we will need to change all accessors to
// be atomic.  Currently, both the agent and the application use non-atomic
// accesses.
#define SCHED_ITEM_RUNNABLE (1U << 0) /* worker thread is runnable */

/*
 * work_class is readonly-after-init so does not include a 'seqcount'
 * for synchronization.
 */
struct work_class {
  uint32_t id;       /* unique identifier for this work_class */
  uint32_t flags;    /* attributes of the work_class */
  uint32_t qos;      /* quality of service for this work class */
  uint64_t exectime; /* execution time in nsecs */
  uint64_t period;   /* period in nsecs for repeating work */
} ABSL_CACHELINE_ALIGNED;
#define WORK_CLASS_ONESHOT (1U << 0)
#define WORK_CLASS_REPEATING (1U << 1)

class PrioTable {
 public:
  // We use an enum to enforce that the client can only use primes as the stream
  // capacity to reduce hash collisions.
  enum class StreamCapacity : uint32_t {
    kStreamCapacity11 = 11,
    kStreamCapacity19 = 19,
    kStreamCapacity31 = 31,
    kStreamCapacity43 = 43,
    kStreamCapacity53 = 53,
    kStreamCapacity67 = 67,
    kStreamCapacity83 = 83,
    kStreamCapacity97 = 97,
    // Likely no need to go higher than 97. We tested a capacity of 83 with a
    // demanding microsecond-scale RocksDB app and that was enough to prevent
    // overflows.
  };

  PrioTable() {}
  PrioTable(uint32_t num_items, uint32_t num_classes,
            StreamCapacity stream_capacity);
  ~PrioTable();

  bool Attach(pid_t remote);

  struct sched_item* sched_item(int i) const;
  struct work_class* work_class(int i) const;

  inline struct ghost_shmem_hdr* hdr() const { return hdr_; }
  inline int NumSchedItems() { return hdr()->si_num; }
  inline int NumWorkClasses() { return hdr()->wc_num; }

  struct stream {
    std::atomic<int> scrape_all;
    std::atomic<int> entries[];
  };
  static constexpr int kStreamNoEntries = -1;
  static constexpr int kStreamOverflow = -2;
  void MarkUpdatedIndex(int idx, int num_retries);
  int NextUpdatedIndex();

  pid_t Owner() const { return shmem_ ? shmem_->Owner() : 0; }

  PrioTable(const PrioTable&) = delete;
  PrioTable(PrioTable&&) = delete;

 private:
  std::unique_ptr<GhostShmem> shmem_;
  struct ghost_shmem_hdr* hdr_ = nullptr;

  static constexpr int kStreamFreeEntry = std::numeric_limits<uint32_t>::max();
  struct stream* stream();
};

//------------------------------------------------------------------------------
// Implementation details below this line.
//------------------------------------------------------------------------------

inline uint32_t seqcount::write_begin() {
  uint32_t seq0 = seqnum.load(std::memory_order_relaxed);

  do {
    while (seq0 & kLocked) {  // Another writer exists, reload and spin/wait.
      Pause();
      seq0 = seqnum.load(std::memory_order_relaxed);
    }
  } while (!seqnum.compare_exchange_weak(seq0, seq0 + kLocked,
                                         std::memory_order_acquire));
  return seq0 + kLocked;
}

inline std::pair<bool, uint32_t> seqcount::try_write_begin() {
  uint32_t seq0 = seqnum.load(std::memory_order_relaxed);
  bool result = ((seq0 & kLocked) == 0) &&
                seqnum.compare_exchange_strong(seq0, seq0 + kLocked,
                                               std::memory_order_acquire);
  return {result, seq0 + kLocked};
}

inline void seqcount::write_end(uint32_t begin) {
  DCHECK_EQ(begin, seqnum.load(std::memory_order_relaxed));
  seqnum.store(begin + kLocked, std::memory_order_release);
}

inline uint32_t seqcount::read_begin() const {
  return seqnum.load(std::memory_order_acquire);
}

inline bool seqcount::read_end(uint32_t begin) const {
  // Compiler barrier with promotion of preceding reads.  This is not perfect
  // since technically the copy operation that exists within the critical read
  // section is not actually ordered by the C++ memory model around this, but
  // there's no real possible respite beyond depending on implementation
  // correctness currently.
  std::atomic_thread_fence(std::memory_order_acquire);
  if ((begin & kLocked) || (begin != seqnum.load(std::memory_order_relaxed)))
    return false;
  return true;
}

inline struct sched_item* PrioTable::sched_item(int i) const {
  DCHECK_GE(i, 0);
  CHECK_LT(i, hdr()->si_num);

  char* bytes = reinterpret_cast<char*>(hdr_);
  return reinterpret_cast<struct sched_item*>(bytes + hdr()->si_off +
                                              i * sizeof(struct sched_item));
}

inline struct work_class* PrioTable::work_class(int i) const {
  DCHECK_GE(i, 0);
  DCHECK_LT(i, hdr()->wc_num);

  char* bytes = reinterpret_cast<char*>(hdr_);
  return reinterpret_cast<struct work_class*>(bytes + hdr()->wc_off +
                                              i * sizeof(struct work_class));
}

}  // namespace ghost

#endif  // GHOST_SHARED_PRIO_TABLE_H
