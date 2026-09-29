#ifndef EVENTLOG_READER_H_
#define EVENTLOG_READER_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "eventlog/error.h"
#include "eventlog/file_ops.h"
#include "eventlog/record.h"

namespace eventlog {

// Outcome of scanning a log. Names match the result classes in docs/PRD.md section 4.
enum class LogStatus {
  kClean,                  // file is a whole number of valid records
  kIncompleteTail,         // trailing byte-prefix of a record; recoverable on request
  kCorruptCompleteRecord,  // full record with a bad CRC, or a full but invalid header
  kSequenceViolation,      // gap or duplicate in the sequence chain
};

std::string_view LogStatusName(LogStatus status);

// `verify` exit semantics from docs/PRD.md section 3: 0 clean, 1 incomplete tail,
// 2 hard corruption.
int LogStatusExitCode(LogStatus status);

struct ScanReport {
  LogStatus status = LogStatus::kClean;
  std::uint64_t valid_records = 0;
  std::uint64_t last_sequence = 0;
  // Offset just past the last fully valid record: the only legal truncation target.
  std::uint64_t valid_prefix_bytes = 0;
  std::uint64_t file_size = 0;
  // Where the problem starts. Meaningless when status == kClean.
  std::uint64_t problem_offset = 0;
  std::string detail;

  std::uint64_t trailing_bytes() const { return file_size - valid_prefix_bytes; }
  bool clean() const { return status == LogStatus::kClean; }
};

// Read-only scan of the whole file. The file is never modified or locked. Returns an
// Error only for I/O failures; structural damage is reported in the ScanReport, because
// "this log is broken" is an answer, not a failure to answer.
Result<ScanReport> ScanLog(const std::filesystem::path& path);

// Sequential record reader, for reading payloads back out of a log.
class LogReader {
 public:
  static Result<LogReader> Open(const std::filesystem::path& path);

  LogReader(LogReader&&) noexcept = default;
  LogReader& operator=(LogReader&&) noexcept = default;
  LogReader(const LogReader&) = delete;
  LogReader& operator=(const LogReader&) = delete;

  // std::nullopt at a clean end of file. An Error for damage, carrying the byte offset.
  Result<std::optional<Record>> Next();

  std::uint64_t records_read() const { return records_read_; }
  std::uint64_t offset() const { return offset_; }

 private:
  explicit LogReader(FileDescriptor fd, std::uint64_t file_size)
      : fd_(std::move(fd)), file_size_(file_size) {}

  FileDescriptor fd_;
  std::uint64_t file_size_ = 0;
  std::uint64_t offset_ = 0;
  std::uint64_t records_read_ = 0;
  std::uint64_t expected_sequence_ = kFirstSequence;
};

}  // namespace eventlog

#endif  // EVENTLOG_READER_H_
