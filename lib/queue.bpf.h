/*
 * Copyright 2022 Google LLC
 *
 * Use of this source code is governed by a BSD-style
 * license that can be found in the LICENSE file or at
 * https://developers.google.com/open-source/licenses/bsd
 *
 * Macros and structs for BSD-style linked lists, a la sys/queue.h, where the
 * objects all belong to the same array.  Instead of pointers, the entries are
 * indexes into the array (offset by 1 so we can zero-initialize structs).
 *
 * These are designed for use in BPF and to be checked by the verifier:
 * - accessors require the array size
 * - iterators require a max number of loops
 *
 * For these to be usable in multithreaded BPF with spinlocks, the full array
 * needs to be a single element in a BPF map.  To do otherwise, you'd need to do
 * a lookup for id->element while holding a lock, and you can't call
 * bpf_map_lookup_elem() while holding a spinlock.
 *
 * The arr_list is what BSD would call a TAILQ: a doubly-linked list where you
 * can add elements to the head or the tail.  Usage is the same as TAILQ, which
 * you can read via 'man queue'.
 *
 * If we're worried about space, we could add an arr_hlist too (singly-linked
 * list, append only to the head).
 */

#ifndef GHOST_LIB_QUEUE_BPF_H_
#define GHOST_LIB_QUEUE_BPF_H_

/* For older gcc, typeof may be undefined. */
#ifndef typeof
#define typeof(x) __typeof__(x)
#endif

/* Helper to prevent the compiler from optimizing bounds check on x. */
#ifndef BPF_MUST_CHECK
#define BPF_MUST_CHECK(x) ({ asm volatile ("" : "+r"(x)); x; })
#endif

struct arr_list {
	unsigned int first;
	unsigned int last;
};

struct arr_list_entry {
	unsigned int next;
	unsigned int prev;
};

/* Zero initialization: 0 == no item, o/w idx = value - 1. */
#define arr_list_init(head) ({						\
	(head)->first = 0;						\
	(head)->last = 0;						\
})

#define ARR_LIST_HEAD_INITIALIZER {0}

/*
 * Lookup the elem for an id.  Returns a pointer to the elem or NULL.
 *
 * Need to prevent the compiler from optimizing the bounds check on __idx so
 * that the verifier knows it was checked.
 */
#define __id_to_elem(arr, arr_sz, id) ({				\
	typeof(&arr[0]) ret = NULL;					\
	if (id) {							\
		unsigned int __idx = id - 1;				\
		if (BPF_MUST_CHECK(__idx) < (arr_sz))			\
			ret = &(arr)[__idx];				\
	}								\
	ret;								\
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

#define arr_list_next(arr, arr_sz, elem, field)				\
	__id_to_elem(arr, arr_sz, (elem)->field.next)

#define arr_list_prev(arr, arr_sz, elem, field)				\
	__id_to_elem(arr, arr_sz, (elem)->field.prev)

#define arr_list_first(arr, arr_sz, head)				\
	__id_to_elem(arr, arr_sz, (head)->first)

#define arr_list_last(arr, arr_sz, head)				\
	__id_to_elem(arr, arr_sz, (head)->last)

#define arr_list_empty(head)				\
	((head)->first == 0)

#define arr_list_insert_head(arr, arr_sz, head, elem, field) ({		\
	unsigned int id = __elem_to_id(arr, elem);			\
	(elem)->field.prev = 0;						\
	(elem)->field.next = (head)->first;				\
	if (arr_list_empty(head)) {					\
		(head)->last = id; 					\
	} else {							\
		typeof(elem) first = arr_list_first(arr, arr_sz, head);	\
		if (first)						\
			first->field.prev = id;				\
	}								\
	(head)->first = id;						\
})

#define arr_list_insert_tail(arr, arr_sz, head, elem, field) ({		\
	unsigned int id = __elem_to_id(arr, elem);			\
	(elem)->field.prev = (head)->last;				\
	(elem)->field.next = 0;						\
	if (arr_list_empty(head)) {					\
		(head)->first = id; 					\
	} else {							\
		typeof(elem) last = arr_list_last(arr, arr_sz, head);	\
		if (last)						\
			last->field.next = id;				\
	}								\
	(head)->last = id;						\
})

#define arr_list_insert_before(arr, arr_sz, head, list_elem, elem, field) ({ \
	unsigned int id = __elem_to_id(arr, elem);			\
	unsigned int lid = __elem_to_id(arr, list_elem);		\
	typeof(elem) prev = arr_list_prev(arr, arr_sz, list_elem, field); \
	if (prev)							\
		prev->field.next = id;					\
	(elem)->field.prev = (list_elem)->field.prev;			\
	(elem)->field.next = lid;					\
	(list_elem)->field.prev = id;					\
	if ((head)->first == lid)					\
		(head)->first = id;					\
})

