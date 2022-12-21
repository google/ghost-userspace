// Copyright 2022 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "lib/queue.bpf.h"

namespace ghost {
namespace {

class BpfQueueTest : public testing::Test {
 protected:
  static const int kNrElems = 50;
  struct elem {
    int x;
    arr_list_entry link;
    arr_list_entry link2;
    arr_list_entry link3;
  } elems[kNrElems] = {{0}};

  void SetUp() override {
    for (int i = 0; i < kNrElems; ++i) {
      elems[i].x = i;
    }
  };

  // Counts elements in list l.
  // List must be linked with entry 'link'
  unsigned int count_list(arr_list *l) {
    elem *e_i;
    int _max;
    int count = 0;
    arr_list_foreach(e_i, elems, kNrElems, l, link, _max, kNrElems) {
      count++;
    }
    return count;
  }

  // Asserts elements in list l are the members of 'should_be'.
  // List must be linked with entry 'link'
  void assert_list_is(arr_list *l, const std::vector<int> &should_be) {
    elem *e_i = arr_list_first(elems, kNrElems, l);
    for (int i : should_be) {
      ASSERT_NE(e_i, nullptr);
      EXPECT_EQ(e_i->x, i);
      e_i = arr_list_next(elems, kNrElems, e_i, link);
    }
    EXPECT_EQ(e_i, nullptr);
  }

