// CPP-T06 and the read-only half of CPP-AC08/AC15: appended records must come back in
// order, byte for byte, and a scan must report the log's shape accurately.

#include "eventlog/reader.h"

#include <sys/stat.h>

#include <cerrno>
#include <memory>
#include <vector>

#include "eventlog/payload.h"
#include "eventlog/writer.h"
#include "scripted_file_ops.h"
#include "test_support.h"

namespace eventlog {
namespace {

void BuildLog(const std::filesystem::path& path, std::uint64_t records, std::size_t payload_bytes) {
  EventLogWriter writer = EventLogWriter::Open(path, WriterOptions{}).value();
  for (std::uint64_t i = 1; i <= records; ++i) {
    if (!writer.AppendAndSync(MakeDeterministicPayload(i, payload_bytes)).ok()) {
      throw test::AssertionFailure("append failed while building a fixture");
    }
  }
  if (!writer.Close().ok()) throw test::AssertionFailure("close failed");
}

TEST(ReadsBackOneThousandRecordsExactly) {
  const test::TempDir dir("reader-roundtrip");
  const auto path = dir.file("events.log");
  constexpr std::uint64_t kRecords = 1000;
  constexpr std::size_t kPayloadBytes = 97;  // deliberately not a power of two

  {
    EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, WriterOptions{}));
    for (std::uint64_t i = 1; i <= kRecords; ++i) {
      EL_EXPECT_OK(writer.AppendAndSync(MakeDeterministicPayload(i, kPayloadBytes)));
    }
    EL_EXPECT_STATUS_OK(writer.Close());
  }

  LogReader reader = EL_EXPECT_OK(LogReader::Open(path));
  std::uint64_t seen = 0;
  while (true) {
    std::optional<Record> record = EL_EXPECT_OK(reader.Next());
    if (!record.has_value()) break;
    ++seen;
    // Order is part of the contract, not an accident of iteration.
    EL_CHECK_EQ(record->sequence, seen);
    EL_CHECK_EQ(record->payload.size(), kPayloadBytes);

    const std::vector<std::byte> expected = MakeDeterministicPayload(seen, kPayloadBytes);
    for (std::size_t i = 0; i < kPayloadBytes; ++i) {
      if (record->payload[i] != expected[i]) {
        EL_FAIL("record " + std::to_string(seen) + " byte " + std::to_string(i) + " differs");
      }
    }
  }
  EL_CHECK_EQ(seen, kRecords);
  EL_CHECK_EQ(reader.records_read(), kRecords);
  EL_CHECK_EQ(reader.offset(), test::FileSizeOf(path));
}

TEST(HandlesZeroLengthPayloads) {
  const test::TempDir dir("reader-empty-payloads");
  const auto path = dir.file("events.log");

  {
    EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, WriterOptions{}));
    for (int i = 0; i < 5; ++i) EL_EXPECT_OK(writer.AppendAndSync({}));
    EL_EXPECT_STATUS_OK(writer.Close());
  }

  EL_CHECK_EQ(test::FileSizeOf(path), std::uint64_t{5 * kHeaderSize});
  const ScanReport report = EL_EXPECT_OK(ScanLog(path));
  EL_CHECK_EQ(report.status, LogStatus::kClean);
  EL_CHECK_EQ(report.valid_records, std::uint64_t{5});

  LogReader reader = EL_EXPECT_OK(LogReader::Open(path));
  const std::optional<Record> first = EL_EXPECT_OK(reader.Next());
  EL_CHECK(first.has_value());
  EL_CHECK(first->payload.empty());
}

TEST(HandlesMaximumPayload) {
  const test::TempDir dir("reader-max-payload");
  const auto path = dir.file("events.log");

  {
    EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, WriterOptions{}));
    EL_EXPECT_OK(writer.AppendAndSync(MakeDeterministicPayload(1, kMaxPayloadBytes)));
    EL_EXPECT_STATUS_OK(writer.Close());
  }

  const ScanReport report = EL_EXPECT_OK(ScanLog(path));
  EL_CHECK_EQ(report.status, LogStatus::kClean);
  EL_CHECK_EQ(report.file_size, std::uint64_t{kHeaderSize} + kMaxPayloadBytes);

  LogReader reader = EL_EXPECT_OK(LogReader::Open(path));
  const std::optional<Record> record = EL_EXPECT_OK(reader.Next());
  EL_CHECK(record.has_value());
  EL_CHECK_EQ(record->payload.size(), std::size_t{kMaxPayloadBytes});
  const std::vector<std::byte> expected = MakeDeterministicPayload(1, kMaxPayloadBytes);
  EL_CHECK(record->payload == expected);
}

