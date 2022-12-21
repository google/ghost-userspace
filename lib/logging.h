// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef GHOST_LIB_LOGGING_H_
#define GHOST_LIB_LOGGING_H_

#include <iostream>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/str_format.h"
#include "third_party/util/util.h"

#ifndef GHOST_DEBUG
#ifdef NDEBUG
#define GHOST_DEBUG 0
#else
#define GHOST_DEBUG 1
#endif  // !NDEBUG
#endif  // !GHOST_DEBUG

#ifndef VLOG
#ifdef NDEBUG
#define VLOG(level) LOG_IF(INFO, false)
#else
#define VLOG(level) LOG_IF(INFO, verbose() < level)
#endif  // !NDEBUG
#endif  // !VLOG

// TODO: Consider deprecating GHOST_DPRINT once we migrate to VLOG.
#define GHOST_DPRINT(level, target, fmt, ...)       \
  do {                                              \
    if (verbose() < level) break;                   \
    absl::FPrintF(target, fmt "\n", ##__VA_ARGS__); \
  } while (0)

#define GHOST_ERROR(fmt, ...)                          \
  do {                                                 \
    LOG(FATAL) << "(" << ghost::GetTID() << ") "       \
               << absl::StrFormat(fmt, ##__VA_ARGS__); \
  } while (0)

#define GHOST_I_AM_HERE                                                   \
  do {                                                                    \
    LOG(INFO) << "GHOST_I_AM_HERE: PID " << getpid() << " "               \
              << ghost::Gtid::Current().describe() << " at " << __func__; \
  } while (0)

#endif  // GHOST_LIB_LOGGING_H_
