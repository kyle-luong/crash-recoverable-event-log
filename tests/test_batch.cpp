// CPP-05 / CPP-AC06 and CPP-06: whole-batch acknowledgment boundaries, the syscall
// tradeoff that batching actually buys, and what recovery finds after a batch fails
// partway through.
//
// The theme of every case is the same: a batch is acknowledged as a unit or not at all,
// and a batch that was never acknowledged may still have left records on disk. That is
// permitted -- the invariant is one-directional. What is never permitted is the reverse.

#include <cstddef>
#include <memory>
#include <vector>

#include "eventlog/payload.h"
#include "eventlog/reader.h"
#include "eventlog/recover.h"
#include "eventlog/writer.h"
#include "scripted_file_ops.h"
#include "test_support.h"

namespace eventlog {
namespace {

using test::ScriptedFileOps;

// Deterministic payloads keyed by sequence, so a recovered log can be checked
// byte-for-byte against what should have been written.
std::vector<std::vector<std::byte>> PayloadsFor(std::uint64_t first_sequence, std::size_t count,
                                                std::size_t size) {
  std::vector<std::vector<std::byte>> payloads;
  payloads.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    payloads.push_back(MakeDeterministicPayload(first_sequence + i, size));
  }
  return payloads;
}

std::vector<std::span<const std::byte>> SpansOf(
    const std::vector<std::vector<std::byte>>& payloads) {
  std::vector<std::span<const std::byte>> spans;
  spans.reserve(payloads.size());
  for (const auto& payload : payloads) spans.emplace_back(payload);
  return spans;
}

WriterOptions BatchOptions(std::size_t batch_size, std::shared_ptr<FileOps> ops = nullptr) {
  WriterOptions options;
  options.policy = SyncPolicy::kSyncBatch;
  options.batch_size = batch_size;
  options.file_ops = std::move(ops);
  return options;
}

// --- CPP-AC06: the batch is the acknowledgment unit ---------------------------

TEST(BatchAcknowledgesEveryRecordTogether) {
  const test::TempDir dir("batch-ack-together");
  const auto path = dir.file("events.log");

  EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, BatchOptions(32)));
  const auto payloads = PayloadsFor(1, 32, 64);
  const auto spans = SpansOf(payloads);

  const BatchAppendResult result = EL_EXPECT_OK(writer.AppendBatchAndSync(spans));
  EL_CHECK_EQ(result.first_sequence, std::uint64_t{1});
  EL_CHECK_EQ(result.last_sequence, std::uint64_t{32});
  EL_CHECK_EQ(result.records, std::uint64_t{32});
  // All 32, not just the last: the whole batch crossed the barrier together.
  EL_CHECK_EQ(result.durable_through_sequence, std::uint64_t{32});
  EL_CHECK_EQ(writer.accepted_through_sequence(), std::uint64_t{32});
  EL_EXPECT_STATUS_OK(writer.Close());

  EL_CHECK_EQ(EL_EXPECT_OK(ScanLog(path)).valid_records, std::uint64_t{32});
}

// The point of batching, stated as an assertion instead of a timing claim: one write and
// one sync for a batch of 32, where sync_each needs 32 of each for the same records.
// Measured throughput is milestone M4; this only pins the syscall ratio.
TEST(BatchSpendsOneSyncWhereSyncEachSpendsThirtyTwo) {
  const test::TempDir dir("batch-syscall-ratio");

  auto each_ops = std::make_shared<ScriptedFileOps>();
  WriterOptions each;
  each.file_ops = each_ops;
  EventLogWriter each_writer = EL_EXPECT_OK(EventLogWriter::Open(dir.file("each.log"), each));
  for (std::uint64_t i = 1; i <= 32; ++i) {
    EL_EXPECT_OK(each_writer.AppendAndSync(MakeDeterministicPayload(i, 64)));
  }
  EL_EXPECT_STATUS_OK(each_writer.Close());

  auto batch_ops = std::make_shared<ScriptedFileOps>();
  EventLogWriter batch_writer =
      EL_EXPECT_OK(EventLogWriter::Open(dir.file("batch.log"), BatchOptions(32, batch_ops)));
  const auto payloads = PayloadsFor(1, 32, 64);
  EL_EXPECT_OK(batch_writer.AppendBatchAndSync(SpansOf(payloads)));
  EL_EXPECT_STATUS_OK(batch_writer.Close());

  EL_CHECK_EQ(each_writer.stats().sync_calls, std::uint64_t{32});
  EL_CHECK_EQ(each_writer.stats().batches, std::uint64_t{32});
  EL_CHECK_EQ(batch_writer.stats().sync_calls, std::uint64_t{1});
  EL_CHECK_EQ(batch_writer.stats().write_calls, std::uint64_t{1});
  EL_CHECK_EQ(batch_writer.stats().batches, std::uint64_t{1});

  // Same records, same bytes on disk: the policies differ only in barrier granularity.
  EL_CHECK_EQ(each_writer.stats().bytes_written, batch_writer.stats().bytes_written);
  EL_CHECK_EQ(test::ReadFileBytes(dir.file("each.log")),
              test::ReadFileBytes(dir.file("batch.log")));
}

