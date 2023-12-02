/*
 * Copyright 2023 Google LLC
 *
 * Use of this source code is governed by a BSD-style
 * license that can be found in the LICENSE file or at
 * https://developers.google.com/open-source/licenses/bsd
 *
 * AVL trees in BPF where the objects all belong to the same array.  Instead of
 * pointers, the entries are indexes into the array (offset by 1 so we can
 * zero-initialize structs).
 *
 * If you're familiar with the linked lists in queue.bpf.h, these are a bit
 * different.  To avoid macro-hell, the functions are in C, but they still take
 * the array, array size, etc.  However, the array (arr) is a void *, and you
 * have to also pass the "el_off": a u64 holding the element size and the field
 * offset within the element.  e.g.  el_off(sizeof(struct flux_thread),
 * offsetof(struct flux_thread, foo.avl))
 *
 * It's a bit clunky, but writing the AVL functions in macros is a nightmare.
 * As a reminder, which I need every couple of years, arguments passed to macros
 * ignore scoping.  So if you have a local variable 'x' in your macro, and pass
 * "x->foo.bar", that x gets pasted into your macro functions.  Given the nature
 * of these AVL functions, I need lots of arguments, and many of these functions
 * call one another.  And don't forget that your macro cannot use an argument
 * more than once, otherwise you evaluate your argument (which could be a
 * function) more than once.
 *
 * Your main API is the avl_tree_ functions:
 *
	avl_tree_lookup(arr, arr_sz, el_off, tree, key)
	avl_tree_insert(arr, arr_sz, el_off, tree, node)
	avl_tree_delete(arr, arr_sz, el_off, tree, node)

	avl_tree_min(arr, arr_sz, el_off, tree))
	avl_tree_max(arr, arr_sz, el_off, tree))

	avl_tree_pop_min(arr, arr_sz, el_off, tree))
	avl_tree_pop_max(arr, arr_sz, el_off, tree))
 *
 * I recommend wrapping those in helper macros for your scheduler, e.g. for
 * Biff, with a struct avl_tree 'tree' in struct biff_flux_sched and
 * struct avl_node 'avl' in biff_flux_thread.  Note these assume the existence
 * of the __t_arr variable in scope.  I use that technique heavily in this file
 * too.
 *

#define biff_el_off() el_off(sizeof(struct flux_thread), \
			     offsetof(struct flux_thread, biff.avl))
#define biff_avl_node_to_elem(node) \
	avl_node_to_elem(__t_arr, node, biff.avl)

#define biff_avl_tree_lookup(tree, key)					\
	biff_avl_node_to_elem(avl_tree_lookup(__t_arr, FLUX_MAX_GTIDS,	\
					      biff_el_off(), tree, key))
#define biff_avl_tree_min(tree)						\
	biff_avl_node_to_elem(avl_tree_min(__t_arr, FLUX_MAX_GTIDS,	\
					   biff_el_off(), tree))
#define biff_avl_tree_pop_min(tree)					\
	biff_avl_node_to_elem(avl_tree_pop_min(__t_arr, FLUX_MAX_GTIDS,	\
					   biff_el_off(), tree))
#define biff_avl_tree_insert(tree, thr)					\
	avl_tree_insert(__t_arr, FLUX_MAX_GTIDS, biff_el_off(), tree,	\
			&(thr)->biff.avl)
#define biff_avl_tree_delete(tree, thr)					\
	avl_tree_delete(__t_arr, FLUX_MAX_GTIDS, biff_el_off(), tree,	\
			&(thr)->biff.avl)


 * All members of a tree share the same offset: biff.avl.  You can belong to
 * multiple trees/lists at a time, but each "arr_struct" uses the same field for
 * all of its members.  i.e. you'll need biff.avl2 for another tree, and you'll
 * want separate macros for each tree (biff_el_off2, biff.avl2, etc.).
 *
 *
 * Apart from the "all pointers are indexes in the array" and the clunkiness,
 * the primary aspect of these functions is that they (should be) proven to the
 * verifier to terminate.  To do so, I limited the maximum height of the tree
 * (MAX_AVL_HEIGHT).  This means it is possible for us to fail to insert a node
 * into the tree.  When this happens, they get stuffed into an orphan list.
 * There are a few consequences of this:
 * - lookup will not find orphans.  We cannot endlessly traverse a linked list
 *   in BPF to search for your key.
 * - min/max may be inaccurate.  Your min could be in the orphan list.  If you
 *   want to be sure you have the min, for whatever reason, you could peak at
 *   tree->first_orphan_id (0 means no orphans).
 * - it is possible, but (probably) unlikely to starve an orphan.  When we
 *   delete an item, we try to stuff an orphan into the tree.  However,
 *   depending on the balance, some orphans may have a hard time getting into
 *   the tree.  e.g. consider a tree where the right half is full and the left
 *   side has some gaps.  An orphan with a large value won't get in, but a
 *   smaller value that finds the left side gaps will get in.  So long as you
 *   actively are pulling items out of the tree (e.g. pop_min or pop_max),
 *   you'll eventually start getting orphans.  But no guarantee.
 *
 * The important idea is that if a node gets into the tree, we expect to be
 * able to delete you from the tree.  The idea is that if we can walk down the
 * tree to stick you in, we can walk down to yank you out.  (Inserts always
 * happen at the leaves).  Rotations to rebalance the tree never increase the
 * overall height of the tree.  See the comment about the "-1" in the
 * MAX_AVL_HEIGHT check in avl_tree_insert().  Because of the "if you can make
 * it in, you're set" policy, we always have the AVL tree balanced and do not
 * need to worry about any rotation retraces failing or anything like that.
 * (Dealing with a partial retrace failure would be a nightmare - your tree
 * would be unbalanced *and* your accounting would be wrong!)
 *
 * You may be wondering, "why an AVL tree instead of the RB tree everyone uses?"
 * a few reasons:
 * - AVL is simpler.
 * - AVL rebalances after every insert and delete.  The tree doesn't get too far
 *   out of whack, (balance is always [-1,1]), and it's easier to convince
 *   myself that the only operation that will ever fail due to running out of
 *   MAX_AVL_HEIGHT loops is insertion.  That's probably true for RB too, but
 *   the proactive insertion also means that if I'm near the max height, we'll
 *   rebalance quickly so future inserts work.  With RB, it might be easier to
 *   starve an orphan, where some tasks can be inserted, but others cannot.
 *   (This can happen with AVL too.)
 * - Allegedly the AVL tree height is ~1.4*log(n), and RB is 2*log(n).  Yeah,
 *   that's not a lot - just another loop around the tree - but it's a concern
 *   of mine.  Note that in ghost, you typically have at most 64k threads, and
 *   1.4 * log(64k) ~= 23, which is the MAX_AVL_HEIGHT.  Complicated schedulers
 *   might have trouble with a height of 23, so if we need to, you can #define
 *   it smaller.  Good luck.
 */

