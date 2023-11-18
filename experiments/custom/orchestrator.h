// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef GHOST_EXPERIMENTS_ROCKSDB_ORCHESTRATOR_H_
#define GHOST_EXPERIMENTS_ROCKSDB_ORCHESTRATOR_H_

#include <filesystem>

#include "experiments/rocksdb/database.h"
#include "experiments/rocksdb/ingress.h"
#include "experiments/rocksdb/latency.h"
#include "experiments/rocksdb/request.h"
#include "experiments/shared/thread_pool.h"
#include "experiments/shared/thread_wait.h"

namespace ghost_test {

enum GhostWaitType {
  kPrioTable,
  kFutex,
};

// Orchestrator configuration options.
struct Options {
  // Parses all command line flags and returns them as an 'Options' instance.
  static Options GetOptions();

  // This pass a string representation of 'options' to 'os'. The options are
  // printed in alphabetical order by name.
  friend std::ostream& operator<<(std::ostream& os, const Options& options);

  latency::PrintOptions print_options;

  // The orchestrator prints the overall results for all request types combined,
  // no matter what.

  // If true, the orchestrator will also print a section with the results for
  // just Get requests.
  bool print_get;

  // If true, the orchestrator will also print a section with the results for
  // just Range queries.
  bool print_range;

  // The path to the RocksDB database.
  std::filesystem::path rocksdb_db_path;

  // The throughput of the generated synthetic load.
  double throughput;

  // The share of requests that are Range queries. This value must be greater
  // than or equal to 0 and less than or equal to 1. The share of requests that
  // are Get requests is '1 - range_query_ratio'.
  double range_query_ratio;

  // The CPUs that the load generator threads run on.
  ghost::CpuList load_generator_cpus = ghost::MachineTopology()->EmptyCpuList();

  // For CFS (Linux Completely Fair Scheduler) experiments, the CPUs that the
  // dispatchers run on.
  ghost::CpuList cfs_dispatcher_cpus = ghost::MachineTopology()->EmptyCpuList();

  // The number of workers. Each worker has one thread.
  size_t num_workers;

  // The CPUs that worker threads run on for CFS (Linux Completely Fair
  // Scheduler) experiments. Each worker thread is pinned to its own CPU. Thus,
  // `worker_cpus.Size()` must be equal to `num_workers`.
  //
  // For ghOSt experiments, ghOSt assigns workers to CPUs. Thus, this vector
  // must be empty when ghOSt is used.
  ghost::CpuList worker_cpus = ghost::MachineTopology()->EmptyCpuList();

  // The worker wait type for CFS (Linux Completely Fair Scheduler)
  // experiments. The workers can either spin while waiting for more work
  // ('kWaitSpin') or they can sleep on a futex while waiting for more work
  // ('kWaitFutex').
  ThreadWait::WaitType cfs_wait_type;

  // The worker wait type for ghOSt experiments. The workers can either interact
  // with ghOSt and wait via the PrioTable or they can wait via a futex. In the
  // former case, ghOSt learns worker runnability status via the PrioTable. In
  // the latter case, ghOSt learns worker runnability status implicitly via
  // TASK_BLOCKED/TASK_WAKEUP messages generated via the futex.
  GhostWaitType ghost_wait_type;

  // The total amount of time spent processing a Get request in RocksDB and
  // doing synthetic work.
  absl::Duration get_duration;

  // The total amount of time spent processing a Range query in RocksDB and
  // doing synthetic work.
  absl::Duration range_duration;

  // The Get request service time distribution can be converted from a fixed
  // distribution to an exponential distribution by adding a sample from the
  // exponential distribution with a mean of 'get_exponential_mean' to
  // 'get_duration' to get the total service time for a Get request.
  //
  // Distribution: 'get_duration' + Exp(1 / 'get_exponential_mean')
  //
  // To keep the Get request service time distribution as a fixed distribution,
  // set 'get_exponential_mean' to 'absl::ZeroDuration()'.
  absl::Duration get_exponential_mean;

