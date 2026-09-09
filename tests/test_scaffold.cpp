// CPP-00: the scaffold's own verification. These cases exist so that a passing
// ./scripts/verify.sh means something before any log logic is written: the harness
// registers and reports cases, the error type carries syscall context, and Result/Status
// propagate a value or an error without either one silently becoming the other.

#include <cerrno>
#include <memory>
#include <string>

#include "eventlog/error.h"
#include "test_support.h"

namespace eventlog {
namespace {

TEST(StatusDefaultsToOk) {
  const Status ok;
  EL_CHECK(ok.ok());
  EL_EXPECT_STATUS_OK(Status::Ok());
}

TEST(StatusCarriesAnError) {
  const Status failed = Error(ErrorKind::kInvalidArgument, "bad input");
  EL_CHECK(!failed.ok());
  EL_CHECK_EQ(failed.error().kind(), ErrorKind::kInvalidArgument);
  EL_CHECK(failed.error().message().find("bad input") != std::string::npos);
}

TEST(ResultHoldsEitherAValueOrAnError) {
  const Result<int> value = 42;
  EL_CHECK(value.ok());
  EL_CHECK_EQ(value.value(), 42);
  EL_CHECK(value.status().ok());

  const Result<int> failed = Error(ErrorKind::kLimitExceeded, "too big");
  EL_CHECK(!failed.ok());
  EL_CHECK_EQ(failed.error().kind(), ErrorKind::kLimitExceeded);
  EL_CHECK(!failed.status().ok());
}

TEST(ResultSupportsMoveOnlyTypes) {
  // The writer will be returned by value from a factory, so this has to work.
  Result<std::unique_ptr<int>> owned = std::make_unique<int>(7);
  EL_CHECK(owned.ok());
  const std::unique_ptr<int> taken = std::move(owned).value();
  EL_CHECK_EQ(*taken, 7);
}

TEST(IoErrorRecordsSyscallAndErrno) {
  const Error error = IoError("write", ENOSPC, "events.log");
  EL_CHECK_EQ(error.kind(), ErrorKind::kIo);
  EL_CHECK_EQ(error.errno_value(), ENOSPC);
  const std::string message = error.message();
  // A storage fault must be diagnosable from the message alone.
  EL_CHECK(message.find("write") != std::string::npos);
  EL_CHECK(message.find("events.log") != std::string::npos);
  EL_CHECK(message.find("errno") != std::string::npos);
}

TEST(ErrorOffsetIsReportedWhenSet) {
  Error error(ErrorKind::kCorruptRecord, "checksum mismatch");
  EL_CHECK(!error.offset().has_value());
  EL_CHECK(error.message().find("offset") == std::string::npos);

  error.at_offset(4096);
  EL_CHECK(error.offset().has_value());
  EL_CHECK_EQ(*error.offset(), std::uint64_t{4096});
  EL_CHECK(error.message().find("offset 4096") != std::string::npos);
}

TEST(EveryErrorKindHasAStableName) {
  // These names are the PRD's result classes and appear in CLI output and JSON, so an
  // unnamed kind would leak "unknown" to a user.
  const ErrorKind kinds[] = {ErrorKind::kIo,
                             ErrorKind::kCorruptRecord,
                             ErrorKind::kIncompleteTail,
                             ErrorKind::kSequenceViolation,
                             ErrorKind::kUnsupportedVersion,
                             ErrorKind::kLockUnavailable,
                             ErrorKind::kUnknownDurability,
                             ErrorKind::kInvalidArgument,
                             ErrorKind::kLimitExceeded,
                             ErrorKind::kUnsupportedPolicy,
                             ErrorKind::kInternal};
  for (const ErrorKind kind : kinds) {
    const std::string name(ErrorKindName(kind));
    EL_CHECK(!name.empty());
    EL_CHECK_NE(name, std::string("unknown"));
  }
}

TEST(ScratchDirectoryIsCreatedAndUsable) {
  // Tests must only ever write inside one of these.
  std::filesystem::path recorded;
  {
    const test::TempDir dir("scaffold");
    recorded = dir.path();
    EL_CHECK(std::filesystem::exists(recorded));
    test::WriteFileBytes(dir.file("probe.bin"), {std::byte{1}, std::byte{2}});
    EL_CHECK_EQ(test::FileSizeOf(dir.file("probe.bin")), std::uint64_t{2});
  }
  // And it must clean up after itself.
  EL_CHECK(!std::filesystem::exists(recorded));
}

}  // namespace
}  // namespace eventlog