#ifndef GHOST_LIB_AVL_BPF_H_
#define GHOST_LIB_AVL_BPF_H_

#ifndef __BPF__
#include <stddef.h>
#include <stdint.h>
#endif

#include "lib/arr_structs.bpf.h"
#include "lib/queue.bpf.h"

#pragma GCC diagnostic ignored "-Wunused-function"

#ifndef MAX_AVL_HEIGHT
#define MAX_AVL_HEIGHT 23
#endif

#ifndef AVL_ASSERT
#define AVL_ASSERT(x)
#endif

#ifndef AVL_PRINTD
#define AVL_PRINTD(args...) {}
#endif

/*
 * You can turn off the cache if you want.  Just #define AVL_DISABLE_CACHING.
 * The PopMin and PopMax tests (not benchmarks) didn't show a benefit.  Perhaps
 * this is a case of premature optimization, but better to add it now when the
 * code is fresh.  (See the alert on Extreme Subtlety).
 */
#ifndef AVL_DISABLE_CACHING
#define AVL_CACHING 1
#endif

struct avl_tree {
	unsigned int root;
#ifdef AVL_CACHING
	unsigned int min_id;
	uint64_t min_key;	/* only applies if min_id != 0 */
	unsigned int max_id;
	uint64_t max_key;	/* only applies if max_id != 0 */
#endif
	unsigned int first_orphan_id;
};

static inline __attribute__((always_inline))
void avl_tree_init(struct avl_tree *t)
{
	t->root = t->first_orphan_id = 0;
#ifdef AVL_CACHING
	t->min_id = t->min_key = t->max_id = t->max_key = 0;
#endif
}

#define AVL_TREE_INITIALIZER {0}

struct avl_node {
	uint64_t key;
	unsigned int left_id;
	unsigned int right_id;
	unsigned int parent_id;
	int balance;
	bool is_orphan;
	unsigned int prev_orphan_id;
	unsigned int next_orphan_id;
};

/*
 * BPF functions can only take 5 arguments.  Passing the arr, arr_sz, elem_sz,
 * and field_off is already 4.  el_off() combines elem_sz and field_off.
 *
 * Don't muck with arr or arr_sz; we want the verifier to easily tell that we do
 * our bounds checks.
 *
 * elem_sz (sizeof some struct) and field_off (offsetof some field) both must be
 * 32 bits or less, which is always the case for our usage.
 */
static inline __attribute__((always_inline))
uint64_t el_off(size_t elem_sz, size_t field_off)
{
	return (elem_sz << 32) | field_off;
}

static inline __attribute__((always_inline))
size_t __el_off_elem(uint64_t el_off)
{
	return el_off >> 32;
}

static inline __attribute__((always_inline))
size_t __el_off_field(uint64_t el_off)
{
	return el_off & ((1ULL << 32) - 1);
}

static inline __attribute__((always_inline))
struct avl_node *__avl_id_to_node(void *arr, size_t arr_sz,
				  uint64_t el_off, unsigned int id)
{
	size_t p;
	size_t elem_sz = __el_off_elem(el_off);
	size_t field_off = __el_off_field(el_off);

	if (id == 0)
		return NULL;
	p = (id - 1) * elem_sz + field_off;
	/*
	 * Regarding the (arr_sz - 1), we're doing manual pointer arithmetic and
	 * will cast our result back to node/elem.  We need to make clear to the
	 * verifier that we're far enough back to have room for the object.
	 *
	 * Forget about field_off for a second.  If we were trying to get a
	 * pointer to the elem, the check "p >= arr_sz * elem_sz" would be
	 * wrong.  As far as the verifier knows, p could be one byte from the
	 * end of the array.  The verifier doesn't know that p is a multiple of
	 * elem_sz (+ field_off in our case).
	 *
	 * p can legitimately be (arr_sz-1) * e + f, when we're looking up the
	 * last node in the array.
	 */
	return (struct avl_node*)
		BOUNDED_ARRAY_IDX((uint8_t*)arr,
				  (arr_sz - 1) * elem_sz + field_off,
				  p);
}

static inline __attribute__((always_inline))
unsigned int __avl_node_to_id(void *arr, uint64_t el_off,
			      const struct avl_node *node)
{
	size_t elem_sz = __el_off_elem(el_off);
	size_t field_off = __el_off_field(el_off);

	if (!node)
		return 0;
	return ((size_t)(((uint8_t*)node - field_off) - (uint8_t*)arr) /
		elem_sz) + 1;
}

/*
 * Helper for our callers, who will need probably wrap this in their own macros.
 * It's macros all the way down!
 *
 * Note for flux schedulers, this returns the flux_thread pointer, and the field
 * is something like "biff.avl", where struct avl_node avl is embedded in struct
 * biff_flux_thread.
 */
#define avl_node_to_elem(arr, node, field) ({				\
	typeof(arr[0]) *___ret = NULL;					\
	struct avl_node *___node = (node);				\
	if (___node)							\
		___ret = container_of(___node, typeof(arr[0]), field);	\
	___ret;								\
})

/*
 * Passing arr, arr_sz, el_off, etc. to every function gets to be a real chore
 * and make the code harder to read.  These convenience macros assume those
 * arguments are already in scope by the caller.
 */
#define avl_id_to_node(id) \
	__avl_id_to_node(arr, arr_sz, el_off, id)
#define avl_node_to_id(node) \
	__avl_node_to_id(arr, el_off, node)

#define avl_next_node(x) \
	__avl_next_node(arr, arr_sz, el_off, x)
#define avl_prev_node(x) \
	__avl_prev_node(arr, arr_sz, el_off, x)

