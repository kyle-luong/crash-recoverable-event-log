#ifndef EVENTLOG_RECOVER_H_
#define EVENTLOG_RECOVER_H_

#include <cstdint>
#include <filesystem>

#include "eventlog/error.h"
#include "eventlog/file_ops.h"
#include "eventlog/reader.h"

namespace eventlog {

struct RecoveryPlan {
  ScanReport scan;
  // True only for LogStatus::kIncompleteTail. Hard corruption is never truncated.
  bool truncation_required = false;
  std::uint64_t truncate_to = 0;
  std::uint64_t bytes_to_discard = 0;
};

// Read-only preflight. Says exactly what a truncation would discard, and changes nothing.
Result<RecoveryPlan> PlanRecovery(const std::filesystem::path& path);

struct RecoveryOutcome {
  std::uint64_t bytes_discarded = 0;
  std::uint64_t valid_records = 0;
  std::uint64_t last_sequence = 0;
};

// Truncates a structurally incomplete trailing record away, then syncs so the shortened
// length is itself durable. Refuses any plan that does not require truncation, and
// refuses to act through a symlink (ADR-002, docs/PRD.md section 6).
Result<RecoveryOutcome> ApplyRecovery(const std::filesystem::path& path, const RecoveryPlan& plan,
                                      FileOps& ops);

}  // namespace eventlog

#endif  // EVENTLOG_RECOVER_H_
