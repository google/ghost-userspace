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

#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "schedulers/edf/edf_scheduler.h"
#include "schedulers/shinjuku/shinjuku_scheduler.h"
#include "schedulers/sol/sol_scheduler.h"

ABSL_FLAG(std::string, o, "/dev/stdout", "output file");

namespace ghost {

enum class WorkClass { kWcIdle, kWcOneShot, kWcRepeatable, kWcNum };

void UpdateSchedItem(PrioTable* table, uint32_t sidx, uint32_t wcid,
                     uint32_t flags, const Gtid& gtid, absl::Duration d) {
  struct sched_item* si;

  si = table->sched_item(sidx);

  const uint32_t seq = si->seqcount.write_begin();
  si->sid = sidx;
  si->wcid = wcid;
  si->flags = flags;
  si->gpid = gtid.id();
  si->deadline = absl::ToUnixNanos(MonotonicNow() + d);
  si->seqcount.write_end(seq);
  table->MarkUpdatedIndex(sidx, /* num_retries = */ 3);
}

void SetupWorkClasses(PrioTable* table) {
  struct work_class* wc;

  wc = table->work_class(static_cast<int>(WorkClass::kWcIdle));
  wc->id = static_cast<int>(WorkClass::kWcIdle);
  wc->flags = 0;
  wc->exectime = 0;

  wc = table->work_class(static_cast<int>(WorkClass::kWcOneShot));
  wc->id = static_cast<int>(WorkClass::kWcOneShot);
  wc->flags = WORK_CLASS_ONESHOT;
  wc->exectime = absl::ToInt64Nanoseconds(absl::Milliseconds(10));

  wc = table->work_class(static_cast<int>(WorkClass::kWcRepeatable));
  wc->id = static_cast<int>(WorkClass::kWcRepeatable);
  wc->flags = WORK_CLASS_REPEATING;
  wc->exectime = absl::ToInt64Nanoseconds(absl::Milliseconds(10));
  wc->period = absl::ToInt64Nanoseconds(absl::Milliseconds(100));
}

// This will print the result to stdout.  Due to the layers of forking, that's
// simpler than trying to pass the result back.
static void RunThreads(FILE* outfile, int nr_task_cpus, int nr_threads,
                       int nr_loops) {
  if (!nr_loops) {
    fprintf(outfile, "%d,%f,%d\n", nr_task_cpus, 0.0, 0);
    return;
  }
  // It's simpler to fork a process to run each set of threads so we can have
  // a fresh Priotable.  Careful with the outfile.  Flush it before forking and
  // again before exiting the child.
  fflush(outfile);
  ForkedProcess fp([&]() {
    std::unique_ptr<PrioTable> table = absl::make_unique<PrioTable>(
        nr_threads, static_cast<int>(WorkClass::kWcNum),
        PrioTable::StreamCapacity::kStreamCapacity19);
    SetupWorkClasses(table.get());

    std::vector<std::unique_ptr<GhostThread>> threads;
    threads.reserve(nr_threads);

    for (int i = 0; i < nr_threads; ++i) {
      threads.emplace_back(
          new GhostThread(GhostThread::KernelScheduler::kGhost, [&] {
            for (int i = 0; i < nr_loops; ++i) {
              sched_yield();
            }
          }));
    }

    absl::Time start = absl::Now();

    for (int i = 0; i < nr_threads; ++i) {
      auto& t = threads[i];
      // Sol doesn't need this, but it's harmless to do here
      UpdateSchedItem(table.get(), i,
                      static_cast<int>(WorkClass::kWcRepeatable),
                      SCHED_ITEM_RUNNABLE, t->gtid(), absl::Milliseconds(1));
    }
    for (int i = 0; i < nr_threads; ++i) {
      auto& t = threads[i];
      t->Join();
    }

    absl::Time finish = absl::Now();

    int64_t total_ns = ToInt64Nanoseconds(finish - start);
    double tput = (1.0 * nr_threads * nr_loops / total_ns) * 1000000000;
    fprintf(outfile, "%d,%f,%d\n", nr_task_cpus, tput, nr_loops);
    fflush(outfile);

    return 0;
  });
  fp.WaitForChildExit();
}

static void RunEdf(FILE* outfile, GlobalConfig cfg, int nr_task_cpus,
                   int nr_threads, int nr_loops) {
  auto uap = new AgentProcess<GlobalEdfAgent<LocalEnclave>, GlobalConfig>(cfg);

  RunThreads(outfile, nr_task_cpus, nr_threads, nr_loops);

  delete uap;
}

static void RunShinjuku(FILE* outfile, ShinjukuConfig cfg, int nr_task_cpus,
                        int nr_threads, int nr_loops) {
  auto uap =
      new AgentProcess<FullShinjukuAgent<LocalEnclave>, ShinjukuConfig>(cfg);

  RunThreads(outfile, nr_task_cpus, nr_threads, nr_loops);

  delete uap;
}

static void RunSol(FILE* outfile, SolConfig cfg, int nr_task_cpus,
                   int nr_threads, int nr_loops) {
  auto uap = new AgentProcess<FullSolAgent<LocalEnclave>, SolConfig>(cfg);

  RunThreads(outfile, nr_task_cpus, nr_threads, nr_loops);

  delete uap;
}

}  // namespace ghost

