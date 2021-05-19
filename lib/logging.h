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

#ifndef GHOST_LIB_LOGGING_H_
#define GHOST_LIB_LOGGING_H_

#include <iostream>

#include "absl/strings/str_format.h"

#if defined(GHOST_LOGGING)

#define __LCHECK(op, invop, expr1, expr2)                                      \
  do {                                                                         \
    auto __val1 = expr1;                                                       \
    auto __val2 = expr2;                                                       \
    if (!(__val1 op __val2)) {                                                 \
      int __errno = errno;                                                     \
      std::cerr << __FILE__ << ":" << __LINE__ << "(" << ghost::GetTID()       \
                << ") "                                                        \
                << "CHECK FAILED: " << #expr1 << " " #op " " << #expr2 << " [" \
                << __val1 << " " #invop " " << __val2 << "]" << std::endl;     \
      if (__errno) {                                                           \
        std::cerr << "errno: " << __errno << " [" << std::strerror(__errno)    \
                  << "]" << std::endl;                                         \
      }                                                                        \
      ghost::Exit(1);                                                          \
    }                                                                          \
  } while (0)

#define CHECK_EQ(val1, val2) __LCHECK(==, !=, val1, val2)
#define CHECK_NE(val1, val2) __LCHECK(!=, ==, val1, val2)
#define CHECK_LT(val1, val2) __LCHECK(<, >=, val1, val2)
#define CHECK_LE(val1, val2) __LCHECK(<=, <, val1, val2)
#define CHECK_GT(val1, val2) __LCHECK(>, <=, val1, val2)
#define CHECK_GE(val1, val2) __LCHECK(>=, >, val1, val2)
#define CHECK(val1) CHECK_NE(val1, 0)
#define CHECK_ZERO(val1) CHECK_EQ(val1, 0)

#ifndef NDEBUG
#define DCHECK_EQ(val1, val2) CHECK_EQ(val1, val2)
#define DCHECK_NE(val1, val2) CHECK_NE(val1, val2)
#define DCHECK_LT(val1, val2) CHECK_LT(val1, val2)
#define DCHECK_LE(val1, val2) CHECK_LE(val1, val2)
#define DCHECK_GT(val1, val2) CHECK_GT(val1, val2)
#define DCHECK_GE(val1, val2) CHECK_GE(val1, val2)
#define DCHECK(condition) CHECK(condition)
#define DCHECK_ZERO(val1) CHECK_ZERO(val1)
#else
// `NDEBUG` is defined, so `DCHECK_EQ(x, y)` and so on do nothing.  However, we
// still want the compiler to parse `x` and `y`, because we don't want to lose
// potentially useful errors and warnings.
//
// (cribbed from ABSL_LOGGING_INTERNAL_DCHECK_NOP)
#define GHOST_DCHECK_NOP(x, y) while (false && ((void)(x), (void)(y), 0))

#define DCHECK_EQ(val1, val2) GHOST_DCHECK_NOP(val1, val2)
#define DCHECK_NE(val1, val2) GHOST_DCHECK_NOP(val1, val2)
#define DCHECK_LT(val1, val2) GHOST_DCHECK_NOP(val1, val2)
#define DCHECK_LE(val1, val2) GHOST_DCHECK_NOP(val1, val2)
#define DCHECK_GT(val1, val2) GHOST_DCHECK_NOP(val1, val2)
#define DCHECK_GE(val1, val2) GHOST_DCHECK_NOP(val1, val2)
#define DCHECK(condition) GHOST_DCHECK_NOP(condition, 0)
#define DCHECK_ZERO(val) GHOST_DCHECK_NOP(val, 0)
#endif

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
