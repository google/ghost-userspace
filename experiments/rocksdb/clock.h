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

#ifndef GHOST_EXPERIMENTS_ROCKSDB_CLOCK_H_
#define GHOST_EXPERIMENTS_ROCKSDB_CLOCK_H_

#include "absl/time/clock.h"

// This is a pure virtual parent class that represents a clock.
class Clock {
 public:
  virtual ~Clock() = 0;

  // Returns the current clock time.
  virtual absl::Time TimeNow() const = 0;
};

inline Clock::~Clock() {}

// This represents a real clock that returns the current time from
// `absl::Now()`.
//
// Example:
// RealClock clock;
// absl::Time now = clock.TimeNow();
class RealClock final : public Clock {
 public:
  // Returns the current time (from `absl::Now()`).
  absl::Time TimeNow() const final { return absl::Now(); }
};

// This represents a simulated clock whose time can be arbitrarily changed. This
// is mainly useful for testing code that depends on time, such as the `Ingress`
// class.
//
// Example:
// SimulatedClock clock;
// clock.SetTime(absl::Now());
// clock.AdvanceTime(absl::Minutes(10));
// absl::Time time = clock.TimeNow();
// (`time` is equal to the time about 10 minutes from now.)
class SimulatedClock final : public Clock {
 public:
  absl::Time TimeNow() const { return time_; }

  // Set the clock to `time`.
  void SetTime(absl::Time time) { time_ = time; }

  // Change the time on the clock by `duration`.
  void AdvanceTime(absl::Duration duration) { time_ += duration; }

 private:
  // The current time for this clock.
  absl::Time time_;
};

#endif  // GHOST_EXPERIMENTS_ROCKSDB_CLOCK_H_
