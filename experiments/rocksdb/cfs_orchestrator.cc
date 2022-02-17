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

#include "experiments/rocksdb/cfs_orchestrator.h"

#include "absl/functional/bind_front.h"

namespace ghost_test {

void CfsOrchestrator::InitThreadPool() {
  // Initialize the thread pool.
  std::vector<ghost::GhostThread::KernelScheduler> kernel_schedulers;
  std::vector<std::function<void(uint32_t)>> thread_work;
  // Set up the load generator thread.
  kernel_schedulers.push_back(ghost::GhostThread::KernelScheduler::kCfs);
  thread_work.push_back(
      absl::bind_front(&CfsOrchestrator::LoadGenerator, this));
  // Set up the dispatcher thread.
  kernel_schedulers.push_back(ghost::GhostThread::KernelScheduler::kCfs);
  thread_work.push_back(absl::bind_front(&CfsOrchestrator::Dispatcher, this));
  // Set up the worker threads.
  kernel_schedulers.insert(kernel_schedulers.end(), options().num_workers,
                           ghost::GhostThread::KernelScheduler::kCfs);
  thread_work.insert(thread_work.end(), options().num_workers,
                     absl::bind_front(&CfsOrchestrator::Worker, this));
  // Checks.
  // Add 2 to account for the load generator thread and the dispatcher thread.
  CHECK_EQ(kernel_schedulers.size(), total_threads());
  CHECK_EQ(kernel_schedulers.size(), thread_work.size());
  // Pass the scheduler types and the thread work to 'Init'.
  thread_pool().Init(kernel_schedulers, thread_work);
}

CfsOrchestrator::CfsOrchestrator(Orchestrator::Options opts)
    // Add 2 to account for the load generator thread and the dispatcher
    // thread.
    : Orchestrator(opts, opts.num_workers + 2),
      thread_wait_(/*num_threads=*/total_threads(), options().cfs_wait_type),
      threads_ready_(total_threads()) {
  CHECK_EQ(options().num_workers, options().worker_cpus.size());

  InitThreadPool();
}

void CfsOrchestrator::Terminate() {
  const absl::Duration runtime = absl::Now() - start();
  // Do this check after calculating 'runtime' to avoid inflating 'runtime'.
  CHECK_GT(start(), absl::UnixEpoch());

  // Have the load generator exit first. This makes it easier in case the load
  // generator logic changes in the future to always expect the dispatcher to be
  // alive while it is running.
  thread_pool().MarkExit(0);
  while (thread_pool().NumExited() < 1) {
  }

  // Have the dispatcher exit second. This makes it easier in case the
  // dispatcher logic changes in the future to always expect all workers to be
  // alive while it is running.
  thread_pool().MarkExit(1);
  while (thread_pool().NumExited() < 2) {
  }

  // Have all workers exit.
  for (size_t i = 2; i < total_threads(); ++i) {
    thread_pool().MarkExit(i);
  }
  while (thread_pool().NumExited() < total_threads()) {
    for (size_t i = 0; i < options().num_workers; ++i) {
      // We add 2 since the load generator (SID 0) and the dispatcher (SID 1)
      // are always runnable.
      thread_wait_.MarkRunnable(i + 2);
    }
  }
  thread_pool().Join();

  PrintResults(runtime);
}

void CfsOrchestrator::LoadGenerator(uint32_t sid) {
  if (!first_run().Triggered(sid)) {
    CHECK(first_run().Trigger(sid));
    CHECK_EQ(sid, kLoadGeneratorSid);
    CHECK_EQ(ghost::Ghost::SchedSetAffinity(
                 ghost::Gtid::Current(),
                 ghost::MachineTopology()->ToCpuList(
                     std::vector<int>{options().load_generator_cpu})),
             0);
    // Use 'printf' instead of 'std::cout' so that the print contents do not get
    // interleaved with the dispatcher's, the workers', and the main thread's
    // print contents. 'printf' acquires a lock whereas 'std::cout' does not.
    printf("Load generator (SID %u, TID: %ld, affined to CPU %u)\n", sid,
           syscall(SYS_gettid), options().load_generator_cpu);
    // Wait until the dispatcher and the workers have initialized themselves
    // before starting the timer and generating load. If we started generating
    // load before the dispatcher and workers are initialized, we will not have
    // functional correctness issues but the load will not be processed, causing
    // us to report bad performance at the end of the experiment that solely
    // reflects initialization costs, which are irrelevant to the experiment.
    threads_ready_.Block();
    set_start(absl::Now());
    network().Start();
  }

  if (worker_work()[kDispatcherSid]->num_requests.load(
          std::memory_order_acquire) != 0) {
    return;
  }
  worker_work()[kDispatcherSid]->requests.clear();
  for (size_t i = 0; i < kLoadGeneratorBatchSize; ++i) {
    Request request;
    if (network().Poll(request)) {
      worker_work()[kDispatcherSid]->requests.push_back(request);
    } else {
      // No more requests waiting in the ingress queue, so give the requests we
      // have so far to the dispatcher.
      break;
    }
  }
  worker_work()[kDispatcherSid]->num_requests.store(
      worker_work()[kDispatcherSid]->requests.size(),
      std::memory_order_release);
}

void CfsOrchestrator::HandleLoadGenerator() {
  uint32_t load_count = worker_work()[kDispatcherSid]->num_requests.load(
      std::memory_order_acquire);
  if (load_count > 0) {
    CHECK_EQ(load_count, worker_work()[kDispatcherSid]->requests.size());
    dispatcher_queue_.insert(dispatcher_queue_.end(),
                             worker_work()[kDispatcherSid]->requests.begin(),
                             worker_work()[kDispatcherSid]->requests.end());
    // The dispatcher is not writing anything visible to the load generator in
    // this critical section, so write to 'num_requests' with a relaxed
    // consistency rather than a release consistency.
    worker_work()[kDispatcherSid]->num_requests.store(
        0, std::memory_order_relaxed);
  }
}

void CfsOrchestrator::GetIdleWorkerSIDs() {
  idle_sids_.clear();
  for (size_t i = 0; i < options().num_workers; ++i) {
    // We add 2 since we cannot assign requests to the load generator (SID 0) or
    // to the dispatcher (SID 1).
    const uint32_t worker_sid = i + 2;
    if (worker_work()[worker_sid]->num_requests.load(
            std::memory_order_acquire) == 0) {
      idle_sids_.push_back(worker_sid);
    }
  }
}

void CfsOrchestrator::Dispatcher(uint32_t sid) {
  if (!first_run().Triggered(sid)) {
    CHECK(first_run().Trigger(sid));
    CHECK_EQ(sid, kDispatcherSid);
    CHECK_EQ(ghost::Ghost::SchedSetAffinity(
                 ghost::Gtid::Current(),
                 ghost::MachineTopology()->ToCpuList(
                     std::vector<int>{options().cfs_dispatcher_cpu})),
             0);
    printf("Dispatcher (SID %u, TID: %ld, affined to CPU %u)\n", sid,
           syscall(SYS_gettid), options().cfs_dispatcher_cpu);
    // The load generator will not start generating requests until itself, the
    // dispatcher, and all of the workers have initialized themselves, so the
    // dispatcher will never attempt to assign requests to uninitialized workers
    // because the load generator gives it no requests to assign.
    threads_ready_.Block();
  }

  HandleLoadGenerator();
  if (dispatcher_queue_.empty()) {
    // There are no requests to assign.
    return;
  }

  GetIdleWorkerSIDs();
  uint32_t size = idle_sids_.size();
  for (uint32_t i = 0; i < size; ++i) {
    uint32_t worker_sid = idle_sids_.front();
    // We can do a relaxed load rather than an acquire load because
    // 'GetIdleWorkerSIDs' already did an acquire load for 'num_requests'.
    CHECK_EQ(
        worker_work()[worker_sid]->num_requests.load(std::memory_order_relaxed),
        0);
    worker_work()[worker_sid]->requests.clear();

    for (uint32_t i = 0; i < options().batch; ++i) {
      // We need to check this here (in addition to above) since the dispatcher
      // queue may become empty before we assign 'options.batch' requests to
      // this worker.
      if (dispatcher_queue_.empty()) {
        break;
      }
      Request& r = dispatcher_queue_.front();

      r.request_assigned = absl::Now();
      worker_work()[worker_sid]->requests.push_back(r);
      dispatcher_queue_.pop_front();
    }

    if (!worker_work()[worker_sid]->requests.empty()) {
      // Assign the batch of requests to the next worker
      idle_sids_.pop_front();
      CHECK_LE(worker_work()[worker_sid]->requests.size(), options().batch);
      worker_work()[worker_sid]->num_requests.store(
          worker_work()[worker_sid]->requests.size(),
          std::memory_order_release);
      thread_wait_.MarkRunnable(worker_sid);
    } else {
      // There is no work waiting in the ingress queue.
      break;
    }
  }
}

void CfsOrchestrator::Worker(uint32_t sid) {
  if (!first_run().Triggered(sid)) {
    CHECK(first_run().Trigger(sid));
    // The first worker SID is 2, so subtract 2 to get the worker's CPU
    // assignment.
    CHECK_EQ(ghost::Ghost::SchedSetAffinity(
                 ghost::Gtid::Current(),
                 ghost::MachineTopology()->ToCpuList(
                     std::vector<int>{options().worker_cpus[sid - 2]})),
             0);
    printf("Worker (SID %u, TID: %ld, affined to CPU %u)\n", sid,
           syscall(SYS_gettid), options().worker_cpus[sid - 2]);
    // Wait until the dispatcher assigns work to this worker.
    thread_wait_.MarkIdle(sid);
    // Do this after 'MarkIdle'. If the worker did it before calling 'MarkIdle',
    // the dispatcher could assign work to this worker and then mark it
    // runnable. Then the worker could mark itself idle and go spin/sleep on
    // 'WaitUntilRunnable', causing the worker to do no work for the duration of
    // the experiment. Remember that 'MarkIdle' does not make the worker
    // spin/sleep -- only 'WaitUntilRunnable' does.
    threads_ready_.Block();
    thread_wait_.WaitUntilRunnable(sid);

    // Return here since it is possible the worker was never assigned a request
    // and is being woken up because the application is exiting.
    return;
  }

  WorkerWork* work = worker_work()[sid].get();

  size_t num_requests = work->num_requests.load(std::memory_order_acquire);
  // The worker should only return from 'WaitUntilRunnable' when it has one or
  // more requests assigned to it.
  CHECK_GT(num_requests, 0);
  CHECK_LE(num_requests, options().batch);
  CHECK_EQ(num_requests, work->requests.size());

  for (size_t i = 0; i < num_requests; ++i) {
    Request& request = work->requests[i];
    request.request_start = absl::Now();
    HandleRequest(request, gen()[sid]);
    request.request_finished = absl::Now();

    requests()[sid].push_back(request);
  }

  thread_wait_.MarkIdle(sid);
  // Set 'num_requests' to 0 after calling 'thread_wait_.MarkIdle' to avoid the
  // same race mentioned above when the worker is initializing.
  work->num_requests.store(0, std::memory_order_release);
  thread_wait_.WaitUntilRunnable(sid);
}

}  // namespace ghost_test
