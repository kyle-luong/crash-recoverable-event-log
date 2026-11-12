#ifndef EVENTLOG_WRITER_H_
#define EVENTLOG_WRITER_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "eventlog/error.h"
#include "eventlog/file_ops.h"
#include "eventlog/record.h"

namespace eventlog {

enum class SyncPolicy {
  kSyncEach,   // one record, one sync, one acknowledgment
  kSyncBatch,  // N records, one sync, whole-batch acknowledgment (milestone M2)
  kWriteOnly,  // diagnostic only: ACCEPTED, never durable (ADR-008)
};

std::string_view SyncPolicyName(SyncPolicy policy);
Result<SyncPolicy> ParseSyncPolicy(std::string_view name);

// Upper bounds on one batch, checked before any batch buffer is allocated (ADR-006).
inline constexpr std::size_t kMaxBatchRecords = 4096;
inline constexpr std::uint64_t kMaxBatchBytes = 64ull << 20;  // 64 MiB of encoded output

struct WriterOptions {
  SyncPolicy policy = SyncPolicy::kSyncEach;
  // Under kSyncEach this must be 1. Under kSyncBatch it is the largest batch this writer
  // will accept, at most kMaxBatchRecords.
  std::size_t batch_size = 1;
  // Required to append to an existing nonempty log. The log must scan clean; a log with
  // an incomplete tail or corruption must go through `recover` first.
  bool allow_existing = false;
  // Null means DefaultFileOps(). Tests substitute a double to inject faults.
  std::shared_ptr<FileOps> file_ops;
};

struct AppendResult {
  std::uint64_t sequence = 0;
  // Highest sequence known durable. 0 means nothing has been acknowledged yet.
  // Never advances past a successful SyncData (ADR-001).
  std::uint64_t durable_through_sequence = 0;
};

// The result of a write_only append. It deliberately carries no durability field at all:
// this policy never calls the barrier, so there is no durable sequence to report and no
// way for a caller to mistake one for being present (ADR-008, docs/PRD.md section 2).
struct AcceptResult {
  std::uint64_t sequence = 0;
  std::uint64_t accepted_through_sequence = 0;
};

// One whole-batch acknowledgment. Every sequence in [first_sequence, last_sequence] is
// durable, or none of them is: a batch is acknowledged as a unit or not at all (ADR-006).
struct BatchAppendResult {
  std::uint64_t first_sequence = 0;
  std::uint64_t last_sequence = 0;
  std::uint64_t records = 0;
  std::uint64_t durable_through_sequence = 0;
};

// Syscall counts for the durability path. These make the batching tradeoff checkable by
// assertion rather than by timing: sync_batch(32) must perform one write and one sync
// where sync_each performs thirty-two of each. Whether that buys proportional throughput
// is a measurement, made by `eventlog bench` and recorded in benchmarks/RESULTS.md.
struct WriterStats {
  std::uint64_t write_calls = 0;  // WriteFully invocations, not short-write retries
  std::uint64_t sync_calls = 0;   // successful and failed SyncData calls on data
  std::uint64_t batches = 0;      // DURABLE acknowledgment units; always 0 under write_only
  std::uint64_t records_accepted = 0;
  std::uint64_t bytes_written = 0;
};

// Single-writer append path for one log file.
//
// Acknowledgment contract: a record is durable only once SyncData has returned success
// for it. accepted_through_sequence() counts records handed to write(2); it says nothing
// about persistence. A failed sync is terminal for this writer (ADR-003).
class EventLogWriter {
 public:
  static Result<EventLogWriter> Open(const std::filesystem::path& path,
                                     const WriterOptions& options);

  EventLogWriter(EventLogWriter&&) noexcept = default;
  EventLogWriter& operator=(EventLogWriter&&) noexcept = default;
  EventLogWriter(const EventLogWriter&) = delete;
  EventLogWriter& operator=(const EventLogWriter&) = delete;

  // Appends one record and syncs it before returning. On any failure no acknowledgment
  // is produced for this record. Requires policy kSyncEach.
  Result<AppendResult> AppendAndSync(std::span<const std::byte> payload);

  // Appends N records as one contiguous write, then syncs once, then acknowledges the
  // whole batch (ADR-006). Requires policy kSyncBatch and 1 <= N <= batch_size. On any
  // failure no record in the batch is acknowledged and the writer becomes terminal;
  // records already on disk are left for recovery to classify.
  Result<BatchAppendResult> AppendBatchAndSync(
      std::span<const std::span<const std::byte>> payloads);

  // Appends one record and returns without calling the durability barrier. Requires
  // policy kWriteOnly. Nothing is acknowledged durable, ever: durable_through_sequence()
  // stays 0 for the life of this writer. Diagnostic only, for measuring what the barrier
  // costs (ADR-008). A failed write is still terminal (ADR-003).
  Result<AcceptResult> AppendWithoutSync(std::span<const std::byte> payload);

  std::uint64_t durable_through_sequence() const { return durable_through_; }
  std::uint64_t accepted_through_sequence() const { return accepted_through_; }
  std::uint64_t file_offset() const { return file_offset_; }
  SyncPolicy policy() const { return policy_; }
  std::size_t batch_size() const { return batch_size_; }
  const WriterStats& stats() const { return stats_; }
  // False once a sync has failed: durability from that point on is unknown.
  bool healthy() const { return healthy_; }

  [[nodiscard]] Status Close();

 private:
  EventLogWriter(FileDescriptor fd, std::shared_ptr<FileOps> ops, SyncPolicy policy,
                 std::size_t batch_size, std::uint64_t last_sequence, std::uint64_t file_offset)
      : fd_(std::move(fd)),
        ops_(std::move(ops)),
        policy_(policy),
        batch_size_(batch_size),
        accepted_through_(last_sequence),
        // write_only makes no durability claim, so a resumed prefix is not one either.
        durable_through_(policy == SyncPolicy::kWriteOnly ? 0 : last_sequence),
        file_offset_(file_offset) {}

  // Shared preconditions for both append paths: open, not terminal, sequence space left.
  Status CheckWritable(std::uint64_t records) const;
  // Writes `encode_buffer_`, advancing acceptance but never durability.
  // `last_sequence` is the highest sequence the buffer contains.
  Status WriteBuffer(std::uint64_t last_sequence, std::uint64_t record_count);
  // WriteBuffer followed by one barrier call, advancing the acknowledgment on success.
  Status WriteAndSyncBuffer(std::uint64_t last_sequence, std::uint64_t record_count);

  FileDescriptor fd_;
  std::shared_ptr<FileOps> ops_;
  SyncPolicy policy_ = SyncPolicy::kSyncEach;
  std::size_t batch_size_ = 1;
  WriterStats stats_;
  std::uint64_t accepted_through_ = 0;
  std::uint64_t durable_through_ = 0;
  std::uint64_t file_offset_ = 0;
  bool healthy_ = true;
  // Reused across appends so the steady-state append path does not allocate.
  std::vector<std::byte> encode_buffer_;
};

}  // namespace eventlog

#endif  // EVENTLOG_WRITER_H_
