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

#ifndef GHOST_SCHEDULERS_EDF_ORCHESTRATOR_H
#define GHOST_SCHEDULERS_EDF_ORCHESTRATOR_H

#include <cstdint>
#include <vector>

#include "lib/ghost.h"
#include "shared/prio_table.h"

namespace ghost {

class SchedParams {
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

  inline bool SeqCopyParams(struct sched_item* src,
                            const struct work_class* wc) {
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
  uint32_t sid_;
  uint32_t wcid_;   // unique identifier for work class
  uint64_t gpid_;   // unique identifier for thread
  uint32_t flags_;  // schedulable attributes
  uint32_t seqcount_;
  uint64_t deadline_;  // deadline in ns (relative to the Unix epoch)

  uint32_t qos_;  // work class QoS class
};

class Orchestrator {
 public:
  Orchestrator() : table_() {}

  bool Init(pid_t remote);

  typedef std::function<void(Orchestrator& orch, const SchedParams* sp,
                             Gtid oldGtid)>
      SchedCallbackFunc;

  uint32_t NumWorkClasses() { return num_work_classes_; }

  void DumpSchedParams() const;
  void GetSchedParams(Gtid gtid, const SchedCallbackFunc& callback);
  void RefreshSchedParams(const SchedCallbackFunc& SchedCallback);

  inline bool Repeating(const SchedParams* sp) {
    const struct work_class* wc = table_.work_class(sp->GetWorkClass());
    return wc->flags & WORK_CLASS_REPEATING;
  }

  void UpdateWorkClassStats(uint32_t wcid, absl::Duration elapsed_runtime,
                            absl::Time deadline);
  absl::Duration EstimateRuntime(uint32_t wcid) const;
  absl::Duration GetWorkClassPeriod(uint32_t wcid) const {
    const struct work_class* wc = table_.work_class(wcid);
    return absl::Nanoseconds(wc->period);
  }

  void MakeEngineRunnable(const SchedParams* sp);

  Orchestrator(const Orchestrator&) = delete;
  Orchestrator operator=(const Orchestrator&) = delete;
  Orchestrator& operator=(Orchestrator&&) = delete;
  Orchestrator(Orchestrator&&) = delete;

 private:
  void RefreshSchedParam(uint32_t sid, const SchedCallbackFunc& SchedCallback);
  void RefreshAllSchedParams(const SchedCallbackFunc& SchedCallback);

  struct WorkClassStats {
    absl::Duration runtimes;
    uint64_t samples;
    uint64_t overshots;
  };

  std::vector<WorkClassStats> wc_stats_;

  PrioTable table_;
  uint32_t num_sched_items_ = 0;
  uint32_t num_work_classes_ = 0;
  std::unique_ptr<SchedParams[]> cachedsids_ = nullptr;
};

}  // namespace ghost

#endif  // GHOST_SCHEDULERS_EDF_ORCHESTRATOR_H