#define avl_rotate_left(x, z) \
	__avl_rotate_left(arr, arr_sz, el_off, x, z)
#define avl_rotate_right(x, z) \
	__avl_rotate_right(arr, arr_sz, el_off, x, z)
#define avl_rotate_right_left(x, z) \
	__avl_rotate_right_left(arr, arr_sz, el_off, x, z)
#define avl_rotate_left_right(x, z) \
	__avl_rotate_left_right(arr, arr_sz, el_off, x, z)

/*
 * The orphans are a doubly linked list, append only to the front.  We can't use
 * the arr_list stuff since those macros expect the "arr" to be an array of real
 * elements, not the void pointer and el_off business.
 *
 * Instead, we have just the list operations we need: insert, remove, pop_first.
 */
static inline __attribute__((always_inline))
void __insert_orphan(void *arr, size_t arr_sz, uint64_t el_off,
		     struct avl_tree *t, struct avl_node *node)
{
	struct avl_node *next;
	unsigned int next_id, node_id;

	node_id = avl_node_to_id(node);

	next_id = t->first_orphan_id;
	next = avl_id_to_node(next_id);

	t->first_orphan_id = node_id;
	if (next)
		next->prev_orphan_id = node_id;

	node->next_orphan_id = next_id;
	node->prev_orphan_id = 0;
}

static inline __attribute__((always_inline))
void __remove_orphan(void *arr, size_t arr_sz, uint64_t el_off,
		     struct avl_tree *t, struct avl_node *node)
{
	struct avl_node *prev, *next;
	unsigned int prev_id, next_id, node_id;

	node_id = avl_node_to_id(node);

	prev_id = node->prev_orphan_id;
	prev = avl_id_to_node(prev_id);

	next_id = node->next_orphan_id;
	next = avl_id_to_node(next_id);

	if (t->first_orphan_id == node_id)
		t->first_orphan_id = next_id;

	if (prev)
		prev->next_orphan_id = next_id;
	if (next)
		next->prev_orphan_id = prev_id;

	node->next_orphan_id = 0;
	node->prev_orphan_id = 0;
}

static inline __attribute__((always_inline))
struct avl_node *__pop_first_orphan(void *arr, size_t arr_sz, uint64_t el_off,
				    struct avl_tree *t)
{
	struct avl_node *first, *next;
	unsigned int next_id;

	first = avl_id_to_node(t->first_orphan_id);
	if (!first)
		return NULL;

	next_id = first->next_orphan_id;
	next = avl_id_to_node(next_id);

	t->first_orphan_id = next_id;
	if (next)
		next->prev_orphan_id = 0;

	first->next_orphan_id = 0;
	first->prev_orphan_id = 0;

	return first;
}

static inline __attribute__((always_inline))
struct avl_node *avl_tree_lookup(void *arr, size_t arr_sz, uint64_t el_off,
				 struct avl_tree *t, uint64_t key)
{
	struct avl_node *x;

	x = avl_id_to_node(t->root);

	for (int i = 0; i < MAX_AVL_HEIGHT; i++) {
		if (!BPF_MUST_CHECK(x))
			return NULL;
		if (x->key == key)
			return x;
		if (key < x->key)
			x = avl_id_to_node(x->left_id);
		else
			x = avl_id_to_node(x->right_id);
	}
	return NULL;
}

static inline __attribute__((always_inline))
struct avl_node *avl_min_node(void *arr, size_t arr_sz, uint64_t el_off,
			      struct avl_node *x)
{
	for (int i = 0; i < MAX_AVL_HEIGHT; i++) {
		if (!BPF_MUST_CHECK(x))
			return NULL;
		if (x->left_id == 0)
			return x;
		x = avl_id_to_node(x->left_id);
	}
	return NULL;
}

static inline __attribute__((always_inline))
struct avl_node *avl_min_node_id(void *arr, size_t arr_sz, uint64_t el_off,
				 unsigned int id)
{
	return avl_min_node(arr, arr_sz, el_off, avl_id_to_node(id));
}

static inline __attribute__((always_inline))
struct avl_node *avl_tree_min(void *arr, size_t arr_sz, uint64_t el_off,
			      struct avl_tree *t)
{
#ifdef AVL_CACHING
	return avl_id_to_node(t->min_id);
#else
	return avl_min_node_id(arr, arr_sz, el_off, t->root);
#endif
}

static inline __attribute__((always_inline))
struct avl_node *avl_max_node(void *arr, size_t arr_sz, uint64_t el_off,
			      struct avl_node *x)
{
	for (int i = 0; i < MAX_AVL_HEIGHT; i++) {
		if (!BPF_MUST_CHECK(x))
			return NULL;
		if (x->right_id == 0)
			return x;
		x = avl_id_to_node(x->right_id);
	}
	return NULL;
}

static inline __attribute__((always_inline))
struct avl_node *avl_max_node_id(void *arr, size_t arr_sz, uint64_t el_off,
				 unsigned int id)
{
	return avl_max_node(arr, arr_sz, el_off, avl_id_to_node(id));
}

static inline __attribute__((always_inline))
struct avl_node *avl_tree_max(void *arr, size_t arr_sz, uint64_t el_off,
			      struct avl_tree *t)
{
#ifdef AVL_CACHING
	return avl_id_to_node(t->max_id);
#else
	return avl_max_node_id(arr, arr_sz, el_off, t->root);
#endif
}

static inline __attribute__((always_inline))
struct avl_node *__avl_next_node(void *arr, size_t arr_sz, uint64_t el_off,
				 struct avl_node *x)
{
	struct avl_node *parent;
	unsigned int x_id;

	if (!BPF_MUST_CHECK(x))
		return NULL;
	if (x->right_id)
		return avl_min_node_id(arr, arr_sz, el_off, x->right_id);
	x_id = avl_node_to_id(x);
	for (int i = 0; i < MAX_AVL_HEIGHT; i++) {
		parent = avl_id_to_node(x->parent_id);
		if (!BPF_MUST_CHECK(parent))
			return NULL;
		if (x_id == parent->left_id)
			return parent;
		x_id = x->parent_id;
		x = parent;
	}
	return NULL;
}

