// Copyright 2023 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <stdlib.h>
#include <sys/param.h>

// define these before including avl.bpf.h.
#define AVL_ASSERT(x) EXPECT_TRUE(x)

bool avl_test_print = false;
#define AVL_PRINTD(args...) do { if (avl_test_print) printf(args); } while (0)

int max_avl_height = 23;
#define MAX_AVL_HEIGHT max_avl_height

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "lib/avl.bpf.h"

namespace ghost {
namespace {

// Helper macros, similar to those that BPF schedulers will use
#define test_el_off() el_off(sizeof(elem), offsetof(elem, avl))

#define test_avl_node_to_elem(node) avl_node_to_elem(elems, node, avl)

#define test_avl_tree_lookup(tree, key) \
	test_avl_node_to_elem(avl_tree_lookup(elems, kNrElems, test_el_off(), tree, \
                                        key))
#define test_avl_tree_insert(tree, elem) \
	avl_tree_insert(elems, kNrElems, test_el_off(), tree,	&(elem)->avl)

#define test_avl_tree_delete(tree, elem)\
	avl_tree_delete(elems, kNrElems, test_el_off(), tree, &(elem)->avl)

#define test_avl_tree_min(tree) \
	test_avl_node_to_elem(avl_tree_min(elems, kNrElems,	test_el_off(), tree))

#define test_avl_tree_max(tree) \
	test_avl_node_to_elem(avl_tree_max(elems, kNrElems,	test_el_off(), tree))

#define test_avl_tree_pop_min(tree) \
	test_avl_node_to_elem(avl_tree_pop_min(elems, kNrElems,	test_el_off(), tree))

#define test_avl_tree_pop_max(tree) \
	test_avl_node_to_elem(avl_tree_pop_max(elems, kNrElems,	test_el_off(), tree))

class BpfAvlTest : public testing::Test {
 protected:
  static const int kNrElems = 200;
  struct elem {
    bool in_tree; // used by some tests
    avl_node avl;
  } elems[kNrElems] = {{0}};

  void SetUp() override {
    for (int i = 0; i < kNrElems; ++i) {
      elems[i].avl.key = i + 1; // Sort by id by default (+1 for no id == 0)
    }
  };

  elem *IdToElem(int id) {
    EXPECT_NE(id, 0);
    return &elems[id - 1];  // elem id 1 == array member 0
  }

  void RandomizeKeys(void) {
    for (int i = 0; i < kNrElems; ++i) {
      uint64_t k = rand();
      elems[i].avl.key = k ?: 1; // no 0-value keys
    }
  }

  avl_node *IdToNode(unsigned int id) {
    if (!id) {
      return nullptr;
    }
    return &elems[id - 1].avl;
  }

  unsigned int NodeToId(avl_node *n) {
    if (!n) {
      return 0;
    }
    struct elem *e = container_of(n, elem, avl);
    return (e - elems) + 1;
  }

  unsigned int ElemToId(elem *e) {
    return NodeToId(&e->avl);
  }

  int Depth(avl_node *n) {
    int depth = 0;
    while (n->parent_id) {
      depth++;
      n = IdToNode(n->parent_id);
    }
    return depth;
  }

  int Height(avl_node *n) {
    if (!n) {
      return 0;
    }
    return MAX(Height(IdToNode(n->left_id)), Height(IdToNode(n->right_id))) + 1;
  }

  // Helper for Walk
  void WalkId(unsigned int id, std::function<void(elem *e)> f) {
    avl_node *x = __avl_id_to_node(elems, kNrElems, test_el_off(), id);
    if (!x) {
      return;
    }
    WalkId(x->left_id, f);
    f(test_avl_node_to_elem(x));
    WalkId(x->right_id, f);
  }

  void Walk(avl_tree *t, std::function<void(elem *e)> f) {
    WalkId(t->root, f);
  }

  // Helper for PreorderWalk
  void PreorderWalkId(unsigned int id, std::function<void(elem *e)> f) {
    avl_node *x = __avl_id_to_node(elems, kNrElems, test_el_off(), id);
    if (!x) {
      return;
    }
    f(test_avl_node_to_elem(x));
    PreorderWalkId(x->left_id, f);
    PreorderWalkId(x->right_id, f);
  }

  void PreorderWalk(avl_tree *t, std::function<void(elem *e)> f) {
    PreorderWalkId(t->root, f);
  }

