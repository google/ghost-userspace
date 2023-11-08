// Copyright 2023 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "lib/ghost_uapi.h"

// The global symbol in combination with alwayslink=1 ensures that only
// a single instance of the ghost_uapi library is depended on by a cc_binary
// target.
//
// Without this we could have some intermediate libraries or the binary
// itself compile against one ABI while other libraries are compiled
// against a different ABI.
//
// Now if a binary inadvertently takes a dependency on 'ghost_uapi'
// _and_ 'ghost_uapi_75' then the linker will complain as follows:
// ld: error: duplicate symbol: did_you_take_an_unintended_ghost_uapi_dependency

int did_you_take_an_unintended_ghost_uapi_dependency = GHOST_VERSION;
