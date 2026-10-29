// CPP-AC03 to CPP-AC05 and CPP-AC07, tests CPP-T04, CPP-T05, CPP-T07.
//
// The acknowledgment contract is the point of this project, so these cases drive the
// writer through injected I/O failures and assert on what it did NOT acknowledge.

#include "eventlog/writer.h"

#include <cerrno>
#include <memory>
#include <string_view>
#include <vector>

#include "eventlog/payload.h"
#include "eventlog/reader.h"
#include "test_support.h"

namespace eventlog {
namespace {

// Narrow test double over the real syscalls. This is the alternate FileOps
// implementation that justifies the interface existing (docs/PRD.md section 3): short
// writes, EINTR, EIO, and a failed sync cannot be produced reliably any other way.
class ScriptedFileOps final : public FileOps {
 public:
  // Caps every write to this many bytes, forcing the short-write loop to run.
  std::size_t max_write_bytes = 0;  // 0 = no cap
  // Returns EINTR this many times before each write is allowed through.
  int eintr_rounds = 0;
  // After this many successful writes, fail with write_errno. -1 disables.
  int fail_write_after = -1;
  int write_errno = EIO;
  // After this many successful syncs, fail with sync_errno. -1 disables.
  int fail_sync_after = -1;
  int sync_errno = EIO;

  int write_calls = 0;
  int eintr_returned = 0;
  int successful_writes = 0;
  int sync_calls = 0;
  int successful_syncs = 0;

  Result<std::size_t> Write(int fd, std::span<const std::byte> data) override {
    ++write_calls;
    if (eintr_returned < eintr_rounds) {
      ++eintr_returned;
      return IoError("write", EINTR, "injected");
    }
    if (fail_write_after >= 0 && successful_writes >= fail_write_after) {
      return IoError("write", write_errno, "injected");
    }
    std::span<const std::byte> slice = data;
    if (max_write_bytes > 0 && slice.size() > max_write_bytes) {
      slice = slice.first(max_write_bytes);
    }
    Result<std::size_t> result = real_.Write(fd, slice);
    if (result.ok()) ++successful_writes;
    return result;
  }

  Status SyncData(int fd) override {
    ++sync_calls;
    if (fail_sync_after >= 0 && successful_syncs >= fail_sync_after) {
      return IoError("fdatasync", sync_errno, "injected");
    }
    Status status = real_.SyncData(fd);
    if (status.ok()) ++successful_syncs;
    return status;
  }

  Status SyncDirectory(const std::filesystem::path& directory) override {
    return real_.SyncDirectory(directory);
  }
  Status Truncate(int fd, std::uint64_t length) override { return real_.Truncate(fd, length); }
  Result<std::uint64_t> FileSize(int fd) override { return real_.FileSize(fd); }

 private:
  PosixFileOps real_;
};

std::vector<std::byte> Payload(std::size_t size, std::byte fill = std::byte{0x5A}) {
  return std::vector<std::byte>(size, fill);
}

// --- normal path -------------------------------------------------------------

TEST(AppendsAndAcknowledgesEachRecord) {
  const test::TempDir dir("writer-basic");
  const auto path = dir.file("events.log");

  EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, WriterOptions{}));
  EL_CHECK_EQ(writer.durable_through_sequence(), std::uint64_t{0});

  for (std::uint64_t i = 1; i <= 50; ++i) {
    const AppendResult result = EL_EXPECT_OK(writer.AppendAndSync(Payload(64)));
    EL_CHECK_EQ(result.sequence, i);
    // Under sync_each the acknowledgment keeps pace with the record.
    EL_CHECK_EQ(result.durable_through_sequence, i);
    EL_CHECK_EQ(writer.accepted_through_sequence(), i);
  }
  EL_CHECK(writer.healthy());
  EL_EXPECT_STATUS_OK(writer.Close());

  const ScanReport report = EL_EXPECT_OK(ScanLog(path));
  EL_CHECK_EQ(report.status, LogStatus::kClean);
  EL_CHECK_EQ(report.valid_records, std::uint64_t{50});
  EL_CHECK_EQ(report.last_sequence, std::uint64_t{50});
}

