// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "experiments/rocksdb/orchestrator.h"

#include <map>

namespace ghost_test {

namespace {
// Returns a string representation of the boolean 'b'.
std::string BoolToString(bool b) { return b ? "true" : "false"; }
}  // namespace

std::ostream& operator<<(std::ostream& os, const Options& options) {
  // Put the options and their values into an 'std::map' so that we can print
  // out the option/value pairs in alphabetical order.
  std::map<std::string, std::string> flags;

  flags["print_format"] = options.print_options.pretty ? "pretty" : "csv";
  flags["print_distribution"] =
      BoolToString(options.print_options.distribution);
  flags["print_ns"] = BoolToString(options.print_options.ns);
  flags["print_get"] = BoolToString(options.print_get);
  flags["print_range"] = BoolToString(options.print_range);
  flags["rocksdb_db_path"] = options.rocksdb_db_path.string();
  flags["throughput"] = std::to_string(options.throughput);
  flags["range_query_ratio"] = std::to_string(options.range_query_ratio);

  std::string load_generator_cpus;
  for (size_t i = 0; i < options.load_generator_cpus.Size(); i++) {
    load_generator_cpus += options.load_generator_cpus.GetNthCpu(i).ToString();
    if (i < options.load_generator_cpus.Size() - 1) {
      load_generator_cpus += " ";
    }
  }
  flags["load_generator_cpus"] = load_generator_cpus;

  std::string cfs_dispatcher_cpus;
  for (size_t i = 0; i < options.cfs_dispatcher_cpus.Size(); i++) {
    cfs_dispatcher_cpus += options.cfs_dispatcher_cpus.GetNthCpu(i).ToString();
    if (i < options.cfs_dispatcher_cpus.Size() - 1) {
      cfs_dispatcher_cpus += " ";
    }
  }
  flags["cfs_dispatcher_cpus"] = cfs_dispatcher_cpus;

  flags["num_workers"] = std::to_string(options.num_workers);

  for (int i = 0; i < options.worker_cpus.Size(); i++) {
    flags["worker_cpus"] +=
        std::to_string(options.worker_cpus.GetNthCpu(i).id());
    if (i < options.worker_cpus.Size() - 1) {
      flags["worker_cpus"] += " ";
    }
  }

  flags["cfs_wait_type"] =
      options.cfs_wait_type == ghost_test::ThreadWait::WaitType::kSpin
          ? "spin"
          : "futex";
  flags["ghost_wait_type"] =
      options.ghost_wait_type == GhostWaitType::kPrioTable ? "prio_table"
                                                           : "futex";
  flags["get_duration"] = absl::FormatDuration(options.get_duration);
  flags["range_duration"] = absl::FormatDuration(options.range_duration);
  flags["get_exponential_mean"] =
      absl::FormatDuration(options.get_exponential_mean);
  flags["batch"] = std::to_string(options.batch);
  flags["experiment_duration"] =
      absl::FormatDuration(options.experiment_duration);
  flags["discard_duration"] = absl::FormatDuration(options.discard_duration);
  flags["scheduler"] =
      (options.scheduler == ghost::GhostThread::KernelScheduler::kCfs)
          ? "cfs"
          : "ghost";
  flags["ghost_qos"] = std::to_string(options.ghost_qos);

  bool first = true;
  for (const auto& [flag, value] : flags) {
    if (!first) {
      os << std::endl;
    }
    first = false;
    os << flag << ": " << value;
  }
  return os;
}

Orchestrator::Orchestrator(Options options, size_t total_threads)
    : options_(options),
      total_threads_(total_threads),
      database_(options_.rocksdb_db_path),
      gen_(total_threads),
      first_run_(total_threads),
      thread_pool_(total_threads) {
  CHECK(!options_.rocksdb_db_path.empty());
  CHECK_GE(options_.range_query_ratio, 0.0);
  CHECK_LE(options_.range_query_ratio, 1.0);
  CHECK(!options_.load_generator_cpus.IsSet(kBackgroundThreadCpu));
  CHECK(options_.scheduler != ghost::GhostThread::KernelScheduler::kCfs ||
        !options_.cfs_dispatcher_cpus.IsSet(kBackgroundThreadCpu));

  double throughput_per_load_generator =
      options_.throughput / options_.load_generator_cpus.Size();
  absl::PrintF("Each load generator generates a throughput of %f req/s\n",
               throughput_per_load_generator);
  for (size_t i = 0; i < options_.load_generator_cpus.Size(); i++) {
    network_.push_back(std::make_unique<SyntheticNetwork>(
        throughput_per_load_generator, options_.range_query_ratio));
  }
  for (const ghost::Cpu& cpu : options_.worker_cpus) {
    CHECK_NE(cpu.id(), kBackgroundThreadCpu);
  }

  for (size_t i = 0;
       i < options_.load_generator_cpus.Size() +
               options_.cfs_dispatcher_cpus.Size() + options_.num_workers;
       ++i) {
    worker_work_.push_back(std::make_unique<WorkerWork>());
    worker_work_.back()->num_requests = 0;
    worker_work_.back()->requests.reserve(options_.batch);
    worker_work_.back()->response.reserve(kResponseReservationSize);

    requests_.push_back(std::vector<Request>());
    // TODO: Can we make this smaller or use an 'std::deque' instead? I'm
    // concerned about the memory allocation overhead for an 'std::deque'
    // though.

    // Reserve enough space in each worker's vector for at most 30 seconds of
    // requests (assuming the worker is handling all requests). We reserve the
    // memory upfront to avoid memory allocations during the experiment. Memory
    // allocations are expensive and could cause threads to block on TCMalloc's
    // mutex, which puts more work on the scheduler.
    const absl::Duration reserve_duration =
        std::min(options_.experiment_duration, absl::Seconds(30));
    // Add one second to 'reserve_duration' so that we do not run the risk of
    // doing a memory allocation just before the end of the experiment.
    const size_t reserve_size =
        absl::ToDoubleSeconds(reserve_duration + absl::Seconds(1)) *
        options_.throughput;
    requests_.back().reserve(reserve_size);

    // The first insert into a vector is slow, likely because the allocator is
    // lazy and needs to assign the physical pages on the first insert. In other
    // words, the virtual pages seem to be allocated but physical pages are not
    // assigned on the call to 'reserve'. We insert and remove a few items here
    // to handle the overhead now. Workers should not handle this initialization
    // overhead since that is not what the benchmark wants to measure.
    for (int i = 0; i < 1000; ++i) {
      requests_.back().emplace_back();
    }
    requests_.back().clear();
  }
}

Orchestrator::~Orchestrator() {}

void Orchestrator::HandleRequest(Request& request, std::string& response,
                                 absl::BitGen& gen) {
  if (request.IsGet()) {
    HandleGet(request, response, gen);
  } else {
    CHECK(request.IsRange());
    HandleRange(request, response, gen);
  }
}

absl::Duration Orchestrator::GetThreadCpuTime() const {
  timespec ts;
  CHECK_EQ(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts), 0);
  return absl::Seconds(ts.tv_sec) + absl::Nanoseconds(ts.tv_nsec);
}

