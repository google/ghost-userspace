// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef GHOST_EXPERIMENTS_ROCKSDB_CLOCK_H_
#define GHOST_EXPERIMENTS_ROCKSDB_CLOCK_H_

#include "absl/time/clock.h"
#include "lib/base.h"

// This is a pure virtual parent class that represents a clock.
class Clock {
 public:
  virtual ~Clock() = 0;

  // Returns the current clock time.
  virtual absl::Time TimeNow() const = 0;
};

inline Clock::~Clock() {}

// This represents a real clock that returns the current time from
// `ghost::MonotonicNow()`.
//
// Example:
// RealClock clock;
// absl::Time now = clock.TimeNow();
class RealClock final : public Clock {
 public:
  // Returns the current time (from `ghost::MonotonicNow()`).
  absl::Time TimeNow() const final { return ghost::MonotonicNow(); }
};

// This represents a simulated clock whose time can be arbitrarily changed. This
// is mainly useful for testing code that depends on time, such as the `Ingress`
// class.
//
// Example:
// SimulatedClock clock;
// clock.SetTime(ghost::MonotonicNow());
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