static bool ShouldSkipPacked(int i, int global_cpu, bool skip_zero) {
  if (i == 0 && skip_zero) return true;
  if (i == global_cpu) return true;
  return false;
}

// Assigns CPUs to pack the SMT siblings.  We fill every *core*, one at a time,
// first the low-id hyperthread, then the high thread.  Fills socket 0 first.
//
// Caveat: this adds the global_cpu first to make sure we have it.
//
// nr_cpus includes cpu0.
std::vector<int> MakeCpuVectorPacked(int nr_cpus, int max_cpus, int global_cpu,
                                     bool skip_zero) {
  std::vector<int> v;
  int sofar;

  CHECK_GE(nr_cpus, 2);

  v.push_back(global_cpu);
  sofar = 1;

  for (int i = 0; i < max_cpus / 2; ++i) {
    int first_thread = i;
    if (!ShouldSkipPacked(first_thread, global_cpu, skip_zero)) {
      v.push_back(first_thread);
      if (++sofar == nr_cpus) return v;
    }

    int second_thread = i + max_cpus / 2;
    if (!ShouldSkipPacked(second_thread, global_cpu, skip_zero)) {
      v.push_back(second_thread);
      if (++sofar == nr_cpus) return v;
    }
  }

  return v;
}

// Assigns CPUs such that we do not pack SMT siblings until we would leave the
// socket.  We do one sibling from each *core* in the socket, and then go and do
// the siblings for those cores.  Then we move on to the second socket.
//
// Assuming we're 2-socket NUMA, then we want the first 1/4 cpus, then the third
// 1/4 of the cpus (the SMT siblings of the first), then the remainder.
//
// nr_cpus includes cpu0.
std::vector<int> MakeCpuVectorSparse(int nr_cpus, int max_cpus,
                                     bool skip_zero) {
  std::vector<int> v;
  int sofar = skip_zero ? 1 : 0;

  // First quarter
  for (int i = max_cpus * 0 / 4; i < max_cpus * 1 / 4; ++i) {
    if (i == 0 && skip_zero) continue;
    v.push_back(i);
    if (++sofar == nr_cpus) return v;
  }
  // Third quarter
  for (int i = max_cpus * 2 / 4; i < max_cpus * 3 / 4; ++i) {
    v.push_back(i);
    if (++sofar == nr_cpus) return v;
  }
  // Second quarter
  for (int i = max_cpus * 1 / 4; i < max_cpus * 2 / 4; ++i) {
    v.push_back(i);
    if (++sofar == nr_cpus) return v;
  }
  // Fourth quarter
  for (int i = max_cpus * 3 / 4; i < max_cpus * 4 / 4; ++i) {
    v.push_back(i);
    if (++sofar == nr_cpus) return v;
  }
  return v;
}

ABSL_FLAG(int32_t, global_cpu, 1, "Primary cpu to run the global agent");
ABSL_FLAG(int32_t, max_cpus, -1, "Max cpus, including agent and cpu0");
ABSL_FLAG(int32_t, total_loops, 5000000, "Number of loops total");
ABSL_FLAG(int32_t, threads_per_cpu, 5, "Number of threads per cpu (unpinned)");
ABSL_FLAG(bool, skip_cpu0, true, "Do not run agents or tasks on cpu0");
ABSL_FLAG(bool, pack_smt, false, "Pack SMT siblings when assigning cpus");

enum class Sched {
  kEdf,
  kShinjuku,
  kSol,
};
static Sched sched_type;
static const char usage[] =
    "edf|shinjuku|sol"
    ;

