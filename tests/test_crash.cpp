// CPP-AC10, CPP-AC11 and tests CPP-T11 to CPP-T13: the acknowledged-prefix oracle.
//
// This suite exercises the oracle directly against hand-damaged logs, with no subprocess
// involved. That split matters: a supervisor that reports "Violations: 0" is only worth
// something if the check it runs can actually fail, so the negative controls below feed
// it logs that break each invariant and require it to say so. The subprocess half -- the
// instrumented checkpoints and the SIGKILL -- is driven through the CLI by
// tests/cli_tests.sh, which has the built binary to re-execute.

#include "eventlog/crash_test.h"

#include <string>
#include <utility>
#include <vector>

#include "eventlog/payload.h"
#include "eventlog/record.h"
#include "eventlog/writer.h"
#include "test_support.h"

namespace eventlog {
namespace {

constexpr std::size_t kPayload = 32;
constexpr std::uint64_t kRecordSize = kHeaderSize + kPayload;

void BuildLog(const std::filesystem::path& path, std::uint64_t records,
              std::size_t payload_bytes = kPayload) {
  EventLogWriter writer = EventLogWriter::Open(path, WriterOptions{}).value();
  for (std::uint64_t i = 1; i <= records; ++i) {
    const AppendResult result =
        writer.AppendAndSync(MakeDeterministicPayload(i, payload_bytes)).value();
    if (result.sequence != i) throw test::AssertionFailure("unexpected sequence while building");
  }
  const Status closed = writer.Close();
  if (!closed.ok()) throw test::AssertionFailure("close failed: " + closed.error().message());
}

bool Mentions(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

// --- scenario encoding -------------------------------------------------------

TEST(EveryCheckpointNameRoundTrips) {
  for (std::size_t i = 0; i < kCheckpointCount; ++i) {
    const Checkpoint checkpoint = static_cast<Checkpoint>(i);
    const std::string name(CheckpointName(checkpoint));
    EL_CHECK_NE(name, std::string("unknown"));
    EL_CHECK_EQ(EL_EXPECT_OK(ParseCheckpoint(name)), checkpoint);
  }
}

TEST(UnknownCheckpointIsRejected) {
  EL_EXPECT_ERROR(ParseCheckpoint("after_lunch"), ErrorKind::kInvalidArgument);
  EL_EXPECT_ERROR(ParseCheckpoint(""), ErrorKind::kInvalidArgument);
}

TEST(ScenarioSpecRoundTrips) {
  CrashScenario scenario;
  scenario.records = 9;
  scenario.payload_bytes = 128;
  scenario.policy = SyncPolicy::kSyncBatch;
  scenario.batch_size = 4;
  scenario.checkpoint = Checkpoint::kAfterSyncBeforeAck;
  scenario.kill_unit = 2;
  scenario.short_write_bytes = 37;

  const std::string spec = EncodeScenario(scenario);
  const CrashScenario parsed = EL_EXPECT_OK(ParseScenario(spec));
  EL_CHECK_EQ(EncodeScenario(parsed), spec);
  EL_CHECK_EQ(parsed.records, scenario.records);
  EL_CHECK_EQ(parsed.payload_bytes, scenario.payload_bytes);
  EL_CHECK_EQ(parsed.policy, scenario.policy);
  EL_CHECK_EQ(parsed.batch_size, scenario.batch_size);
  EL_CHECK_EQ(parsed.checkpoint, scenario.checkpoint);
  EL_CHECK_EQ(parsed.kill_unit, scenario.kill_unit);
  EL_CHECK_EQ(parsed.short_write_bytes, scenario.short_write_bytes);
}

TEST(ScenarioSpecRejectsMalformedAndOutOfRangeFields) {
  const std::string base =
      "records=4,payload=8,policy=sync_each,batch=1,at=mid_write,unit=2,short=5";
  EL_EXPECT_OK(ParseScenario(base));

  EL_EXPECT_ERROR(ParseScenario("records=4"), ErrorKind::kInvalidArgument);
  EL_EXPECT_ERROR(ParseScenario("records,payload=8"), ErrorKind::kInvalidArgument);
  EL_EXPECT_ERROR(
      ParseScenario("records=0,payload=8,policy=sync_each,batch=1,at=none,unit=1,short=1"),
      ErrorKind::kInvalidArgument);
  // A kill unit past the end of the run would silently become an uninterrupted run.
  EL_EXPECT_ERROR(
      ParseScenario("records=4,payload=8,policy=sync_each,batch=1,at=mid_write,unit=5,short=5"),
      ErrorKind::kLimitExceeded);
  // Policy and batch size must agree, exactly as they must at Writer::Open.
  EL_EXPECT_ERROR(
      ParseScenario("records=4,payload=8,policy=sync_each,batch=2,at=none,unit=1,short=1"),
      ErrorKind::kInvalidArgument);
  EL_EXPECT_ERROR(
      ParseScenario("records=4,payload=8,policy=sync_batch,batch=1,at=none,unit=1,short=1"),
      ErrorKind::kInvalidArgument);
  // Payload bound and total-bytes bound are both checked before anything is allocated.
  EL_EXPECT_ERROR(
      ParseScenario("records=1,payload=1048577,policy=sync_each,batch=1,at=none,unit=1,short=1"),
      ErrorKind::kLimitExceeded);
  EL_EXPECT_ERROR(
      ParseScenario("records=4000,payload=1048576,policy=sync_each,batch=1,at=none,unit=1,short=1"),
      ErrorKind::kLimitExceeded);
}

// docs/PRD.md sections 2 and 4: write_only produces no durable acknowledgment, so it has
// nothing to compare and MUST NEVER appear in the acknowledged-prefix claim.
TEST(ScenarioSpecRefusesWriteOnly) {
  EL_EXPECT_ERROR(
      ParseScenario("records=4,payload=8,policy=write_only,batch=1,at=none,unit=1,short=1"),
      ErrorKind::kUnsupportedPolicy);
}

TEST(AcknowledgmentUnitsFollowThePolicy) {
  CrashScenario each;
  each.records = 7;
  each.policy = SyncPolicy::kSyncEach;
  each.batch_size = 1;
  EL_CHECK_EQ(AcknowledgmentUnits(each), 7u);

  CrashScenario batched;
  batched.records = 7;
  batched.policy = SyncPolicy::kSyncBatch;
  batched.batch_size = 3;
  // Three full batches would be nine records, so the last unit is a short one.
  EL_CHECK_EQ(AcknowledgmentUnits(batched), 3u);

  batched.records = 6;
  EL_CHECK_EQ(AcknowledgmentUnits(batched), 2u);
}

// --- the oracle accepts what it must ----------------------------------------

TEST(OracleAcceptsALogThatMatchesItsAcknowledgments) {
  const test::TempDir dir("crash-oracle-clean");
  const auto log = dir.file("clean.log");
  BuildLog(log, 6);

  const RunOutcome outcome = EL_EXPECT_OK(VerifyRecoveredLog(log, 6, kPayload, false));
  EL_CHECK_EQ(outcome.violation, std::string());
  EL_CHECK_EQ(outcome.recovered_records, 6u);
  EL_CHECK_EQ(outcome.recovered_last_sequence, 6u);
  EL_CHECK_EQ(outcome.status, LogStatus::kClean);
  EL_CHECK(!outcome.recovered_incomplete_tail);
}

// CPP-AC11: a record that reached the disk but was never acknowledged is permitted. The
// invariant is one-directional, and demanding equality here would be wrong.
TEST(OracleAcceptsValidRecordsThatWereNeverAcknowledged) {
  const test::TempDir dir("crash-oracle-extra");
  const auto log = dir.file("extra.log");
  BuildLog(log, 6);

  const RunOutcome outcome = EL_EXPECT_OK(VerifyRecoveredLog(log, 4, kPayload, true));
  EL_CHECK_EQ(outcome.violation, std::string());
  EL_CHECK_EQ(outcome.acknowledged_through, 4u);
  EL_CHECK_EQ(outcome.recovered_last_sequence, 6u);
}

// --- the oracle catches what it must ----------------------------------------
//
// Each case below damages a log in a way the supported fault model cannot produce, and
// requires the oracle to report it. Without these, "Violations: 0" would be unfalsifiable.

TEST(OracleCatchesALostAcknowledgedRecord) {
  const test::TempDir dir("crash-oracle-lost");
  const auto log = dir.file("lost.log");
  BuildLog(log, 6);
  // Whole records removed, so the log still scans clean: the only thing wrong with it is
  // that it no longer holds everything the supervisor was told was durable.
  test::TruncateFileTo(log, 4 * kRecordSize);

  const RunOutcome outcome = EL_EXPECT_OK(VerifyRecoveredLog(log, 6, kPayload, true));
  EL_CHECK(Mentions(outcome.violation, "lost acknowledged records"));
  EL_CHECK(Mentions(outcome.violation, "through sequence 6"));
  EL_CHECK_EQ(outcome.recovered_last_sequence, 4u);
}

TEST(OracleCatchesHardCorruptionAndLeavesTheFileAlone) {
  const test::TempDir dir("crash-oracle-corrupt");
  const auto log = dir.file("corrupt.log");
  BuildLog(log, 5);
  // A complete record with a bad CRC. Never truncated, whether it is last or not
  // (ADR-002), and not something a single writer stopped between syscalls can produce.
  test::FlipByteAt(log, 2 * kRecordSize + kHeaderSize + 1);
  const std::uint64_t size_before = test::FileSizeOf(log);

  const RunOutcome outcome = EL_EXPECT_OK(VerifyRecoveredLog(log, 2, kPayload, true));
  EL_CHECK(Mentions(outcome.violation, "hard corruption"));
  EL_CHECK(Mentions(outcome.violation, "corrupt_complete_record"));
  EL_CHECK(Mentions(outcome.violation, std::to_string(2 * kRecordSize)));
  EL_CHECK_EQ(outcome.status, LogStatus::kCorruptCompleteRecord);
  EL_CHECK_EQ(test::FileSizeOf(log), size_before);
}

// A record can be structurally perfect -- right sequence, valid CRC -- and still not be
// the record that was written. The oracle regenerates the expected payload rather than
// trusting the file to describe itself.
TEST(OracleCatchesAFabricatedPayload) {
  const test::TempDir dir("crash-oracle-fabricated");
  const auto log = dir.file("fabricated.log");
  BuildLog(log, 4);

  std::vector<std::byte> bytes = test::ReadFileBytes(log);
  const std::vector<std::byte> impostor =
      EncodeRecord(3, MakeDeterministicPayload(999, kPayload)).value();
  EL_CHECK_EQ(impostor.size(), static_cast<std::size_t>(kRecordSize));
  for (std::size_t i = 0; i < impostor.size(); ++i) {
    bytes[static_cast<std::size_t>(2 * kRecordSize) + i] = impostor[i];
  }
  test::WriteFileBytes(log, bytes);

  const RunOutcome outcome = EL_EXPECT_OK(VerifyRecoveredLog(log, 4, kPayload, true));
  EL_CHECK(Mentions(outcome.violation, "payload of recovered record 3"));
  EL_CHECK_EQ(outcome.status, LogStatus::kClean);
}

TEST(OracleCatchesASequenceGap) {
  const test::TempDir dir("crash-oracle-gap");
  const auto log = dir.file("gap.log");
  std::vector<std::byte> bytes;
  for (const std::uint64_t sequence : {std::uint64_t{1}, std::uint64_t{2}, std::uint64_t{4}}) {
    const std::vector<std::byte> record =
        EncodeRecord(sequence, MakeDeterministicPayload(sequence, kPayload)).value();
    bytes.insert(bytes.end(), record.begin(), record.end());
  }
  test::WriteFileBytes(log, bytes);

  const RunOutcome outcome = EL_EXPECT_OK(VerifyRecoveredLog(log, 2, kPayload, true));
  EL_CHECK(Mentions(outcome.violation, "hard corruption"));
  EL_CHECK(Mentions(outcome.violation, "sequence_violation"));
}

// --- the tail the fault model does produce -----------------------------------

TEST(OracleRecoversAnIncompleteTailAfterAKill) {
  const test::TempDir dir("crash-oracle-tail");
  const auto log = dir.file("tail.log");
  BuildLog(log, 5);
  // A torn trailing record: the only tail shape a writer stopped between syscalls can
  // leave behind. Harness-owned recovery discards exactly it (docs/PRD.md section 3).
  test::TruncateFileTo(log, 4 * kRecordSize + 11);

  const RunOutcome outcome = EL_EXPECT_OK(VerifyRecoveredLog(log, 4, kPayload, true));
  EL_CHECK_EQ(outcome.violation, std::string());
  EL_CHECK(outcome.recovered_incomplete_tail);
  EL_CHECK_EQ(outcome.status, LogStatus::kIncompleteTail);
  EL_CHECK_EQ(outcome.recovered_records, 4u);
  EL_CHECK_EQ(test::FileSizeOf(log), 4 * kRecordSize);
}

// The same file, but nobody killed the writer. A writer that returned from every call
// and exited normally cannot leave a partial record behind, so this is a defect rather
// than a crash artifact -- and recovering it quietly would hide that.
TEST(OracleRejectsAnIncompleteTailFromACleanExit) {
  const test::TempDir dir("crash-oracle-tail-clean-exit");
  const auto log = dir.file("tail.log");
  BuildLog(log, 5);
  test::TruncateFileTo(log, 4 * kRecordSize + 11);
  const std::uint64_t size_before = test::FileSizeOf(log);

  const RunOutcome outcome = EL_EXPECT_OK(VerifyRecoveredLog(log, 4, kPayload, false));
  EL_CHECK(Mentions(outcome.violation, "exited normally left an incomplete trailing record"));
  EL_CHECK(!outcome.recovered_incomplete_tail);
  EL_CHECK_EQ(test::FileSizeOf(log), size_before);
}

TEST(OracleAcceptsAnEmptyLogWithNoAcknowledgments) {
  const test::TempDir dir("crash-oracle-empty");
  const auto log = dir.file("empty.log");
  // What a writer killed before its first write leaves: the file exists, created and
  // directory-synced by Open, and holds nothing.
  test::WriteFileBytes(log, {});

  const RunOutcome outcome = EL_EXPECT_OK(VerifyRecoveredLog(log, 0, kPayload, true));
  EL_CHECK_EQ(outcome.violation, std::string());
  EL_CHECK_EQ(outcome.recovered_records, 0u);
}

}  // namespace
}  // namespace eventlog