static inline __attribute__((always_inline))
struct avl_node *__avl_prev_node(void *arr, size_t arr_sz, uint64_t el_off,
				 struct avl_node *x)
{
	struct avl_node *parent;
	unsigned int x_id;

	if (!BPF_MUST_CHECK(x))
		return NULL;
	if (x->left_id)
		return avl_max_node_id(arr, arr_sz, el_off, x->left_id);
	x_id = avl_node_to_id(x);
	for (int i = 0; i < MAX_AVL_HEIGHT; i++) {
		parent = avl_id_to_node(x->parent_id);
		if (!BPF_MUST_CHECK(parent))
			return NULL;
		if (x_id == parent->right_id)
			return parent;
		x_id = x->parent_id;
		x = parent;
	}
	return NULL;
}

/*
 * Perform a left rotation of the subtree rooted at X.
 * Returns the root of the rotated tree.  Caller needs to manage the connection
 * between X's original parent and the new root.
 *
 * X is the parent of Z, Z is the right child of X, Z is right heavy (or
 * balanced), and Z is +2 deeper than the left child of X (x->balance == +2).
 *
 * 0          _X___                                 ___Z_                       
 * 1         /     Z             ===>              X     \                      
 * ...      /     / \                             / \     \                     
 * h+0     t1    /   \                           /   \     \                    
 * h+1          t23   \                        t1    t23    t4                  
 * h+2           *     t4                             *                         
 *
 * Each line is one level/height of the tree.  The subtrees t1, t23, and t4 are
 * the children of X and Z, but they can consist of many nodes, and their height
 * is the lowest node of their subtree.  e.g. the first '/' to the left of X is
 * the node at the root of t1, and t1's lowest node is at h+0.  'h' can be
 * arbitrarily deep, but if you count each / as a level in this picture, h=3.
 *
 * Their ultimate heights are normally within one of each other (the AVL
 * balance), but they are off by +2, and in need of rotation.
 *
 * Note that t23 could also be at the same height as t4 (z->balance == 0), which
 * would occur if t1 had a deletion that decreased its height.  That's the '*'
 * node above.
 *
 * Why "t23"?  Because in other rotations there will be t2 and t3, both
 * left descendants of Z.
 *
 * Importantly, the rotation has no effect on the "horizontal order" of the
 * nodes and trees, which is the sorted, in-order traversal of the keys in each
 * node.  the order both before and after the rotation is: t1, x, t2e, z, t4.
 */
static inline __attribute__((always_inline))
struct avl_node *__avl_rotate_left(void *arr, size_t arr_sz, uint64_t el_off,
				   struct avl_node *x, struct avl_node *z)
{
	unsigned int t23_id, x_id, z_id;
	struct avl_node *t23;

	x_id = avl_node_to_id(x);
	z_id = avl_node_to_id(z);

	AVL_PRINTD("rotate left, x_id %d, z_id %d, z bal %d\n", x_id, z_id,
		   z->balance);

	t23_id = z->left_id;
	t23 = avl_id_to_node(t23_id);

	x->right_id = t23_id;
	if (t23)
		t23->parent_id = x_id;
	z->left_id = x_id;
	x->parent_id = z_id;

	if (z->balance == 0) {
		/* the '*' case, where t23 was the same height as t4 */
		x->balance = 1;
		z->balance = -1;
	} else {
		x->balance = 0;
		z->balance = 0;
	}
	return z;
}

/*
 * Perform a right rotation of the subtree rooted at X.
 * Returns the root of the rotated tree.  Caller needs to manage the connection
 * between X's original parent and the new root.
 *
 * X is the parent of Z, Z is the left child of X, Z is left heavy (or
 * balanced), and Z is +2 deeper than the right child of X (x->balance == -2).
 *
 * 0          ___X_                              _Z___                        
 * 1         Z     \             ===>           /     X                       
 * ...      / \     \                          /     / \                      
 * h+0     /   \    t4                        /     /   \                     
 * h+1    /   t23                           t1     t23   t4                   
 * h+2   t1    *                                    *                         
 */
static inline __attribute__((always_inline))
struct avl_node *__avl_rotate_right(void *arr, size_t arr_sz, uint64_t el_off,
				    struct avl_node *x, struct avl_node *z)
{
	unsigned int t23_id, x_id, z_id;
	struct avl_node *t23;

	x_id = avl_node_to_id(x);
	z_id = avl_node_to_id(z);

	AVL_PRINTD("rotate right, x_id %d, z_id %d, z bal %d\n", x_id, z_id,
		   z->balance);

	t23_id = z->right_id;
	t23 = avl_id_to_node(t23_id);

	x->left_id = t23_id;
	if (t23)
		t23->parent_id = x_id;
	z->right_id = x_id;
	x->parent_id = z_id;

	if (z->balance == 0) {
		/* the '*' case, where t23 was the same height as t4 */
		x->balance = -1;
		z->balance = 1;
	} else {
		x->balance = 0;
		z->balance = 0;
	}
	return z;
}

/*
 * Perform a right-left rotation of the subtree rooted at X.
 * Returns the root of the rotated tree.  Caller needs to manage the connection
 * between X's original parent and the new root.
 *
 * X is the parent of Z, Z is the right child of X, Z is left heavy, and Z is +2
 * deeper than the right child of X (x->balance == +2).
 *
 * 0        _X______    ===>    _X___        ===>      ____Y____             
 * 1       /      __Z          /     Y___             X         Z            
 * 2      /      Y   \        /     /    Z           / \       / \           
 * ...   /      / \   \      /     /    / \         /   \     /   \          
 * h+0  t1     /   \   \    t1    t2   /   \       /    t2   t3    \         
 * h+1        t2   t3  t4          *  t3    \     t1     *    *    t4        
 * h+2         *    *                  *    t4                               
 *
 * At least one of t2 or t3 is at h+2 (the star).  If not Z is balanced (not
 * left heavy) and we'd do a simple left rotation instead.
 *
 * The first rotation is "Z-Y right", then "X-Y left".  For the code, we don't
 * have to do the intermediate rotation and just jump right to the final state.
 */
static inline __attribute__((always_inline))
struct avl_node *__avl_rotate_right_left(void *arr, size_t arr_sz,
					 uint64_t el_off, struct avl_node *x,
					 struct avl_node *z)
{
	unsigned int t2_id, t3_id, x_id, y_id, z_id;
	struct avl_node *t2, *t3, *y;