#define arr_list_insert_after(arr, arr_sz, head, list_elem, elem, field) ({ \
	unsigned int id = __elem_to_id(arr, elem);			\
	unsigned int lid = __elem_to_id(arr, list_elem);		\
	typeof(elem) next = arr_list_next(arr, arr_sz, list_elem, field); \
	if (next)							\
		next->field.prev = id;					\
	(elem)->field.prev = lid;					\
	(elem)->field.next = (list_elem)->field.next;			\
	(list_elem)->field.next = id;					\
	if ((head)->last == lid)					\
		(head)->last = id;					\
})

#define arr_list_remove(arr, arr_sz, head, elem, field) ({		\
	unsigned int id = __elem_to_id(arr, elem);			\
	typeof(elem) prev = arr_list_prev(arr, arr_sz, elem, field);	\
	if (prev)							\
		prev->field.next = (elem)->field.next;			\
	typeof(elem) next = arr_list_next(arr, arr_sz, elem, field);	\
	if (next)							\
		next->field.prev = (elem)->field.prev;			\
	if ((head)->first == id)					\
		(head)->first = (elem)->field.next;			\
	if ((head)->last == id)						\
		(head)->last = (elem)->field.prev;			\
	(elem)->field.prev = 0;						\
	(elem)->field.next = 0;						\
})

#define arr_list_pop_first(arr, arr_sz, head, field) ({			\
	typeof(&arr[0]) first = arr_list_first(arr, arr_sz, head);	\
	unsigned int id = (head)->first;				\
	if (first) {							\
		typeof(&arr[0]) next = arr_list_next(arr, arr_sz, first, field);\
		if (next)						\
			next->field.prev = 0;				\
		(head)->first = (first)->field.next;			\
		if ((head)->last == id)					\
			(head)->last = 0;				\
		first->field.next = 0;					\
	}								\
	first;								\
})

/*
 * Foreach iterators, bounded by 'max' loops to ensure the loop terminates.
 * var and _i must be declared outside the loop.  If 'var' != NULL after the
 * loop, you were stopped short.
 */
#define arr_list_foreach(var, arr, arr_sz, head, field, _i, max)	\
	for ((var) = arr_list_first(arr, arr_sz, head), (_i) = 0;	\
	     BPF_MUST_CHECK(var) && _i < (max);				\
	     (var) = arr_list_next(arr, arr_sz, (var), field), _i++)

/*
 * Note: in the assignment to (tvar), the ", 1" is so that the loop check
 * evaluates to true even when tvar == NULL.  When tvar is NULL, we bail out
 * *next* time.
 */
#define arr_list_foreach_safe(var, arr, arr_sz, head, field, tvar, _i, max) \
	for ((var) = arr_list_first(arr, arr_sz, head), (_i) = 0;	\
	     BPF_MUST_CHECK(var) && _i < (max) &&			\
	     	((tvar) = arr_list_next(arr, arr_sz, (var), field), 1);	\
	     (var) = (tvar), _i++)

#define arr_list_foreach_reverse(var, arr, arr_sz, head, field, _i, max) \
	for ((var) = arr_list_last(arr, arr_sz, head), (_i) = 0;	\
	     BPF_MUST_CHECK(var) && _i < (max);				\
	     (var) = arr_list_prev(arr, arr_sz, (var), field), _i++)

#define arr_list_foreach_reverse_safe(var, arr, arr_sz, head, field, tvar, \
				      _i, max)				\
	for ((var) = arr_list_last(arr, arr_sz, head), (_i) = 0;	\
	     BPF_MUST_CHECK(var) && _i < (max) &&			\
		((tvar) = arr_list_prev(arr, arr_sz, (var), field), 1);	\
	     (var) = (tvar), _i++)

#define arr_list_concat(arr, arr_sz, head1, head2, field) ({		\
	typeof(&arr[0]) h1_l = arr_list_last(arr, arr_sz, head1);	\
	if (h1_l)							\
		h1_l->field.next = (head2)->first;			\
	typeof(&arr[0]) h2_f = arr_list_first(arr, arr_sz, head2);	\
	if (h2_f)							\
		h2_f->field.prev = (head1)->last;			\
	if (arr_list_empty(head1))					\
		(head1)->first = (head2)->first;			\
	(head1)->last = (head2)->last;					\
	arr_list_init(head2);						\
})

#endif // GHOST_LIB_QUEUE_BPF_H_
