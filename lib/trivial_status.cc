#include "lib/trivial_status.h"
#include "absl/strings/str_format.h"

namespace ghost {

namespace {

template <size_t ArraySize>
void CopyString(std::array<char, ArraySize>& dest, absl::string_view s) {
  static_assert(ArraySize > 0);
  const size_t chars_to_copy = std::min(ArraySize - 1, s.size());
  if (chars_to_copy < s.size()) {
    absl::FPrintF(stderr,
                  "Source string too large to fit in TrivialStatus: %zu, vs "
                  "max_size %zu\n",
                  s.size(), chars_to_copy);
  }
  std::copy_n(s.begin(), chars_to_copy, dest.begin());
  dest[chars_to_copy] = '\0';
}

}  // namespace

TrivialStatus::TrivialStatus(const absl::Status& s) {
  code_ = s.code();

  CopyString<kMaxErrorMessageSize>(error_message_, s.message());
}

TrivialStatusOrString::TrivialStatusOrString(
    const absl::StatusOr<std::string>& s)
    : status_(TrivialStatus(s.status())) {
  if (s.ok()) {
    string_length_ = s.value().size();
    CopyString<kMaxStringSize>(str_, s.value());
  }
}

absl::StatusOr<std::string> TrivialStatusOrString::ToStatusOr() const
{
  absl::Status s = status_.ToStatus();
  if (s.ok()) {
    return std::string(str_.data(), string_length_);
  }
  return s;
}

}  // namespace ghost