	x_id = avl_node_to_id(x);
	z_id = avl_node_to_id(z);

	y_id = z->left_id;
	y = avl_id_to_node(y_id);
	if (!y) {
		/* Z is left heavy so Y must exist.  Need to convince BPF. */
		return x;
	}

	AVL_PRINTD("rotate right left, x_id %d, z_id %d, y_id %d, y bal %d\n",
		   x_id, z_id, y_id, y->balance);

	t2_id = y->left_id;
	t3_id = y->right_id;
	t2 = avl_id_to_node(t2_id);
	t3 = avl_id_to_node(t3_id);

	y->left_id = x_id;
	x->parent_id = y_id;
	x->right_id = t2_id;
	if (t2)
		t2->parent_id = x_id;

	y->right_id = z_id;
	z->parent_id = y_id;
	z->left_id = t3_id;
	if (t3)
		t3->parent_id = z_id;

	if (y->balance == 0) {
		/* t2 and t3 both have star nodes */
		x->balance = 0;
		y->balance = 0;
		z->balance = 0;
	} else if (y->balance > 0) {
		/* t3 only has a star node */
		x->balance = -1;
		y->balance = 0;
		z->balance = 0;
	} else {
		/* t2 only has a star node */
		x->balance = 0;
		y->balance = 0;
		z->balance = 1;
	}

	return y;
}

/*
 * Perform a left-right rotation of the subtree rooted at X.
 * Returns the root of the rotated tree.  Caller needs to manage the connection
 * between X's original parent and the new root.
 *
 * X is the parent of Z, Z is the left child of X, Z is right heavy, and Z is +2
 * deeper than the left child of X (x->balance == -2).
 *
 * 0        ______X_    ===>       ___X_     ===>      ____Y____             
 * 1       Z__      \          ___Y     \             Z         X            
 * 2      /   Y      \        Z    \     \           / \       / \           
 * ...   /   / \      \      / \    \     \         /   \     /   \          
 * h+0  /   /   \     t4    /   \   t3    t4       /    t2   t3    \         
 * h+1 t1  t2   t3         /    t2   *            t1     *    *    t4        
 * h+2      *    *        t1     *                                           
 *
 * At least one of t2 or t3 is at h+2 (the star).  If not Z is balanced (not
 * right heavy) and we'd do a simple right rotation instead.
 *
 * The first rotation is "Z-Y eft", then "X-Y right".  For the code, we don't
 * have to do the intermediate rotation and just jump right to the final state.
 */
static inline __attribute__((always_inline))
struct avl_node *__avl_rotate_left_right(void *arr, size_t arr_sz,
					 uint64_t el_off, struct avl_node *x,
					 struct avl_node *z)
{
	unsigned int t2_id, t3_id, x_id, y_id, z_id;
	struct avl_node *t2, *t3, *y;

	x_id = avl_node_to_id(x);
	z_id = avl_node_to_id(z);

	y_id = z->right_id;
	y = avl_id_to_node(y_id);
	if (!y) {
		/* Z is right heavy so Y must exist.  Need to convince BPF. */
		return x;
	}

	AVL_PRINTD("rotate left right, x_id %d, z_id %d, y_id %d, y bal %d\n",
		   x_id, z_id, y_id, y->balance);

	t2_id = y->left_id;
	t3_id = y->right_id;
	t2 = avl_id_to_node(t2_id);
	t3 = avl_id_to_node(t3_id);

	y->left_id = z_id;
	z->parent_id = y_id;
	z->right_id = t2_id;
	if (t2)
		t2->parent_id = z_id;

	y->right_id = x_id;
	x->parent_id = y_id;
	x->left_id = t3_id;
	if (t3)
		t3->parent_id = x_id;

	if (y->balance == 0) {
		/* t2 and t3 both have star nodes */
		x->balance = 0;
		y->balance = 0;
		z->balance = 0;
	} else if (y->balance > 0) {
		/* t3 only has a star node */
		x->balance = 0;
		y->balance = 0;
		z->balance = -1;
	} else {
		/* t2 only has a star node */
		x->balance = 1;
		y->balance = 0;
		z->balance = 0;
	}

	return y;
}

/* Insert node into t.  Caller must set node->key. */
static inline __attribute__((always_inline))
void avl_tree_insert(void *arr, size_t arr_sz, uint64_t el_off,
		     struct avl_tree *t, struct avl_node *node)
{
	struct avl_node *x = NULL;
	unsigned int x_id = 0;
	struct avl_node *z;
	unsigned int node_id, next_id, z_id;
	bool left = false;

	node->left_id = node->right_id = node->balance = 0;
	node->is_orphan = false;

	node_id = avl_node_to_id(node);

	/*
	 * Why the minus one?  We must never grow the tree beyond what BPF can
	 * search, a.k.a. MAX_AVL_HEIGHT.
	 *
	 * When we insert a node, we potentially grow the height of the tree.
	 * This loop searches for the future parent of node.  Stop one level
	 * short of the max to ensure the tree doesn't grow too big.
	 */
	next_id = t->root;
	for (int i = 0; i < MAX_AVL_HEIGHT - 1; i++) {
		if (!next_id)
			break;

		x_id = next_id;
		x = avl_id_to_node(x_id);
		if (!BPF_MUST_CHECK(x)) {
			/* Should always have x for non-zero next_id */
			AVL_ASSERT(0);
			break;
		}
		/*
		 * You might think that we should update the balance on the way
		 * down the tree.  However, we don't know if a node will change
		 * the weight of our child until we work our way back up.
		 * Consider the case where we add node to a leaf where it has a
		 * sibling, i.e. the parent was heavy: there's no change in
		 * the height at all, only in the parent goes from heavy (-1 or
		 * 1) to neutral (0).
		 */
		if (node->key < x->key) {
			next_id = x->left_id;
			left = true;
		} else {
			next_id = x->right_id;
			left = false;
		}
	}

	/*
	 * Exit criteria:
	 * - if no x, the tree was empty (!t->root) or corrupted such that
	 *   id_to_node failed.
	 * - normally, x is a leaf node with the appropriate child slot (left or
	 *   right) available (id == 0).
	 * - if next_id is still set, then x's child slot isn't open, and we ran
	 *   out of loop iterations for BPF.
	 */