TEST(CreatesFileAndStartsAtSequenceOne) {
  const test::TempDir dir("writer-create");
  const auto path = dir.file("nested.log");
  EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, WriterOptions{}));
  const AppendResult result = EL_EXPECT_OK(writer.AppendAndSync(Payload(1)));
  EL_CHECK_EQ(result.sequence, kFirstSequence);
  EL_EXPECT_STATUS_OK(writer.Close());
  EL_CHECK_EQ(test::FileSizeOf(path), std::uint64_t{kHeaderSize + 1});
}

// --- CPP-AC03 / CPP-T04: short writes and EINTR ------------------------------

TEST(ShortWritesAreLoopedUntilComplete) {
  const test::TempDir dir("writer-short");
  const auto path = dir.file("events.log");

  auto ops = std::make_shared<ScriptedFileOps>();
  ops->max_write_bytes = 7;  // a 24+128 byte record needs 22 writes

  WriterOptions options;
  options.file_ops = ops;
  EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, options));

  for (int i = 0; i < 5; ++i) {
    const AppendResult result = EL_EXPECT_OK(writer.AppendAndSync(Payload(128)));
    EL_CHECK_EQ(result.durable_through_sequence, static_cast<std::uint64_t>(i + 1));
  }
  EL_EXPECT_STATUS_OK(writer.Close());

  // The loop really ran, and the resulting file is byte-exact anyway.
  EL_CHECK(ops->write_calls > 5 * 20);
  const ScanReport report = EL_EXPECT_OK(ScanLog(path));
  EL_CHECK_EQ(report.status, LogStatus::kClean);
  EL_CHECK_EQ(report.valid_records, std::uint64_t{5});
}

TEST(InterruptedWritesAreRetried) {
  const test::TempDir dir("writer-eintr");
  const auto path = dir.file("events.log");

  auto ops = std::make_shared<ScriptedFileOps>();
  ops->eintr_rounds = 4;  // EINTR four times, then let writes through

  WriterOptions options;
  options.file_ops = ops;
  EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, options));

  // EINTR is retried transparently, so the append still succeeds.
  const AppendResult result = EL_EXPECT_OK(writer.AppendAndSync(Payload(100)));
  EL_CHECK_EQ(result.durable_through_sequence, std::uint64_t{1});
  EL_CHECK_EQ(ops->eintr_returned, 4);
  EL_EXPECT_STATUS_OK(writer.Close());

  EL_CHECK_EQ(EL_EXPECT_OK(ScanLog(path)).status, LogStatus::kClean);
}

TEST(WriteErrorIsPropagatedAndNotAcknowledged) {
  const test::TempDir dir("writer-eio");
  const auto path = dir.file("events.log");

  auto ops = std::make_shared<ScriptedFileOps>();
  ops->fail_write_after = 2;  // two records land, the third fails

  WriterOptions options;
  options.file_ops = ops;
  EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, options));

  EL_EXPECT_OK(writer.AppendAndSync(Payload(32)));
  EL_EXPECT_OK(writer.AppendAndSync(Payload(32)));
  EL_CHECK_EQ(writer.durable_through_sequence(), std::uint64_t{2});

  // EIO must surface, not be retried.
  EL_EXPECT_ERROR(writer.AppendAndSync(Payload(32)), ErrorKind::kIo);
  // No acknowledgment for the failed record, and the writer is done.
  EL_CHECK_EQ(writer.durable_through_sequence(), std::uint64_t{2});
  EL_CHECK(!writer.healthy());
  EL_EXPECT_STATUS_OK(writer.Close());

  // The two acknowledged records are intact and readable.
  const ScanReport report = EL_EXPECT_OK(ScanLog(path));
  EL_CHECK(report.valid_records >= 2);
  EL_CHECK(report.last_sequence >= 2);
}

