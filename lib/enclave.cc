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

#include "lib/enclave.h"

#include <sys/mman.h>

#include <filesystem>
#include <regex>  // NOLINT: no RE2; ghost limits itself to absl

#include "absl/base/attributes.h"
#include "kernel/ghost_uapi.h"
#include "lib/agent.h"
#include "lib/scheduler.h"

namespace ghost {

Enclave::Enclave(Topology* topology, CpuList enclave_cpus)
    : topology_(topology), enclave_cpus_(std::move(enclave_cpus)) {}
Enclave::Enclave(Topology* topology, int enclave_fd)
    : topology_(topology),
      enclave_cpus_(CpuList(*topology)),
      dir_fd_(enclave_fd) {}

Enclave::~Enclave() {}

void Enclave::Ready() {
  auto ready = [this]() {
    mu_.AssertHeld();
    return agents_.size() == enclave_cpus_.Size();
  };

  mu_.LockWhen(absl::Condition(&ready));

  DerivedReady();

  if (!schedulers_.empty()) {
    // There can be only one default queue, so take the first scheduler's
    // default.
    // There are tests that have agents, but do not have a scheduler.  Those
    // must call SetEnclaveDefault() manually.
    CHECK(schedulers_.front()->GetDefaultChannel().SetEnclaveDefault());
  }

  for (auto scheduler : schedulers_) scheduler->EnclaveReady();

  // We could use agents_ here, but this allows extra checking.
  for (auto cpu : enclave_cpus_) {
    Agent* agent = GetAgent(cpu);
    CHECK_NE(agent, nullptr);  // Every Enclave CPU should have an agent.
    agent->EnclaveReady();
  }
  mu_.Unlock();

  AdvertiseReady();
}

void Enclave::AttachAgent(Cpu cpu, Agent* agent) {
  absl::MutexLock h(&mu_);
  agents_.push_back(agent);
}

void Enclave::DetachAgent(Agent* agent) {
  absl::MutexLock h(&mu_);
  agents_.remove(agent);
}

void Enclave::AttachScheduler(Scheduler* scheduler) {
  absl::MutexLock h(&mu_);
  schedulers_.push_back(scheduler);
}

void Enclave::DetachScheduler(Scheduler* scheduler) {
  absl::MutexLock h(&mu_);
  schedulers_.remove(scheduler);
}

bool Enclave::CommitRunRequests(const CpuList& cpu_list) {
  SubmitRunRequests(cpu_list);

  bool success = true;
  for (const Cpu& cpu : cpu_list) {
    RunRequest* req = GetRunRequest(cpu);
    if (!CompleteRunRequest(req)) success = false;
  }
  return success;
}

void Enclave::SubmitRunRequests(const CpuList& cpu_list) {
  for (const Cpu& cpu : cpu_list) {
    RunRequest* req = GetRunRequest(cpu);
    SubmitRunRequest(req);
  }
}

// Reads bytes from fd and returns a string.  Suitable for sysfs.
std::string ReadString(int fd) {
  char buf[4096];
  char* p;
  char* end = buf + sizeof(buf);
  ssize_t amt = 1;
  for (p = buf; amt > 0 && p < end; p += amt) {
    amt = read(fd, p, end - p);
    CHECK_GE(amt, 0);
  }
  CHECK_EQ(amt, 0);
  return std::string(buf, p - buf);
}

// Makes the next available enclave from ghostfs.  Returns the FD for the ctl
// file in whichever enclave directory we get.
// static
int LocalEnclave::MakeNextEnclave() {
  int top_ctl =
      open(absl::StrCat(Ghost::kGhostfsMount, "/ctl").c_str(), O_WRONLY);
  if (top_ctl < 0) {
    return -1;
  }
  int id = 1;

  while (true) {
    std::string cmd = absl::StrCat("create ", id);
    if (write(top_ctl, cmd.c_str(), cmd.length()) == cmd.length()) {
      break;
    }
    if (errno == EEXIST) {
      id++;
      continue;
    }
    close(top_ctl);
    return -1;
  }
  close(top_ctl);

  return open(
      absl::StrCat(Ghost::kGhostfsMount, "/enclave_", id, "/ctl").c_str(),
      O_RDWR);
}

// Open the directory containing our ctl_fd, sufficient for openat calls
// (O_PATH).  For example, if you have the ctl for enclave 314, open
// "/sys/fs/ghost/enclave_314".  The ctl file tells you the enclave ID.
// static
int LocalEnclave::GetEnclaveDirectory(int ctl_fd) {
  CHECK_GE(ctl_fd, 0);
  // 20 for the u64 in ascii, 2 for 0x, 1 for \0, 9 in case of a mistake.
  constexpr int kU64InAsciiBytes = 20 + 2 + 1 + 9;
  char buf[kU64InAsciiBytes] = {0};
  CHECK_GT(read(ctl_fd, buf, sizeof(buf)), 0);
  uint64_t id;
  CHECK(absl::SimpleAtoi(buf, &id));
  return open(absl::StrCat(Ghost::kGhostfsMount, "/enclave_", id).c_str(),
              O_PATH);
}

// static
int LocalEnclave::GetCpuDataRegion(int dir_fd) {
  CHECK_GE(dir_fd, 0);
  return openat(dir_fd, "cpu_data", O_RDWR);
}

// static
void LocalEnclave::DestroyEnclave(int ctl_fd) {
  constexpr const char kCommand[] = "destroy";
  ssize_t msg_sz = sizeof(kCommand) - 1;  // No need for the \0
  // It's possible to get an error if the enclave is concurrently destroyed.
  write(ctl_fd, kCommand, msg_sz);
}

// static
void LocalEnclave::DestroyAllEnclaves() {
  std::error_code ec;
  auto f = std::filesystem::directory_iterator(Ghost::kGhostfsMount, ec);
  auto end = std::filesystem::directory_iterator();
  for (/* f */; !ec && f != end; f.increment(ec)) {
    if (std::regex_match(f->path().filename().string(),
                         std::regex("^enclave_.*"))) {
      int ctl = open((f->path() / "ctl").string().c_str(), O_RDWR);
      // If there is a concurrent destroyer, we could see the enclave directory
      // but fail to open ctl.
      if (ctl >= 0) {
        LocalEnclave::DestroyEnclave(ctl);
        close(ctl);
      }
    }
  }
}

void LocalEnclave::CommonInit() {
  CHECK_LE(topology_->num_cpus(), kMaxCpus);

  // Bug out if we already have a non-default global enclave.  We shouldn't have
  // more than one enclave per process at a time, at least not until we have
  // fully moved away from default enclaves.
  CHECK_EQ(Ghost::GetGlobalEnclaveCtlFd(), -1);
  Ghost::SetGlobalEnclaveCtlFd(ctl_fd_);

  int data_fd = LocalEnclave::GetCpuDataRegion(dir_fd_);
  CHECK_GE(data_fd, 0);
  data_region_size_ = GetFileSize(data_fd);
  data_region_ = static_cast<struct ghost_cpu_data*>(
      mmap(/*addr=*/nullptr, data_region_size_, PROT_READ | PROT_WRITE,
           MAP_SHARED, data_fd, /*offset=*/0));
  close(data_fd);
  CHECK_NE(data_region_, MAP_FAILED);

  Ghost::SetGlobalStatusWordTable(new StatusWordTable(dir_fd_, 0, 0));
}

// Initialize a CpuRep for each cpu in enclaves_cpus_ (aka, cpus()).
void LocalEnclave::BuildCpuReps() {
  cpu_set_t cpuset = topology_->ToCpuSet(*cpus());

  for (int i = 0; i < topology_->num_cpus(); ++i) {
    if (!CPU_ISSET(i, &cpuset)) {
      continue;
    }
    ghost_txn* txn = &data_region_[i].txn;
    Cpu cpu = topology_->cpu(txn->cpu);
    struct CpuRep* rep = &cpus_[cpu.id()];
    rep->req.Init(this, cpu, txn);
    rep->agent = nullptr;
  }
}

// We are attaching to an existing enclave, given the fd for the directory to
// e.g. /sys/fs/ghost/enclave_123/.  We use the cpus that already belong to the
// enclave.
LocalEnclave::LocalEnclave(Topology* topology, int enclave_fd)
    : Enclave(topology, enclave_fd) {
  destroy_when_destructed_ = false;

  CHECK_EQ(enclave_fd, dir_fd_);  // set by Enclave::Enclave
  CHECK_GE(dir_fd_, 0);

  ctl_fd_ = openat(dir_fd_, "ctl", O_RDWR);
  CHECK_GE(ctl_fd_, 0);

  CommonInit();

  int cpulist_fd = openat(dir_fd_, "cpulist", O_RDONLY);
  CHECK_GE(cpulist_fd, 0);
  std::string cpulist = ReadString(cpulist_fd);
  enclave_cpus_ = topology->ParseCpuStr(cpulist);
  close(cpulist_fd);

  BuildCpuReps();
}

// We are creating a new enclave.  We attempt to allocate enclave_cpus.
LocalEnclave::LocalEnclave(Topology* topology, CpuList enclave_cpus)
    : Enclave(topology, std::move(enclave_cpus)) {
  destroy_when_destructed_ = true;

  // Note that if you CHECK fail in this function, the LocalEnclave dtor won't
  // run, and you'll leak the enclave.  Closing the fd isn't enough; you need to
  // explicitly DestroyEnclave().  Given that enclaves are supposed to persist
  // past agent death, this is somewhat OK.
  ctl_fd_ = LocalEnclave::MakeNextEnclave();
  CHECK_GE(ctl_fd_, 0);

  dir_fd_ = LocalEnclave::GetEnclaveDirectory(ctl_fd_);
  CHECK_GE(dir_fd_, 0);

  CommonInit();

  int cpumask_fd = openat(dir_fd_, "cpumask", O_RDWR);
  CHECK_GE(cpumask_fd, 0);
  std::string cpumask = enclave_cpus.CpuMaskStr();
  // Note that if you ask for more cpus than the kernel knows about, i.e. more
  // than nr_cpu_ids, it may silently succeed.  The kernel only checks the cpus
  // it knows about.  However, since this cpumask was constructed from a
  // topology, there should not be more than nr_cpu_ids.
  CHECK_EQ(write(cpumask_fd, cpumask.c_str(), cpumask.length()),
           cpumask.length());
  close(cpumask_fd);

  BuildCpuReps();
}

LocalEnclave::~LocalEnclave() {
  (void)munmap(data_region_, data_region_size_);
  if (destroy_when_destructed_) {
    LocalEnclave::DestroyEnclave(ctl_fd_);
  }
  close(ctl_fd_);
  // agent_test has some cases where it creates new enclaves within the same
  // process, so reset the global enclave ghost variables
  Ghost::SetGlobalEnclaveCtlFd(-1);
  delete Ghost::GetGlobalStatusWordTable();
  Ghost::SetGlobalStatusWordTable(nullptr);
}

bool LocalEnclave::CommitRunRequests(const CpuList& cpu_list) {
  SubmitRunRequests(cpu_list);

  bool success = true;
  for (const Cpu& cpu : cpu_list) {
    RunRequest* req = GetRunRequest(cpu);
    if (!CompleteRunRequest(req)) success = false;
  }
  return success;
}

void LocalEnclave::SubmitRunRequests(const CpuList& cpu_list) {
  CHECK_EQ(Ghost::Commit(topology()->ToCpuSet(cpu_list)), 0);
}

bool LocalEnclave::CommitSyncRequests(const CpuList& cpu_list) {
  if (SubmitSyncRequests(cpu_list)) {
    // The sync group committed successfully. The kernel has already released
    // ownership of transactions that were part of the sync group.
    return true;
  }

  // Commit failed so we expect (and CHECK) that this flips to true after
  // calling CompleteRunRequest on cpus in `cpu_list`.
  bool failure_detected = false;
  for (const Cpu& cpu : cpu_list) {
    RunRequest* req = GetRunRequest(cpu);
    if (!CompleteRunRequest(req)) {
      // Cannot bail early even though the return value has been finalized
      // because we must call CompleteRunRequest() on all cpus in the list.
      failure_detected = true;
    }
  }
  CHECK(failure_detected);

  ReleaseSyncRequests(cpu_list);

  return false;
}

bool LocalEnclave::SubmitSyncRequests(const CpuList& cpu_list) {
  int ret = Ghost::SyncCommit(topology()->ToCpuSet(cpu_list));
  CHECK(ret == 0 || ret == 1);
  return ret;
}

void LocalEnclave::ReleaseSyncRequests(const CpuList& cpu_list) {
  for (const Cpu& cpu : cpu_list) {
    RunRequest* req = GetRunRequest(cpu);
    CHECK(req->committed());
    CHECK(req->sync_group_owned());
    req->sync_group_owner_set(kSyncGroupNotOwned);
  }
}

bool LocalEnclave::CommitRunRequest(RunRequest* req) {
  SubmitRunRequest(req);
  return CompleteRunRequest(req);
}

void LocalEnclave::SubmitRunRequest(RunRequest* req) {
  GHOST_DPRINT(2, stderr, "COMMIT(%d): %s %d", req->cpu().id(),
               Gtid(req->txn_->gtid).describe(), req->txn_->task_barrier);

  if (req->open()) {
    CHECK_EQ(Ghost::Commit(req->cpu().id()), 0);
  } else {
    // Request already picked up by target CPU for commit.
  }
}

bool LocalEnclave::CompleteRunRequest(RunRequest* req) {
  // If request was picked up asynchronously then wait for the commit.
  //
  // N.B. we must do this even if we call Ghost::Commit() because the
  // the request could be committed asynchronously even in that case.
  while (!req->committed()) {
    asm volatile("pause");
  }

  ghost_txn_state state = req->state();

  if (state == GHOST_TXN_COMPLETE) {
    return true;
  }

  // The following failures are expected:
  switch (state) {
    // task is dead (agent preempted a task and ran it again subsequently
    // while racing with task exit).
    case GHOST_TXN_TARGET_NOT_FOUND:
      break;

    // stale task or agent barriers.
    case GHOST_TXN_TARGET_STALE:
    case GHOST_TXN_AGENT_STALE:
      break;

    // target CPU is running a higher priority sched_class.
    case GHOST_TXN_CPU_UNAVAIL:
      break;

    // txn poisoned due to sync-commit failure.
    case GHOST_TXN_POISONED:
      break;

    // target already on cpu
    case GHOST_TXN_TARGET_ONCPU:
      if (req->allow_txn_target_on_cpu()) {
        break;
      }
      ABSL_FALLTHROUGH_INTENDED;

    default:
      GHOST_ERROR("unexpected state after commit: %d", state);
      return false;  // ERROR needs no-return annotation.
  }
  return false;
}

bool LocalEnclave::LocalYieldRunRequest(
    const RunRequest* const req, const StatusWord::BarrierToken agent_barrier,
    const int flags) {
  DCHECK_EQ(sched_getcpu(), req->cpu().id());
  int error = Ghost::Run(Gtid(0), agent_barrier, StatusWord::NullBarrierToken(),
                         req->cpu().id(), flags);
  if (error != 0) CHECK_EQ(errno, ESTALE);

  return error == 0;
}

bool LocalEnclave::PingRunRequest(RunRequest* req, Cpu remote_cpu) {
  const int run_flags = 0;
  int rc = Ghost::Run(Gtid(GHOST_AGENT_GTID),
                      StatusWord::NullBarrierToken(),  // agent_barrier
                      StatusWord::NullBarrierToken(),  // task_barrier
                      remote_cpu.id(), run_flags);
  return rc == 0;
}

void LocalEnclave::AttachAgent(Cpu cpu, Agent* agent) {
  CHECK_EQ(rep(cpu)->agent, nullptr);
  rep(cpu)->agent = agent;
  Enclave::AttachAgent(cpu, agent);
}

void LocalEnclave::DetachAgent(Agent* agent) {
  rep(agent->cpu())->agent = nullptr;
  Enclave::DetachAgent(agent);
}

void LocalEnclave::AdvertiseReady() {
  // There can be only one open, writable file for agent_online, per enclave.
  // As long as this FD (and any dups) are held open, the system will believe
  // there is an agent capable of scheduling this enclave.
  //
  // We don't need to store the FD anywhere either.  If we crash or exit, the
  // kernel will close it for us.
  int fd = openat(dir_fd_, "agent_online", O_RDWR);
  CHECK_GE(fd, 0);
}

void RunRequest::Open(const RunRequestOptions& options) {
  // Wait for current owner to relinquish ownership of the sync_group txn.
  if (options.sync_group_owner != kSyncGroupNotOwned) {
    while (sync_group_owned()) {
      asm volatile("pause");
    }
  } else {
    CHECK(!sync_group_owned());
  }

  // We don't allow transaction clobbering.
  //
  // Once a transaction is opened it must be committed (sync or async)
  // before opening it again. We rely on caller doing a Submit() which
  // guarantees a commit-barrier on the transaction.
  CHECK(committed());

  txn_->agent_barrier = options.agent_barrier;
  txn_->gtid = options.target.id();
  txn_->task_barrier = options.target_barrier;
  txn_->run_flags = options.run_flags;
  txn_->commit_flags = options.commit_flags;
  if (options.sync_group_owner != kSyncGroupNotOwned) {
    sync_group_owner_set(options.sync_group_owner);
  }
  allow_txn_target_on_cpu_ = options.allow_txn_target_on_cpu;
  if (allow_txn_target_on_cpu_)
    CHECK(sync_group_owned());
  txn_->state.store(GHOST_TXN_READY, std::memory_order_release);
}

bool RunRequest::Abort() {
  int32_t expected = txn_->state.load(std::memory_order_relaxed);
  if (expected == GHOST_TXN_READY) {
    // We do a compare exchange since we may race with the kernel trying to
    // commit this transaction.
    const bool aborted = txn_->state.compare_exchange_strong(
        expected, GHOST_TXN_ABORTED, std::memory_order_release);
    if (aborted && sync_group_owned()) {
      sync_group_owner_set(kSyncGroupNotOwned);
    }
    return aborted;
  }
  // This transaction has already committed.
  return false;
}

}  // namespace ghost
