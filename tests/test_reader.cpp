// CPP-T06 and the read-only half of CPP-AC08/AC15: appended records must come back in
// order, byte for byte, and a scan must report the log's shape accurately.

#include "eventlog/reader.h"

#include <vector>

#include "eventlog/payload.h"
#include "eventlog/writer.h"
#include "test_support.h"

namespace eventlog {
namespace {

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

}  // namespace
}  // namespace eventlog
