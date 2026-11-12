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
  if (options.policy == SyncPolicy::kWriteOnly && options.batch_size != 1) {
    // write_only has no acknowledgment unit to size, so a batch size would describe
    // nothing (ADR-008).
    return Error(ErrorKind::kInvalidArgument, "write_only requires batch-size 1");
  }
  if (options.policy == SyncPolicy::kSyncEach && options.batch_size != 1) {
    return Error(ErrorKind::kInvalidArgument, "sync_each requires batch-size 1");
  }
  if (options.policy == SyncPolicy::kSyncBatch) {
    if (options.batch_size < 2) {
      return Error(ErrorKind::kInvalidArgument,
                   "sync_batch requires batch-size of at least 2; use sync_each for 1");
    }
    if (options.batch_size > kMaxBatchRecords) {
      return Error(ErrorKind::kLimitExceeded, "batch-size " + std::to_string(options.batch_size) +
                                                  " exceeds the limit of " +
                                                  std::to_string(kMaxBatchRecords));
    }
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

    // The scan proves those records are present and structurally complete. It does not
    // prove any of them was ever synced: a previous writer may have been killed between
    // write() and SyncData, or have run write_only. Presence is not durability, so this
    // session establishes it with one sync rather than inheriting a claim it cannot check
    // (ADR-001). Only after this does durable_through_ start at last_sequence.
    //
    // write_only is exempt because it claims nothing: syncing here would make this policy
    // call the barrier, which is the one thing it is defined not to do. Its
    // durable_through_sequence() stays 0 instead (ADR-008).
    if (options.policy != SyncPolicy::kWriteOnly) {
      Status resumed = ops->SyncData(file.fd.get());
      if (!resumed.ok()) {
        return Error(ErrorKind::kUnknownDurability,
                     std::string(SyncPrimitiveName()) + " failed while resuming " + path.string() +
                         "; whether its " + std::to_string(scan.value().valid_records) +
                         " existing records are durable is unknown (" + resumed.error().message() +
                         ")",
                     resumed.error().errno_value());
      }
    }
  }

  EventLogWriter writer(std::move(file.fd), std::move(ops), options.policy, options.batch_size,
                        last_sequence, size.value());
  return writer;
}

Status EventLogWriter::CheckWritable(std::uint64_t records) const {
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
  if (records > std::numeric_limits<std::uint64_t>::max() - accepted_through_) {
    return Error(ErrorKind::kLimitExceeded, "the 64-bit sequence space is exhausted");
  }
  return Status::Ok();
}

// Acceptance only: the bytes were handed to write(2) and nothing more is claimed.
Status EventLogWriter::WriteBuffer(std::uint64_t last_sequence, std::uint64_t record_count) {
  ++stats_.write_calls;
  Status written = WriteFully(*ops_, fd_.get(), encode_buffer_, file_offset_);
  if (!written.ok()) {
    // Some bytes may have landed. Whatever is on disk is a strict prefix of this write,
    // so the log now ends in a recoverable incomplete tail -- or, mid-batch, in whole
    // records that were never acknowledged. Appending past either would bury it
    // mid-file and make it unrecoverable, so the writer stops here.
    healthy_ = false;
    return written;
  }

  file_offset_ += encode_buffer_.size();
  accepted_through_ = last_sequence;
  stats_.records_accepted += record_count;
  stats_.bytes_written += encode_buffer_.size();
  return Status::Ok();
}

// One write, one sync, then the acknowledgment. Both failure paths are terminal and
// neither acknowledges anything, which is what makes the batch an all-or-nothing unit.
Status EventLogWriter::WriteAndSyncBuffer(std::uint64_t last_sequence, std::uint64_t record_count) {
  const std::uint64_t record_offset = file_offset_;

  Status written = WriteBuffer(last_sequence, record_count);
  if (!written.ok()) return written;

  ++stats_.sync_calls;
  Status synced = ops_->SyncData(fd_.get());
  if (!synced.ok()) {
    healthy_ = false;
    Error error(ErrorKind::kUnknownDurability,
                std::string(SyncPrimitiveName()) + " failed after writing " +
                    std::to_string(record_count) + " record(s) through sequence " +
                    std::to_string(last_sequence) + "; durability from sequence " +
                    std::to_string(durable_through_ + 1) + " onward is unknown (" +
                    synced.error().message() + ")",
                synced.error().errno_value());
    return error.at_offset(record_offset);
  }

  // The only place durable_through_ ever advances.
  durable_through_ = last_sequence;
  ++stats_.batches;
  return Status::Ok();
}

