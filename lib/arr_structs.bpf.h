/*
 * Copyright 2023 Google LLC
 *
 * Use of this source code is governed by a BSD-style
 * license that can be found in the LICENSE file or at
 * https://developers.google.com/open-source/licenses/bsd
 *
 * Helpers for building structures like linked lists where the elements are
 * indexes in an array instead of pointers.
 */

#ifndef GHOST_LIB_ARR_STRUCTS_BPF_H_
#define GHOST_LIB_ARR_STRUCTS_BPF_H_

#ifdef __BPF__
#include "third_party/bpf/common.bpf.h"
#else
#define BOUNDED_ARRAY_IDX(arr, arr_sz, idx) &(arr)[(idx)]
#endif

/* For older gcc, typeof may be undefined. */
#ifndef typeof
#define typeof(x) __typeof__(x)
#endif

/* Helper to prevent the compiler from optimizing bounds check on x. */
#ifndef BPF_MUST_CHECK
#define BPF_MUST_CHECK(x) ({ asm volatile ("" : "+r"(x)); x; })
#endif

/*
 * Lookup the elem for an id.  Returns a pointer to the elem or NULL.
 */
#define __id_to_elem(arr, arr_sz, id) ({				\
	size_t ___id = id;						\
	___id ? BOUNDED_ARRAY_IDX(arr, arr_sz, ___id - 1) : NULL;	\
})

/*
 * Lookup the id for an elem.  elem must be in arr.
 *
 * The manual pointer arithmetic avoids signed division, which is not allowed in
 * BPF.  (The difference of pointers is signed).
 */
#define __elem_to_id(arr, elem)						\
	(((size_t)((unsigned char*)(elem) - (unsigned char*)(arr))	\
	  / sizeof(*elem)) + 1)

#ifndef offsetof
#define offsetof(type, member)  ((size_t) (&((type*)0)->member))
#endif

#ifndef container_of
#define container_of(ptr, type, member) ({				\
	(type*)((char*)ptr - offsetof(type, member));			\
})
#endif


#endif // GHOST_LIB_ARR_STRUCTS_BPF_H_