TEST(EmptyFileIsClean) {
  const test::TempDir dir("reader-empty-file");
  const auto path = dir.file("events.log");
  test::WriteFileBytes(path, {});

  const ScanReport report = EL_EXPECT_OK(ScanLog(path));
  EL_CHECK_EQ(report.status, LogStatus::kClean);
  EL_CHECK_EQ(report.valid_records, std::uint64_t{0});
  EL_CHECK_EQ(report.last_sequence, std::uint64_t{0});
  EL_CHECK_EQ(report.valid_prefix_bytes, std::uint64_t{0});
  EL_CHECK_EQ(report.file_size, std::uint64_t{0});

  LogReader reader = EL_EXPECT_OK(LogReader::Open(path));
  EL_CHECK(!EL_EXPECT_OK(reader.Next()).has_value());
}

TEST(MissingFileIsAnIoError) {
  const test::TempDir dir("reader-missing");
  EL_EXPECT_ERROR(ScanLog(dir.file("absent.log")), ErrorKind::kIo);
  EL_EXPECT_ERROR(LogReader::Open(dir.file("absent.log")), ErrorKind::kIo);
}

TEST(ScanReportDescribesACleanLogPrecisely) {
  const test::TempDir dir("reader-report");
  const auto path = dir.file("events.log");
  constexpr std::size_t kPayloadBytes = 40;
  constexpr std::uint64_t kRecords = 7;

  {
    EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, WriterOptions{}));
    for (std::uint64_t i = 1; i <= kRecords; ++i) {
      EL_EXPECT_OK(writer.AppendAndSync(MakeDeterministicPayload(i, kPayloadBytes)));
    }
    EL_EXPECT_STATUS_OK(writer.Close());
  }

  const std::uint64_t expected_size = kRecords * (kHeaderSize + kPayloadBytes);
  const ScanReport report = EL_EXPECT_OK(ScanLog(path));
  EL_CHECK_EQ(report.status, LogStatus::kClean);
  EL_CHECK_EQ(report.valid_records, kRecords);
  EL_CHECK_EQ(report.last_sequence, kRecords);
  EL_CHECK_EQ(report.file_size, expected_size);
  // A clean log has no trailing bytes: the valid prefix is the whole file.
  EL_CHECK_EQ(report.valid_prefix_bytes, expected_size);
  EL_CHECK_EQ(report.trailing_bytes(), std::uint64_t{0});
  EL_CHECK(report.detail.empty());
  EL_CHECK(report.clean());
}

TEST(DeterministicPayloadsAreStableAndSequenceDependent) {
  // The crash harness and the benchmark both depend on regenerating identical bytes.
  const std::vector<std::byte> a = MakeDeterministicPayload(42, 128);
  const std::vector<std::byte> b = MakeDeterministicPayload(42, 128);
  EL_CHECK(a == b);

  const std::vector<std::byte> other = MakeDeterministicPayload(43, 128);
  EL_CHECK(a != other);

  // A shorter request is a prefix of a longer one, so partial records stay comparable.
  const std::vector<std::byte> shorter = MakeDeterministicPayload(42, 64);
  for (std::size_t i = 0; i < shorter.size(); ++i) EL_CHECK_EQ(shorter[i], a[i]);

  EL_CHECK(MakeDeterministicPayload(1, 0).empty());
}

TEST(ExitCodesMatchThePrdTable) {
  EL_CHECK_EQ(LogStatusExitCode(LogStatus::kClean), 0);
  EL_CHECK_EQ(LogStatusExitCode(LogStatus::kIncompleteTail), 1);
  EL_CHECK_EQ(LogStatusExitCode(LogStatus::kCorruptCompleteRecord), 2);
  EL_CHECK_EQ(LogStatusExitCode(LogStatus::kSequenceViolation), 2);
  EL_CHECK_EQ(std::string(LogStatusName(LogStatus::kClean)), std::string("clean"));
}

// --- an I/O error is not a verdict about the log --------------------------------
//
// `ScanLog` answers "what shape is this log" and reserves Errors for "I could not find
// out". Confusing the two would be the worst kind of wrong answer this tool can give: a
// failing disk reported as corrupt data sends someone to the wrong problem, and a user
// told their log is damaged may discard it. These cases exist because the distinction is
// load-bearing and was previously untestable -- the read path did not go through the
// injectable boundary at all.

TEST(AReadFailureIsReportedAsIoNeverAsCorruption) {
  const test::TempDir dir("reader-read-fault");
  const auto path = dir.file("events.log");
  BuildLog(path, 5, 32);

  auto ops = std::make_shared<test::ScriptedFileOps>();
  ops->fail_read_after = 0;  // the very first header read fails
  Result<ScanReport> scanned = ScanLog(path, ops);
  EL_CHECK(!scanned.ok());
  EL_CHECK_EQ(scanned.error().kind(), ErrorKind::kIo);
  EL_CHECK_EQ(scanned.error().errno_value(), EIO);
}