	if (next_id) {
		node->is_orphan = true;
		__insert_orphan(arr, arr_sz, el_off, t, node);
		/* orphans don't count towards min/max */
		return;
	}

	node->parent_id = x_id;
	if (!x) {
		/* skip the balance; a tree of one node is balanced */
		t->root = node_id;
#ifdef AVL_CACHING
		t->min_id = node_id;
		t->min_key = node->key;
		t->max_id = node_id;
		t->max_key = node->key;
#endif
		return;
	} else if (left) {
		x->left_id = node_id;
	} else {
		x->right_id = node_id;
	}

	/*
	 * Need to walk up the tree, rebalancing, until we "absorb" the height
	 * increase at X.  Z, child of X, had its height increase by one.  (In
	 * the initial case, Z is the new node, whose height went from 0 to +1).
	 * Recall that height is how many levels of nodes below you (max of left
	 * and right), and balance is the difference of your left and right
	 * heights.
	 *
	 * You absorb the increase of Z's height at X in a couple ways:
	 * 1) If X was heavy, and the height increase was in the opposite child.
	 *    e.g. left heavy (balance == -1), and the right subtree at Z got an
	 *    additoinal level.  No change in X's height.
	 * 2) Any rotation.  The child that increased made X unbalanced (-2/+2),
	 *    which the rotations fix.  The insertion made X's height +1.  But
	 *    all rotations *for inserts* reduce the height by one.  (Note the
	 *    'star' case in the simple rotations only happens on deletions).
	 *
	 * The only other case is 3) if we don't need a rotation, and X wasn't
	 * heavy.  In which case, X's height increases, and we have to go up the
	 * tree.  Ultimately, if X is the tree root, then the entire tree grew
	 * in height.
	 */
	z_id = node_id;
	z = node;
	for (int i = 0; i < MAX_AVL_HEIGHT; i++) {
		struct avl_node *x_parent, *str;
		unsigned int x_parent_id, str_id;

		if (!x)
			break;
		left = (x->left_id == z_id);

		if (x->balance == 0) {
			/* case 3: can't absorb, pass it up the tree */
			if (left)
				x->balance = -1;
			else
				x->balance = 1;
			z_id = x_id;
			z = x;
			x_id = x->parent_id;
			x = avl_id_to_node(x_id);
			/* This is the only way to stay in the for loop */
			continue;
		}

		if (( left && x->balance > 0) ||
		    (!left && x->balance < 0)) {
			/* absorbed by a heavy x (case 1) */
			x->balance = 0;
			break;
		}

		/*
		 * case 2: rotate to absorb height.  Recall that rotations
		 * return the subtree root (Z or Y), and it is up to us to track
		 * the subtree's parent and reconnect after the rotation.
		 */
		x_parent_id = x->parent_id;
		x_parent = avl_id_to_node(x_parent_id);

		if (left) {
			if (z->balance <= 0)
				str = avl_rotate_right(x, z);
			else
				str = avl_rotate_left_right(x, z);
		} else {
			if (z->balance >= 0)
				str = avl_rotate_left(x, z);
			else
				str = avl_rotate_right_left(x, z);
		}

		str_id = avl_node_to_id(str);
		str->parent_id = x_parent_id;
		if (!x_parent) {
			t->root = str_id;
			break;
		}
		if (x_parent->left_id == x_id)
			x_parent->left_id = str_id;
		else
			x_parent->right_id = str_id;

		/* Inserts break after any rotation. */
		break;
	}

#ifdef AVL_CACHING
	/*
	 * Extreme Subtlety alert!  When we delete a node, we use next and prev
	 * to find the new min and max.  Using those to find the min/max assumes
	 * we were the leftmost/rightmost node.  So we need to be sure our min
	 * is leftmost and max is rightmost.  When we insert, "ties go to the
	 * right", meaning if two nodes have the same key, the newer one will be
	 * the right child of the older one.  Thus, once a min gets in the tree,
	 * no newer nodes of the same key will take its spot as leftmost, hence
	 * the "<" check below.  However, a new max will always take over the
	 * rightmost slot, hence the ">=" check below.
	 *
	 * What about rotations?  Don't worry.  Rotations never change the
	 * leftmost/rightmost, by virtue of never changing the horizontal
	 * ordering.  Take a look at the rotation pictures, and note that t1 and
	 * t4 are always the exterior trees.
	 */
	if (!t->min_id || node->key < t->min_key) {
		t->min_id = node_id;
		t->min_key = node->key;
	}
	if (!t->max_id || node->key >= t->max_key) {
		t->max_id = node_id;
		t->max_key = node->key;
	}
#endif
}

/*
 * Helper for delete.  Replaces x with with_id in the tree.  X is removed and
 * with_id is put in its place.  Does *not* account for x's old children.
 *
 * Returns true if x was the root, in which case the caller must set t->root =
 * with_id, unless you otherwise know that x wasn't root.  Yes, that *is*
 * particularly disgusting.  I'd like to pass the tree, but due to BPF
 * limitations, we can't pass more than 5 arguments, and we're already packing
 * stuff in with el_off.  But don't worry!  I wrap it in a macro for our own
 * use, which may be more or less disgusting, depending on your taste.
 */
static inline __attribute__((always_inline))
bool __avl_replace_node(void *arr, size_t arr_sz, uint64_t el_off,
			const struct avl_node *x, unsigned int with_id)
{
	struct avl_node *x_parent, *with;
	bool x_was_root = x->parent_id == 0;

	x_parent = avl_id_to_node(x->parent_id);
	if (x_parent) {
		if (x_parent->left_id == avl_node_to_id(x))
			x_parent->left_id = with_id;
		else
			x_parent->right_id = with_id;
	} else {
		AVL_ASSERT(x_was_root);
	}
	with = avl_id_to_node(with_id);
	if (with)
		with->parent_id = x->parent_id;

	return x_was_root;
}

#define avl_replace_node(x, with_id) do {				\
	unsigned int ___with_id = (with_id);				\
	if (__avl_replace_node(arr, arr_sz, el_off, x, ___with_id))	\
		t->root = ___with_id;					\
} while (0)