  // Helper for PostorderWalk
  void PostorderWalkId(unsigned int id, std::function<void(elem *e)> f) {
    avl_node *x = __avl_id_to_node(elems, kNrElems, test_el_off(), id);
    if (!x) {
      return;
    }
    PostorderWalkId(x->left_id, f);
    PostorderWalkId(x->right_id, f);
    f(test_avl_node_to_elem(x));
  }

  void PostorderWalk(avl_tree *t, std::function<void(elem *e)> f) {
    PostorderWalkId(t->root, f);
  }

  int Count(avl_tree *t) {
    int count = 0;
    Walk(t, [&](elem *e){
      count++;
    });
    return count;
  }

  uint64_t MinKey(avl_tree *t) {
    uint64_t min = UINT64_MAX;
    Walk(t, [&](elem *e){
      min = MIN(e->avl.key, min);
    });
    return min;
  }

  uint64_t MaxKey(avl_tree *t) {
    uint64_t max = 0;
    Walk(t, [&](elem *e){
      max = MAX(e->avl.key, max);
    });
    return max;
  }

  void DumpTree(avl_tree *t, const char *msg = "") {
    Walk(t, [&](elem *e){
      printf("%s %d: K%lu, L %d, R %d, P %d, B %d\n", msg, ElemToId(e),
             e->avl.key, e->avl.left_id, e->avl.right_id, e->avl.parent_id,
             e->avl.balance);
    });
  }

  // Attempts to print the tree.  You'll need a wide terminal, and even then,
  // it's a bit iffy.  Don't worry too much about this; I use it for debugging.
  void PrintTree(avl_tree *t) {
    static const int kNrFullTree = 128;
    int full_tree[kNrFullTree] = {0};
    int deepest = 0;
    bool tree_truncated = false;
    // For each node, write its id in its slot in the full_tree.
    // Empty slots will remain 0
    Walk(t, [&](elem *e) {
      avl_node *n = &e->avl;
      avl_node *parent;
      int depth = 0;
      int left_right_hist = 0;
      for (avl_node *i = n; i->parent_id; i = parent) {
        depth++;
        parent = IdToNode(i->parent_id);
        left_right_hist <<= 1;
        if (NodeToId(i) == parent->right_id) {
          left_right_hist |= 1;
        }
      }
      // depth tells us which chunk of the array we're in.  2^(depth) - 1.
      // h = 0 -> full[0], 1 slot (root)
      // h = 1 -> full[1], 2 slots
      // h = 2 -> full[3], 4 slots
      // h = 4 -> full[7], 8 slots
      int *chunk = full_tree + (1 << depth) - 1;
      int width = 1 << depth;
      // left_right_hist tells us which slot (an int *) we get in the chunk
      //    X------- is all lefts
      //    -X------ is 1 immediate right child (as we walk up), other lefts
      //    --X----- is 1 left, 1 right, other lefts
      //    ---X---- is 1 right, 1 right, other lefts
      //    ------X- is 1 left, other rights
      //    -------X is all rights
      //
      // left_right_hist tells us if the most recent turn from the top was a
      // right turn or not.
      //
      // we want to figure out our slot in chunk[0..width-1].
      //
      // if the first bit is right, we're in the upper half of whatever chunk is
      // remaining.
      int *slot = chunk;
      while (left_right_hist) {
        width /= 2;
        if (left_right_hist & 1) {
          slot += width;
        }
        left_right_hist >>= 1;
      }
      if (slot - full_tree < kNrFullTree) {
        *slot = n->key;
        deepest = MAX(depth, deepest);
      } else {
        tree_truncated = true;
      }
    });

    // Too deep of a tree is hard to read in a console
    static const int kMaxDepth = 6;
    if (deepest > kMaxDepth) {
      tree_truncated = true;
      deepest = kMaxDepth;
    }
    int depth = 0;
    int slot_counter = 1 << depth;
    int width_of_line = (1 << deepest) * 3; // one space + %2d
    int nr_spaces = width_of_line / slot_counter - 2;
    // divide width by nr items.  then -2
    for (int i = 0; i < kNrFullTree; ++i) {
      if (depth > deepest) {
        break;
      }
      // the higher we are in the tree, the more space between our nodes.
      // want the space split before and after
      for (int j = 0; j < nr_spaces / 2 + nr_spaces % 2; ++j) {
        printf(" ");
      }
      if (full_tree[i]) {
        printf("%2d", full_tree[i]);
      } else {
        printf("__");
      }
      for (int j = 0; j < nr_spaces / 2; ++j) {
        printf(" ");
      }
      slot_counter--;
      if (!slot_counter) {
        printf("\n");
        depth++;
        slot_counter = 1 << depth;
        if (depth == deepest) {
          nr_spaces = 1;
        } else {
          nr_spaces = width_of_line / slot_counter - 2;
        }
      }
    }
    if (tree_truncated) {
      printf("(tree truncated)\n");
    }
  }