void Orchestrator::HandleGet(Request& request, std::string& response,
                             absl::BitGen& gen) {
  CHECK(request.IsGet());

  absl::Duration start_duration = GetThreadCpuTime();
  absl::Duration service_time = options_.get_duration;
  if (options_.get_exponential_mean > absl::ZeroDuration()) {
    service_time +=
        Request::GetExponentialHandleTime(gen, options_.get_exponential_mean);
  }

  Request::Get& get = std::get<Request::Get>(request.work);
  CHECK(database_.Get(get.entry, response));

  absl::Duration now_duration = GetThreadCpuTime();
  if (now_duration - start_duration < service_time) {
    Spin(service_time - (now_duration - start_duration), now_duration);
  }
}

void Orchestrator::HandleRange(Request& request, std::string& response,
                               absl::BitGen& gen) {
  CHECK(request.IsRange());

  absl::Duration start_duration = GetThreadCpuTime();
  absl::Duration service_time = options_.range_duration;

  Request::Range& range = std::get<Request::Range>(request.work);
  CHECK(database_.RangeQuery(range.start_entry, range.size, response));

  absl::Duration now_duration = GetThreadCpuTime();
  if (now_duration - start_duration < service_time) {
    Spin(service_time - (now_duration - start_duration), now_duration);
  }
}

void Orchestrator::PrintResultsHelper(
    const std::string& results_name, absl::Duration experiment_duration,
    const std::vector<Request>& requests) const {
  std::cout << results_name << ":" << std::endl;
  latency::Print(requests, experiment_duration, options_.print_options);
}

std::vector<Request> Orchestrator::FilterRequests(
    const std::vector<std::vector<Request>>& requests,
    std::function<bool(const Request&)> should_include) const {
  std::vector<Request> filtered;
  for (const std::vector<Request>& worker_requests : requests) {
    for (const Request& r : worker_requests) {
      if (should_include(r)) {
        filtered.push_back(r);
      }
    }
  }
  return filtered;
}

bool Orchestrator::ShouldDiscard(const Request& request) const {
  return request.request_generated < start_ + options_.discard_duration;
}

void Orchestrator::PrintResults(absl::Duration experiment_duration) const {
  std::cout << "Stats:" << std::endl;
  // We discard some of the results, so subtract this discard period from the
  // experiment duration so that the correct throughput is calculated.
  absl::Duration tracked_duration =
      experiment_duration - options_.discard_duration;
  if (options_.print_get) {
    PrintResultsHelper(
        "Get", tracked_duration,
        FilterRequests(requests_, [this](const Request& r) -> bool {
          return !ShouldDiscard(r) && r.IsGet();
        }));
  }
  if (options_.print_range) {
    PrintResultsHelper(
        "Range", tracked_duration,
        FilterRequests(requests_, [this](const Request& r) -> bool {
          return !ShouldDiscard(r) && r.IsRange();
        }));
  }
  PrintResultsHelper(
      "All", tracked_duration,
      FilterRequests(requests_, [this](const Request& r) -> bool {
        return !ShouldDiscard(r);
      }));
}

void Orchestrator::Spin(absl::Duration duration,
                        absl::Duration start_duration) const {
  if (duration <= absl::ZeroDuration()) {
    return;
  }

  // We are using the CPU time consumed by the thread to determine how much
  // synthetic work has been performed. We are not just looking at wall clock
  // times, since the thread could be preempted and the clock would still
  // advance, wrongly causing us to think that the thread has performed
  // synthetic work while it was preempted. Using the CPU time consumed by the
  // thread is an accurate way of tracking synthetic work since the CPU time
  // only advances while the thread is running on a CPU, not while the thread is
  // preempted.
  while (GetThreadCpuTime() - start_duration < duration) {
    // We are doing synthetic work, so do not issue 'pause' instructions.
  }
}

}  // namespace ghost_test
