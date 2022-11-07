// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef GHOST_EXPERIMENTS_SHARED_PRIO_TABLE_HELPER_H_
#define GHOST_EXPERIMENTS_SHARED_PRIO_TABLE_HELPER_H_

#include "shared/prio_table.h"

namespace ghost_test {

// Helper class for applications scheduled by ghOSt that use a PrioTable. This
// class manages the PrioTable and facilitates accesses to the PrioTable,
// including sched item accesses and work class accesses.
//
// Example:
// PrioTableHelper helper_;
// (Initialize with the number of sched items and the number of work classes.)
// ...
// ghost::sched_item si;
// (Fill in the sched item.)
// helper_.SetSchedItem(/*sid=*/2, si);
// ...
// ghost::work_class wc;
// (Fill in the work class.)
// helper_.SetWorkClass(/*wcid=*/1, wc);
// ...
// Main thread: helper_.MarkIdle(/*sid=*/3);
// Thread with SID 3: helper_.IsIdle(/*sid=*/3);
// (Returns true.)
// helper_.WaitUntilRunnable(/*sid=*/3);
// (Thread with SID 3 is descheduled by ghOSt.)
// Thread with SID 1: helper_.MarkRunnable(/*sid=*/3);
// (ghOSt eventually schedules the thread with SID 3.)
class PrioTableHelper {
 public:
  // Constructs the class. 'num_sched_items' is the number of sched items in the
  // PrioTable (i.e., the number of threads scheduled by ghOSt).
  // 'num_work_classes' is the number of work classes in the PrioTable. Sched
  // items are placed into work classes and then attributes are set on each work
  // class, which in turn are applied to the sched items in each work class. You
  // need at least one work class if you have at least one sched item.
  PrioTableHelper(uint32_t num_sched_items, uint32_t num_work_classes);

  // PrioTable methods. 'sid' refers to the identifier for a sched item ('sid' =
  // sched item identifier). 'wcid' refers to the identifier for a work class
  // ('wcid' = work class identifier).

  // Gets the work class for 'wcid'.
  void GetWorkClass(uint32_t wcid, ghost::work_class& wc) const;
  // Sets the work class for 'wcid' to 'wc'.
  void SetWorkClass(uint32_t wcid, const ghost::work_class& wc);
  // Gets the sched item for 'sid'. There is no synchronization via the sequence
  // counter (see comment below for 'SetSchedItem'). We do not need
  // synchronization since the agent only writes the 'flags' field of the sched
  // item. Since the agent only writes one field (and the application is the
  // only other writer to sched items), it is not possible for the application
  // to read a sched item in an inconsistent state due to agent writes. Even if
  // the application reads a sched item while the agent writes to 'flags', it
  // will either read the old value for 'flags' or the new value. But either
  // way, the sched item is not in an inconsistent state since the value of
  // 'flags' *when written by the agent* is not tied to the values of other
  // fields by invariants.
  void GetSchedItem(uint32_t sid, ghost::sched_item& si) const;
  // Sets the sched item for 'sid'. The update is protected by the sequence
  // counter and the sched item is marked as updated in the stream once the
  // update is complete. We need to protect the update with a sequence counter
  // as otherwise the agent could read a sched item in an inconsistent state.
  // For example, we could write one field of the sched item and then be in the
  // midst of writing another field when the agent reads the entire sched item.
  // Now the agent has read a sched item in an inconsistent state. The sequence
  // counter synchronization ensures that the agent only reads sched items in a
  // consistent state.
  void SetSchedItem(uint32_t sid, const ghost::sched_item& si);
  // Marks 'sid' as runnable.
  void MarkRunnable(uint32_t sid);
  // Marks 'sid' as idle.
  void MarkIdle(uint32_t sid);
  // Waits until 'sid' is runnable.
  void WaitUntilRunnable(uint32_t sid) const;

  // Returns true if 'sid' is idle and false otherwise.
  inline bool IsIdle(uint32_t sid) const {
    CheckSchedItemInRange(sid);

    ghost::sched_item* si = table_.sched_item(sid);
    std::atomic<uint32_t>* flags =
        reinterpret_cast<std::atomic<uint32_t>*>(&si->flags);
    return (flags->load(std::memory_order_acquire) & SCHED_ITEM_RUNNABLE) == 0;
  }

  // Returns true if 'sid' is a one-shot and false otherwise.
  inline bool IsOneShot(uint32_t sid) const {
    CheckSchedItemInRange(sid);

    ghost::sched_item* si = table_.sched_item(sid);
    ghost::work_class* wc = table_.work_class(si->wcid);
    return wc->flags & WORK_CLASS_ONESHOT;
  }

  // Marks 'sid' as updated in the table.
  inline void MarkUpdatedTableIndex(uint32_t sid) {
    CheckSchedItemInRange(sid);

    table_.MarkUpdatedIndex(sid, kNumUpdatedIndexRetries);
  }

  // 'ghost::sched_item' stores the deadline as a 'uint64_t', so we need to
  // convert the 'absl::Time' deadline to 'uint64_t'.
  static inline uint64_t ToRawDeadline(absl::Time deadline) {
    return absl::ToUnixNanos(deadline);
  }

 private:
  // Checks that 'sid' (i.e., the sched item identifier) is in range. If not,
  // this indicates a bug since the caller is trying to access a sched item that
  // does not exist.
  inline void CheckSchedItemInRange(uint32_t sid) const {
    CHECK_LT(sid, table_.hdr()->si_num);
  }

  // Checks that 'wcid' (i.e., the work class identifier) is in range. If not,
  // this indicates a bug since the caller is trying to access a work class that
  // does not exist.
  inline void CheckWorkClassInRange(uint32_t wcid) const {
    CHECK_LT(wcid, table_.hdr()->wc_num);
  }

  // Copies the 'src' sched item into the 'dst' sched item. Note that the
  // sequence counter ('seqcount') is not copied and no synchronization is
  // performed. Sequence counter synchronization must be performed by the
  // caller.
  void CopySchedItem(ghost::sched_item& dst,
                     const ghost::sched_item& src) const;

  // Marks 'sid' as runnable when 'runnable' == true and not runnable when
  // 'runnable' == false.
  void MarkRunnability(uint32_t sid, bool runnable);

  // Try to mark the index as updated up to 3 times. If we fail after 3 tries,
  // then just let the stream overflow as it is probably close to being full (or
  // is full already) and we shouldn't waste any more time updating the stream
  // for this index.
  static constexpr uint32_t kNumUpdatedIndexRetries = 3;

  ghost::PrioTable table_;
};

}  // namespace ghost_test

#endif  // GHOST_EXPERIMENTS_SHARED_PRIO_TABLE_HELPER_H_
