#ifndef GHOST_LIB_TRIVIAL_STATUS_H_
#define GHOST_LIB_TRIVIAL_STATUS_H_

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace ghost {

// This is a trivially copyable version of absl::Status. This is useful
// because it can be serialized across the shared memory AgentRpcBuffer.
class TrivialStatus {
 public:
  explicit TrivialStatus() : TrivialStatus(absl::OkStatus()) {}
  explicit TrivialStatus(const absl::Status& s);

  // Returns the absl::Status version of this object.
  absl::Status ToStatus() const {
    return absl::Status(code_, std::string(error_message_.data()));
  }

  bool ok() const { return code_ == absl::StatusCode::kOk; }

 private:
  absl::StatusCode code_;

  // Sized large enough to handle most error messages. Must fit in
  // AgentRpcBuffer BufferBytes.
  std::array<char, 1000> error_message_;
};

// This is a trivially copyable version of absl::StatusOr. This is useful
// because it can be serialized across the shared memory AgentRpcBuffer.
template <typename T>
class TrivialStatusOr {
 public:
  explicit TrivialStatusOr() : status_(TrivialStatus(absl::OkStatus())) {}

  // Constructs a TrivialStatusOr from an error status.
  explicit TrivialStatusOr(const absl::Status& s) : status_(TrivialStatus(s)) {
    CHECK(!s.ok());
  }

  explicit TrivialStatusOr(const T& val)
      : status_(TrivialStatus(absl::OkStatus())) {
    value_ = val;
  }

  explicit TrivialStatusOr(const absl::StatusOr<T>& s)
      : status_(TrivialStatus(s.status())) {
    if (s.ok()) {
      value_ = s.value();
    }
  }

  // Returns the absl::StatusOr<T> version of this object.
  absl::StatusOr<T> ToStatusOr() const {
    absl::Status s = status_.ToStatus();
    if (s.ok()) {
      return value_;
    }
    return s;
  }

  bool ok() const { return status_.ok(); }

 private:
  TrivialStatus status_;

  // If the status is OK, this stores the contained value.
  T value_;
};

}  // namespace ghost

#endif  // GHOST_LIB_TRIVIAL_STATUS_H_