TEST(PartialWriteThenErrorLeavesRecoverableTail) {
  const test::TempDir dir("writer-torn");
  const auto path = dir.file("events.log");

  auto ops = std::make_shared<ScriptedFileOps>();
  ops->max_write_bytes = 16;
  // One full record is 24+64=88 bytes = 6 writes. Allow 6 plus 2 of the next, then fail.
  ops->fail_write_after = 8;

  WriterOptions options;
  options.file_ops = ops;
  EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, options));

  EL_EXPECT_OK(writer.AppendAndSync(Payload(64)));
  EL_EXPECT_ERROR(writer.AppendAndSync(Payload(64)), ErrorKind::kIo);
  EL_CHECK_EQ(writer.durable_through_sequence(), std::uint64_t{1});
  EL_EXPECT_STATUS_OK(writer.Close());

  // A torn trailing record is exactly the recoverable case, never hard corruption.
  const ScanReport report = EL_EXPECT_OK(ScanLog(path));
  EL_CHECK_EQ(report.status, LogStatus::kIncompleteTail);
  EL_CHECK_EQ(report.valid_records, std::uint64_t{1});
  EL_CHECK_EQ(report.valid_prefix_bytes, std::uint64_t{kHeaderSize + 64});
}

// --- CPP-AC05 / CPP-AC07 / CPP-T05: sync failures ----------------------------

TEST(FailedSyncNeverAcknowledges) {
  const test::TempDir dir("writer-sync-fail");
  const auto path = dir.file("events.log");

  auto ops = std::make_shared<ScriptedFileOps>();
  ops->fail_sync_after = 0;  // the very first sync fails

  WriterOptions options;
  options.file_ops = ops;
  EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, options));

  EL_EXPECT_ERROR(writer.AppendAndSync(Payload(48)), ErrorKind::kUnknownDurability);

  // The bytes reached the file, but nothing was ever acknowledged durable. This is the
  // inequality the PRD insists on: accepted is not durable.
  EL_CHECK_EQ(writer.durable_through_sequence(), std::uint64_t{0});
  EL_CHECK_EQ(writer.accepted_through_sequence(), std::uint64_t{1});
  EL_CHECK(!writer.healthy());
  EL_EXPECT_STATUS_OK(writer.Close());

  EL_CHECK_EQ(EL_EXPECT_OK(ScanLog(path)).valid_records, std::uint64_t{1});
}

TEST(FailedSyncIsTerminalForTheWriter) {
  const test::TempDir dir("writer-sync-terminal");
  const auto path = dir.file("events.log");

  auto ops = std::make_shared<ScriptedFileOps>();
  ops->fail_sync_after = 3;  // three records acknowledged, then sync breaks

  WriterOptions options;
  options.file_ops = ops;
  EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, options));

  for (int i = 0; i < 3; ++i) EL_EXPECT_OK(writer.AppendAndSync(Payload(16)));
  EL_CHECK_EQ(writer.durable_through_sequence(), std::uint64_t{3});

  EL_EXPECT_ERROR(writer.AppendAndSync(Payload(16)), ErrorKind::kUnknownDurability);
  const int syncs_at_failure = ops->sync_calls;

  // ADR-003: no retry, no further acknowledgment, and no further sync attempts.
  for (int i = 0; i < 3; ++i) {
    EL_EXPECT_ERROR(writer.AppendAndSync(Payload(16)), ErrorKind::kUnknownDurability);
  }
  EL_CHECK_EQ(ops->sync_calls, syncs_at_failure);
  EL_CHECK_EQ(writer.durable_through_sequence(), std::uint64_t{3});
  EL_EXPECT_STATUS_OK(writer.Close());
}

// --- CPP-AC04 / CPP-T07: single writer ---------------------------------------

TEST(SecondCooperatingWriterIsRejected) {
  const test::TempDir dir("writer-lock");
  const auto path = dir.file("events.log");

  EventLogWriter first = EL_EXPECT_OK(EventLogWriter::Open(path, WriterOptions{}));
  EL_EXPECT_OK(first.AppendAndSync(Payload(8)));

  WriterOptions second_options;
  second_options.allow_existing = true;
  EL_EXPECT_ERROR(EventLogWriter::Open(path, second_options), ErrorKind::kLockUnavailable);

  // Once the first writer closes, the lock is released and a new writer can continue.
  EL_EXPECT_STATUS_OK(first.Close());
  EventLogWriter second = EL_EXPECT_OK(EventLogWriter::Open(path, second_options));
  const AppendResult result = EL_EXPECT_OK(second.AppendAndSync(Payload(8)));
  EL_CHECK_EQ(result.sequence, std::uint64_t{2});
  EL_EXPECT_STATUS_OK(second.Close());
}

