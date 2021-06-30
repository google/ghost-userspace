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

#include "schedulers/edf/orchestrator.h"

namespace ghost {

void Orchestrator::RefreshSchedParam(uint32_t sid,
                                     const SchedCallbackFunc& SchedCallback) {
  struct sched_item* si = table_.sched_item(sid);
  const struct work_class* wc = table_.work_class(si->wcid);
  SchedParams* sp = &cachedsids_[sid];
  Gtid oldGtid = sp->GetGtid();

  if (!sp->SeqCopyParams(si, wc)) {
    // It is alright to just ignore sched items that cannot be copied. When the
    // writer is finished updating the sched item, the writer will enqueue the
    // sched item into the stream again. Thus, we will not miss this sched item.
    // Also note that there is no way for us to recover whatever the
    // intermediate update is that we missed since the intermediate update has
    // already been overwritten by the writer.
    return;
  }

  SchedCallback(*this, sp, oldGtid);
}

// This is the slowpath. The fastpath will only iterate over sched_items that
// have changed.
void Orchestrator::RefreshAllSchedParams(
    const SchedCallbackFunc& SchedCallback) {
  for (uint32_t sid = 0; sid < num_sched_items_; sid++) {
    RefreshSchedParam(sid, SchedCallback);
  }
}

void Orchestrator::RefreshSchedParams(const SchedCallbackFunc& SchedCallback) {
  int updatedIndex;

  // Limit the number of iterations that we do before exiting this function. If
  // we were to replace this for loop with a while true loop, a malicious or
  // malfunctioning application could repeatedly overflow the stream and cause
  // the agent to get stuck in an infinite loop. The for loop we have right now
  // iterates up to 'table_.hdr()->st_cap' times, which is enough times to
  // drain a full stream. Additionally, if there are multiple overflows, the
  // first overflow will be picked up here and subsequent overflows will be
  // handled in future calls to this function.
  for (uint32_t i = 0; i < table_.hdr()->st_cap; i++) {
    updatedIndex = table_.NextUpdatedIndex();
    if (updatedIndex >= 0 && updatedIndex < num_sched_items_) {
      RefreshSchedParam(updatedIndex, SchedCallback);
    } else if (updatedIndex == PrioTable::kStreamOverflow) {
      RefreshAllSchedParams(SchedCallback);
      break;
    } else if (updatedIndex == PrioTable::kStreamNoEntries) {
      break;
    } else {
      GHOST_ERROR(
          "Dequeued unknown value 0x%x from the stream, cap 0x%x, "
          "num_sched_items_ 0x%x",
          updatedIndex, table_.hdr()->st_cap, num_sched_items_);
    }
  }
}

void Orchestrator::GetSchedParams(Gtid gtid,
                                  const SchedCallbackFunc& callback) {
  for (uint32_t sid = 0; sid < num_sched_items_; sid++) {
    SchedParams* sp = &cachedsids_[sid];
    if (sp->GetGtid() == gtid) {
      callback(*this, sp, sp->GetGtid());
      break;
    }
  }
}

void Orchestrator::DumpSchedParams() const {
  fprintf(stderr, "SchedParams:\n");
  fprintf(stderr, "TASK     WCID    FLAGS   SEQCOUNT\n");
  for (uint32_t sid = 0; sid < num_sched_items_; sid++) {
    struct sched_item* si = table_.sched_item(sid);
    SchedParams* sp = &cachedsids_[sid];

    CHECK_EQ(sp->GetGtid().id(), si->gpid);
    CHECK_EQ(sp->GetSID(), si->sid);

    if (si->gpid == 0) continue;

    absl::FPrintF(
        stderr, "%s %8u/%-8u %#08x/%#08x %8u/%-8u\n", sp->GetGtid().describe(),
        si->wcid, sp->GetWorkClass(), si->flags, sp->GetFlags(),
        si->seqcount.seqnum.load(std::memory_order_relaxed), sp->GetSeqCount());
  }
}

void Orchestrator::UpdateWorkClassStats(uint32_t wcid,
                                        absl::Duration elapsed_runtime,
                                        absl::Time deadline) {
  WorkClassStats& stats = wc_stats_[wcid];

  stats.runtimes += elapsed_runtime;
  stats.samples++;
  if (MonotonicNow() > deadline) {
    stats.overshots++;
  }
}

absl::Duration Orchestrator::EstimateRuntime(uint32_t wcid) const {
  const WorkClassStats& stats = wc_stats_[wcid];

  CHECK_GE(stats.samples, 1);
  absl::Duration runtime = stats.runtimes / stats.samples;
  // Ensure the estimated runtime is at least 1 ns to avoid an infinite loop in
  // 'CalculateSchedDeadline'.
  return runtime > absl::ZeroDuration() ? runtime : absl::Nanoseconds(1);
}

void Orchestrator::MakeEngineRunnable(const SchedParams* sp) {
  uint32_t sid = sp - &cachedsids_[0];
  CHECK_LT(sid, num_sched_items_);
  struct sched_item* item = table_.sched_item(sid);

  CHECK(item->gpid == sp->GetGtid().id());
  CHECK(Repeating(sp));

  CHECK(!sp->HasWork());
  cachedsids_[sid].SetRunnable();
  item->flags |= SCHED_ITEM_RUNNABLE;
}

bool Orchestrator::Init(pid_t remote) {
  bool ret = table_.Attach(remote);
  if (ret) {
    num_sched_items_ = table_.NumSchedItems();
    num_work_classes_ = table_.NumWorkClasses();
    cachedsids_ = absl::make_unique<SchedParams[]>(num_sched_items_);

    for (uint32_t wcid = 0; wcid < num_work_classes_; wcid++) {
      const struct work_class* wc = table_.work_class(wcid);
      WorkClassStats stats = {absl::Nanoseconds(wc->exectime), 1, 0};
      wc_stats_.push_back(stats);
    }
  }
  return ret;
}

}  // namespace ghost
