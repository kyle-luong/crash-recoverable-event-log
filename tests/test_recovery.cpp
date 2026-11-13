// CPP-AC08, CPP-AC09, CPP-AC15 and tests CPP-T08 to CPP-T10.
//
// The dividing line under test is ADR-002: a trailing byte-prefix is recoverable, and
// anything else is reported with its offset and left strictly alone.

#include "eventlog/recover.h"

#include <utility>
#include <vector>

#include "eventlog/endian.h"
#include "eventlog/payload.h"
#include "eventlog/record.h"
#include "eventlog/writer.h"
#include "test_support.h"

namespace eventlog {
namespace {

// Writes records straight to disk so a test can manufacture states a correct writer
// would never produce, such as a sequence gap.
void BuildRawLog(const std::filesystem::path& path,
                 const std::vector<std::pair<std::uint64_t, std::size_t>>& records) {
  std::vector<std::byte> file;
  for (const auto& [sequence, size] : records) {
    const std::vector<std::byte> encoded =
        EncodeRecord(sequence, MakeDeterministicPayload(sequence, size)).value();
    file.insert(file.end(), encoded.begin(), encoded.end());
  }
  test::WriteFileBytes(path, file);
}

void BuildLogWithWriter(const std::filesystem::path& path, std::uint64_t records,
                        std::size_t payload_bytes) {
  EventLogWriter writer = EventLogWriter::Open(path, WriterOptions{}).value();
  for (std::uint64_t i = 1; i <= records; ++i) {
    const AppendResult result =
        writer.AppendAndSync(MakeDeterministicPayload(i, payload_bytes)).value();
    if (result.sequence != i) throw test::AssertionFailure("unexpected sequence while building");
  }
  const Status closed = writer.Close();
  if (!closed.ok()) throw test::AssertionFailure("close failed: " + closed.error().message());
}

// --- CPP-T08: every possible cut point of a trailing record ------------------

// Cuts the final record at every byte offset and checks that each cut is classified as a
// recoverable tail, planned against the same truncation target, and recovers to the same
// clean prefix.
void ExerciseEveryTailCut(std::size_t payload_bytes, std::uint64_t records) {
  const test::TempDir dir("recovery-tail-cuts");
  const auto pristine = dir.file("pristine.log");
  BuildLogWithWriter(pristine, records, payload_bytes);

  const std::vector<std::byte> whole = test::ReadFileBytes(pristine);
  const std::uint64_t record_size = kHeaderSize + payload_bytes;
  const std::uint64_t prefix_bytes = (records - 1) * record_size;

  for (std::uint64_t cut = 1; cut < record_size; ++cut) {
    const auto path = dir.file("cut.log");
    std::vector<std::byte> truncated(
        whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(prefix_bytes + cut));
    test::WriteFileBytes(path, truncated);

    const ScanReport report = EL_EXPECT_OK(ScanLog(path));
    if (report.status != LogStatus::kIncompleteTail) {
      EL_FAIL("cut at +" + std::to_string(cut) + " of the last record gave status " +
              std::string(LogStatusName(report.status)) + ", expected incomplete_tail");
    }
    EL_CHECK_EQ(report.valid_records, records - 1);
    EL_CHECK_EQ(report.last_sequence, records - 1);
    EL_CHECK_EQ(report.valid_prefix_bytes, prefix_bytes);
    EL_CHECK_EQ(report.problem_offset, prefix_bytes);
    EL_CHECK_EQ(report.trailing_bytes(), cut);

    const RecoveryPlan plan = EL_EXPECT_OK(PlanRecovery(path));
    EL_CHECK(plan.truncation_required);
    EL_CHECK_EQ(plan.truncate_to, prefix_bytes);
    EL_CHECK_EQ(plan.bytes_to_discard, cut);

    // The preflight must not have changed anything.
    EL_CHECK(test::ReadFileBytes(path) == truncated);

    const RecoveryOutcome outcome = EL_EXPECT_OK(ApplyRecovery(path, plan, *DefaultFileOps()));
    EL_CHECK_EQ(outcome.bytes_discarded, cut);
    EL_CHECK_EQ(outcome.valid_records, records - 1);
    EL_CHECK_EQ(test::FileSizeOf(path), prefix_bytes);

    const ScanReport after = EL_EXPECT_OK(ScanLog(path));
    EL_CHECK_EQ(after.status, LogStatus::kClean);
    EL_CHECK_EQ(after.valid_records, records - 1);

    // Recovery kept the surviving records byte-identical; it only removed the tail.
    const std::vector<std::byte> recovered = test::ReadFileBytes(path);
    for (std::uint64_t i = 0; i < prefix_bytes; ++i) {
      if (recovered[i] != whole[i]) EL_FAIL("recovery altered a surviving byte");
    }
    std::filesystem::remove(path);
  }
}

TEST(EveryTailCutOfASmallRecordIsRecoverable) {
  ExerciseEveryTailCut(32, 3);
}

TEST(EveryTailCutOfAZeroLengthRecordIsRecoverable) {
  // A header-only record has no payload bytes, so only header cuts exist.
  ExerciseEveryTailCut(0, 3);
}

#ifdef EVENTLOG_SLOW_TESTS
TEST(EveryTailCutOfAPageSizedRecordIsRecoverable) {
  ExerciseEveryTailCut(4096, 2);
}
#endif

TEST(ACompleteTrailingRecordIsClean) {
  const test::TempDir dir("recovery-complete-tail");
  const auto path = dir.file("events.log");
  BuildLogWithWriter(path, 3, 32);

  const ScanReport report = EL_EXPECT_OK(ScanLog(path));
  EL_CHECK_EQ(report.status, LogStatus::kClean);
  EL_CHECK_EQ(report.valid_records, std::uint64_t{3});

  // Nothing to do, and asking anyway must be refused rather than silently truncating.
  const RecoveryPlan plan = EL_EXPECT_OK(PlanRecovery(path));
  EL_CHECK(!plan.truncation_required);
  EL_EXPECT_ERROR(ApplyRecovery(path, plan, *DefaultFileOps()), ErrorKind::kInvalidArgument);
}

TEST(TruncationTargetsTheExactValidPrefix) {
  const test::TempDir dir("recovery-prefix");
  const auto path = dir.file("events.log");
  BuildLogWithWriter(path, 5, 64);
  const std::uint64_t record_size = kHeaderSize + 64;

  // Append garbage that does not even start a header.
  std::vector<std::byte> bytes = test::ReadFileBytes(path);
  for (int i = 0; i < 7; ++i) bytes.push_back(std::byte{0xAB});
  test::WriteFileBytes(path, bytes);

  const RecoveryPlan plan = EL_EXPECT_OK(PlanRecovery(path));
  EL_CHECK_EQ(plan.scan.status, LogStatus::kIncompleteTail);
  EL_CHECK_EQ(plan.truncate_to, 5 * record_size);
  EL_CHECK_EQ(plan.bytes_to_discard, std::uint64_t{7});

  EL_EXPECT_OK(ApplyRecovery(path, plan, *DefaultFileOps()));
  EL_CHECK_EQ(test::FileSizeOf(path), 5 * record_size);
  EL_CHECK_EQ(EL_EXPECT_OK(ScanLog(path)).valid_records, std::uint64_t{5});
}

// --- CPP-T09: a complete final record with a bad CRC is NOT truncated --------

TEST(CorruptFinalRecordIsHardCorruption) {
  const test::TempDir dir("recovery-corrupt-final");
  const auto path = dir.file("events.log");
  BuildLogWithWriter(path, 3, 48);
  const std::uint64_t record_size = kHeaderSize + 48;
  const std::vector<std::byte> before = test::ReadFileBytes(path);

  // Damage a payload byte of the last record. It is still structurally complete.
  test::FlipByteAt(path, 2 * record_size + kHeaderSize + 7);

  const ScanReport report = EL_EXPECT_OK(ScanLog(path));
  EL_CHECK_EQ(report.status, LogStatus::kCorruptCompleteRecord);
  EL_CHECK_EQ(report.valid_records, std::uint64_t{2});
  EL_CHECK_EQ(report.problem_offset, 2 * record_size);
  EL_CHECK(report.detail.find("CRC32C mismatch") != std::string::npos);

  // Being last is not a reason to discard it (docs/PRD.md section 3).
  const RecoveryPlan plan = EL_EXPECT_OK(PlanRecovery(path));
  EL_CHECK(!plan.truncation_required);
  EL_EXPECT_ERROR(ApplyRecovery(path, plan, *DefaultFileOps()), ErrorKind::kInvalidArgument);

  // Not one byte changed.
  std::vector<std::byte> after = test::ReadFileBytes(path);
  EL_CHECK_EQ(after.size(), before.size());
  EL_CHECK(after != before);  // our own damage is still there
  after[2 * record_size + kHeaderSize + 7] =
      after[2 * record_size + kHeaderSize + 7] ^ std::byte{0x01};
  EL_CHECK(after == before);  // and nothing else was touched
}

TEST(CorruptFinalHeaderIsHardCorruptionNotATail) {
  const test::TempDir dir("recovery-corrupt-header");
  const auto path = dir.file("events.log");
  BuildLogWithWriter(path, 2, 32);
  const std::uint64_t record_size = kHeaderSize + 32;

  // ADR-002: a full 24-byte header that is invalid cannot come from a process crash, so
  // it is corruption rather than a recoverable tail. Damage the magic of the last record.
  test::FlipByteAt(path, record_size + 1);

  const ScanReport report = EL_EXPECT_OK(ScanLog(path));
  EL_CHECK_EQ(report.status, LogStatus::kCorruptCompleteRecord);
  EL_CHECK_EQ(report.problem_offset, record_size);
  EL_CHECK_EQ(report.valid_records, std::uint64_t{1});
  EL_CHECK(!EL_EXPECT_OK(PlanRecovery(path)).truncation_required);
}

TEST(CorruptSequenceFieldIsCaughtByTheChecksum) {
  const test::TempDir dir("recovery-corrupt-seq-field");
  const auto path = dir.file("events.log");
  BuildLogWithWriter(path, 2, 16);
  const std::uint64_t record_size = kHeaderSize + 16;

  // The CRC covers the sequence field, so tampering is caught as corruption rather than
  // misreported as a sequence violation.
  test::FlipByteAt(path, record_size + kSequenceOffset);
  const ScanReport report = EL_EXPECT_OK(ScanLog(path));
  EL_CHECK_EQ(report.status, LogStatus::kCorruptCompleteRecord);
}

// --- CPP-T10: corruption in the middle of the log ---------------------------

TEST(CorruptMiddleRecordStopsWithAnExplicitLocation) {
  const test::TempDir dir("recovery-corrupt-middle");
  const auto path = dir.file("events.log");
  BuildLogWithWriter(path, 5, 32);
  const std::uint64_t record_size = kHeaderSize + 32;
  const std::uint64_t original_size = test::FileSizeOf(path);

  // Damage record 3 of 5.
  test::FlipByteAt(path, 2 * record_size + kHeaderSize + 1);

  const ScanReport report = EL_EXPECT_OK(ScanLog(path));
  EL_CHECK_EQ(report.status, LogStatus::kCorruptCompleteRecord);
  // Says exactly where, and does not pretend the later records are gone.
  EL_CHECK_EQ(report.problem_offset, 2 * record_size);
  EL_CHECK_EQ(report.valid_records, std::uint64_t{2});
  EL_CHECK_EQ(report.valid_prefix_bytes, 2 * record_size);
  EL_CHECK_EQ(report.file_size, original_size);

  // Mid-log damage is never repaired in the MVP: truncating here would silently throw
  // away the two valid records that follow it.
  const RecoveryPlan plan = EL_EXPECT_OK(PlanRecovery(path));
  EL_CHECK(!plan.truncation_required);
  EL_EXPECT_ERROR(ApplyRecovery(path, plan, *DefaultFileOps()), ErrorKind::kInvalidArgument);
  EL_CHECK_EQ(test::FileSizeOf(path), original_size);
}

// --- sequence chain ---------------------------------------------------------

TEST(SequenceGapIsRejected) {
  const test::TempDir dir("recovery-seq-gap");
  const auto path = dir.file("events.log");
  BuildRawLog(path, {{1, 16}, {2, 16}, {4, 16}});

  const ScanReport report = EL_EXPECT_OK(ScanLog(path));
  EL_CHECK_EQ(report.status, LogStatus::kSequenceViolation);
  EL_CHECK_EQ(report.valid_records, std::uint64_t{2});
  EL_CHECK_EQ(report.problem_offset, 2 * (kHeaderSize + 16));
  EL_CHECK(report.detail.find("expected sequence 3") != std::string::npos);
  EL_CHECK(!EL_EXPECT_OK(PlanRecovery(path)).truncation_required);
}

TEST(DuplicateSequenceIsRejected) {
  const test::TempDir dir("recovery-seq-dup");
  const auto path = dir.file("events.log");
  BuildRawLog(path, {{1, 16}, {2, 16}, {2, 16}});

  const ScanReport report = EL_EXPECT_OK(ScanLog(path));
  EL_CHECK_EQ(report.status, LogStatus::kSequenceViolation);
  EL_CHECK_EQ(report.valid_records, std::uint64_t{2});
}

TEST(LogNotStartingAtOneIsRejected) {
  const test::TempDir dir("recovery-seq-start");
  const auto path = dir.file("events.log");
  BuildRawLog(path, {{5, 16}, {6, 16}});

  const ScanReport report = EL_EXPECT_OK(ScanLog(path));
  EL_CHECK_EQ(report.status, LogStatus::kSequenceViolation);
  EL_CHECK_EQ(report.valid_records, std::uint64_t{0});
  EL_CHECK_EQ(report.problem_offset, std::uint64_t{0});
}

TEST(UnsupportedVersionIsReportedAndBytesPreserved) {
  const test::TempDir dir("recovery-version");
  const auto path = dir.file("events.log");
  BuildRawLog(path, {{1, 16}});

  std::vector<std::byte> bytes = test::ReadFileBytes(path);
  StoreLe16(bytes.data() + kVersionOffset, 2);
  test::WriteFileBytes(path, bytes);

  // Not corruption, and not recoverable: this build simply cannot read the file.
  EL_EXPECT_ERROR(ScanLog(path), ErrorKind::kUnsupportedVersion);
  EL_EXPECT_ERROR(PlanRecovery(path), ErrorKind::kUnsupportedVersion);
  EL_CHECK(test::ReadFileBytes(path) == bytes);
}

// --- CPP-AC15: read-only means read-only ------------------------------------

TEST(ScanAndPreflightNeverModifyTheFile) {
  const test::TempDir dir("recovery-readonly");

  struct Case {
    const char* name;
    std::uint64_t truncate_by;
    bool flip;
  };
  const Case cases[] = {{"clean", 0, false}, {"incomplete-tail", 13, false}, {"corrupt", 0, true}};

  for (const Case& scenario : cases) {
    const auto path = dir.file(std::string("case-") + scenario.name + ".log");
    BuildLogWithWriter(path, 4, 32);
    if (scenario.truncate_by > 0) {
      test::TruncateFileTo(path, test::FileSizeOf(path) - scenario.truncate_by);
    }
    if (scenario.flip) test::FlipByteAt(path, 3 * (kHeaderSize + 32) + kHeaderSize + 2);

    const std::vector<std::byte> before = test::ReadFileBytes(path);
    (void)ScanLog(path);
    (void)PlanRecovery(path);
    // Run them repeatedly: a read-only operation is idempotent by definition.
    (void)ScanLog(path);
    (void)PlanRecovery(path);
    const std::vector<std::byte> after = test::ReadFileBytes(path);

    if (before != after) {
      EL_FAIL(std::string("scan or preflight modified the file in case ") + scenario.name);
    }
  }
}

TEST(RecoveryRefusesToFollowASymlink) {
  const test::TempDir dir("recovery-symlink");
  const auto real = dir.file("real.log");
  const auto link = dir.file("link.log");
  BuildLogWithWriter(real, 2, 32);
  test::TruncateFileTo(real, test::FileSizeOf(real) - 9);

  std::error_code ec;
  std::filesystem::create_symlink(real, link, ec);
  if (ec) return;  // symlinks unavailable on this filesystem; nothing to assert

  const RecoveryPlan plan = EL_EXPECT_OK(PlanRecovery(link));
  EL_CHECK(plan.truncation_required);
  // A destructive operation must not be redirected through a symlink.
  EL_EXPECT_ERROR(ApplyRecovery(link, plan, *DefaultFileOps()), ErrorKind::kInvalidArgument);
  EL_CHECK_EQ(test::FileSizeOf(real), plan.scan.file_size);
}

TEST(StalePlanIsRejected) {
  const test::TempDir dir("recovery-stale");
  const auto path = dir.file("events.log");
  BuildLogWithWriter(path, 3, 32);
  test::TruncateFileTo(path, test::FileSizeOf(path) - 11);

  const RecoveryPlan plan = EL_EXPECT_OK(PlanRecovery(path));
  EL_CHECK(plan.truncation_required);

  // The file grows after the preflight; applying the old plan would discard valid data.
  std::vector<std::byte> bytes = test::ReadFileBytes(path);
  const std::vector<std::byte> extra = EncodeRecord(99, MakeDeterministicPayload(99, 8)).value();
  bytes.insert(bytes.end(), extra.begin(), extra.end());
  test::WriteFileBytes(path, bytes);

  EL_EXPECT_ERROR(ApplyRecovery(path, plan, *DefaultFileOps()), ErrorKind::kInvalidArgument);
  EL_CHECK_EQ(test::FileSizeOf(path), bytes.size());
}

// The length guard above cannot catch a path that was replaced by a DIFFERENT file of
// the same length, and applying the old truncation target to it would discard complete
// valid records -- the one thing recovery must never do (ADR-002). The replacement is
// built while the original still exists, so the two necessarily have different inodes,
// and it is renamed into place so the identity that lands is the replacement's own.
//
// This is a stale-plan guard. It is not a claim that concurrent modification is safe:
// docs/PRD.md section 2 supports one writer at a time and nothing else.
TEST(PlanIsRejectedWhenTheFileWasReplacedAtTheSameSize) {
  const test::TempDir dir("recovery-replaced");
  const auto path = dir.file("events.log");
  const auto replacement = dir.file("events.log.new");

  BuildLogWithWriter(path, 3, 32);  // 3 x 56 bytes
  test::TruncateFileTo(path, 168 - 11);

  const RecoveryPlan plan = EL_EXPECT_OK(PlanRecovery(path));
  EL_CHECK(plan.truncation_required);
  EL_CHECK_EQ(plan.truncate_to, std::uint64_t{112});

  // A clean two-record log of exactly 157 bytes: (24+54) + (24+55).
  BuildRawLog(replacement, {{1, 54}, {2, 55}});
  EL_CHECK_EQ(test::FileSizeOf(replacement), plan.scan.file_size);
  std::error_code ec;
  std::filesystem::rename(replacement, path, ec);
  if (ec) return;  // cannot rename on this filesystem; nothing to assert

  // Same length, so the length check cannot be what rejects this.
  EL_CHECK_EQ(test::FileSizeOf(path), plan.scan.file_size);
  EL_EXPECT_ERROR(ApplyRecovery(path, plan, *DefaultFileOps()), ErrorKind::kInvalidArgument);

  // The replacement is intact: its second record ends at 157 and was never cut to 112.
  EL_CHECK_EQ(test::FileSizeOf(path), std::uint64_t{157});
  const ScanReport after = EL_EXPECT_OK(ScanLog(path));
  EL_CHECK_EQ(after.status, LogStatus::kClean);
  EL_CHECK_EQ(after.valid_records, std::uint64_t{2});
}

TEST(RecoveredLogCanBeAppendedToAgain) {
  const test::TempDir dir("recovery-then-append");
  const auto path = dir.file("events.log");
  BuildLogWithWriter(path, 3, 32);
  test::TruncateFileTo(path, test::FileSizeOf(path) - 20);

  const RecoveryPlan plan = EL_EXPECT_OK(PlanRecovery(path));
  EL_EXPECT_OK(ApplyRecovery(path, plan, *DefaultFileOps()));

  // Recovery leaves the log in a state a writer will accept, continuing the sequence.
  WriterOptions options;
  options.allow_existing = true;
  EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, options));
  EL_CHECK_EQ(writer.accepted_through_sequence(), std::uint64_t{2});
  EL_CHECK_EQ(EL_EXPECT_OK(writer.AppendAndSync(MakeDeterministicPayload(3, 32))).sequence,
              std::uint64_t{3});
  EL_EXPECT_STATUS_OK(writer.Close());

  const ScanReport report = EL_EXPECT_OK(ScanLog(path));
  EL_CHECK_EQ(report.status, LogStatus::kClean);
  EL_CHECK_EQ(report.valid_records, std::uint64_t{3});
}

}  // namespace
}  // namespace eventlog
