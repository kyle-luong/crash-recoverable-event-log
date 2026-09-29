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
  kWriteOnly,  // diagnostic only: ACCEPTED, never durable (milestone M4)
};

std::string_view SyncPolicyName(SyncPolicy policy);
Result<SyncPolicy> ParseSyncPolicy(std::string_view name);

struct WriterOptions {
  SyncPolicy policy = SyncPolicy::kSyncEach;
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

  // Appends one record and, under kSyncEach, syncs it before returning. On any failure
  // no acknowledgment is produced for this record.
  Result<AppendResult> AppendAndSync(std::span<const std::byte> payload);

  std::uint64_t durable_through_sequence() const { return durable_through_; }
  std::uint64_t accepted_through_sequence() const { return accepted_through_; }
  std::uint64_t file_offset() const { return file_offset_; }
  SyncPolicy policy() const { return policy_; }
  // False once a sync has failed: durability from that point on is unknown.
  bool healthy() const { return healthy_; }

  [[nodiscard]] Status Close();

 private:
  EventLogWriter(FileDescriptor fd, std::shared_ptr<FileOps> ops, SyncPolicy policy,
                 std::uint64_t last_sequence, std::uint64_t file_offset)
      : fd_(std::move(fd)),
        ops_(std::move(ops)),
        policy_(policy),
        accepted_through_(last_sequence),
        durable_through_(last_sequence),
        file_offset_(file_offset) {}

  FileDescriptor fd_;
  std::shared_ptr<FileOps> ops_;
  SyncPolicy policy_ = SyncPolicy::kSyncEach;
  std::uint64_t accepted_through_ = 0;
  std::uint64_t durable_through_ = 0;
  std::uint64_t file_offset_ = 0;
  bool healthy_ = true;
  // Reused across appends so the steady-state append path does not allocate.
  std::vector<std::byte> encode_buffer_;
};

}  // namespace eventlog

#endif  // EVENTLOG_WRITER_H_