  // Runs various tests on the tree, particularly making sure it is balanced
  // (within [-1,1]), the balance is correctly maintained (based on heights),
  // and the structure is in key order.
  bool ValidateTree(avl_tree *t) {
    bool ok = true;
    uint64_t min_key = UINT64_MAX;
    uint64_t max_key = 0;;

    Walk(t, [&](elem *e) {
      avl_node *n = &e->avl;
      if (n->balance < -1 || n->balance > 1) {
        ok = false;
        printf("Bad balance %d of node %d\n", n->balance, NodeToId(n));
        return;
      }
      avl_node *left = IdToNode(n->left_id);
      avl_node *right = IdToNode(n->right_id);
      int left_height = Height(left);
      int right_height = Height(right);
      if (n->balance != right_height - left_height) {
        ok = false;
        printf("Height balance mismatch %d of node %d, %d %d\n", n->balance,
               NodeToId(n), left_height, right_height);
        return;
      }
      // keys are ids, which are never 0.
      if (n->key == 0) {
        ok = false;
        printf("Node key was 0.  Uninitialized elems array?");
        return;
      }
      min_key = MIN(n->key, min_key);
      max_key = MAX(n->key, max_key);
      if (n->key < max_key) {
        ok = false;
        printf("Ordering violation node %d, key %lu max_key %lu\n",
               NodeToId(n), n->key, max_key);
        return;
      }
    });
    if (!ok) {
      DumpTree(t);
      PrintTree(t);
    }
    EXPECT_TRUE(ok);
    return ok;
  }
};

TEST_F(BpfAvlTest, InsertAndDeleteFullInOrder) {
  avl_tree tree = AVL_TREE_INITIALIZER;
  for (int i = 0; i < kNrElems; ++i) {
    test_avl_tree_insert(&tree, &elems[i]);
  }
  ValidateTree(&tree);
  EXPECT_EQ(Count(&tree), (int)kNrElems);
  for (int i = 0; i < kNrElems; ++i) {
    test_avl_tree_delete(&tree, &elems[i]);
  }
  ValidateTree(&tree);
  EXPECT_EQ(Count(&tree), 0);
}

TEST_F(BpfAvlTest, SkipAFew) {
  avl_tree tree = AVL_TREE_INITIALIZER;
  for (int i = 0; i < 24; ++i) {
    if (i == 9 || i == 15 || i == 18 || i == 20) {
      continue;
    }
    test_avl_tree_insert(&tree, &elems[i]);
  }
  ValidateTree(&tree);
}

TEST_F(BpfAvlTest, Lookup) {
  avl_tree tree = AVL_TREE_INITIALIZER;
  std::vector<int> skip_ids = {9, 15, 18, 20};
  for (int i = 0; i < 24; ++i) {
    if (std::count(skip_ids.begin(), skip_ids.end(), i+1)) {
      continue;
    }
    test_avl_tree_insert(&tree, &elems[i]);
  }
  ValidateTree(&tree);

  for (int i : skip_ids) {
    EXPECT_EQ(test_avl_tree_lookup(&tree, i), nullptr);
  }
  for (int i : skip_ids) {
    test_avl_tree_insert(&tree, IdToElem(i));
    ValidateTree(&tree);
    EXPECT_EQ(test_avl_tree_lookup(&tree, i), IdToElem(i));
  }
}

TEST_F(BpfAvlTest, InsertBackwards) {
  avl_tree tree = AVL_TREE_INITIALIZER;
  for (int i = 23; i >= 0; --i) {
    test_avl_tree_insert(&tree, &elems[i]);
  }
  ValidateTree(&tree);
}

TEST_F(BpfAvlTest, CrazyKeys) {
  avl_tree tree = AVL_TREE_INITIALIZER;
  RandomizeKeys();
  for (int i = 0; i < kNrElems; ++i) {
    test_avl_tree_insert(&tree, &elems[i]);
  }
  ValidateTree(&tree);
}

TEST_F(BpfAvlTest, MinStuff) {
  avl_tree tree = AVL_TREE_INITIALIZER;
  RandomizeKeys();
  for (int i = 0; i < kNrElems; ++i) {
    test_avl_tree_insert(&tree, &elems[i]);
  }
  ValidateTree(&tree);
  elem *min_e = test_avl_tree_min(&tree);
  EXPECT_EQ(min_e->avl.key, MinKey(&tree));

  // make sure we actually removed min_e
  test_avl_tree_delete(&tree, min_e);
  ValidateTree(&tree);
  elem *min_e2 = test_avl_tree_min(&tree);
  EXPECT_EQ(min_e2->avl.key, MinKey(&tree));
  EXPECT_NE(min_e2, min_e);
  ValidateTree(&tree);
}

TEST_F(BpfAvlTest, MaxStuff) {
  avl_tree tree = AVL_TREE_INITIALIZER;
  RandomizeKeys();
  for (int i = 0; i < kNrElems; ++i) {
    test_avl_tree_insert(&tree, &elems[i]);
  }
  ValidateTree(&tree);
  elem *max_e = test_avl_tree_max(&tree);
  EXPECT_EQ(max_e->avl.key, MaxKey(&tree));

  // make sure we actually removed max_e
  test_avl_tree_delete(&tree, max_e);
  ValidateTree(&tree);
  elem *max_e2 = test_avl_tree_max(&tree);
  EXPECT_EQ(max_e2->avl.key, MaxKey(&tree));
  EXPECT_NE(max_e2, max_e);
  ValidateTree(&tree);
}

TEST_F(BpfAvlTest, PopMin) {
  avl_tree tree = AVL_TREE_INITIALIZER;
  RandomizeKeys();
  for (int i = 0; i < kNrElems; ++i) {
    test_avl_tree_insert(&tree, &elems[i]);
  }
  ValidateTree(&tree);
  uint64_t prev_min = 0;
  elem *min_e;
  while ((min_e = test_avl_tree_pop_min(&tree))) {
    EXPECT_GE(min_e->avl.key, prev_min);
    prev_min = min_e->avl.key;
    ValidateTree(&tree);
  }
}

TEST_F(BpfAvlTest, PopMax) {
  avl_tree tree = AVL_TREE_INITIALIZER;
  RandomizeKeys();
  for (int i = 0; i < kNrElems; ++i) {
    test_avl_tree_insert(&tree, &elems[i]);
  }
  ValidateTree(&tree);
  uint64_t prev_max = UINT64_MAX;
  elem *max_e;
  while ((max_e = test_avl_tree_pop_max(&tree))) {
    EXPECT_LE(max_e->avl.key, prev_max);
    prev_max = max_e->avl.key;
    ValidateTree(&tree);
  }
}

TEST_F(BpfAvlTest, DupMin) {
  avl_tree tree = AVL_TREE_INITIALIZER;
  for (int i = 0; i < kNrElems; ++i) {
    if (i != 10) {
      elems[i].avl.key = 1;
    } else {
      elems[i].avl.key = 99;
    }
    test_avl_tree_insert(&tree, &elems[i]);
  }
  ValidateTree(&tree);
  uint64_t prev_min = 0;
  elem *min_e;
  while ((min_e = test_avl_tree_min(&tree))) {
    EXPECT_EQ(min_e->avl.key, MinKey(&tree));
    EXPECT_GE(min_e->avl.key, prev_min);
    prev_min = min_e->avl.key;
    test_avl_tree_delete(&tree, min_e);
    ValidateTree(&tree);
  }
}

TEST_F(BpfAvlTest, DupMax) {
  avl_tree tree = AVL_TREE_INITIALIZER;
  for (int i = 0; i < kNrElems; ++i) {
    if (i != 10) {
      elems[i].avl.key = 99;
    } else {
      elems[i].avl.key = 1;
    }
    test_avl_tree_insert(&tree, &elems[i]);
  }
  ValidateTree(&tree);
  uint64_t prev_max = UINT64_MAX;
  elem *max_e;
  while ((max_e = test_avl_tree_max(&tree))) {
    EXPECT_EQ(max_e->avl.key, MaxKey(&tree));
    EXPECT_LE(max_e->avl.key, prev_max);
    prev_max = max_e->avl.key;
    test_avl_tree_delete(&tree, max_e);
    ValidateTree(&tree);
  }
}

TEST_F(BpfAvlTest, MinMaxOne) {
  avl_tree tree = AVL_TREE_INITIALIZER;
  test_avl_tree_insert(&tree, &elems[0]);
  ValidateTree(&tree);

  elem *min_e = test_avl_tree_min(&tree);
  EXPECT_EQ(min_e->avl.key, MinKey(&tree));
  EXPECT_EQ(min_e, &elems[0]);

  elem *max_e = test_avl_tree_max(&tree);
  EXPECT_EQ(max_e->avl.key, MaxKey(&tree));
  EXPECT_EQ(max_e, &elems[0]);

  test_avl_tree_delete(&tree, &elems[0]);

  EXPECT_EQ(test_avl_tree_min(&tree), nullptr);;
  EXPECT_EQ(test_avl_tree_max(&tree), nullptr);;
}

TEST_F(BpfAvlTest, DeleteNodeWithChildren) {
  // Don't randomize.  These are specific tests based on the construction of a
  // specific tree.
  avl_tree tree = AVL_TREE_INITIALIZER;
  for (int i = 0; i < kNrElems / 2; ++i) {
    elems[i].in_tree = true;
    test_avl_tree_insert(&tree, &elems[i]);
  }
  ValidateTree(&tree);
  // Subtree at 20, remove 18
  //         20    ===>      20
  //       /   \           /   \
  //      18   22         19   22
  //     / \   / \       /     / \
  //   17 19  21 23    17     21 23
  //
  // This is the "immediate child next" removal case.  18 is removed, 19 is
  // next, and 19 is 18's right child.
  test_avl_tree_delete(&tree, IdToElem(18));
  ValidateTree(&tree);

  // Remove 20
  //
  //           24             24
  //          /               /
  //         20    ===>      21
  //        /  \            /  \
  //       19  22          19  22
  //      /    / \        /      \
  //    17    21 23     17       23
  //
  // This is the "deep next" removal case.  20 is removed, 21 is next, and 21 is
  // not 20's immediate child.
  test_avl_tree_delete(&tree, IdToElem(20));
  ValidateTree(&tree);

  // Remove 22
  //
  //          24                 24      
  //         /  \               /  \
  //        21   t   ===>      21   t   
  //       /  \               /  \
  //      19  22             19  23     
  //     /      \           /           
  //   17       23        17            
  //
  // This is the "move up my child because my other child is null" removal case.
  test_avl_tree_delete(&tree, IdToElem(22));
  ValidateTree(&tree);

  // Remove 21
  //
  //          24                 24               24
  //         /  \               /  \             /  \
  //        21   t   ===>      23   t  ===>     19   t     
  //       /  \               /                /  \
  //      19  23             19               17   23   
  //     /                  /                            
  //   17                 17                             
  //
  // This is another 'immediate next', this time causing a rotation
  test_avl_tree_delete(&tree, IdToElem(21));
  ValidateTree(&tree);

  test_avl_tree_delete(&tree, IdToElem(77));
  test_avl_tree_delete(&tree, IdToElem(79));
  test_avl_tree_delete(&tree, IdToElem(65));
  test_avl_tree_delete(&tree, IdToElem(67));
  test_avl_tree_delete(&tree, IdToElem(69));
  test_avl_tree_delete(&tree, IdToElem(71));
}

TEST_F(BpfAvlTest, DeleteLeftRightRotation) {
  // Don't randomize.  This is a specific test based on the construction of a
  // specific tree.
  avl_tree tree = AVL_TREE_INITIALIZER;
  for (int i = 0; i < kNrElems / 2; ++i) {
    elems[i].in_tree = true;
    test_avl_tree_insert(&tree, &elems[i]);
  }
  ValidateTree(&tree);

  test_avl_tree_delete(&tree, IdToElem(77));
  test_avl_tree_delete(&tree, IdToElem(79));

  test_avl_tree_delete(&tree, IdToElem(65));
  test_avl_tree_delete(&tree, IdToElem(67));
  test_avl_tree_delete(&tree, IdToElem(69));
  test_avl_tree_delete(&tree, IdToElem(71));
  test_avl_tree_delete(&tree, IdToElem(70));

  //                       72                   
  //         68                      76         
  //   66          __          74          78   
  //__    __    __    __    73    75    __    __
  //
  // When we remove 66, 72 (X) will be right heavy, and the 76 child (Z) is left
  // heavy, triggering a right_left rotation.   74 (Y) should be the new subtree
  // root.  (this also will trickle up above the subtree.  If you're curious,
  // add in some PrintTree calls.

  test_avl_tree_delete(&tree, IdToElem(66));
  EXPECT_EQ(IdToElem(74)->avl.left_id, 72);
  EXPECT_EQ(IdToElem(74)->avl.right_id, 76);
}

TEST_F(BpfAvlTest, RandomRemovals) {
  RandomizeKeys();
  avl_tree tree = AVL_TREE_INITIALIZER;
  for (int i = 0; i < kNrElems; ++i) {
    elems[i].in_tree = true;
    test_avl_tree_insert(&tree, &elems[i]);
  }
  ValidateTree(&tree);
  static const int kNrTries = kNrElems * 10;
  for (int i = 0; i < kNrTries; ++i) {
    size_t idx = rand() % kNrElems;
    if (!elems[idx].in_tree) {
      continue;
    }
    test_avl_tree_delete(&tree, &elems[idx]);
    elems[idx].in_tree = false;
    if (!ValidateTree(&tree)) {
      break;
    }
  }
}

TEST_F(BpfAvlTest, RandomInsertsAndRemovals) {
  RandomizeKeys();
  avl_tree tree = AVL_TREE_INITIALIZER;
  static const int kNrTries = kNrElems * 10;
  for (int i = 0; i < kNrTries; ++i) {
    size_t idx = rand() % kNrElems;
    if (elems[idx].in_tree) {
      test_avl_tree_delete(&tree, &elems[idx]);
      elems[idx].in_tree = false;
    } else {
      test_avl_tree_insert(&tree, &elems[idx]);
      elems[idx].in_tree = true;
    }
    if (!ValidateTree(&tree)) {
      break;
    }
  }
}

TEST_F(BpfAvlTest, OrphanPromotion) {
  int old_max_avl_height = max_avl_height;
  // Can contain at most 31 nodes.  Recall that Insert does MAX_AVL_HEIGHT - 1.
  max_avl_height = 5;

  avl_tree tree = AVL_TREE_INITIALIZER;
  for (int i = 0; i < 50; ++i) {
    test_avl_tree_insert(&tree, &elems[i]);
  }
  ValidateTree(&tree);

  bool has_orphan = false;
  for (int i = 0; i < 50; ++i) {
    if (elems[i].avl.is_orphan) {
      has_orphan = true;
      break;
    }
  }
  EXPECT_TRUE(has_orphan);

  // Lookup won't check the orphan list, but as we delete elements, the orphans
  // will be moved onto the tree.
  while (test_avl_tree_pop_min(&tree)) {
    ;
  }
  has_orphan = false;
  for (int i = 0; i < 50; ++i) {
    if (elems[i].avl.is_orphan) {
      has_orphan = true;
      break;
    }
  }
  EXPECT_FALSE(has_orphan);

  max_avl_height = old_max_avl_height;
}

TEST_F(BpfAvlTest, OrphanInsertsAndRemovals) {
  int old_max_avl_height = max_avl_height;
  // Can contain at most 31 nodes.  Recall that Insert does MAX_AVL_HEIGHT - 1.
  max_avl_height = 5;

  RandomizeKeys();
  avl_tree tree = AVL_TREE_INITIALIZER;
  static const int kNrTries = kNrElems * 10;
  for (int i = 0; i < kNrTries; ++i) {
    size_t idx = rand() % kNrElems;
    if (elems[idx].in_tree) {
      test_avl_tree_delete(&tree, &elems[idx]);
      elems[idx].in_tree = false;
    } else {
      test_avl_tree_insert(&tree, &elems[idx]);
      elems[idx].in_tree = true;
    }
    if (!ValidateTree(&tree)) {
      break;
    }
  }

  max_avl_height = old_max_avl_height;
}

}  // namespace
}  // namespace ghost

int main(int argc, char **argv) {
  testing::InitGoogleMock(&argc, argv);

  return RUN_ALL_TESTS();
}
