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

#include "experiments/antagonist/results.h"

#include <iomanip>
#include <iostream>

#include "lib/base.h"

namespace ghost_test {

namespace {
constexpr size_t kWorkerLen = 8;
constexpr size_t kDurationLen = 20;
constexpr size_t kShareLen = 12;
// Add 2 to the end to account for the space between each column in the results.
constexpr size_t kNumDashes = kWorkerLen + kDurationLen + kShareLen + 2;

// Prints the results in human-readable form.
template <class T, class U>
void PrintLinePretty(std::ostream& os, const std::string& worker,
                     T run_duration, U work_share, bool dashes) {
  os << std::left;
  os << std::setw(kWorkerLen) << worker << " ";
  os << std::setw(kDurationLen) << run_duration << " ";
  os << std::setw(kShareLen) << work_share << " ";
  os << std::endl;
  if (dashes) {
    os << std::string(kNumDashes, '-') << std::endl;
  }
}

// Prints the results in CSV form.
template <class T, class U>
void PrintLineCsv(std::ostream& os, const std::string& worker, T run_duration,
                  U work_share) {
  os << std::left;
  os << worker << ",";
  os << run_duration << ",";
  os << work_share;
  os << std::endl;
}

// Prints the preface to the results if pretty mode is set.
void PrintPrettyPreface(PrintOptions options) {
  CHECK(options.pretty);

  PrintLinePretty(*options.os, "Worker", "Run Duration (ns)", "Work Share",
                  /*dashes=*/true);
}

// Adds/averages all results and prints out the summary.
void PrintTotal(const std::vector<absl::Duration>& run_durations,
                absl::Duration runtime, PrintOptions options) {
  absl::Duration run_duration;
  for (const absl::Duration& r : run_durations) {
    run_duration += r;
  }
  const double work_share =
      absl::ToDoubleMilliseconds(run_duration) /
      (run_durations.size() * absl::ToDoubleMilliseconds(runtime));

  if (options.pretty) {
    PrintLinePretty(*options.os, "Total",
                    absl::ToInt64Nanoseconds(run_duration), work_share,
                    /*dashes=*/false);
  } else {
    PrintLineCsv(*options.os, "Total", absl::ToInt64Nanoseconds(run_duration),
                 work_share);
  }
}
}  // namespace

// Prints all results.
void Print(const std::vector<absl::Duration>& run_durations,
           absl::Duration runtime, const PrintOptions& options) {
  CHECK_NE(options.os, nullptr);

  if (options.pretty) {
    PrintPrettyPreface(options);
  }

  for (size_t i = 0; i < run_durations.size(); i++) {
    const double work_share = absl::ToDoubleMilliseconds(run_durations[i]) /
                              absl::ToDoubleMilliseconds(runtime);
    const int64_t run_duration = absl::ToInt64Nanoseconds(run_durations[i]);
    if (options.pretty) {
      PrintLinePretty(*options.os, std::to_string(i), run_duration, work_share,
                      /*dashes=*/false);
    } else {
      PrintLineCsv(*options.os, std::to_string(i), run_duration, work_share);
    }
  }
  PrintTotal(run_durations, runtime, options);
}

}  // namespace ghost_test
