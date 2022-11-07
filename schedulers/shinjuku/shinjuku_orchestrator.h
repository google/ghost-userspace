// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef GHOST_SCHEDULERS_SHINJUKU_SHINJUKU_ORCHESTRATOR_H
#define GHOST_SCHEDULERS_SHINJUKU_SHINJUKU_ORCHESTRATOR_H

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "absl/time/time.h"
#include "lib/ghost.h"
#include "shared/prio_table.h"

namespace ghost {

// Stores a copy of a sched item's options from the PrioTable.
// This class handles the synchronization required to copy the options out.
// To use, create an instance of this class and call 'SeqCopyParams' with the
// sched item's corresponding 'sched_item' and 'work_class'.
class ShinjukuSchedParams {
 public:
  inline void SetRunnable() { flags_ |= SCHED_ITEM_RUNNABLE; }
  inline bool HasWork() const { return flags_ & SCHED_ITEM_RUNNABLE; }
  inline uint32_t GetFlags() { return flags_; }
  inline uint32_t GetSeqCount() { return seqcount_; }
  inline uint32_t GetSID() { return sid_; }

  inline uint32_t GetWorkClass() const { return wcid_; }
  inline Gtid GetGtid() const { return Gtid(gpid_); }
  inline absl::Time GetDeadline() const {
    return absl::FromUnixNanos(deadline_);
  }
  inline uint32_t GetQoS() const { return qos_; }

  // Copy the sched item's options from its 'sched_item' and 'work_class'.
  // Handles the synchronization required to copy the options out.
  // Returns true if the SchedParams changed, necessitating running the
  // SchedParamsCallback.
  inline bool SeqCopyParams(const sched_item* src, const work_class* wc) {
    uint32_t begin;
    bool success;

    begin = src->seqcount.read_begin();
    // Elide copy if nothing changed. Should be the common case.
    if (begin == seqcount_) return false;

    // If we fail to read_end(), we could read intermediate state due to
    // concurrent writes.  We'll only save them into the SchedParams on success;
    // hence the stack variables.
    uint32_t sid_l = src->sid;
    uint32_t wcid_l = src->wcid;
    uint64_t gpid_l = src->gpid;
    uint32_t flags_l = src->flags;
    uint64_t deadline_l = src->deadline;

    success = src->seqcount.read_end(begin);

    qos_ = wc->qos;

    if (!success) {
      // If writer is in the middle of an update then make sure the agent
      // doesn't yank the CPU from underneath it.  This is in case the writer
      // *is* the task itself.  It may be modifying fields other than its
      // runnability.  See cl/322185592 for an example.
      if (!(flags_ & SCHED_ITEM_RUNNABLE)) {
        // When we set seqcount_ = begin, we elide future copies from the
        // sched_item into the SchedParams (see above).  It is safe to elide
        // future copies in this case: once flags_ is marked runnable, it will
        // not be cleared until we detect the seqcounter has changed and reread
        // the PrioTable.  Since it is runnable, we do not need to worry about
        // yanking the CPU from the sched_item writer.
        seqcount_ = begin;
        flags_ |= SCHED_ITEM_RUNNABLE;
        return true;
      }
      // Otherwise, we don't update seqcount_ to ensure that the next
      // SeqCopyParams() call picks up all the fields again.
      return false;
    }

    seqcount_ = begin;
    sid_ = sid_l;
    wcid_ = wcid_l;
    gpid_ = gpid_l;
    flags_ = flags_l;
    deadline_ = deadline_l;
    return true;
  }

 private:
  uint32_t sid_;       // sched item ID
  uint32_t wcid_;      // unique identifier for work class
  uint64_t gpid_;      // unique identifier for thread
  uint32_t flags_;     // schedulable attributes
  uint32_t seqcount_;  // last sequence counter seen
  uint64_t deadline_;  // deadline in ns (relative to the Unix epoch)
  uint32_t qos_;       // work class QoS class
};

// Manages communication with the scheduled application via the PrioTable.
// To use, construct the class and call 'Init' with the application's PID.
class ShinjukuOrchestrator {
 public:
  typedef std::function<void(ShinjukuOrchestrator& orch,
                             const ShinjukuSchedParams* sp, Gtid oldGtid)>
      SchedCallbackFunc;

  ShinjukuOrchestrator() : table_() {}
  ShinjukuOrchestrator(const ShinjukuOrchestrator&) = delete;
  ShinjukuOrchestrator operator=(const ShinjukuOrchestrator&) = delete;
  ShinjukuOrchestrator& operator=(ShinjukuOrchestrator&&) = delete;
  ShinjukuOrchestrator(ShinjukuOrchestrator&&) = delete;

  // Attaches to the shared PrioTable belonging to the scheduled process.
  bool Init(pid_t remote);

  uint32_t NumWorkClasses() { return num_work_classes_; }

  // Prints all cached ShinjukuSchedParams.
  void DumpSchedParams() const;

  // Refreshes the sched params for the sched item corresponding to 'gtid' and
  // passes the sched item to 'callback'.
  void GetSchedParams(Gtid gtid, const SchedCallbackFunc& callback);

  // Refreshes all updated sched items with their updated options from the
  // PrioTable and calls 'SchedCallback' with each updated sched item. Note that
  // this function uses the PrioTable stream to efficiently detect which sched
  // items have been updated, but if the stream has overflown, then the entire
  // PrioTable is scraped and all sched items are refreshed and passed to the
  // callback regardless of whether they have been updated or not.
  void RefreshSchedParams(const SchedCallbackFunc& SchedCallback);

  // Calls 'RefreshSchedParam' with 'SchedCallback' on all sched items in the
  // PrioTable.
  void RefreshAllSchedParams(const SchedCallbackFunc& SchedCallback);

  // Returns 'true' if the sched item is a repeatable.
  inline bool Repeating(const ShinjukuSchedParams* sp) {
    const work_class* wc = table_.work_class(sp->GetWorkClass());
    return wc->flags & WORK_CLASS_REPEATING;
  }

  // Returns the work class period for the work class.
  absl::Duration GetWorkClassPeriod(uint32_t wcid) const {
    const work_class* wc = table_.work_class(wcid);
    return absl::Nanoseconds(wc->period);
  }

  // Marks an engine runnable, both in the cached 'ShinjukuSchedParams'
  // corresponding to the engine along with in the live PrioTable sched item
  // corresponding to the engine.
  void MakeEngineRunnable(const ShinjukuSchedParams* sp);

 private:
  // Copies the options for the sched item corresponding to 'sid' out of the
  // PrioTable and then calls 'SchedCallback'.
  void RefreshSchedParam(uint32_t sid, const SchedCallbackFunc& SchedCallback);

  PrioTable table_;
  uint32_t num_sched_items_ = 0;
  uint32_t num_work_classes_ = 0;
  std::unique_ptr<ShinjukuSchedParams[]> cachedsids_;
};

}  // namespace ghost

#endif  // GHOST_SCHEDULERS_SHINJUKU_SHINJUKU_ORCHESTRATOR_H
