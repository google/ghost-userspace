// Copyright 2022 Google LLC
//
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT.

#ifndef GHOST_THIRD_PARTY_UTIL_UTIL_H_
#define GHOST_THIRD_PARTY_UTIL_UTIL_H_

#include <type_traits>

// The code below is derived from
// https://stackoverflow.com/questions/34672441/stdis-base-of-for-template-classes.
template <template <typename...> class Base, typename Derived>
struct is_base_of_template_impl {
  template <typename... Ts>
  static constexpr std::true_type Test(const Base<Ts...>*);
  static constexpr std::false_type Test(...);
  using type = decltype(Test(std::declval<Derived*>()));
};

template <template <typename...> class Base, typename Derived>
inline constexpr bool is_base_of_template_v =
    is_base_of_template_impl<Base, Derived>::type::value;

#endif  // GHOST_THIRD_PARTY_UTIL_UTIL_H_
