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

#include "experiments/rocksdb/orchestrator.h"

#include <map>

namespace ghost_test {

namespace {
// Returns a string representation of the boolean 'b'.
std::string BoolToString(bool b) { return b ? "true" : "false"; }
}  // namespace

std::ostream& operator<<(std::ostream& os,
                         const Orchestrator::Options& options) {
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
  flags["load_generator_cpu"] = std::to_string(options.load_generator_cpu);
  flags["cfs_dispatcher_cpu"] = std::to_string(options.cfs_dispatcher_cpu);
  flags["num_workers"] = std::to_string(options.num_workers);

  for (int i = 0; i < options.worker_cpus.size(); i++) {
    flags["worker_cpus"] += std::to_string(options.worker_cpus[i]);
    if (i < options.worker_cpus.size() - 1) {
      flags["worker_cpus"] += " ";
    }
  }

  flags["cfs_wait_type"] =
      options.cfs_wait_type ==
              ghost_test::CompletelyFairScheduler::WaitType::kWaitSpin
          ? "spin"
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
      network_(options_.throughput, options_.range_query_ratio),
      gen_(total_threads),
      first_run_(total_threads),
      thread_pool_(total_threads) {
  CHECK(!options_.rocksdb_db_path.empty());
  CHECK_GE(options_.range_query_ratio, 0.0);
  CHECK_LE(options_.range_query_ratio, 1.0);
  CHECK_GE(options_.load_generator_cpu, 0);
  CHECK_NE(options_.load_generator_cpu, kBackgroundThreadCpu);
  CHECK(options_.scheduler != ghost::GhostThread::KernelScheduler::kCfs ||
        options_.cfs_dispatcher_cpu != kBackgroundThreadCpu);
  CHECK(options_.scheduler != ghost::GhostThread::KernelScheduler::kCfs ||
        options_.cfs_dispatcher_cpu >= 0);
  for (const int cpu : options_.worker_cpus) {
    CHECK_GE(cpu, 0);
    CHECK_NE(cpu, kBackgroundThreadCpu);
  }

  // Add 2 to account for the load generator thread and the dispatcher thread.
  for (size_t i = 0; i < options_.num_workers + 2; ++i) {
    worker_work_.push_back(std::make_unique<WorkerWork>());
    worker_work_.back()->num_requests = 0;

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

void Orchestrator::HandleRequest(Request& request, absl::BitGen& gen) {
  if (request.IsGet()) {
    HandleGet(request, gen);
  } else {
    CHECK(request.IsRange());
    HandleRange(request, gen);
  }
}

absl::Duration Orchestrator::GetThreadCpuTime() const {
  timespec ts;
  CHECK_EQ(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts), 0);
  return absl::Seconds(ts.tv_sec) + absl::Nanoseconds(ts.tv_nsec);
}

void Orchestrator::HandleGet(Request& request, absl::BitGen& gen) {
  CHECK(request.IsGet());

  absl::Duration start_duration = GetThreadCpuTime();
  absl::Duration service_time = options_.get_duration;
  if (options_.get_exponential_mean > absl::ZeroDuration()) {
    service_time +=
        Request::GetExponentialHandleTime(gen, options_.get_exponential_mean);
  }

  std::string response;
  Request::Get& get = std::get<Request::Get>(request.work);
  CHECK(database_.Get(get.entry, response));

  absl::Duration now_duration = GetThreadCpuTime();
  if (now_duration - start_duration < service_time) {
    Spin(service_time - (now_duration - start_duration), now_duration);
  }
}

void Orchestrator::HandleRange(Request& request, absl::BitGen& gen) {
  CHECK(request.IsRange());

  absl::Duration start_duration = GetThreadCpuTime();
  absl::Duration service_time = options_.range_duration;

  std::string response;
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

  while (GetThreadCpuTime() - start_duration < duration) {
    // We are doing synthetic work, so do not issue 'pause' instructions.
  }
}

}  // namespace ghost_test
