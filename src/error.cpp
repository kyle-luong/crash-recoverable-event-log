#include "eventlog/error.h"

#include <cstring>
#include <string>

namespace eventlog {

std::string_view ErrorKindName(ErrorKind kind) {
  switch (kind) {
    case ErrorKind::kIo:
      return "io_error";
    case ErrorKind::kCorruptRecord:
      return "corrupt_complete_record";
    case ErrorKind::kIncompleteTail:
      return "incomplete_tail";
    case ErrorKind::kSequenceViolation:
      return "sequence_violation";
    case ErrorKind::kUnsupportedVersion:
      return "unsupported_version";
    case ErrorKind::kLockUnavailable:
      return "lock_unavailable";
    case ErrorKind::kUnknownDurability:
      return "unknown_durability";
    case ErrorKind::kInvalidArgument:
      return "invalid_argument";
    case ErrorKind::kLimitExceeded:
      return "limit_exceeded";
    case ErrorKind::kUnsupportedPolicy:
      return "unsupported_policy";
    case ErrorKind::kInternal:
      return "internal_error";
  }
  return "unknown";
}

std::string Error::message() const {
  std::string out(ErrorKindName(kind_));
  out += ": ";
  out += context_;
  if (offset_.has_value()) {
    out += " (at offset ";
    out += std::to_string(*offset_);
    out += ")";
  }
  if (errno_value_ != 0) {
    out += ": ";
    out += std::strerror(errno_value_);
    out += " (errno ";
    out += std::to_string(errno_value_);
    out += ")";
  }
  return out;
}

Error IoError(std::string_view syscall, int errno_value, std::string_view context) {
  std::string description(syscall);
  description += " failed";
  if (!context.empty()) {
    description += " on ";
    description += context;
  }
  return Error(ErrorKind::kIo, std::move(description), errno_value);
}

}  // namespace eventlog