// --- existing files and bounds ------------------------------------------------

TEST(RefusesExistingLogWithoutExplicitFlag) {
  const test::TempDir dir("writer-existing");
  const auto path = dir.file("events.log");

  EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, WriterOptions{}));
  EL_EXPECT_OK(writer.AppendAndSync(Payload(8)));
  EL_EXPECT_STATUS_OK(writer.Close());

  // Protects an existing log from an accidental second run.
  EL_EXPECT_ERROR(EventLogWriter::Open(path, WriterOptions{}), ErrorKind::kInvalidArgument);
}

TEST(ContinuesSequenceNumberingOnExistingLog) {
  const test::TempDir dir("writer-continue");
  const auto path = dir.file("events.log");

  {
    EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, WriterOptions{}));
    for (int i = 0; i < 4; ++i) EL_EXPECT_OK(writer.AppendAndSync(Payload(20)));
    EL_EXPECT_STATUS_OK(writer.Close());
  }

  WriterOptions options;
  options.allow_existing = true;
  EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, options));
  EL_CHECK_EQ(writer.accepted_through_sequence(), std::uint64_t{4});
  // Durable only because Open synced the resumed prefix; see ResumeSyncsTheExistingPrefix.
  EL_CHECK_EQ(writer.durable_through_sequence(), std::uint64_t{4});
  EL_CHECK_EQ(EL_EXPECT_OK(writer.AppendAndSync(Payload(20))).sequence, std::uint64_t{5});
  EL_EXPECT_STATUS_OK(writer.Close());

  EL_CHECK_EQ(EL_EXPECT_OK(ScanLog(path)).last_sequence, std::uint64_t{5});
}

// Resuming a log must not infer durability from the records merely being present. The
// scan reads through the page cache, so it cannot distinguish a prefix that was synced
// from one written by a writer that died before its sync (or, from M4, from a
// deliberately non-durable policy). Open establishes the claim with one sync instead.
TEST(ResumeSyncsTheExistingPrefix) {
  const test::TempDir dir("writer-resume-sync");
  const auto path = dir.file("events.log");

  {
    EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, WriterOptions{}));
    for (int i = 0; i < 3; ++i) EL_EXPECT_OK(writer.AppendAndSync(Payload(12)));
    EL_EXPECT_STATUS_OK(writer.Close());
  }

  auto ops = std::make_shared<ScriptedFileOps>();
  WriterOptions options;
  options.allow_existing = true;
  options.file_ops = ops;

  EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, options));
  // One sync, charged to Open, before a single record of this session has been written.
  EL_CHECK_EQ(ops->sync_calls, 1);
  EL_CHECK_EQ(ops->write_calls, 0);
  EL_CHECK_EQ(writer.durable_through_sequence(), std::uint64_t{3});
  EL_EXPECT_STATUS_OK(writer.Close());
}

// If that resume sync fails, the writer must not open at all: it cannot honestly report
// the existing records as durable, and ADR-003 forbids retrying the sync.
TEST(ResumeFailsOpenWhenTheSyncFails) {
  const test::TempDir dir("writer-resume-sync-fails");
  const auto path = dir.file("events.log");

  {
    EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, WriterOptions{}));
    for (int i = 0; i < 2; ++i) EL_EXPECT_OK(writer.AppendAndSync(Payload(12)));
    EL_EXPECT_STATUS_OK(writer.Close());
  }

  auto ops = std::make_shared<ScriptedFileOps>();
  ops->fail_sync_after = 0;  // the resume sync itself fails
  WriterOptions options;
  options.allow_existing = true;
  options.file_ops = ops;

  EL_EXPECT_ERROR(EventLogWriter::Open(path, options), ErrorKind::kUnknownDurability);
  EL_CHECK_EQ(ops->sync_calls, 1);
  EL_CHECK_EQ(ops->write_calls, 0);

  // The log itself is untouched: a failed resume is not a destructive event.
  const ScanReport report = EL_EXPECT_OK(ScanLog(path));
  EL_CHECK(report.clean());
  EL_CHECK_EQ(report.valid_records, std::uint64_t{2});
}