// A short final batch is acknowledged exactly like a full one. This is what lets a
// streaming caller chunk its records without a flush path (ADR-006).
TEST(ShortFinalBatchIsAcknowledgedLikeAFullOne) {
  const test::TempDir dir("batch-short-final");
  const auto path = dir.file("events.log");

  auto ops = std::make_shared<ScriptedFileOps>();
  EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, BatchOptions(32, ops)));

  std::uint64_t next = 1;
  for (const std::size_t group :
       {std::size_t{32}, std::size_t{32}, std::size_t{32}, std::size_t{4}}) {
    const auto payloads = PayloadsFor(next, group, 48);
    const BatchAppendResult result = EL_EXPECT_OK(writer.AppendBatchAndSync(SpansOf(payloads)));
    EL_CHECK_EQ(result.first_sequence, next);
    EL_CHECK_EQ(result.durable_through_sequence, next + group - 1);
    next += group;
  }
  EL_CHECK_EQ(writer.durable_through_sequence(), std::uint64_t{100});
  EL_CHECK_EQ(ops->sync_calls, 4);  // not 100
  EL_EXPECT_STATUS_OK(writer.Close());

  EL_CHECK_EQ(EL_EXPECT_OK(ScanLog(path)).valid_records, std::uint64_t{100});
}

TEST(BatchedRecordsRoundTripByteForByte) {
  const test::TempDir dir("batch-roundtrip");
  const auto path = dir.file("events.log");

  EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, BatchOptions(16)));
  std::uint64_t next = 1;
  for (int round = 0; round < 4; ++round) {
    const auto payloads = PayloadsFor(next, 16, static_cast<std::size_t>(round) * 100);
    EL_EXPECT_OK(writer.AppendBatchAndSync(SpansOf(payloads)));
    next += 16;
  }
  EL_EXPECT_STATUS_OK(writer.Close());

  LogReader reader = EL_EXPECT_OK(LogReader::Open(path));
  std::uint64_t expected = 1;
  while (true) {
    std::optional<Record> record = EL_EXPECT_OK(reader.Next());
    if (!record.has_value()) break;
    EL_CHECK_EQ(record->sequence, expected);
    const std::size_t size = static_cast<std::size_t>((expected - 1) / 16) * 100;
    EL_CHECK(record->payload == MakeDeterministicPayload(expected, size));
    ++expected;
  }
  EL_CHECK_EQ(expected, std::uint64_t{65});
}

// --- failure: a batch acknowledges all or nothing -----------------------------

TEST(FailedBatchWriteAcknowledgesNoRecordInTheBatch) {
  const test::TempDir dir("batch-write-fails");
  const auto path = dir.file("events.log");

  auto ops = std::make_shared<ScriptedFileOps>();
  ops->max_write_bytes = 100;  // force the short-write loop inside one batch
  ops->fail_write_after = 3;   // then break partway through
  EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, BatchOptions(16, ops)));

  const auto payloads = PayloadsFor(1, 16, 64);
  EL_EXPECT_ERROR(writer.AppendBatchAndSync(SpansOf(payloads)), ErrorKind::kIo);

  // Not one record of the batch is acknowledged, and nothing was synced.
  EL_CHECK_EQ(writer.durable_through_sequence(), std::uint64_t{0});
  EL_CHECK_EQ(writer.accepted_through_sequence(), std::uint64_t{0});
  EL_CHECK_EQ(ops->sync_calls, 0);
  EL_CHECK(!writer.healthy());

  // Terminal: no retry, no later acknowledgment (ADR-003).
  EL_EXPECT_ERROR(writer.AppendBatchAndSync(SpansOf(payloads)), ErrorKind::kUnknownDurability);
  EL_CHECK_EQ(ops->sync_calls, 0);
  EL_EXPECT_STATUS_OK(writer.Close());
}