/*
 * Delete node from the tree.
 *
 * If node has a free child slot, we can pull up the other child into its place.
 * e.g. if there's no left, we can pull up right (or NULL) into node's spot.
 *
 * It's trickier if we have both left and right.  The next (successor) node is
 * on the node's right branch and it has no left child (a left child would be
 * less than 'next' and be the actual 'next).  Since next has no left child, we
 * can eventually put it in node's place (it is greater than all of node's lefts
 * and less than all of node's rights).
 *
 * If next is node's (right) child, we can shift it up (avl_replace_node), just
 * like with the empty children cases, and hang node->left off of next->left.
 *
 * If not, next is deeper in the right tree, and we have to pull it out,
 * swapping it with its right child (next->right_id below).  Now we can stuff
 * next into node's old spot: next->right := node->right, then next->left :=
 * node->left.
 *
 * If that wasn't bad enough, we need to retrace to keep the tree balanced.
 *
 * Each time around the balance loop, we're trying to both restore balance (i.e.
 * get back to -1,0,1) and absorb a height change if possible.  When we're at a
 * given node X, we'll need to know if the height loss was from left or right,
 * based on the child we're tracing up from though that child might not be there
 * anymore.  Our child, if it exists, is AVL balanced.
 *
 * So we need to identify which node is "X": the one whose balance must be
 * restored and might absorb the height change.
 *
 * If you do not change a node's children, you do not change its balance.  When
 * we shift up for the free child slot cases (e.g. !node->left_id), we don't
 * change its children.  Thus it is AVL balanced (if it exists).  So X will be
 * the parent of node (the new parent of the shifted up child).  And
 * from_the_left depends on whether node was a left or right child of X.
 *
 * Let's consider the case where next is deeper in the tree, i.e. not next's
 * immediate child.  We're removing next from its old location and stuffing at
 * node's location.  As far as heights go, it's like we removed next, not node.
 * So X is actually *next's* parent.  We changed next's children, so we need to
 * set its balance.  Since next took node's place, next *gets* node's balance.
 * Picture sticking node back where next was (ignoring key order), and then
 * removing node from there.  That'll create an imbalance/height change which
 * will get corrected as we retrace, but may also get fixed before we make it up
 * to next's new location absorbed down the tree.
 *
 * Finally, let's consider when next is node's right child, and we pull it up
 * into node's spot.  Again, we do change next's children (it gets node's old
 * left child), so we need to adjust next's balance.  It will get the same
 * balance that node did (and we'll rebalance it shortly).  Picture the balance
 * belonging to the *spot* where node was, not to node itself.  (That actually
 * might be a better way to think about balance in general).  But wait, you may
 * say: Node's position had a balance, and then when we pull up node's right
 * child (a.k.a. 'next') *and leave left child in place*, the right subtree's
 * height decreased by one and the left subtree didn't change: i.e. the balance
 * of node is one less!  Yes, but what that means is the right child 'next' is
 * our X: the one whose balance must be restored and might absorb the height
 * change.
 *
 * Simple example:
 *
 *                /     ===>       /     
 *              Node             Next    
 *             /    \           /    
 *            L     Next       L     
 *
 * Next moved up, absorbed the height change, and no one up above needs to hear
 * about it.  The subtree's original balance was 0, height 2.  After the
 * removal, its balance is -1, but height is still 2.  "X" was next, and the
 * loss came from the right.
 *
 * I think.
 */
static inline __attribute__((always_inline))
void avl_tree_delete(void *arr, size_t arr_sz, uint64_t el_off,
		     struct avl_tree *t, struct avl_node *node)
{
	struct avl_node *x, *o;
	unsigned int x_id;
	bool from_the_left = false;
	unsigned int node_id = avl_node_to_id(node);

	if (node->is_orphan) {
		__remove_orphan(arr, arr_sz, el_off, t, node);
		node->is_orphan = false;
		return;
	}

#ifdef AVL_CACHING
	/*
	 * You may be tempted to store next in case we're the min and in the
	 * "deep next" or "immediate child next" cases below.  But if we're min,
	 * we have no left child, and won't follow the "next" cases.  Similar
	 * for max.
	 */
	if (node_id == t->min_id) {
		struct avl_node *next = avl_next_node(node);

		t->min_id = avl_node_to_id(next);
		if (next)
			t->min_key = next->key;

	}
	if (node_id == t->max_id) {
		struct avl_node *prev = avl_prev_node(node);

		t->max_id = avl_node_to_id(prev);
		if (prev)
			t->max_key = prev->key;
	}
#endif

	/* For the "next" cases, we'll override x and from_the_left */
	x_id = node->parent_id;
	x = avl_id_to_node(x_id);
	/*
	 * We could be removing the root, so !x is possible, and from_the_left
	 * doesn't matter.
	 */
	if (x && x->left_id == node_id)
		from_the_left = true;

	if (!node->left_id) {
		avl_replace_node(node, node->right_id);
	} else if (!node->right_id) {
		avl_replace_node(node, node->left_id);
	} else {
		struct avl_node *next, *node_right, *node_left;
		unsigned int next_id;

		next = avl_next_node(node);
		if (!next) {
			/* Node has a right child, should always have next */
			AVL_ASSERT(next);
			return;
		}
		next_id = avl_node_to_id(next);

		/*
		 * in both cases, next takes node's slot, so it gets node's
		 * balance.  it will get corrected as we retrace to balance up.
		 */
		next->balance = node->balance;
		if (next->parent_id != node_id) {
			/* the "deep next" case */
			x_id = next->parent_id;
			x = avl_id_to_node(x_id);
			if (!x) {
				/* next must have a parent, ultimately node */
				AVL_ASSERT(x);
				return;
			}
			from_the_left = (x->left_id == next_id);

			avl_replace_node(next, next->right_id);
			next->right_id = node->right_id;
			node_right = avl_id_to_node(node->right_id);
			if (!node_right) {
				AVL_ASSERT(node_right);
				return;
			}
			node_right->parent_id = next_id;
		} else {
			/* The "immediate child next" case. */
			x = next;
			x_id = next_id;
			from_the_left = false;
		}

		avl_replace_node(node, next_id);
		next->left_id = node->left_id;
		node_left = avl_id_to_node(node->left_id);
		if (!node_left) {
			AVL_ASSERT(node_left);
			return;
		}
		node_left->parent_id = next_id;
	}

