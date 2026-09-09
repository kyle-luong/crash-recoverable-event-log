#ifndef EVENTLOG_ERROR_H_
#define EVENTLOG_ERROR_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace eventlog {

// The result classes required by docs/PRD.md section 4, plus the argument and bound
// cases the CLI needs. Every failure in this library maps to exactly one of these.
enum class ErrorKind {
  kIo,                  // syscall failure; errno_value is set
  kCorruptRecord,       // complete record with a bad CRC or an invalid full header
  kIncompleteTail,      // trailing record is a strict byte-prefix; recoverable
  kSequenceViolation,   // gap or duplicate in the sequence chain
  kUnsupportedVersion,  // format version this build does not implement
  kLockUnavailable,     // another cooperating writer holds the advisory lock
  kUnknownDurability,   // a sync failed: persistence of recent records is unknown
  kInvalidArgument,     // caller or CLI passed something outside the contract
  kLimitExceeded,       // payload or count above a fixed bound
  kUnsupportedPolicy,   // policy exists in the enum but is not implemented yet
  kInternal,
};

std::string_view ErrorKindName(ErrorKind kind);

// Carries enough context to diagnose a storage fault without a debugger: what failed,
// the errno if a syscall was involved, and where in the file it happened.
class Error {
 public:
  Error(ErrorKind kind, std::string context) : kind_(kind), context_(std::move(context)) {}
  Error(ErrorKind kind, std::string context, int errno_value)
      : kind_(kind), context_(std::move(context)), errno_value_(errno_value) {}

  ErrorKind kind() const { return kind_; }
  const std::string& context() const { return context_; }
  int errno_value() const { return errno_value_; }
  std::optional<std::uint64_t> offset() const { return offset_; }

  Error& at_offset(std::uint64_t offset) {
    offset_ = offset;
    return *this;
  }

  // e.g. "io: write(events.log) at offset 4096: No space left on device (errno 28)"
  std::string message() const;

 private:
  ErrorKind kind_;
  std::string context_;
  int errno_value_ = 0;
  std::optional<std::uint64_t> offset_;
};

Error IoError(std::string_view syscall, int errno_value, std::string_view context);

// Either success, or an Error. Deliberately not std::expected: that is C++23, and
// docs/PRD.md section 3 asks for a small local type rather than a framework.
class Status {
 public:
  Status() = default;
  Status(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  static Status Ok() { return Status(); }

  bool ok() const { return !error_.has_value(); }
  // Precondition: !ok().
  const Error& error() const { return *error_; }

 private:
  std::optional<Error> error_;
};

// Either a value of type T, or an Error. Exactly one is always populated.
template <typename T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}      // NOLINT(google-explicit-constructor)
  Result(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  bool ok() const { return value_.has_value(); }

  // Preconditions: ok() for value(), !ok() for error().
  T& value() & { return *value_; }
  const T& value() const& { return *value_; }
  T&& value() && { return std::move(*value_); }
  const Error& error() const { return *error_; }

  Status status() const { return ok() ? Status::Ok() : Status(*error_); }

 private:
  std::optional<T> value_;
  std::optional<Error> error_;
};

}  // namespace eventlog

#endif  // EVENTLOG_ERROR_H_