  // List must be linked with entry 'link'
  void dump_list(arr_list *l, const char *msg) {
    elem *e_i;
    int _max;
    arr_list_foreach(e_i, elems, kNrElems, l, link, _max, kNrElems) {
      printf("%s %d\n", msg, e_i->x);
    }
  }
};

TEST_F(BpfQueueTest, PrevNext) {
  elem *prev, *next;
  arr_list list = ARR_LIST_HEAD_INITIALIZER;
  for (int i = 0; i < kNrElems; ++i) {
    arr_list_insert_tail(elems, kNrElems, &list, &elems[i], link);
  }
  for (int i = 0; i < kNrElems; ++i) {
    prev = arr_list_prev(elems, kNrElems, &elems[i], link);
    next = arr_list_next(elems, kNrElems, &elems[i], link);
    if (i != 0) {
      ASSERT_NE(prev, nullptr);
      EXPECT_EQ(prev->x, i - 1);
    }
    if (i != kNrElems - 1) {
      ASSERT_NE(next, nullptr);
      EXPECT_EQ(next->x, i + 1);
    }
  }
}

TEST_F(BpfQueueTest, FirstLast) {
  arr_list list = ARR_LIST_HEAD_INITIALIZER;
  for (int i = 0; i < kNrElems; ++i) {
    arr_list_insert_tail(elems, kNrElems, &list, &elems[i], link);
  }
  EXPECT_EQ(arr_list_first(elems, kNrElems, &list), &elems[0]);
  EXPECT_EQ(arr_list_last(elems, kNrElems, &list), &elems[kNrElems - 1]);
}

TEST_F(BpfQueueTest, FirstLastSingle) {
  arr_list list = ARR_LIST_HEAD_INITIALIZER;
  arr_list_insert_tail(elems, kNrElems, &list, &elems[0], link);
  EXPECT_EQ(arr_list_first(elems, kNrElems, &list), &elems[0]);
  EXPECT_EQ(arr_list_last(elems, kNrElems, &list), &elems[0]);
}

TEST_F(BpfQueueTest, InsertRemoveEmpty) {
  arr_list list = ARR_LIST_HEAD_INITIALIZER;
  arr_list_insert_tail(elems, kNrElems, &list, &elems[0], link);
  arr_list_remove(elems, kNrElems, &list, &elems[0], link);
  EXPECT_TRUE(arr_list_empty(&list));
  EXPECT_EQ(arr_list_last(elems, kNrElems, &list), nullptr);
}

TEST_F(BpfQueueTest, InsertHead) {
  arr_list list = ARR_LIST_HEAD_INITIALIZER;
  for (int i = 0; i < 5; ++i) {
    arr_list_insert_head(elems, kNrElems, &list, &elems[i], link);
  }
  assert_list_is(&list, {4, 3, 2, 1, 0});
}

TEST_F(BpfQueueTest, InsertTail) {
  arr_list list = ARR_LIST_HEAD_INITIALIZER;
  for (int i = 0; i < 5; ++i) {
    arr_list_insert_tail(elems, kNrElems, &list, &elems[i], link);
  }
  assert_list_is(&list, {0, 1, 2, 3, 4});
}

TEST_F(BpfQueueTest, InsertBefore) {
  arr_list list = ARR_LIST_HEAD_INITIALIZER;
  for (int i = 0; i < 4; ++i) {
    arr_list_insert_tail(elems, kNrElems, &list, &elems[i], link);
  }
  assert_list_is(&list, {0, 1, 2, 3});
  arr_list_insert_before(elems, kNrElems, &list, &elems[2], &elems[7], link);
  assert_list_is(&list, {0, 1, 7, 2, 3});
}

TEST_F(BpfQueueTest, InsertBeforeFirst) {
  arr_list list = ARR_LIST_HEAD_INITIALIZER;
  for (int i = 0; i < 4; ++i) {
    arr_list_insert_tail(elems, kNrElems, &list, &elems[i], link);
  }
  assert_list_is(&list, {0, 1, 2, 3});
  elem *first = arr_list_first(elems, kNrElems, &list);
  arr_list_insert_before(elems, kNrElems, &list, first, &elems[7], link);
  assert_list_is(&list, {7, 0, 1, 2, 3});
}

TEST_F(BpfQueueTest, InsertAfter) {
  arr_list list = ARR_LIST_HEAD_INITIALIZER;
  for (int i = 0; i < 4; ++i) {
    arr_list_insert_tail(elems, kNrElems, &list, &elems[i], link);
  }
  assert_list_is(&list, {0, 1, 2, 3});
  arr_list_insert_after(elems, kNrElems, &list, &elems[2], &elems[7], link);
  assert_list_is(&list, {0, 1, 2, 7, 3});
}

TEST_F(BpfQueueTest, InsertAfterLast) {
  arr_list list = ARR_LIST_HEAD_INITIALIZER;
  for (int i = 0; i < 4; ++i) {
    arr_list_insert_tail(elems, kNrElems, &list, &elems[i], link);
  }
  assert_list_is(&list, {0, 1, 2, 3});
  elem *last = arr_list_last(elems, kNrElems, &list);
  arr_list_insert_after(elems, kNrElems, &list, last, &elems[7], link);
  assert_list_is(&list, {0, 1, 2, 3, 7});
}

TEST_F(BpfQueueTest, PopFirst) {
  arr_list list = ARR_LIST_HEAD_INITIALIZER;
  for (int i = 0; i < 5; ++i) {
    arr_list_insert_tail(elems, kNrElems, &list, &elems[i], link);
  }
  assert_list_is(&list, {0, 1, 2, 3, 4});

  std::vector<int> should_be = {0, 1, 2, 3, 4};
  elem *first;
  for (int i : {0, 1, 2, 3, 4}) {
        first = arr_list_pop_first(elems, kNrElems, &list, link);
        ASSERT_NE(first, nullptr);
        EXPECT_EQ(first->x, i);
        should_be.erase(should_be.begin());
        assert_list_is(&list, should_be);
  }
  first = arr_list_pop_first(elems, kNrElems, &list, link);
  EXPECT_EQ(first, nullptr);
}

TEST_F(BpfQueueTest, ForEach) {
  static const int kNrIters = 20;  // testing foreach with < kNrElems
  arr_list list = ARR_LIST_HEAD_INITIALIZER;
  for (int i = 0; i < kNrIters; ++i) {
    arr_list_insert_tail(elems, kNrElems, &list, &elems[i], link);
  }
  elem *e_i;
  int _max;
  int sofar = 0;
  arr_list_foreach(e_i, elems, kNrElems, &list, link, _max, kNrIters) {
    EXPECT_EQ(e_i->x, sofar++);
  }
  EXPECT_EQ(e_i, nullptr);
  arr_list_foreach_reverse(e_i, elems, kNrElems, &list, link, _max, kNrIters) {
    EXPECT_EQ(e_i->x, --sofar);
  }
  EXPECT_EQ(e_i, nullptr);
}

TEST_F(BpfQueueTest, ForEachSafe) {
  static const int kNrIters = 20;
  arr_list list = ARR_LIST_HEAD_INITIALIZER;
  for (int i = 0; i < kNrIters; ++i) {
    arr_list_insert_tail(elems, kNrElems, &list, &elems[i], link);
  }
  elem *e_i, *t;
  int _max;
  arr_list_foreach_safe(e_i, elems, kNrElems, &list, link, t, _max, kNrIters) {
    if (e_i->x % 2 == 1) {
      arr_list_remove(elems, kNrElems, &list, e_i, link);
    }
  }
  EXPECT_EQ(e_i, nullptr);
  arr_list_foreach(e_i, elems, kNrElems, &list, link, _max, kNrIters) {
    EXPECT_EQ(e_i->x % 2, 0);
  }
  EXPECT_EQ(e_i, nullptr);
}

TEST_F(BpfQueueTest, ForEachReverseSafe) {
  static const int kNrIters = 20;
  arr_list list = ARR_LIST_HEAD_INITIALIZER;
  for (int i = 0; i < kNrIters; ++i) {
    arr_list_insert_tail(elems, kNrElems, &list, &elems[i], link);
  }
  elem *e_i, *t;
  int _max;
  arr_list_foreach_reverse_safe(e_i, elems, kNrElems, &list, link, t, _max,
                                kNrIters) {
    if (e_i->x % 2 == 1) {
      arr_list_remove(elems, kNrElems, &list, e_i, link);
    }
  }
  EXPECT_EQ(e_i, nullptr);
  arr_list_foreach(e_i, elems, kNrElems, &list, link, _max, kNrIters) {
    EXPECT_EQ(e_i->x % 2, 0);
  }
  EXPECT_EQ(e_i, nullptr);
}

// kNrElems in the list, not kNrIters.  foreach should stop short.
TEST_F(BpfQueueTest, ShortForEach) {
  static const int kNrIters = 20;
  arr_list list = ARR_LIST_HEAD_INITIALIZER;
  for (int i = 0; i < kNrElems; ++i) {
    arr_list_insert_tail(elems, kNrElems, &list, &elems[i], link);
  }
  elem *e_i;
  int _max;
  arr_list_foreach(e_i, elems, kNrElems, &list, link, _max, kNrIters) {
    ;
  }
  EXPECT_NE(e_i, nullptr);
  EXPECT_EQ(_max, kNrIters);
}

TEST_F(BpfQueueTest, Concat) {
  arr_list odds = ARR_LIST_HEAD_INITIALIZER;
  arr_list evens = ARR_LIST_HEAD_INITIALIZER;
  arr_list all = ARR_LIST_HEAD_INITIALIZER;
  for (int i = 0; i < kNrElems; ++i) {
    if (i % 2 == 1) {
      arr_list_insert_tail(elems, kNrElems, &odds, &elems[i], link);
    } else {
      arr_list_insert_tail(elems, kNrElems, &evens, &elems[i], link);
    }
  }
  arr_list_concat(elems, kNrElems, &all, &odds, link);
  arr_list_concat(elems, kNrElems, &all, &evens, link);
  EXPECT_TRUE(arr_list_empty(&odds));
  EXPECT_TRUE(arr_list_empty(&evens));
  EXPECT_EQ(count_list(&all), int(kNrElems));
}

TEST_F(BpfQueueTest, ConcatEmptyLeft) {
  arr_list list = ARR_LIST_HEAD_INITIALIZER;
  arr_list was_empty = ARR_LIST_HEAD_INITIALIZER;
  for (int i = 0; i < kNrElems; ++i) {
    arr_list_insert_tail(elems, kNrElems, &list, &elems[i], link);
  }
  arr_list_concat(elems, kNrElems, &was_empty, &list, link);
  EXPECT_TRUE(arr_list_empty(&list));
  EXPECT_EQ(count_list(&was_empty), int(kNrElems));
}

TEST_F(BpfQueueTest, ConcatEmptyRight) {
  arr_list list = ARR_LIST_HEAD_INITIALIZER;
  arr_list empty = ARR_LIST_HEAD_INITIALIZER;
  for (int i = 0; i < kNrElems; ++i) {
    arr_list_insert_tail(elems, kNrElems, &list, &elems[i], link);
  }
  arr_list_concat(elems, kNrElems, &list, &empty, link);
  EXPECT_TRUE(arr_list_empty(&empty));
  EXPECT_EQ(count_list(&list), int(kNrElems));
}

TEST_F(BpfQueueTest, MultiList) {
  arr_list all = ARR_LIST_HEAD_INITIALIZER;
  arr_list odds = ARR_LIST_HEAD_INITIALIZER;
  arr_list evens = ARR_LIST_HEAD_INITIALIZER;
  for (int i = 0; i < kNrElems; ++i) {
    arr_list_insert_tail(elems, kNrElems, &all, &elems[i], link);
    if (i % 2 == 1) {
      arr_list_insert_tail(elems, kNrElems, &odds, &elems[i], link2);
    } else {
      arr_list_insert_tail(elems, kNrElems, &evens, &elems[i], link3);
    }
  }
  EXPECT_EQ(count_list(&all), int(kNrElems));
  elem *e_i;
  int _max;
// clang-format off
// explicitly test not using {} on the for loop
  arr_list_foreach(e_i, elems, kNrElems, &odds, link2, _max, kNrElems)
    EXPECT_TRUE(e_i->x % 2 == 1);
  arr_list_foreach(e_i, elems, kNrElems, &evens, link3, _max, kNrElems)
    EXPECT_TRUE(e_i->x % 2 == 0);
// clang-format on
}

}  // namespace
}  // namespace ghost

int main(int argc, char **argv) {
  testing::InitGoogleMock(&argc, argv);

  return RUN_ALL_TESTS();
}
