#include "lib/trivial_status.h"

namespace ghost {

namespace {

template <size_t ArraySize>
void CopyString(absl::string_view s, std::array<char, ArraySize>& dest) {
  static_assert(ArraySize > 0);
  const size_t chars_to_copy = std::min(ArraySize - 1, s.size());
  std::copy_n(s.begin(), chars_to_copy, dest.begin());
  dest[std::min(chars_to_copy, ArraySize - 1)] = '\0';
}

}  // namespace

TrivialStatus::TrivialStatus(const absl::Status& s) {
  code_ = s.code();

  CopyString<1000>(s.message(), error_message_);
}

}  // namespace ghost
