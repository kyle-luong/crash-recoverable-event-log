#include "eventlog/writer.h"

#include <limits>
#include <string>
#include <utility>

#include "eventlog/reader.h"

namespace eventlog {

std::string_view SyncPolicyName(SyncPolicy policy) {
  switch (policy) {
    case SyncPolicy::kSyncEach:
      return "sync_each";
    case SyncPolicy::kSyncBatch:
      return "sync_batch";
    case SyncPolicy::kWriteOnly:
      return "write_only";
  }
  return "unknown";
}

Result<SyncPolicy> ParseSyncPolicy(std::string_view name) {
  if (name == "sync_each") return SyncPolicy::kSyncEach;
  if (name == "sync_batch") return SyncPolicy::kSyncBatch;
  if (name == "write_only") return SyncPolicy::kWriteOnly;
  return Error(ErrorKind::kInvalidArgument,
               "unknown sync policy \"" + std::string(name) +
                   "\"; expected sync_each, sync_batch, or write_only");
}

Result<EventLogWriter> EventLogWriter::Open(const std::filesystem::path& path,
                                            const WriterOptions& options) {
  // Milestone M1 implements one policy. The others are rejected here rather than
  // silently degraded, so no code path can report an unimplemented policy's result as
  // durable (docs/PLAN.md, milestone 1 scope boundary).
  if (options.policy != SyncPolicy::kSyncEach) {
    return Error(ErrorKind::kUnsupportedPolicy,
                 std::string(SyncPolicyName(options.policy)) +
                     " is not implemented yet; milestone M1 provides sync_each only");
  }
  if (options.batch_size != 1) {
    return Error(ErrorKind::kInvalidArgument, "sync_each requires batch-size 1");
  }

  std::shared_ptr<FileOps> ops = options.file_ops ? options.file_ops : DefaultFileOps();

  Result<OpenedFile> opened = OpenLogForAppend(path);
  if (!opened.ok()) return opened.error();
  OpenedFile file = std::move(opened).value();

  // Taken before anything is written, and released by close(). Cooperating writers only.
  Status lock = AcquireExclusiveLock(file.fd.get(), path);
  if (!lock.ok()) return lock.error();

  if (file.created) {
    // A brand-new file's directory entry must be durable before any record in it can be
    // acknowledged, or the first acknowledgment could outlive the file name (ADR-001).
    const std::filesystem::path parent =
        path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    Status directory = ops->SyncDirectory(parent);
    if (!directory.ok()) return directory.error();
  }

  Result<std::uint64_t> size = ops->FileSize(file.fd.get());
  if (!size.ok()) return size.error();

  std::uint64_t last_sequence = 0;
  if (size.value() > 0) {
    if (!options.allow_existing) {
      return Error(ErrorKind::kInvalidArgument,
                   path.string() + " already holds " + std::to_string(size.value()) +
                       " bytes; pass --append-existing to continue it or --overwrite to "
                       "replace it");
    }
    // Appending to a damaged log would bury the damage mid-file, where it can never be
    // recovered. Recovery is a separate, explicit step (ADR-002).
    Result<ScanReport> scan = ScanLog(path);
    if (!scan.ok()) return scan.error();
    if (!scan.value().clean()) {
      return Error(ErrorKind::kCorruptRecord, "refusing to append to a log whose status is " +
                                                  std::string(LogStatusName(scan.value().status)) +
                                                  " (" + scan.value().detail +
                                                  "); run `eventlog recover` first")
          .at_offset(scan.value().problem_offset);
    }
    last_sequence = scan.value().last_sequence;
  }

  EventLogWriter writer(std::move(file.fd), std::move(ops), options.policy, last_sequence,
                        size.value());
  return writer;
}

Result<AppendResult> EventLogWriter::AppendAndSync(std::span<const std::byte> payload) {
  if (!fd_.valid()) {
    return Error(ErrorKind::kInvalidArgument, "writer is closed");
  }
  if (!healthy_) {
    // ADR-003: a failed write or sync is terminal. Retrying a sync on Linux can return
    // success after the dirty page was already discarded, which would manufacture a
    // false acknowledgment.
    return Error(ErrorKind::kUnknownDurability,
                 "writer stopped after an earlier failure; durability beyond sequence " +
                     std::to_string(durable_through_) +
                     " is unknown. Close this writer, verify the log, and reopen it.");
  }
  if (payload.size() > kMaxPayloadBytes) {
    return Error(ErrorKind::kLimitExceeded, "payload is " + std::to_string(payload.size()) +
                                                " bytes; the v1 limit is " +
                                                std::to_string(kMaxPayloadBytes));
  }
  if (accepted_through_ == std::numeric_limits<std::uint64_t>::max()) {
    return Error(ErrorKind::kLimitExceeded, "the 64-bit sequence space is exhausted");
  }

  const std::uint64_t sequence = accepted_through_ + 1;
  Status encoded = EncodeRecordInto(encode_buffer_, sequence, payload);
  if (!encoded.ok()) return encoded.error();

  const std::uint64_t record_offset = file_offset_;
  Status written = WriteFully(*ops_, fd_.get(), encode_buffer_, record_offset);
  if (!written.ok()) {
    // Some bytes may have landed. Whatever is on disk is a strict prefix of this record,
    // so the log now ends in a recoverable incomplete tail. Appending another record
    // would bury that partial record mid-file and make it unrecoverable, so the writer
    // stops here. This record is not acknowledged.
    healthy_ = false;
    return written.error();
  }

  file_offset_ = record_offset + encode_buffer_.size();
  accepted_through_ = sequence;

  Status synced = ops_->SyncData(fd_.get());
  if (!synced.ok()) {
    healthy_ = false;
    Error error(ErrorKind::kUnknownDurability,
                std::string(SyncPrimitiveName()) + " failed after writing sequence " +
                    std::to_string(sequence) + "; durability from sequence " +
                    std::to_string(durable_through_ + 1) + " onward is unknown (" +
                    synced.error().message() + ")",
                synced.error().errno_value());
    return error.at_offset(record_offset);
  }

  // The only place durable_through_ ever advances.
  durable_through_ = sequence;
  return AppendResult{sequence, durable_through_};
}

Status EventLogWriter::Close() {
  // No final sync: under sync_each every acknowledged record was already synced, and
  // anything unsynced was never acknowledged. close() releases the advisory lock.
  return fd_.Close();
}

}  // namespace eventlog