int main(int argc, char* argv[]) {
 absl::SetProgramUsageMessage(usage);
 std::vector<char*> pos_args = absl::ParseCommandLine(argc, argv);

  FILE* outfile = fopen(absl::GetFlag(FLAGS_o).c_str(), "w");
  CHECK_NE(outfile, nullptr);
  ghost::Topology* t = ghost::MachineTopology();

  int nr_threads_per_cpu = absl::GetFlag(FLAGS_threads_per_cpu);
  int global_cpu = absl::GetFlag(FLAGS_global_cpu);
  bool skip_cpu0 = absl::GetFlag(FLAGS_skip_cpu0);
  bool pack_smt = absl::GetFlag(FLAGS_pack_smt);
  int total_loops = absl::GetFlag(FLAGS_total_loops);
  int num_cpus = absl::GetFlag(FLAGS_max_cpus);
  if (num_cpus == -1) num_cpus = t->num_cpus();
  if (num_cpus - (skip_cpu0 ? 2 : 1) < 1) {
    fprintf(stderr, "num_cpus is %d, need at least 1.  (max %d, skip0 %s)\n",
            num_cpus, absl::GetFlag(FLAGS_max_cpus),
            skip_cpu0 ? "set" : "unset");
    exit(1);
  }

  if (pos_args.size() < 2) {
    fprintf(stderr, "Need a scheduler type\n");
    exit(1);
  }
  if (!strcmp(pos_args[1], "edf")) {
    sched_type = Sched::kEdf;
  } else if (!strcmp(pos_args[1], "shinjuku")) {
    sched_type = Sched::kShinjuku;
  } else if (!strcmp(pos_args[1], "sol")) {
    sched_type = Sched::kSol;
  } else {
    fprintf(stderr, "Unrecognized scheduler '%s'\n", pos_args[1]);
    exit(1);
  }

  fprintf(outfile, "testing sched %s\n", pos_args[1]);
  fprintf(outfile, "global agent on cpu %d\n", global_cpu);
  fprintf(outfile, "testing up to %d worker cpus\n",
          num_cpus - (skip_cpu0 ? 2 : 1));
  fprintf(outfile, "nr_loops total: %d\n", total_loops);
  fprintf(outfile, "nr_threads per cpu: %d\n", nr_threads_per_cpu);
  fprintf(outfile, "%sskipping cpu 0\n", skip_cpu0 ? "" : "not ");
  fprintf(outfile, "\n");
  fprintf(outfile, "nr_task_cpus,scheds_per_sec,loops_per_thread\n");

  // num_cpus includes cpu0, which we might be skipping.  We're going to run the
  // test for all possible numbers of cpus for which the test can run, i.e. from
  // min_cpus to num_cpus.  The minimum number of cpus depends on whether or not
  // we are skipping cpu0.  cpu0 counts for 1 cpu (conditionally).  We need two
  // cpus regardless of cpu0: one cpu for the agent, and another cpu to run the
  // ghost tasks.
  int min_cpus = skip_cpu0 ? 3 : 2;
  for (int i = min_cpus; i <= num_cpus; ++i) {
    int nr_task_cpus = i - (skip_cpu0 ? 2 : 1);
    int nr_threads = nr_threads_per_cpu * nr_task_cpus;
    int nr_loops = total_loops / nr_threads;
    ghost::CpuList cpus =
        pack_smt ? t->ToCpuList(
                       MakeCpuVectorPacked(i, num_cpus, global_cpu, skip_cpu0))
                 : t->ToCpuList(MakeCpuVectorSparse(i, num_cpus, skip_cpu0));

    switch (sched_type) {
      case Sched::kEdf: {
        ghost::GlobalConfig cfg(t, cpus, t->cpu(global_cpu));
        ghost::RunEdf(outfile, cfg, nr_task_cpus, nr_threads, nr_loops);
        break;
      }
      case Sched::kShinjuku: {
        ghost::ShinjukuConfig cfg(t, cpus, t->cpu(global_cpu));
        cfg.preemption_time_slice_ = absl::Microseconds(50),
        ghost::RunShinjuku(outfile, cfg, nr_task_cpus, nr_threads, nr_loops);
        break;
      }
      case Sched::kSol: {
        ghost::SolConfig cfg(t, cpus, t->cpu(global_cpu));
        ghost::RunSol(outfile, cfg, nr_task_cpus, nr_threads, nr_loops);
        break;
      }
    }
  }
  fclose(outfile);
  return 0;
}
