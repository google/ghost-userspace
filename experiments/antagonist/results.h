// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

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
