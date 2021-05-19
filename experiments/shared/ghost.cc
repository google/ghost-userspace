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

#include "experiments/shared/ghost.h"

namespace ghost_test {

void Ghost::GetWorkClass(uint32_t wcid, ghost::work_class& wc) const {
  CheckWorkClassInRange(wcid);

  wc = *table_.work_class(wcid);
}

void Ghost::SetWorkClass(uint32_t wcid, const ghost::work_class& wc) {
  CHECK_EQ(wcid, wc.id);
  CheckWorkClassInRange(wcid);

  *table_.work_class(wcid) = wc;
}

void Ghost::CopySchedItem(ghost::sched_item& dst,
                          const ghost::sched_item& src) const {
  dst.sid = src.sid;
  dst.wcid = src.wcid;
  dst.gpid = src.gpid;
  dst.flags = src.flags;
  dst.deadline = src.deadline;
}

void Ghost::GetSchedItem(uint32_t sid, ghost::sched_item& si) const {
  CheckSchedItemInRange(sid);

  CopySchedItem(si, *table_.sched_item(sid));
}

void Ghost::SetSchedItem(uint32_t sid, const ghost::sched_item& si) {
  CHECK_EQ(sid, si.sid);
  CheckSchedItemInRange(si.sid);
  CheckWorkClassInRange(si.wcid);

  ghost::sched_item* curr = table_.sched_item(sid);
  uint32_t begin = curr->seqcount.write_begin();
  CopySchedItem(*curr, si);
  curr->seqcount.write_end(begin);
  MarkUpdatedTableIndex(curr->sid);
}

Ghost::Ghost(uint32_t num_sched_items, uint32_t num_work_classes)
    : table_(num_sched_items, num_work_classes,
             ghost::PrioTable::StreamCapacity::kStreamCapacity83) {
  CHECK(num_sched_items == 0 || num_work_classes >= 1);
}

void Ghost::MarkRunnability(uint32_t sid, bool runnable) {
  CheckSchedItemInRange(sid);

  ghost::sched_item* si = table_.sched_item(sid);
  uint32_t begin = si->seqcount.write_begin();
  if (runnable) {
    si->flags |= SCHED_ITEM_RUNNABLE;
  } else {
    si->flags &= ~SCHED_ITEM_RUNNABLE;
  }
  si->seqcount.write_end(begin);
  MarkUpdatedTableIndex(si->sid);
}

void Ghost::MarkRunnable(uint32_t sid) {
  MarkRunnability(sid, /*runnable=*/true);
}

void Ghost::MarkIdle(uint32_t sid) { MarkRunnability(sid, /*runnable=*/false); }

void Ghost::WaitUntilRunnable(uint32_t sid) const {
  CheckSchedItemInRange(sid);

  ghost::sched_item* si = table_.sched_item(sid);
  std::atomic<uint32_t>* flags =
      reinterpret_cast<std::atomic<uint32_t>*>(&si->flags);
  while ((flags->load(std::memory_order_acquire) & SCHED_ITEM_RUNNABLE) == 0) {
    asm volatile("pause");
  }
}

}  // namespace ghost_test