  // The maximum number of requests to assign at a time to a worker. Generally
  // this should be set to 1 (otherwise centralized queuing and Shinjuku cannot
  // be faithfully implemented) but we support larger batches for future
  // experiments that may want them.
  size_t batch;

  // The experiment duration.
  absl::Duration experiment_duration;

  // Discards all results from when the experiment starts to when the 'discard'
  // duration has elapsed. We do not want the results to include initialization
  // costs, such as page faults.
  absl::Duration discard_duration;

  // The scheduler that schedules the experiment. This is either CFS (Linux
  // Completely Fair Scheduler) or ghOSt.
  ghost::GhostThread::KernelScheduler scheduler;

  // For the ghOSt experiments, this is the QoS (Quality-of-Service) class for
  // the PrioTable work class that all worker sched items are added to.
  uint32_t ghost_qos;
};

// This class is the central orchestrator of the RocksDB experiment. It manages
// the load generator, the dispatcher (if one exists), and the workers. It
// prints out the results when the experiment is finished.
//
// Note that this is an abstract class so it cannot be constructed.
class Orchestrator {
 public:
  // Threads use this type to pass requests to each other. In the CFS (Linux
  // Completely Fair Scheduler) experiments, the load generator uses this to
  // pass requests to the dispatcher and the dispatcher uses this to pass
  // requests to workers. In the ghOSt experiments, the load generator uses this
  // to pass requests to workers.
  //
  // When 'num_requests' is greater than zero, there are pending requests for
  // the worker. When 'num_requests' is 0, there are no pending requests for the
  // worker, so the dispatcher should add requests.
  struct WorkerWork {
    // The number of requests in 'requests'. We use this atomic rather than just
    // look at 'requests.size()' since the dispatcher and the worker need an
    // atomic to sync on. This number should never be greater than
    // 'options_.batch'.
    std::atomic<size_t> num_requests;
    // The requests.
    std::vector<Request> requests;
    std::string response;
    absl::Time last_finished;
  } ABSL_CACHELINE_ALIGNED;

  // Affine all background threads to this CPU.
  static constexpr int kBackgroundThreadCpu = 0;

  virtual ~Orchestrator() = 0;

  // Stops the experiment, joins all threads (i.e., the load generator, the
  // dispatcher (if one exists), and the workers) and prints the results
  // specified in 'options_'.
  virtual void Terminate() = 0;

  // Returns the CPU runtime for the calling thread.
  absl::Duration GetThreadCpuTime() const;

 protected:
  // Constructs the orchestrator. 'options' is the experiment settings.
  // 'total_threads' is the total number of threads managed by the orchestrator,
  // including the load generator thread, the worker threads, and if relevant,
  // the dispatcher thread.
  Orchestrator(Options options, size_t total_threads);

  // This method is executed in a loop by the load generator thread. This method
  // checks the ingress queue for pending requests. 'sid' is the sched item
  // identifier for the load generator thread.
  virtual void LoadGenerator(uint32_t sid) = 0;

  // This method is executed in a loop by the dispatcher thread, if one exists.
  // If so, this method receives requests from the load generator and assigns
  // them to workers. 'sid' is the sched item identifier for the dispatcher
  // thread.
  virtual void Dispatcher(uint32_t sid) = 0;

  // This method is executed in a loop by worker threads. This method handles
  // the request assigned to it by accessing the RocksDB database and doing
  // synthetic work. 'sid' is the sched item identifier for the worker thread.
  virtual void Worker(uint32_t sid) = 0;

  // Handles 'request' which is either a Get request or a Range query. 'gen' is
  // a random bit generator used for Get requests that have an exponential
  // service time. 'gen' is used to generate a sample from the exponential
  // distribution.
  void HandleRequest(Request& request, std::string& response,
                     absl::BitGen& gen);

  // Prints all results (total numbers of requests, throughput, and latency
  // percentiles). 'experiment_duration' is the duration of the experiment.
  void PrintResults(absl::Duration experiment_duration) const;