// A newly created log has no prefix to establish, so Open syncs the directory entry and
// nothing else. Guards against charging every fresh writer a pointless data sync.
TEST(FreshLogDoesNotSyncDataAtOpen) {
  const test::TempDir dir("writer-fresh-no-sync");
  const auto path = dir.file("events.log");

  auto ops = std::make_shared<ScriptedFileOps>();
  WriterOptions options;
  options.file_ops = ops;

  EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, options));
  EL_CHECK_EQ(ops->sync_calls, 0);
  EL_CHECK_EQ(writer.durable_through_sequence(), std::uint64_t{0});
  EL_EXPECT_STATUS_OK(writer.Close());
}

TEST(RefusesToAppendToADamagedLog) {
  const test::TempDir dir("writer-damaged");
  const auto path = dir.file("events.log");

  {
    EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, WriterOptions{}));
    for (int i = 0; i < 3; ++i) EL_EXPECT_OK(writer.AppendAndSync(Payload(32)));
    EL_EXPECT_STATUS_OK(writer.Close());
  }

  // Appending past an incomplete tail would bury it mid-file, where recovery can never
  // reach it.
  test::TruncateFileTo(path, test::FileSizeOf(path) - 10);
  WriterOptions options;
  options.allow_existing = true;
  EL_EXPECT_ERROR(EventLogWriter::Open(path, options), ErrorKind::kCorruptRecord);
}

TEST(RejectsOversizedPayload) {
  const test::TempDir dir("writer-bounds");
  const auto path = dir.file("events.log");
  EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, WriterOptions{}));

  EL_EXPECT_ERROR(writer.AppendAndSync(Payload(std::size_t{kMaxPayloadBytes} + 1)),
                  ErrorKind::kLimitExceeded);
  // A rejected payload must not consume a sequence number or disturb the writer.
  EL_CHECK_EQ(writer.accepted_through_sequence(), std::uint64_t{0});
  EL_CHECK(writer.healthy());
  EL_CHECK_EQ(EL_EXPECT_OK(writer.AppendAndSync(Payload(kMaxPayloadBytes))).sequence,
              std::uint64_t{1});
  EL_EXPECT_STATUS_OK(writer.Close());
}

TEST(UnimplementedPoliciesAreRejectedAtOpen) {
  const test::TempDir dir("writer-policy");

  // Milestone M1 must not let an unimplemented policy report anything as durable.
  WriterOptions batch;
  batch.policy = SyncPolicy::kSyncBatch;
  batch.batch_size = 16;
  EL_EXPECT_ERROR(EventLogWriter::Open(dir.file("a.log"), batch), ErrorKind::kUnsupportedPolicy);

  WriterOptions write_only;
  write_only.policy = SyncPolicy::kWriteOnly;
  EL_EXPECT_ERROR(EventLogWriter::Open(dir.file("b.log"), write_only),
                  ErrorKind::kUnsupportedPolicy);
}

TEST(PolicyNamesRoundTrip) {
  EL_CHECK_EQ(EL_EXPECT_OK(ParseSyncPolicy("sync_each")), SyncPolicy::kSyncEach);
  EL_CHECK_EQ(EL_EXPECT_OK(ParseSyncPolicy("sync_batch")), SyncPolicy::kSyncBatch);
  EL_CHECK_EQ(EL_EXPECT_OK(ParseSyncPolicy("write_only")), SyncPolicy::kWriteOnly);
  EL_EXPECT_ERROR(ParseSyncPolicy("fsync"), ErrorKind::kInvalidArgument);
  EL_CHECK_EQ(std::string(SyncPolicyName(SyncPolicy::kSyncEach)), std::string("sync_each"));
  // The reported barrier must name a real primitive, for honest result reporting.
  EL_CHECK(!SyncPrimitiveName().empty());
}

}  // namespace
}  // namespace eventlog
