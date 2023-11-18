// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "experiments/rocksdb/latency.h"

#include <cstdint>
#include <iomanip>
#include <iostream>

#include "lib/base.h"

namespace ghost_test {

namespace latency {

// Generates and prints the latencies for the interval that starts with
// 'first_timestamp_name' and ends with 'second_timestamp_name'.
// If 'second_timestamp_name' hasn't been filled in yet for a request, then the
// request hasn't made it to this stage, so the request's latency is not
// included in this stage's results.
//
// We use a macro since only the struct member names for the timestamps change
// between stages, but there is no way to pass member names as parameters to a
// method. Thus, we need to use a macro to avoid code duplication.
// Note that we wrap the macro body in curly braces to avoid variable name
// conflicts for the 'std::function' variables if this macro is used more than
// once in a given scope.
#define HANDLE_STAGE(stage, requests, runtime, first_timestamp_name, \
                     second_timestamp_name, options)                 \
  {                                                                  \
    std::function<bool(const Request&)> should_include =             \
        [](const Request& r) -> bool {                               \
      return r.second_timestamp_name != absl::UnixEpoch();           \
    };                                                               \
    std::function<absl::Duration(const Request&)> difference =       \
        [](const Request& r) -> absl::Duration {                     \
      return r.second_timestamp_name - r.first_timestamp_name;       \
    };                                                               \
    std::vector<absl::Duration> results =                            \
        GetStageResults(requests, should_include, difference);       \
    PrintStage(results, runtime, stage, options);                    \
  }

template <class T>
struct Results {
  // The total number of requests.
  T total;
  // The throughput;
  T throughput;
  // The min latency.
  T min;
  // The 50th percentile latency.
  T fifty;
  // The 99th percentile latency.
  T ninetynine;
  // The 99.5th percentile latency.
  T ninetyninefive;
  // The 99.9th percentile latency.
  T ninetyninenine;
  // The max latency.
  T max;
};

// Prints the results in human-readable form.
template <class T>
static void PrintLinePretty(std::ostream& os, const std::string& stage,
                            bool dashes, const Results<T>& results) {
  os << std::left;
  os << std::setw(kStageLen) << stage << " ";
  os << std::setw(kTotalRequestsLen) << results.total << " ";
  os << std::setw(kThroughputLen) << results.throughput << " ";
  os << std::setw(kResultLen) << results.min << " ";
  os << std::setw(kResultLen) << results.fifty << " ";
  os << std::setw(kResultLen) << results.ninetynine << " ";
  os << std::setw(kResultLen) << results.ninetyninefive << " ";
  os << std::setw(kResultLen) << results.ninetyninenine << " ";
  os << std::setw(kResultLen) << results.max;
  os << std::endl;
  if (dashes) {
    os << std::string(kNumDashes, '-') << std::endl;
  }
}

// Prints the results in CSV form.
template <class T>
static void PrintLineCsv(std::ostream& os, const Results<T>& results) {
  os << std::left;
  os << results.total << ",";
  os << results.throughput << ",";
  os << results.min << ",";
  os << results.fifty << ",";
  os << results.ninetynine << ",";
  os << results.ninetyninefive << ",";
  os << results.ninetyninenine << ",";
  os << results.max;
  os << std::endl;
}

// Prints the distribution for a stage.
static void PrintDistribution(std::ostream& os,
                              const std::vector<absl::Duration>& durations,
                              absl::Duration divisor) {
  os << "Distribution:" << std::endl;
  for (absl::Duration d : durations) {
    os << d / divisor << " ";
  }
  os << std::endl;
}

// Prints the results for one stage. 'durations' contains the latencies for all
// requests, 'runtime' is the total runtime of the app (used to calculate
// throughput), 'stage' is the stage name, and 'options' contains print options.
static void PrintStage(const std::vector<absl::Duration>& durations,
                       absl::Duration runtime, const std::string& stage,
                       PrintOptions options) {
  if (durations.empty()) {
    const Results<std::string> results = {.total = "-",
                                          .throughput = "-",
                                          .min = "-",
                                          .fifty = "-",
                                          .ninetynine = "-",
                                          .ninetyninefive = "-",
                                          .ninetyninenine = "-",
                                          .max = "-"};
    if (options.pretty) {
      PrintLinePretty(*options.os, stage, /*dashes=*/false, results);
    } else {
      PrintLineCsv(*options.os, results);
    }
    return;
  }

  Results<uint64_t> results;
  absl::Duration divisor =
      options.ns ? absl::Nanoseconds(1) : absl::Microseconds(1);
  results.total = durations.size();
  results.throughput =
      (durations.size() / absl::ToDoubleMilliseconds(runtime)) * 1000;
  // When the number of latencies is even, I prefer to subtract 1 to get the
  // correct index for a percentile when we multiply the size by the percentile.
  // For example, when the size is 10, I want the 50th percentile to correspond
  // to index 4, which is only possibly when we subtract 1 from 10. When the
  // number of latencies is odd, we do not need to subtract 1. For example, when
  // the number of latencies is 9, 9 * 0.5 = 4 (using integer division), so the
  // 50th percentile corresponds to index 4.
  size_t size =
      durations.size() % 2 == 0 ? durations.size() - 1 : durations.size();
  results.min = durations.front() / divisor;
  results.fifty = durations.at(size * 0.5) / divisor;
  results.ninetynine = durations.at(size * 0.99) / divisor;
  results.ninetyninefive = durations.at(size * 0.995) / divisor;
  results.ninetyninenine = durations.at(size * 0.999) / divisor;
  results.max = durations.back() / divisor;

  if (options.pretty) {
    PrintLinePretty(*options.os, stage, /*dashes=*/false, results);
  } else {
    PrintLineCsv(*options.os, results);
  }

  if (options.distribution) {
    PrintDistribution(*options.os, durations, divisor);
  }
}

// Returns the durations for a certain stage. 'requests' includes all requests.
// A request's latency is only included in the results if 'should_include'
// returns true for that request. The return value of 'difference' for each
// request is included in the results.
//
// Some requests should not be included in the results. These requests were
// generally started by the app but were never processed to completion by the
// app before it stopped. This is why we need 'should_include'.
static std::vector<absl::Duration> GetStageResults(
    const std::vector<Request>& requests,
    const std::function<bool(const Request&)>& should_include,
    const std::function<absl::Duration(const Request&)>& difference) {
  std::vector<absl::Duration> results;
  results.reserve(requests.size());

  for (const Request& r : requests) {
    if (should_include(r)) {
      results.push_back(difference(r));
    }
  }
  std::sort(results.begin(), results.end());
  return results;
}

// Prints the preface to the results if pretty mode is set.
void PrintPrettyPreface(PrintOptions options) {
  CHECK(options.pretty);

  Results<std::string> results;
  std::string unit = options.ns ? "ns" : "us";
  results.total = "Total Requests";
  results.throughput = "Throughput (req/s)";
  results.min = "Min (" + unit + ")";
  results.fifty = "50% (" + unit + ")";
  results.ninetynine = "99% (" + unit + ")";
  results.ninetyninefive = "99.5% (" + unit + ")";
  results.ninetyninenine = "99.9% (" + unit + ")";
  results.max = "Max (" + unit + ")";
  PrintLinePretty(*options.os, std::string("Stage"), /*dashes=*/true, results);
}

// Prints all results.
void Print(const std::vector<Request>& requests, absl::Duration runtime,
           PrintOptions options) {
  CHECK_NE(options.os, nullptr);

  if (options.pretty) {
    PrintPrettyPreface(options);
  }

  if (!options.print_last) {
    HANDLE_STAGE("Ingress Queue Time", requests, runtime, request_generated,
                 request_received, options);
    HANDLE_STAGE("Repeatable Handle Time", requests, runtime, request_received,
                 request_assigned, options);
    HANDLE_STAGE("Worker Queue Time", requests, runtime, request_assigned,
                 request_start, options);
    HANDLE_STAGE("Worker Handle Time", requests, runtime, request_start,
                 request_finished, options);
  }
  // Total time in system
  HANDLE_STAGE("Total", requests, runtime, request_generated, request_finished,
               options);
}

}  // namespace latency

}  // namespace ghost_test