  virtual const Options& options() const { return options_; }

  size_t total_threads() const { return total_threads_; }

  SyntheticNetwork& network(uint32_t sid) { return *network_[sid]; }

  ExperimentThreadPool& thread_pool() { return thread_pool_; }

  absl::Time start() const { return start_; }
  void set_start(absl::Time start) { start_ = start; }

  std::vector<std::unique_ptr<WorkerWork>>& worker_work() {
    return worker_work_;
  }

  std::vector<std::vector<Request>>& requests() { return requests_; }

  std::vector<absl::BitGen>& gen() { return gen_; }

  ThreadTrigger& first_run() { return first_run_; }

 private:
  // The bytes to pre-allocate on the heap for each response.
  static constexpr size_t kResponseReservationSize = 4096;

  // Processes 'request', which must be a Get request (a CHECK will fail if
  // not).
  void HandleGet(Request& request, std::string& response, absl::BitGen& gen);

  // Processes 'request', which must be a Range query (a CHECK will fail if
  // not).
  void HandleRange(Request& request, std::string& response, absl::BitGen& gen);

  // Prints the results for 'requests' (total number of requests in 'requests',
  // throughput, and latency percentiles). 'results_name' is a name printed with
  // the results (e.g., "Get" for Get requests) and 'experiment_duration' is the
  // duration of the experiment.
  void PrintResultsHelper(const std::string& results_name,
                          absl::Duration experiment_duration,
                          const std::vector<Request>& requests) const;

  // Squashes the two-dimensional 'requests' vector into a one-dimensional
  // vector and returns the one-dimensional vector. Each request is only added
  // into the one-dimensional vector if 'should_include' returns true when the
  // request is passed as a parameter to it.
  std::vector<Request> FilterRequests(
      const std::vector<std::vector<Request>>& requests,
      std::function<bool(const Request&)> should_include) const;

  // Returns true if 'request' was generated during the discard period should
  // not be included in the results. Returns false if 'request' was generated
  // after the discard and should be included in the results.
  bool ShouldDiscard(const Request& request) const;

  // Spins for 'duration'. 'start_duration' is the CPU time consumed by the
  // thread when calling this method. There is overhead to calling this method,
  // such as creating the stack frame, so passing 'start_duration' allows the
  // method to count this overhead toward its synthetic work.
  void Spin(absl::Duration duration, absl::Duration start_duration) const;

  // Orchestrator options.
  const Options options_;

  // The total number of threads managed by the orchestrator, including the load
  // generator thread, the worker threads, and if relevant, the dispatcher
  // thread.
  const size_t total_threads_;

  // The RocksDB database.
  Database database_;

  // The synthetic networks that the load generators use to generate synthetic
  // requests.
  std::vector<std::unique_ptr<SyntheticNetwork>> network_;

  // The time that the experiment started at (after initialization).
  absl::Time start_;

  // Shared memory used by the dispatcher to pass requests to workers. Worker
  // 'i' accesses index 'i' in this vector. We wrap each 'WorkerWork' struct in
  // a unique pointer since the struct contains an atomic and therefore does not
  // have a copy constructor, so it cannot be stored directly into a vector.
  std::vector<std::unique_ptr<WorkerWork>> worker_work_;

  // The requests processed to completion by workers.
  std::vector<std::vector<Request>> requests_;

  // Random bit generators. Each thread has its own bit generator since the bit
  // generators are not thread safe.
  std::vector<absl::BitGen> gen_;

  // A thread triggers itself the first time it runs. Each thread does work on
  // its first iteration, so each thread uses the trigger to know if it is in
  // its first iteration or not.
  ThreadTrigger first_run_;

  // The thread pool.
  ExperimentThreadPool thread_pool_;
};

}  // namespace ghost_test

#endif  // GHOST_EXPERIMENTS_ROCKSDB_ORCHESTRATOR_H_