TEST(FailedBatchSyncAcknowledgesNoRecordInTheBatch) {
  const test::TempDir dir("batch-sync-fails");
  const auto path = dir.file("events.log");

  auto ops = std::make_shared<ScriptedFileOps>();
  ops->fail_sync_after = 0;  // the batch is written in full, then the barrier fails
  EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, BatchOptions(8, ops)));

  const auto payloads = PayloadsFor(1, 8, 32);
  EL_EXPECT_ERROR(writer.AppendBatchAndSync(SpansOf(payloads)), ErrorKind::kUnknownDurability);

  // Every record reached write(2) -- and none of them is durable.
  EL_CHECK_EQ(writer.accepted_through_sequence(), std::uint64_t{8});
  EL_CHECK_EQ(writer.durable_through_sequence(), std::uint64_t{0});
  EL_CHECK(!writer.healthy());
  EL_EXPECT_STATUS_OK(writer.Close());

  // The records are on disk and perfectly valid. They were still never acknowledged;
  // a complete log is not the same claim as a durable one.
  const ScanReport report = EL_EXPECT_OK(ScanLog(path));
  EL_CHECK(report.clean());
  EL_CHECK_EQ(report.valid_records, std::uint64_t{8});
}

// After a batch dies partway through, recovery must still produce a contiguous log, and
// every record that WAS acknowledged must survive it. The unit-level form of the
// acknowledged-prefix invariant; M3 demonstrates it across a real process kill.
TEST(RecoveryAfterAFailedBatchKeepsEveryAcknowledgedRecord) {
  const test::TempDir dir("batch-recovery");
  const auto path = dir.file("events.log");

  auto ops = std::make_shared<ScriptedFileOps>();
  EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, BatchOptions(10, ops)));

  // Two batches acknowledged, then a third that dies mid-write.
  const auto first = PayloadsFor(1, 10, 70);
  EL_EXPECT_OK(writer.AppendBatchAndSync(SpansOf(first)));
  const auto second = PayloadsFor(11, 10, 70);
  EL_EXPECT_OK(writer.AppendBatchAndSync(SpansOf(second)));
  const std::uint64_t acknowledged = writer.durable_through_sequence();
  EL_CHECK_EQ(acknowledged, std::uint64_t{20});

  ops->max_write_bytes = 64;
  ops->fail_write_after = ops->successful_writes + 5;
  const auto third = PayloadsFor(21, 10, 70);
  EL_EXPECT_ERROR(writer.AppendBatchAndSync(SpansOf(third)), ErrorKind::kIo);
  EL_CHECK_EQ(writer.durable_through_sequence(), acknowledged);
  EL_EXPECT_STATUS_OK(writer.Close());

  // Whatever the torn batch left behind, recovery yields a clean contiguous log that
  // still contains every acknowledged record.
  const RecoveryPlan plan = EL_EXPECT_OK(PlanRecovery(path));
  if (plan.truncation_required) {
    PosixFileOps real;
    EL_EXPECT_OK(ApplyRecovery(path, plan, real));
  }
  const ScanReport report = EL_EXPECT_OK(ScanLog(path));
  EL_CHECK(report.clean());
  EL_CHECK(report.last_sequence >= acknowledged);

  // And the surviving records are the ones that were meant to be there.
  LogReader reader = EL_EXPECT_OK(LogReader::Open(path));
  std::uint64_t expected = 1;
  while (true) {
    std::optional<Record> record = EL_EXPECT_OK(reader.Next());
    if (!record.has_value()) break;
    EL_CHECK_EQ(record->sequence, expected);
    EL_CHECK(record->payload == MakeDeterministicPayload(expected, 70));
    ++expected;
  }
  EL_CHECK(expected - 1 >= acknowledged);
}

// A short write inside a batch is looped to completion and still counts as one batch:
// write_calls counts acknowledgment-unit writes, not syscall retries.
TEST(ShortWritesInsideABatchStillFormOneAcknowledgment) {
  const test::TempDir dir("batch-short-writes");
  const auto path = dir.file("events.log");

  auto ops = std::make_shared<ScriptedFileOps>();
  ops->max_write_bytes = 7;  // 7 bytes at a time through a multi-kilobyte batch
  ops->eintr_rounds = 2;
  EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, BatchOptions(8, ops)));

  const auto payloads = PayloadsFor(1, 8, 40);
  const BatchAppendResult result = EL_EXPECT_OK(writer.AppendBatchAndSync(SpansOf(payloads)));
  EL_CHECK_EQ(result.durable_through_sequence, std::uint64_t{8});
  EL_CHECK_EQ(writer.stats().write_calls, std::uint64_t{1});
  EL_CHECK_EQ(writer.stats().sync_calls, std::uint64_t{1});
  EL_CHECK(ops->write_calls > 8);  // many syscalls, one acknowledgment
  EL_EXPECT_STATUS_OK(writer.Close());

  EL_CHECK_EQ(EL_EXPECT_OK(ScanLog(path)).valid_records, std::uint64_t{8});
}

