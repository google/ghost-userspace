// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef GHOST_LIB_LOGGING_H_
#define GHOST_LIB_LOGGING_H_

#include <iostream>

#include "absl/log/check.h"
#include "absl/strings/str_format.h"
#include "third_party/util/util.h"

#if defined(GHOST_LOGGING)

#ifndef GHOST_DEBUG
#ifdef NDEBUG
#define GHOST_DEBUG 0
#else
#define GHOST_DEBUG 1
#endif
#endif

#else

#endif  // !GHOST_LOGGING

#define GHOST_DPRINT(level, target, fmt, ...)       \
  do {                                              \
    if (verbose() < level) break;                   \
    absl::FPrintF(target, fmt "\n", ##__VA_ARGS__); \
  } while (0)

#define GHOST_ERROR(...)                                               \
  do {                                                                 \
    std::cerr << __FILE__ << ":" << __LINE__ << "(" << ghost::GetTID() \
              << ") ERROR: ";                                          \
    absl::FPrintF(stderr, __VA_ARGS__);                                \
    absl::FPrintF(stderr, "\n");                                       \
    ghost::Exit(1);                                                    \
  } while (0)

#define GHOST_I_AM_HERE                                                      \
  absl::FPrintF(stderr, "PID %d %s is in %s at line %d in %s\n", getpid(),   \
                std::string(Gtid::Current().describe()), __func__, __LINE__, \
                __FILE__)

#endif  // GHOST_LIB_LOGGING_H_
