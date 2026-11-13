#include "eventlog/recover.h"

#include <string>
#include <system_error>
#include <utility>

namespace eventlog {

Result<RecoveryPlan> PlanRecovery(const std::filesystem::path& path) {
  // Identified before the scan, never after: the recorded identity is then always at
  // least as old as the scan, so a file swapped in later is rejected by ApplyRecovery
  // instead of inheriting the scan of a file it is not.
  Result<FileDescriptor> opened = OpenLogForRead(path);
  if (!opened.ok()) return opened.error();
  Result<FileIdentity> identity = IdentifyFile(opened.value().get());
  if (!identity.ok()) return identity.error();

  // Read-only: this must be safe to run against a log nobody intends to change.
  Result<ScanReport> scan = ScanLog(path);
  if (!scan.ok()) return scan.error();

  RecoveryPlan plan;
  plan.scan = std::move(scan).value();
  plan.identity = identity.value();
  if (plan.scan.status == LogStatus::kIncompleteTail) {
    plan.truncation_required = true;
    plan.truncate_to = plan.scan.valid_prefix_bytes;
    plan.bytes_to_discard = plan.scan.file_size - plan.scan.valid_prefix_bytes;
  }
  return plan;
}

Result<RecoveryOutcome> ApplyRecovery(const std::filesystem::path& path, const RecoveryPlan& plan,
                                      FileOps& ops) {
  if (!plan.truncation_required) {
    std::string reason =
        "nothing to truncate: log status is " + std::string(LogStatusName(plan.scan.status));
    if (plan.scan.status != LogStatus::kClean) {
      reason += ". Hard corruption is never truncated automatically (ADR-002)";
    }
    return Error(ErrorKind::kInvalidArgument, std::move(reason));
  }

  // A destructive operation must not be redirected through a symlink. Checked here and
  // enforced again by O_NOFOLLOW below, which closes the gap between the two.
  std::error_code ec;
  if (std::filesystem::is_symlink(path, ec)) {
    return Error(ErrorKind::kInvalidArgument,
                 path.string() + " is a symlink; refusing to truncate through it");
  }

  Result<FileDescriptor> opened = OpenLogForUpdate(path);
  if (!opened.ok()) return opened.error();
  FileDescriptor fd = std::move(opened).value();

  // Truncating underneath a live writer would corrupt its view of the file.
  Status lock = AcquireExclusiveLock(fd.get(), path);
  if (!lock.ok()) return lock.error();

  // The plan was built from a separate read-only pass. If the file changed in between,
  // the truncation target is stale and could discard valid records -- and valid records
  // are exactly what recovery must never discard (ADR-002). Identity is checked as well
  // as length, because a path can be replaced by a different file of the same size. This
  // rejects a stale plan; it does not make concurrent modification safe, and nothing in
  // docs/PRD.md section 2 supports a second process writing this log.
  Result<FileIdentity> identity = IdentifyFile(fd.get());
  if (!identity.ok()) return identity.error();
  if (identity.value() != plan.identity) {
    return Error(ErrorKind::kInvalidArgument,
                 path.string() + " is no longer the file the preflight examined; re-run recovery");
  }

  Result<std::uint64_t> size = ops.FileSize(fd.get());
  if (!size.ok()) return size.error();
  if (size.value() != plan.scan.file_size) {
    return Error(ErrorKind::kInvalidArgument,
                 "file changed since the preflight (was " + std::to_string(plan.scan.file_size) +
                     " bytes, now " + std::to_string(size.value()) + "); re-run recovery");
  }

  Status truncated = ops.Truncate(fd.get(), plan.truncate_to);
  if (!truncated.ok()) return truncated.error();

  // The shortened length must itself be durable: otherwise a crash here could leave the
  // discarded tail back on disk, and the next writer would append past it.
  Status synced = ops.SyncData(fd.get());
  if (!synced.ok()) return synced.error();

  Status closed = fd.Close();
  if (!closed.ok()) return closed.error();

  return RecoveryOutcome{plan.bytes_to_discard, plan.scan.valid_records, plan.scan.last_sequence};
}

}  // namespace eventlog
