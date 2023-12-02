// Copyright 2023 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef GHOST_LIB_GHOST_UAPI_H_
#define GHOST_LIB_GHOST_UAPI_H_

#ifndef GHOST_SELECT_ABI
#include "abi/latest/kernel/ghost.h"
#elif GHOST_SELECT_ABI == 84
#include "abi/84/kernel/ghost.h"
#elif GHOST_SELECT_ABI == 90
#include "abi/90/kernel/ghost.h"
#else
#error "missing an abi?"
#endif

#endif  // GHOST_LIB_GHOST_UAPI_H_
