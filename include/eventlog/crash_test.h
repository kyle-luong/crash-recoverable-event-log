#ifndef EVENTLOG_CRASH_TEST_H_
#define EVENTLOG_CRASH_TEST_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "eventlog/error.h"
#include "eventlog/reader.h"
#include "eventlog/writer.h"

namespace eventlog {

// The instrumented points at which the supervisor can stop a writer, from the list in
// docs/PRD.md section 4. Kill timing is controlled, not racy: the child announces that it
// has reached the checkpoint and then stops making progress, so the supervisor kills a
// process whose exact state it knows.
//
// A SIGKILL ends a process. It does not clear the kernel page cache and it is not a power
// failure, so what this harness proves is bounded to the process-crash model (ADR-001).
enum class Checkpoint {
  kNone,                  // run to completion and exit normally; the control case
  kBeforeWrite,           // stopped before write(2) puts any byte of the unit on disk
  kMidWrite,              // stopped after a controlled partial write: a torn record
  kAfterWriteBeforeSync,  // unit fully written, the durability barrier not yet called
  kAfterSyncBeforeAck,    // barrier returned success, supervisor not yet told
  kAfterAck,              // supervisor has already recorded the acknowledgment
};

inline constexpr std::size_t kCheckpointCount = 6;

std::string_view CheckpointName(Checkpoint checkpoint);
Result<Checkpoint> ParseCheckpoint(std::string_view name);

// One scenario: what the child writes, and where it is stopped.
struct CrashScenario {
  std::uint64_t records = 1;
  std::size_t payload_bytes = 64;
  SyncPolicy policy = SyncPolicy::kSyncEach;
  std::size_t batch_size = 1;
  Checkpoint checkpoint = Checkpoint::kNone;
  // 1-based acknowledgment unit to stop at. Ignored when checkpoint is kNone.
  std::uint64_t kill_unit = 1;
  // kMidWrite only: how many bytes of the unit land before the child stops. Clamped to
  // leave the record torn.
  std::size_t short_write_bytes = 1;
};

// Acknowledgment units in a scenario: one per record under sync_each, one per batch under
// sync_batch. The supervisor picks a kill unit from this and the child groups records by
// it, so the single definition lives here.
std::uint64_t AcknowledgmentUnits(const CrashScenario& scenario);

// A scenario as one `key=value,...` token. It is both the child's command line argument
// and how a scenario is named in a violation report, so there is one spelling to keep
// correct and a failing run can be replayed by hand.
std::string EncodeScenario(const CrashScenario& scenario);
Result<CrashScenario> ParseScenario(std::string_view spec);

// What one run proved.
struct RunOutcome {
  // The supervisor's own acknowledgment record, built from frames received before any
  // kill. Independent of the log: this is the oracle the recovered file is checked
  // against, never a value read back out of the file.
  std::uint64_t acknowledged_through = 0;
  std::uint64_t recovered_records = 0;
  std::uint64_t recovered_last_sequence = 0;
  LogStatus status = LogStatus::kClean;  // as first scanned, before any recovery
  bool recovered_incomplete_tail = false;
  bool killed = false;
  // Empty when every invariant held. Otherwise what broke, with the offset when the log
  // can name one.
  std::string violation;

  bool upheld() const { return violation.empty(); }
};

struct CrashTestSummary {
  std::uint64_t scenarios = 0;
  std::uint64_t violations = 0;
  std::uint64_t incomplete_tails_recovered = 0;
  std::uint64_t killed = 0;
  std::uint64_t acknowledged_records = 0;
  std::uint64_t recovered_records = 0;
  // Valid records recovered that were never acknowledged. Permitted, and counted rather
  // than tolerated silently: it is the evidence for CPP-AC11.
  std::uint64_t extra_unacknowledged = 0;
  std::array<std::uint64_t, kCheckpointCount> by_checkpoint{};
  std::vector<std::string> violation_details;
};

struct CrashTestOptions {
  std::uint64_t runs = 100;
  std::uint64_t seed = 42;
  // Scratch root. The harness creates one fresh subdirectory inside it per invocation,
  // writes only there, and removes only what it created.
  std::filesystem::path workdir;
  // The program to re-execute in child mode. Normally this executable.
  std::filesystem::path child_program;
  // Forces every run to one checkpoint instead of cycling through all of them.
  std::optional<Checkpoint> forced_checkpoint;
};

// Checks one recovered log against the supervisor's acknowledgment record.
//
// Spawns nothing: the oracle is separable from the subprocess machinery, which is what
// lets its negative controls be tested directly against hand-damaged logs.
//
// An incomplete trailing record is recovered here, which docs/PRD.md section 3 permits
// for harness-owned recovery. Hard corruption is never truncated (ADR-002); it is
// reported as a violation, because nothing in the supported fault model can produce it.
//
// Returns an Error only when the harness itself could not do its job (an I/O failure).
// A broken invariant is a RunOutcome with `violation` set, not an Error.
Result<RunOutcome> VerifyRecoveredLog(const std::filesystem::path& path,
                                      std::uint64_t acknowledged_through, std::size_t payload_bytes,
                                      bool child_was_killed);

// Runs the whole matrix: spawn, kill at the checkpoint, recover, check, repeat.
Result<CrashTestSummary> RunCrashTest(const CrashTestOptions& options);

// The child side. Writes its frames to `frame_fd` and stops itself at the scenario's
// checkpoint. Returns the process exit code.
int RunCrashChild(const std::filesystem::path& log, const CrashScenario& scenario, int frame_fd);

// This executable's path, for re-execution in child mode. `argv0` is the fallback used
// where /proc/self/exe does not exist.
Result<std::filesystem::path> SelfExecutablePath(std::string_view argv0);

}  // namespace eventlog

#endif  // EVENTLOG_CRASH_TEST_H_