// --- argument and policy boundaries -------------------------------------------

TEST(BatchRejectsEmptyAndOversizedBatches) {
  const test::TempDir dir("batch-bounds");
  const auto path = dir.file("events.log");

  EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, BatchOptions(4)));

  const std::vector<std::span<const std::byte>> empty;
  EL_EXPECT_ERROR(writer.AppendBatchAndSync(empty), ErrorKind::kInvalidArgument);

  const auto too_many = PayloadsFor(1, 5, 8);  // batch-size is 4
  EL_EXPECT_ERROR(writer.AppendBatchAndSync(SpansOf(too_many)), ErrorKind::kLimitExceeded);

  // An oversized payload anywhere in the batch is rejected before anything is written.
  auto payloads = PayloadsFor(1, 3, 8);
  payloads[1].assign(kMaxPayloadBytes + 1, std::byte{0x11});
  EL_EXPECT_ERROR(writer.AppendBatchAndSync(SpansOf(payloads)), ErrorKind::kLimitExceeded);

  // Nothing above touched the file or the acknowledgment.
  EL_CHECK_EQ(writer.durable_through_sequence(), std::uint64_t{0});
  EL_CHECK_EQ(writer.stats().write_calls, std::uint64_t{0});
  EL_EXPECT_STATUS_OK(writer.Close());
  EL_CHECK_EQ(test::FileSizeOf(path), std::uint64_t{0});
}

TEST(OpenRejectsBatchSizesOutsideTheSupportedRange) {
  const test::TempDir dir("batch-open-bounds");

  WriterOptions too_small = BatchOptions(1);
  EL_EXPECT_ERROR(EventLogWriter::Open(dir.file("a.log"), too_small), ErrorKind::kInvalidArgument);

  WriterOptions too_large = BatchOptions(kMaxBatchRecords + 1);
  EL_EXPECT_ERROR(EventLogWriter::Open(dir.file("b.log"), too_large), ErrorKind::kLimitExceeded);
}

// The policy label on a result must always describe how that result was produced, so the
// method and the policy have to agree (ADR-006).
TEST(MethodAndPolicyMustAgree) {
  const test::TempDir dir("batch-policy-match");

  EventLogWriter batch_writer =
      EL_EXPECT_OK(EventLogWriter::Open(dir.file("batch.log"), BatchOptions(8)));
  EL_EXPECT_ERROR(batch_writer.AppendAndSync(MakeDeterministicPayload(1, 16)),
                  ErrorKind::kUnsupportedPolicy);
  EL_EXPECT_STATUS_OK(batch_writer.Close());

  EventLogWriter each_writer =
      EL_EXPECT_OK(EventLogWriter::Open(dir.file("each.log"), WriterOptions{}));
  const auto payloads = PayloadsFor(1, 2, 16);
  EL_EXPECT_ERROR(each_writer.AppendBatchAndSync(SpansOf(payloads)), ErrorKind::kUnsupportedPolicy);
  EL_EXPECT_STATUS_OK(each_writer.Close());
}

TEST(BatchWriterResumesAnExistingLog) {
  const test::TempDir dir("batch-resume");
  const auto path = dir.file("events.log");

  {
    EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, BatchOptions(4)));
    const auto payloads = PayloadsFor(1, 4, 24);
    EL_EXPECT_OK(writer.AppendBatchAndSync(SpansOf(payloads)));
    EL_EXPECT_STATUS_OK(writer.Close());
  }

  WriterOptions options = BatchOptions(4);
  options.allow_existing = true;
  EventLogWriter writer = EL_EXPECT_OK(EventLogWriter::Open(path, options));
  EL_CHECK_EQ(writer.durable_through_sequence(), std::uint64_t{4});

  const auto payloads = PayloadsFor(5, 4, 24);
  const BatchAppendResult result = EL_EXPECT_OK(writer.AppendBatchAndSync(SpansOf(payloads)));
  EL_CHECK_EQ(result.first_sequence, std::uint64_t{5});
  EL_CHECK_EQ(result.last_sequence, std::uint64_t{8});
  EL_EXPECT_STATUS_OK(writer.Close());

  EL_CHECK_EQ(EL_EXPECT_OK(ScanLog(path)).last_sequence, std::uint64_t{8});
}

}  // namespace
}  // namespace eventlog