Result<AppendResult> EventLogWriter::AppendAndSync(std::span<const std::byte> payload) {
  if (policy_ != SyncPolicy::kSyncEach) {
    return Error(ErrorKind::kUnsupportedPolicy,
                 "this writer's policy is " + std::string(SyncPolicyName(policy_)) +
                     "; use AppendBatchAndSync, so the policy on a result always describes "
                     "how that result was produced (ADR-006)");
  }
  Status writable = CheckWritable(1);
  if (!writable.ok()) return writable.error();
  if (payload.size() > kMaxPayloadBytes) {
    return Error(ErrorKind::kLimitExceeded, "payload is " + std::to_string(payload.size()) +
                                                " bytes; the v1 limit is " +
                                                std::to_string(kMaxPayloadBytes));
  }

  const std::uint64_t sequence = accepted_through_ + 1;
  Status encoded = EncodeRecordInto(encode_buffer_, sequence, payload);
  if (!encoded.ok()) return encoded.error();

  Status done = WriteAndSyncBuffer(sequence, 1);
  if (!done.ok()) return done.error();
  return AppendResult{sequence, durable_through_};
}

Result<BatchAppendResult> EventLogWriter::AppendBatchAndSync(
    std::span<const std::span<const std::byte>> payloads) {
  if (policy_ != SyncPolicy::kSyncBatch) {
    return Error(ErrorKind::kUnsupportedPolicy, "this writer's policy is " +
                                                    std::string(SyncPolicyName(policy_)) +
                                                    "; batch appends require sync_batch (ADR-006)");
  }
  if (payloads.empty()) {
    return Error(ErrorKind::kInvalidArgument,
                 "an empty batch has no acknowledgment boundary; pass at least one record");
  }
  if (payloads.size() > batch_size_) {
    return Error(ErrorKind::kLimitExceeded, "batch of " + std::to_string(payloads.size()) +
                                                " records exceeds this writer's batch-size of " +
                                                std::to_string(batch_size_));
  }
  Status writable = CheckWritable(payloads.size());
  if (!writable.ok()) return writable.error();

  // Every payload is bounded, and the encoded total is bounded, BEFORE the batch buffer
  // is sized from it. kMaxBatchRecords * (kHeaderSize + kMaxPayloadBytes) cannot overflow
  // a uint64, so the running total is safe to accumulate.
  std::uint64_t total_bytes = 0;
  for (std::size_t i = 0; i < payloads.size(); ++i) {
    if (payloads[i].size() > kMaxPayloadBytes) {
      return Error(ErrorKind::kLimitExceeded, "record " + std::to_string(i) + " of the batch is " +
                                                  std::to_string(payloads[i].size()) +
                                                  " bytes; the v1 limit is " +
                                                  std::to_string(kMaxPayloadBytes));
    }
    total_bytes += static_cast<std::uint64_t>(kHeaderSize) + payloads[i].size();
  }
  if (total_bytes > kMaxBatchBytes) {
    return Error(ErrorKind::kLimitExceeded, "batch encodes to " + std::to_string(total_bytes) +
                                                " bytes; the limit is " +
                                                std::to_string(kMaxBatchBytes));
  }

  const std::uint64_t first_sequence = accepted_through_ + 1;
  const std::uint64_t last_sequence = accepted_through_ + payloads.size();

  encode_buffer_.clear();
  encode_buffer_.reserve(static_cast<std::size_t>(total_bytes));
  for (std::size_t i = 0; i < payloads.size(); ++i) {
    Status encoded = AppendRecordTo(encode_buffer_, first_sequence + i, payloads[i]);
    if (!encoded.ok()) return encoded.error();
  }

  Status done = WriteAndSyncBuffer(last_sequence, payloads.size());
  if (!done.ok()) return done.error();
  return BatchAppendResult{first_sequence, last_sequence, payloads.size(), durable_through_};
}

Result<AcceptResult> EventLogWriter::AppendWithoutSync(std::span<const std::byte> payload) {
  if (policy_ != SyncPolicy::kWriteOnly) {
    return Error(ErrorKind::kUnsupportedPolicy,
                 "this writer's policy is " + std::string(SyncPolicyName(policy_)) +
                     "; AppendWithoutSync requires write_only, so a result's type always says "
                     "whether a durability claim exists (ADR-008)");
  }
  Status writable = CheckWritable(1);
  if (!writable.ok()) return writable.error();
  if (payload.size() > kMaxPayloadBytes) {
    return Error(ErrorKind::kLimitExceeded, "payload is " + std::to_string(payload.size()) +
                                                " bytes; the v1 limit is " +
                                                std::to_string(kMaxPayloadBytes));
  }

  const std::uint64_t sequence = accepted_through_ + 1;
  Status encoded = EncodeRecordInto(encode_buffer_, sequence, payload);
  if (!encoded.ok()) return encoded.error();

  // No barrier call, so stats_.batches and durable_through_ are both left alone. They
  // stay 0 for this writer's whole life, which is the point of the policy.
  Status done = WriteBuffer(sequence, 1);
  if (!done.ok()) return done.error();
  return AcceptResult{sequence, accepted_through_};
}

Status EventLogWriter::Close() {
  // No final sync: under sync_each every acknowledged record was already synced, and
  // anything unsynced was never acknowledged. close() releases the advisory lock.
  return fd_.Close();
}

}  // namespace eventlog