	/*
	 * If you haven't read how Insert handles retracing, read it first.
	 * It's a little easier than deletion.
	 *
	 * Again, we're working our way up the tree, from X, trying to absorb
	 * the height difference and fixing any imbalances.  A basic example:
	 * consider a node X with two children and no grandchildren.  The left
	 * one is removed.  X's balance changes, but the decrease in left's
	 * height does not affect X's height, since the right child is still
	 * there.  (Recall your height H = max(H_left, H_right) + 1).  There
	 * was a height change, and it affected X's balance, but not its height.
	 * Therefore it won't affect any heights or balances above X.
	 *
	 * So we can stop when:
	 * 1) X's balance was 0: the height change of a child doesn't affect X's
	 *    final height, just its balance.
	 * 2) *Some* simple rotations.  Spoiler: it's when z->balance == 0.
	 *    Recall from insertion that any rotation
	 *    decreased the freshly-inserted height by 1.  We're in the opposite
	 *    case, where we removed a node, decreased the height and need to
	 *    rotate, but the rotation might not absorb the height loss
	 *    (rotations always fix the balance).
	 *
	 * Consider the simple left rotation:
	 *
	 * 0          _X___                                 ___Z_       
	 * 1         /     Z             ===>              X     \      
	 * ...      /     / \                             / \     \     
	 * h+0     t1    /   \                           /   \     \    
	 * h+1          t23   \                        t1    t23    t4  
	 * h+2           *     t4                             *         
	 *
	 * Remember that '*"?  That's when t23 is actually at h+2, and it only
	 * happens when there was a deletion somewhere in t1.  The removal could
	 * have been t1's lowest node at h+1, or some other node in t1, left of
	 * X.  Either way, t1 decreased in height from h+1 to where it is in
	 * that picture at h+0, which is unbalancing X.
	 *
	 * When we do the left rotation, if the star node was there, the subtree
	 * stays at h+2.  Yes, X's height changes in the rotation, but as far as
	 * X's *parent* is concerned, its child subtree's height remains h+2,
	 * regardless of whether its child is X or Z.  In this manner, the
	 * rotation absorbed the height change of t1.
	 *
	 * Anyway, this happens when Z balance == 0.  And the complex rotations
	 * don't absorb.  Check out the picture above and convince yourself.
	 */

	for (int i = 0; i < MAX_AVL_HEIGHT; i++) {
		struct avl_node *x_parent, *str;
		unsigned int x_parent_id, str_id;
		bool absorbed = false;

		if (!x)
			break;

		if (x->balance == 0) {
			/* case 1: absorb the height loss. */
			if (from_the_left)
				x->balance = 1;
			else
				x->balance = -1;
			break;
		}

		x_parent_id = x->parent_id;
		x_parent = avl_id_to_node(x_parent_id);

		if (( from_the_left && x->balance < 0) ||
		    (!from_the_left && x->balance > 0)) {
			/*
			 * X lost height in the direction it was already heavy.
			 * This fully balances it, but we can't absorb.
			 */
			x->balance = 0;

			if (!x_parent)
				break;
			from_the_left = (x_parent->left_id == x_id);
			x_id = x_parent_id;
			x = x_parent;
			continue;
		}

		/* Lost height and now we're +/- 2 imbalanced */
		if (from_the_left) {
			struct avl_node *z;

			/*
			 * We lost height on the left and balance was >= 0, so
			 * we must have a right child, and that's the one we
			 * need to rotate up.
			 */
			z = avl_id_to_node(x->right_id);
			if (!z) {
				AVL_ASSERT(z);
				return;
			}
			absorbed = (z->balance == 0);
			if (z->balance >= 0) {
				/* right-right heavy */
				str = avl_rotate_left(x, z);
			} else {
				/* right-left */
				str = avl_rotate_right_left(x, z);
			}
		} else {
			struct avl_node *z;

			z = avl_id_to_node(x->left_id);
			if (!z) {
				AVL_ASSERT(z);
				return;
			}
			absorbed = (z->balance == 0);
			if (z->balance <= 0) {
				/* left-left heavy */
				str = avl_rotate_right(x, z);
			} else {
				/* left-right heavy */
				str = avl_rotate_left_right(x, z);
			}
		}

		str_id = avl_node_to_id(str);
		str->parent_id = x_parent_id;
		if (!x_parent) {
			t->root = str_id;
			break;
		}
		if (x_parent->left_id == x_id) {
			x_parent->left_id = str_id;
			from_the_left = true;
		} else {
			x_parent->right_id = str_id;
			from_the_left = false;
		}

		if (absorbed)
			break;
		x_id = x_parent_id;
		x = x_parent;
	}

	/*
	 * We could try to be clever and only pop orphans when we know we
	 * decreased the height of the tree (making room).  However, we can't
	 * be sure we handle all of the orphans at once, since we have to bound
	 * our loops.
	 *
	 * Simplest thing is to check for orphans on every delete and try and
	 * squeeze them into the tree.  Note that Insert will stick them back
	 * onto the orphan list on failure.  Just because we deleted an element
	 * (or several!), that doesn't mean we can stick a new one in.  Our
	 * orphan could still be on the heavy end of the tree - enough to create
	 * a new level.
	 */
	o = __pop_first_orphan(arr, arr_sz, el_off, t);
	if (o)
		avl_tree_insert(arr, arr_sz, el_off, t, o);
}

static inline __attribute__((always_inline))
struct avl_node *avl_tree_pop_min(void *arr, size_t arr_sz, uint64_t el_off,
				  struct avl_tree *t)
{
	struct avl_node *min = avl_tree_min(arr, arr_sz, el_off, t);

	if (min)
		avl_tree_delete(arr, arr_sz, el_off, t, min);
	return min;
}

static inline __attribute__((always_inline))
struct avl_node *avl_tree_pop_max(void *arr, size_t arr_sz, uint64_t el_off,
				  struct avl_tree *t)
{
	struct avl_node *max = avl_tree_max(arr, arr_sz, el_off, t);

	if (max)
		avl_tree_delete(arr, arr_sz, el_off, t, max);
	return max;
}

#endif // GHOST_LIB_AVL_BPF_H_