TEST(AReadFailureMidRecordIsStillIo) {
  const test::TempDir dir("reader-read-fault-payload");
  const auto path = dir.file("events.log");
  BuildLog(path, 5, 64);

  // The header read succeeds and the payload read fails, so the failure lands after a
  // valid header has been decoded -- the point where a bad classification is most
  // tempting, because the record looks real and its bytes will not come back.
  auto ops = std::make_shared<test::ScriptedFileOps>();
  ops->fail_read_after = 1;
  Result<ScanReport> scanned = ScanLog(path, ops);
  EL_CHECK(!scanned.ok());
  EL_CHECK_EQ(scanned.error().kind(), ErrorKind::kIo);
  EL_CHECK(scanned.error().offset().has_value());
}

TEST(ALaterReadFailureDoesNotRetractTheRecordsAlreadyRead) {
  const test::TempDir dir("reader-read-fault-late");
  const auto path = dir.file("events.log");
  BuildLog(path, 6, 16);

  // Two reads per record, so failing after six lets three records through.
  auto ops = std::make_shared<test::ScriptedFileOps>();
  ops->fail_read_after = 6;
  Result<ScanReport> scanned = ScanLog(path, ops);
  EL_CHECK(!scanned.ok());
  EL_CHECK_EQ(scanned.error().kind(), ErrorKind::kIo);
  // And nothing was reported as a status: a partial scan is not a verdict.
}

TEST(ShortReadsAreLoopedOverRatherThanTreatedAsEof) {
  const test::TempDir dir("reader-short-reads");
  const auto path = dir.file("events.log");
  BuildLog(path, 4, 100);

  // Seven bytes at a time: every header and every payload needs several reads. A loop
  // that mistook a short read for end of file would report an incomplete tail here.
  auto ops = std::make_shared<test::ScriptedFileOps>();
  ops->max_read_bytes = 7;
  const ScanReport report = EL_EXPECT_OK(ScanLog(path, ops));
  EL_CHECK(report.clean());
  EL_CHECK_EQ(report.valid_records, std::uint64_t{4});
  EL_CHECK_EQ(report.last_sequence, std::uint64_t{4});
  EL_CHECK(ops->read_calls > 8);
}

TEST(LogReaderSurfacesAReadFailure) {
  const test::TempDir dir("reader-next-fault");
  const auto path = dir.file("events.log");
  BuildLog(path, 3, 24);

  auto ops = std::make_shared<test::ScriptedFileOps>();
  ops->fail_read_after = 2;  // the second record's header read fails
  LogReader reader = EL_EXPECT_OK(LogReader::Open(path, ops));
  EL_CHECK(EL_EXPECT_OK(reader.Next()).has_value());
  Result<std::optional<Record>> second = reader.Next();
  EL_CHECK(!second.ok());
  EL_CHECK_EQ(second.error().kind(), ErrorKind::kIo);
}

// --- a log must be a regular file -------------------------------------------

TEST(ADirectoryIsNotALog) {
  const test::TempDir dir("reader-directory");
  Result<ScanReport> scanned = ScanLog(dir.path());
  EL_CHECK(!scanned.ok());
  EL_CHECK_EQ(scanned.error().kind(), ErrorKind::kInvalidArgument);
  EL_CHECK(scanned.error().message().find("regular file") != std::string::npos);
}

// A named pipe has no size, no offsets and no truncation, so none of the format's
// operations mean anything on one. Opening it must refuse rather than block waiting for
// a peer, which is what O_NONBLOCK on every log open is for.
TEST(ANamedPipeIsRefusedRatherThanBlocking) {
  const test::TempDir dir("reader-fifo");
  const auto path = dir.file("pipe.log");
  if (::mkfifo(path.c_str(), 0600) != 0) return;  // filesystem without FIFOs

  Result<ScanReport> scanned = ScanLog(path);
  EL_CHECK(!scanned.ok());
  EL_CHECK_EQ(scanned.error().kind(), ErrorKind::kInvalidArgument);

  Result<EventLogWriter> opened = EventLogWriter::Open(path, WriterOptions{});
  EL_CHECK(!opened.ok());
  // ENXIO from the kernel or the regular-file check; either is a refusal, and neither
  // hangs.
  EL_CHECK(opened.error().kind() == ErrorKind::kInvalidArgument ||
           opened.error().kind() == ErrorKind::kIo);
}

}  // namespace
}  // namespace eventlog
