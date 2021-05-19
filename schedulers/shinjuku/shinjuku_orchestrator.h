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
  inline bool SeqCopyParams(const sched_item* src, const work_class* wc) {
    uint32_t begin;
    bool success;

    begin = src->seqcount.read_begin();
    // Elide copy if nothing changed. Should be the common case.
    if (begin == seqcount_) return false;

    // If writer is in the middle of an update then make sure the agent
    // doesn't yank the CPU from underneath it.
    if ((begin & seqcount::kLocked) && !(flags_ & SCHED_ITEM_RUNNABLE)) {
      seqcount_ = begin;
      flags_ |= SCHED_ITEM_RUNNABLE;
      return true;
    }

    sid_ = src->sid;
    wcid_ = src->wcid;
    gpid_ = src->gpid;
    flags_ = src->flags;
    deadline_ = src->deadline;
    success = src->seqcount.read_end(begin);

    qos_ = wc->qos;
    CHECK_EQ(wcid_, wc->id);

    // The Client may have concurrently clobbered various fields in the
    // row while we were copying them individually. On !success, ensure
    // that the next SeqCopyParams() call picks up all the fields again.
    if (success) {
      seqcount_ = begin;
      return true;
    }
    return false;
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

  // Calls 'RefreshSchedParam' with 'SchedCallback' on all sched items in the
  // PrioTable.
  void RefreshAllSchedParams(const SchedCallbackFunc& SchedCallback);

  PrioTable table_;
  uint32_t num_sched_items_ = 0;
  uint32_t num_work_classes_ = 0;
  std::unique_ptr<ShinjukuSchedParams[]> cachedsids_;
};

}  // namespace ghost

#endif  // GHOST_SCHEDULERS_SHINJUKU_SHINJUKU_ORCHESTRATOR_H
