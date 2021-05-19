/*
 * Copyright 2021 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef GHOST_EXPERIMENTS_ANTAGONIST_RESULTS_H_
#define GHOST_EXPERIMENTS_ANTAGONIST_RESULTS_H_

#include "absl/time/clock.h"

namespace ghost_test {

struct PrintOptions {
  // If true, prints the results in human-readable form. Otherwise, prints the
  // results in CSV form.
  bool pretty;
  // The output stream to send the results to. We make 'os' a pointer rather
  // than a reference since a reference cannot be reassigned.
  //
  // 'os' is owned by whoever instantiated this struct.
  std::ostream* os;
};

// Prints the results for the workers.
void Print(const std::vector<absl::Duration>& run_durations,
           absl::Duration runtime, const PrintOptions& options);

}  // namespace ghost_test

#endif  // GHOST_EXPERIMENTS_ANTAGONIST_RESULTS_H_
