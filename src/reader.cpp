#include "eventlog/reader.h"

#include <unistd.h>

#include <cerrno>
#include <string>
#include <utility>

namespace eventlog {
namespace {

// Positional read: pread leaves the descriptor's own offset alone, so a reader cannot
// perturb anything else holding the same file. Short reads happen only at end of file.
Result<std::size_t> ReadAt(int fd, std::uint64_t offset, std::span<std::byte> out) {
  std::size_t total = 0;
  while (total < out.size()) {
    const ssize_t got =
        ::pread(fd, out.data() + total, out.size() - total, static_cast<off_t>(offset + total));
    if (got < 0) {
      if (errno == EINTR) continue;
      return IoError("pread", errno, "");
    }
    if (got == 0) break;  // end of file
    total += static_cast<std::size_t>(got);
  }
  return total;
}

// Reads and fully validates the record at `offset`.
//
// The payload is delivered in the caller's scratch buffer so a scan of a large log does
// not allocate per record. Classification follows ADR-002: a short byte-prefix is a
// recoverable incomplete tail, while a complete-but-invalid record is hard corruption.
Result<RecordHeader> ReadRecordAt(int fd, std::uint64_t offset, std::uint64_t file_size,
                                  std::uint64_t expected_sequence,
                                  std::vector<std::byte>& payload) {
  const std::uint64_t remaining = file_size - offset;

  if (remaining < kHeaderSize) {
    return Error(ErrorKind::kIncompleteTail,
                 "trailing partial header: " + std::to_string(remaining) + " of " +
                     std::to_string(kHeaderSize) + " bytes")
        .at_offset(offset);
  }

  HeaderBytes header_bytes{};
  Result<std::size_t> header_read = ReadAt(fd, offset, std::span<std::byte>(header_bytes));
  if (!header_read.ok()) return header_read.error();
  if (header_read.value() < kHeaderSize) {
    return Error(ErrorKind::kIo, "file shrank while it was being read").at_offset(offset);
  }

  Result<RecordHeader> decoded = DecodeHeader(header_bytes);
  if (!decoded.ok()) {
    Error error = decoded.error();
    return error.at_offset(offset);
  }
  const RecordHeader header = decoded.value();

  // The length is checked against the bytes actually present before any buffer is sized
  // from it, so a damaged or hostile length cannot cause a large allocation.
  if (remaining < header.total_size()) {
    return Error(ErrorKind::kIncompleteTail, "trailing partial payload for sequence " +
                                                 std::to_string(header.sequence) + ": " +
                                                 std::to_string(remaining - kHeaderSize) + " of " +
                                                 std::to_string(header.payload_length) + " bytes")
        .at_offset(offset);
  }

  payload.assign(header.payload_length, std::byte{0});
  if (header.payload_length > 0) {
    Result<std::size_t> payload_read =
        ReadAt(fd, offset + kHeaderSize, std::span<std::byte>(payload));
    if (!payload_read.ok()) return payload_read.error();
    if (payload_read.value() < header.payload_length) {
      return Error(ErrorKind::kIo, "file shrank while it was being read").at_offset(offset);
    }
  }

  // CRC before sequence: a record that failed its checksum cannot be trusted to report
  // its own sequence number.
  Status crc = VerifyRecordCrc(header, payload);
  if (!crc.ok()) {
    Error error = crc.error();
    return error.at_offset(offset);
  }

  if (header.sequence != expected_sequence) {
    return Error(ErrorKind::kSequenceViolation, "expected sequence " +
                                                    std::to_string(expected_sequence) +
                                                    " but found " + std::to_string(header.sequence))
        .at_offset(offset);
  }

  return header;
}

}  // namespace

std::string_view LogStatusName(LogStatus status) {
  switch (status) {
    case LogStatus::kClean:
      return "clean";
    case LogStatus::kIncompleteTail:
      return "incomplete_tail";
    case LogStatus::kCorruptCompleteRecord:
      return "corrupt_complete_record";
    case LogStatus::kSequenceViolation:
      return "sequence_violation";
  }
  return "unknown";
}

int LogStatusExitCode(LogStatus status) {
  switch (status) {
    case LogStatus::kClean:
      return 0;
    case LogStatus::kIncompleteTail:
      return 1;
    case LogStatus::kCorruptCompleteRecord:
    case LogStatus::kSequenceViolation:
      return 2;
  }
  return 2;
}

Result<ScanReport> ScanLog(const std::filesystem::path& path) {
  Result<FileDescriptor> opened = OpenLogForRead(path);
  if (!opened.ok()) return opened.error();
  const FileDescriptor fd = std::move(opened).value();

  PosixFileOps ops;
  Result<std::uint64_t> size = ops.FileSize(fd.get());
  if (!size.ok()) return size.error();

  ScanReport report;
  report.file_size = size.value();

  std::uint64_t offset = 0;
  std::uint64_t expected_sequence = kFirstSequence;
  std::vector<std::byte> payload;

  while (offset < report.file_size) {
    Result<RecordHeader> header =
        ReadRecordAt(fd.get(), offset, report.file_size, expected_sequence, payload);
    if (!header.ok()) {
      const Error& error = header.error();
      report.problem_offset = error.offset().value_or(offset);
      report.detail = error.context();
      switch (error.kind()) {
        case ErrorKind::kIncompleteTail:
          report.status = LogStatus::kIncompleteTail;
          return report;
        case ErrorKind::kCorruptRecord:
          report.status = LogStatus::kCorruptCompleteRecord;
          return report;
        case ErrorKind::kSequenceViolation:
          report.status = LogStatus::kSequenceViolation;
          return report;
        default:
          // I/O failures and unsupported format versions are not a verdict about the
          // log's contents, so they propagate as errors instead of as a status.
          return error;
      }
    }

    offset += header.value().total_size();
    report.valid_prefix_bytes = offset;
    report.last_sequence = header.value().sequence;
    ++report.valid_records;
    expected_sequence = header.value().sequence + 1;
  }

  report.status = LogStatus::kClean;
  report.problem_offset = 0;
  report.detail.clear();
  return report;
}

Result<LogReader> LogReader::Open(const std::filesystem::path& path) {
  Result<FileDescriptor> opened = OpenLogForRead(path);
  if (!opened.ok()) return opened.error();
  FileDescriptor fd = std::move(opened).value();

  PosixFileOps ops;
  Result<std::uint64_t> size = ops.FileSize(fd.get());
  if (!size.ok()) return size.error();

  return LogReader(std::move(fd), size.value());
}

Result<std::optional<Record>> LogReader::Next() {
  if (offset_ >= file_size_) return std::optional<Record>{};

  std::vector<std::byte> payload;
  Result<RecordHeader> header =
      ReadRecordAt(fd_.get(), offset_, file_size_, expected_sequence_, payload);
  if (!header.ok()) return header.error();

  Record record;
  record.sequence = header.value().sequence;
  record.payload = std::move(payload);

  offset_ += header.value().total_size();
  expected_sequence_ = header.value().sequence + 1;
  ++records_read_;
  return std::optional<Record>{std::move(record)};
}

}  // namespace eventlog
