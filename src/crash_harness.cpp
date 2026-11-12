#include "eventlog/crash_test.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/prctl.h>
#endif

#include <algorithm>
#include <cerrno>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "eventlog/payload.h"
#include "eventlog/recover.h"
#include "eventlog/scratch_dir.h"

namespace eventlog {
namespace {

// Matches kExitHarnessFailure in cli.h: a harness that cannot do its job is reported
// separately from a log that broke an invariant (docs/PRD.md section 3).
constexpr int kChildHarnessFailure = 3;

// Bounds on a hand-written scenario, so a bad spec cannot ask for an unreasonable run.
constexpr std::uint64_t kMaxScenarioRecords = 4096;
constexpr std::uint64_t kMaxScenarioBytes = 64ull << 20;

// --- frames ------------------------------------------------------------------
//
// The child reports to the supervisor as one short line per event:
//
//   ack <first> <last>   a whole acknowledgment unit is durable
//   park <checkpoint>    the child has reached its checkpoint and stopped
//   done <sequence>      the child finished and is about to exit 0
//   fail <message>       the child could not continue
//
// A line is far below PIPE_BUF, so the kernel delivers it in one piece. That is what
// lets the supervisor say it never kills a child while an acknowledgment frame is half
// received (docs/PRD.md section 4); the loop below exists only for completeness.

Status SendFrame(int fd, std::string line) {
  // An embedded newline would split one frame into two. Error messages can contain them.
  std::replace(line.begin(), line.end(), '\n', ' ');
  line.push_back('\n');
  std::size_t written = 0;
  while (written < line.size()) {
    const ssize_t got = ::write(fd, line.data() + written, line.size() - written);
    if (got < 0) {
      if (errno == EINTR) continue;
      return IoError("write", errno, "crash-test frame channel");
    }
    written += static_cast<std::size_t>(got);
  }
  return Status::Ok();
}

// Announces the checkpoint, then stops making progress and waits to be killed. pause()
// cannot return for SIGKILL, which is uncatchable, so this never resumes. Parking rather
// than self-terminating is what makes the supervisor the one that issues the kill, and
// what makes the kill point exact instead of a timing guess.
[[noreturn]] void Park(int frame_fd, Checkpoint checkpoint) {
  Status sent = SendFrame(frame_fd, "park " + std::string(CheckpointName(checkpoint)));
  // If the supervisor is already gone there is nobody to park for.
  if (!sent.ok()) ::_exit(kChildHarnessFailure);
  for (;;) ::pause();
}

int FailChild(int frame_fd, const std::string& message) {
  Status sent = SendFrame(frame_fd, "fail " + message);
  static_cast<void>(sent);  // nothing better to do if the supervisor is already gone
  return kChildHarnessFailure;
}

// --- child-side instrumentation ----------------------------------------------

// Wraps the real POSIX operations and stops the process at one instrumented point. It is
// armed by the child immediately before the append call that is to be interrupted, so it
// can never fire on Open's own syscalls or on an earlier unit.
class CheckpointFileOps final : public FileOps {
 public:
  CheckpointFileOps(Checkpoint checkpoint, std::size_t short_write_bytes, int frame_fd)
      : checkpoint_(checkpoint), short_write_bytes_(short_write_bytes), frame_fd_(frame_fd) {}

  void Arm() { armed_ = true; }

  Result<std::size_t> Write(int fd, std::span<const std::byte> data) override {
    if (armed_ && checkpoint_ == Checkpoint::kBeforeWrite) Park(frame_fd_, checkpoint_);
    if (armed_ && checkpoint_ == Checkpoint::kMidWrite) {
      if (data.empty()) Park(frame_fd_, checkpoint_);
      // A real partial write, then no further progress. The log is left ending in a
      // strict byte-prefix of a record, which is the only tail shape a single writer
      // stopped between syscalls can produce (ADR-002). Clamped below the full length so
      // the stop never lands on a clean record boundary.
      std::size_t keep = short_write_bytes_ == 0 ? 1 : short_write_bytes_;
      if (keep >= data.size()) keep = data.size() - 1;
      if (keep > 0) {
        Result<std::size_t> partial = real_.Write(fd, data.first(keep));
        if (!partial.ok()) return partial.error();
      }
      Park(frame_fd_, checkpoint_);
    }
    return real_.Write(fd, data);
  }

  Status SyncData(int fd) override {
    if (armed_ && checkpoint_ == Checkpoint::kAfterWriteBeforeSync) Park(frame_fd_, checkpoint_);
    Status synced = real_.SyncData(fd);
    if (!synced.ok()) return synced;
    // Durable, but the supervisor has not been told. A record recovered from here is a
    // permitted extra, not a lost acknowledgment (CPP-AC11).
    if (armed_ && checkpoint_ == Checkpoint::kAfterSyncBeforeAck) Park(frame_fd_, checkpoint_);
    return Status::Ok();
  }

  Status SyncDirectory(const std::filesystem::path& directory) override {
    return real_.SyncDirectory(directory);
  }
  Status Truncate(int fd, std::uint64_t length) override { return real_.Truncate(fd, length); }
  Result<std::uint64_t> FileSize(int fd) override { return real_.FileSize(fd); }

 private:
  PosixFileOps real_;
  Checkpoint checkpoint_ = Checkpoint::kNone;
  std::size_t short_write_bytes_ = 1;
  int frame_fd_ = -1;
  bool armed_ = false;
};

// --- supervisor-side process and frame handling ------------------------------

// Nothing may outlive the supervisor: a parked child holds the log's advisory lock until
// it dies, so the kill and reap happen on every exit path.
class ChildProcess {
 public:
  explicit ChildProcess(pid_t pid) : pid_(pid) {}
  ~ChildProcess() {
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
      int status = 0;
      while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
      }
    }
  }
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  Status Kill() {
    if (::kill(pid_, SIGKILL) == 0) return Status::Ok();
    // ESRCH means it is already gone, which is the state Kill wanted.
    if (errno == ESRCH) return Status::Ok();
    return IoError("kill", errno, "crash-test child");
  }

  Result<int> Wait() {
    int status = 0;
    while (::waitpid(pid_, &status, 0) < 0) {
      if (errno == EINTR) continue;
      return IoError("waitpid", errno, "crash-test child");
    }
    pid_ = -1;
    return status;
  }

 private:
  pid_t pid_ = -1;
};

class FrameStream {
 public:
  explicit FrameStream(int fd) : fd_(fd) {}

  // One complete frame, or nullopt once the child's end of the pipe is closed. A
  // trailing partial line is discarded rather than acted on: half an acknowledgment
  // frame is not an acknowledgment.
  Result<std::optional<std::string>> Next() {
    for (;;) {
      const std::size_t newline = buffer_.find('\n');
      if (newline != std::string::npos) {
        std::string frame = buffer_.substr(0, newline);
        buffer_.erase(0, newline + 1);
        return std::optional<std::string>(std::move(frame));
      }
      char chunk[256];
      const ssize_t got = ::read(fd_, chunk, sizeof(chunk));
      if (got < 0) {
        if (errno == EINTR) continue;
        return IoError("read", errno, "crash-test frame channel");
      }
      if (got == 0) return std::optional<std::string>{};
      buffer_.append(chunk, static_cast<std::size_t>(got));
    }
  }

 private:
  int fd_ = -1;
  std::string buffer_;
};

bool StartsWith(const std::string& text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

// Parses "<first> <last>" out of an ack frame.
Result<std::pair<std::uint64_t, std::uint64_t>> ParseAck(const std::string& payload) {
  const std::size_t space = payload.find(' ');
  if (space == std::string::npos) {
    return Error(ErrorKind::kInternal, "malformed ack frame \"" + payload + "\"");
  }
  const std::string first = payload.substr(0, space);
  const std::string last = payload.substr(space + 1);
  if (first.empty() || last.empty() || first.find_first_not_of("0123456789") != std::string::npos ||
      last.find_first_not_of("0123456789") != std::string::npos) {
    return Error(ErrorKind::kInternal, "malformed ack frame \"" + payload + "\"");
  }
  return std::pair<std::uint64_t, std::uint64_t>(std::stoull(first), std::stoull(last));
}

Result<RunOutcome> RunOneScenario(const CrashScenario& scenario, const std::filesystem::path& log,
                                  const std::filesystem::path& program) {
  const std::string spec = EncodeScenario(scenario);

  // argv is built before the fork: between fork and exec the child does no allocation.
  std::vector<std::string> owned = {program.string(), "crash-child", log.string(), spec};
  std::vector<char*> argv;
  argv.reserve(owned.size() + 1);
  for (std::string& argument : owned) argv.push_back(argument.data());
  argv.push_back(nullptr);

  int fds[2] = {-1, -1};
  if (::pipe(fds) != 0) return IoError("pipe", errno, "crash-test frame channel");
  FileDescriptor read_end(fds[0]);
  FileDescriptor write_end(fds[1]);

  const pid_t pid = ::fork();
  if (pid < 0) return IoError("fork", errno, program.string());
  if (pid == 0) {
    // Child. The frame channel becomes stdout; stderr stays inherited so a crash report
    // from the child is visible in test output.
    if (::dup2(fds[1], STDOUT_FILENO) < 0) ::_exit(kChildHarnessFailure);
    if (fds[0] != STDOUT_FILENO) ::close(fds[0]);
    if (fds[1] != STDOUT_FILENO) ::close(fds[1]);
#if defined(__linux__)
    // If this supervisor dies without reaping, the kernel kills the child rather than
    // leaving a parked process holding a lock forever.
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
    ::execv(argv[0], argv.data());
    ::_exit(kChildHarnessFailure);
  }

  ChildProcess child(pid);
  // The supervisor's copy must go, or read() never sees end of file.
  write_end.reset();

  std::uint64_t acknowledged_through = 0;
  bool parked = false;
  bool saw_done = false;
  std::string park_checkpoint;
  std::string child_failure;

  FrameStream frames(read_end.get());
  for (;;) {
    Result<std::optional<std::string>> next = frames.Next();
    if (!next.ok()) return next.error();
    if (!next.value().has_value()) break;
    const std::string frame = std::move(next).value().value();

    if (StartsWith(frame, "ack ")) {
      Result<std::pair<std::uint64_t, std::uint64_t>> range = ParseAck(frame.substr(4));
      if (!range.ok()) return range.error();
      if (range.value().first != acknowledged_through + 1) {
        return Error(ErrorKind::kInternal, "child acknowledged sequence " +
                                               std::to_string(range.value().first) +
                                               " out of order (" + spec + ")");
      }
      // Recorded here, in the supervisor's own memory, before anything else happens --
      // in particular before the kill that the next frame may trigger. This record is
      // the oracle; nothing read back out of the log ever feeds it.
      acknowledged_through = range.value().second;
    } else if (StartsWith(frame, "park ")) {
      parked = true;
      park_checkpoint = frame.substr(5);
      break;
    } else if (StartsWith(frame, "done ")) {
      saw_done = true;
    } else if (StartsWith(frame, "fail ")) {
      child_failure = frame.substr(5);
    } else {
      return Error(ErrorKind::kInternal, "unrecognized frame \"" + frame + "\" (" + spec + ")");
    }
  }

  if (parked) {
    Status killed = child.Kill();
    if (!killed.ok()) return killed.error();
  }
  Result<int> waited = child.Wait();
  if (!waited.ok()) return waited.error();
  // Not const: macOS's W* macros reinterpret the address of their argument, which drops
  // a const qualifier and trips -Wcast-qual.
  int status = waited.value();

  if (parked) {
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
      return Error(ErrorKind::kInternal, "child parked at " + park_checkpoint +
                                             " but did not die of SIGKILL (" + spec + ")");
    }
  } else {
    if (!child_failure.empty()) {
      return Error(ErrorKind::kInternal,
                   "crash-test child failed: " + child_failure + " (" + spec + ")");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      return Error(ErrorKind::kInternal, "crash-test child did not exit cleanly (" + spec + ")");
    }
    if (!saw_done) {
      return Error(ErrorKind::kInternal,
                   "child exited 0 without reporting completion (" + spec + ")");
    }
  }

  Result<RunOutcome> verified =
      VerifyRecoveredLog(log, acknowledged_through, scenario.payload_bytes, parked);
  if (!verified.ok()) return verified.error();
  RunOutcome outcome = std::move(verified).value();

  // A writer that was never interrupted must leave exactly what it acknowledged. The
  // general invariant permits extras; this control case does not.
  if (!parked && outcome.upheld() && outcome.recovered_last_sequence != acknowledged_through) {
    outcome.violation = "a writer that exited normally acknowledged through sequence " +
                        std::to_string(acknowledged_through) + " but its log recovered " +
                        std::to_string(outcome.recovered_last_sequence);
  }
  return outcome;
}

CrashScenario MakeScenario(std::uint64_t& rng, std::uint64_t index,
                           const std::optional<Checkpoint>& forced) {
  static constexpr std::array<Checkpoint, kCheckpointCount> kCycle = {
      Checkpoint::kNone,
      Checkpoint::kBeforeWrite,
      Checkpoint::kMidWrite,
      Checkpoint::kAfterWriteBeforeSync,
      Checkpoint::kAfterSyncBeforeAck,
      Checkpoint::kAfterAck};
  static constexpr std::array<std::size_t, 7> kPayloadSizes = {0, 1, 17, 64, 128, 1024, 4096};

  CrashScenario scenario;
  // Cycled rather than drawn at random, so every checkpoint is exercised even at a small
  // --runs; the seed varies the workload around it.
  scenario.checkpoint = forced.has_value() ? *forced : kCycle[index % kCheckpointCount];
  scenario.payload_bytes =
      kPayloadSizes[static_cast<std::size_t>(SplitMix64(rng) % kPayloadSizes.size())];
  if (SplitMix64(rng) % 2 == 0) {
    scenario.policy = SyncPolicy::kSyncEach;
    scenario.batch_size = 1;
  } else {
    scenario.policy = SyncPolicy::kSyncBatch;
    scenario.batch_size = 2 + static_cast<std::size_t>(SplitMix64(rng) % 7);
  }
  scenario.records = 1 + SplitMix64(rng) % 12;

  const std::uint64_t units = AcknowledgmentUnits(scenario);
  // Always inside the run: a kill unit past the end would silently become a clean run.
  scenario.kill_unit = 1 + SplitMix64(rng) % units;

  const std::uint64_t group = scenario.policy == SyncPolicy::kSyncBatch ? scenario.batch_size : 1;
  const std::uint64_t in_unit =
      std::min<std::uint64_t>(group, scenario.records - (scenario.kill_unit - 1) * group);
  const std::uint64_t record_size =
      static_cast<std::uint64_t>(kHeaderSize) + scenario.payload_bytes;
  const std::uint64_t unit_bytes = in_unit * record_size;

  // Somewhere strictly inside the unit, and never on a record boundary: a stop that
  // happens to land between two whole records leaves no torn record, which would quietly
  // make this a different scenario. A record is at least kHeaderSize bytes, so stepping
  // back off a boundary cannot reach zero.
  std::uint64_t partial = 1 + SplitMix64(rng) % (unit_bytes - 1);
  if (partial % record_size == 0) --partial;
  scenario.short_write_bytes = static_cast<std::size_t>(partial);
  return scenario;
}

}  // namespace

// --- names and scenario encoding ---------------------------------------------

std::string_view CheckpointName(Checkpoint checkpoint) {
  switch (checkpoint) {
    case Checkpoint::kNone:
      return "none";
    case Checkpoint::kBeforeWrite:
      return "before_write";
    case Checkpoint::kMidWrite:
      return "mid_write";
    case Checkpoint::kAfterWriteBeforeSync:
      return "after_write_before_sync";
    case Checkpoint::kAfterSyncBeforeAck:
      return "after_sync_before_ack";
    case Checkpoint::kAfterAck:
      return "after_ack";
  }
  return "unknown";
}

Result<Checkpoint> ParseCheckpoint(std::string_view name) {
  if (name == "none") return Checkpoint::kNone;
  if (name == "before_write") return Checkpoint::kBeforeWrite;
  if (name == "mid_write") return Checkpoint::kMidWrite;
  if (name == "after_write_before_sync") return Checkpoint::kAfterWriteBeforeSync;
  if (name == "after_sync_before_ack") return Checkpoint::kAfterSyncBeforeAck;
  if (name == "after_ack") return Checkpoint::kAfterAck;
  return Error(ErrorKind::kInvalidArgument,
               "unknown checkpoint \"" + std::string(name) +
                   "\"; expected none, before_write, mid_write, after_write_before_sync, "
                   "after_sync_before_ack, or after_ack");
}

std::uint64_t AcknowledgmentUnits(const CrashScenario& scenario) {
  const std::uint64_t group = scenario.policy == SyncPolicy::kSyncBatch && scenario.batch_size > 0
                                  ? scenario.batch_size
                                  : 1;
  return (scenario.records + group - 1) / group;
}

std::string EncodeScenario(const CrashScenario& scenario) {
  return "records=" + std::to_string(scenario.records) +
         ",payload=" + std::to_string(scenario.payload_bytes) +
         ",policy=" + std::string(SyncPolicyName(scenario.policy)) +
         ",batch=" + std::to_string(scenario.batch_size) +
         ",at=" + std::string(CheckpointName(scenario.checkpoint)) +
         ",unit=" + std::to_string(scenario.kill_unit) +
         ",short=" + std::to_string(scenario.short_write_bytes);
}

Result<CrashScenario> ParseScenario(std::string_view spec) {
  std::map<std::string, std::string> fields;
  std::size_t position = 0;
  while (position < spec.size()) {
    std::size_t comma = spec.find(',', position);
    if (comma == std::string_view::npos) comma = spec.size();
    const std::string_view token = spec.substr(position, comma - position);
    position = comma + 1;
    const std::size_t equals = token.find('=');
    if (equals == std::string_view::npos) {
      return Error(ErrorKind::kInvalidArgument,
                   "scenario field \"" + std::string(token) + "\" is not key=value");
    }
    fields[std::string(token.substr(0, equals))] = std::string(token.substr(equals + 1));
  }

  const auto number = [&fields](const char* key, std::uint64_t max) -> Result<std::uint64_t> {
    const auto found = fields.find(key);
    if (found == fields.end()) {
      return Error(ErrorKind::kInvalidArgument, std::string("scenario is missing ") + key);
    }
    const std::string& raw = found->second;
    if (raw.empty() || raw.find_first_not_of("0123456789") != std::string::npos) {
      return Error(ErrorKind::kInvalidArgument,
                   std::string(key) + "=\"" + raw + "\" is not a non-negative integer");
    }
    std::uint64_t value = 0;
    for (const char digit : raw) {
      const std::uint64_t next = value * 10 + static_cast<std::uint64_t>(digit - '0');
      if (next < value || next > max) {
        return Error(ErrorKind::kLimitExceeded,
                     std::string(key) + "=" + raw + " exceeds the limit of " + std::to_string(max));
      }
      value = next;
    }
    return value;
  };

  CrashScenario scenario;

  Result<std::uint64_t> records = number("records", kMaxScenarioRecords);
  if (!records.ok()) return records.error();
  if (records.value() == 0) {
    return Error(ErrorKind::kInvalidArgument, "a scenario must write at least one record");
  }
  scenario.records = records.value();

  Result<std::uint64_t> payload = number("payload", kMaxPayloadBytes);
  if (!payload.ok()) return payload.error();
  scenario.payload_bytes = static_cast<std::size_t>(payload.value());

  const auto policy_field = fields.find("policy");
  if (policy_field == fields.end()) {
    return Error(ErrorKind::kInvalidArgument, "scenario is missing policy");
  }
  Result<SyncPolicy> policy = ParseSyncPolicy(policy_field->second);
  if (!policy.ok()) return policy.error();
  if (policy.value() == SyncPolicy::kWriteOnly) {
    // docs/PRD.md sections 2 and 4: write_only produces no durable acknowledgment, so it
    // has nothing to compare against and MUST NEVER appear in the acknowledged-prefix
    // claim. Excluding it here keeps that true by construction rather than by convention.
    return Error(ErrorKind::kUnsupportedPolicy,
                 "write_only has no durable acknowledgments, so it is excluded from the "
                 "acknowledged-prefix claim this harness checks");
  }
  scenario.policy = policy.value();

  Result<std::uint64_t> batch = number("batch", kMaxBatchRecords);
  if (!batch.ok()) return batch.error();
  scenario.batch_size = static_cast<std::size_t>(batch.value());
  if (scenario.policy == SyncPolicy::kSyncEach && scenario.batch_size != 1) {
    return Error(ErrorKind::kInvalidArgument, "sync_each requires batch=1");
  }
  if (scenario.policy == SyncPolicy::kSyncBatch && scenario.batch_size < 2) {
    return Error(ErrorKind::kInvalidArgument, "sync_batch requires batch of at least 2");
  }

  const auto checkpoint_field = fields.find("at");
  if (checkpoint_field == fields.end()) {
    return Error(ErrorKind::kInvalidArgument, "scenario is missing at");
  }
  Result<Checkpoint> checkpoint = ParseCheckpoint(checkpoint_field->second);
  if (!checkpoint.ok()) return checkpoint.error();
  scenario.checkpoint = checkpoint.value();

  // Checked before anything allocates a payload buffer or writes a byte.
  const std::uint64_t total_bytes =
      scenario.records * (static_cast<std::uint64_t>(kHeaderSize) + scenario.payload_bytes);
  if (total_bytes > kMaxScenarioBytes) {
    return Error(ErrorKind::kLimitExceeded, "scenario would write " + std::to_string(total_bytes) +
                                                " bytes; the limit is " +
                                                std::to_string(kMaxScenarioBytes));
  }

  const std::uint64_t units = AcknowledgmentUnits(scenario);
  Result<std::uint64_t> unit = number("unit", units);
  if (!unit.ok()) return unit.error();
  if (unit.value() == 0) {
    return Error(ErrorKind::kInvalidArgument, "acknowledgment units are numbered from 1");
  }
  scenario.kill_unit = unit.value();

  Result<std::uint64_t> short_write = number("short", kMaxScenarioBytes);
  if (!short_write.ok()) return short_write.error();
  if (short_write.value() == 0) {
    return Error(ErrorKind::kInvalidArgument,
                 "short=0 would leave nothing on disk; use at=before_write for that");
  }
  scenario.short_write_bytes = static_cast<std::size_t>(short_write.value());

  return scenario;
}

Result<std::filesystem::path> SelfExecutablePath(std::string_view argv0) {
#if defined(__linux__)
  std::error_code link_ec;
  const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", link_ec);
  if (!link_ec && !self.empty()) return self;
#endif
  const std::filesystem::path candidate(argv0);
  if (candidate.has_parent_path() && !candidate.parent_path().empty()) {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(candidate, ec);
    if (!ec) return absolute;
  }
  return Error(ErrorKind::kInvalidArgument,
               "could not determine this executable's path from \"" + std::string(argv0) +
                   "\"; invoke eventlog by path so crash-test can re-execute it");
}

// --- the oracle --------------------------------------------------------------

Result<RunOutcome> VerifyRecoveredLog(const std::filesystem::path& path,
                                      std::uint64_t acknowledged_through, std::size_t payload_bytes,
                                      bool child_was_killed) {
  RunOutcome outcome;
  outcome.acknowledged_through = acknowledged_through;
  outcome.killed = child_was_killed;

  Result<ScanReport> scanned = ScanLog(path);
  if (!scanned.ok()) {
    // ScanLog puts structural damage in its report and reserves Errors for I/O failures
    // and for a format version this build does not implement. The first is a harness
    // failure; the second cannot happen to a log this harness just wrote, so it counts
    // as a violation rather than as a shrug.
    if (scanned.error().kind() == ErrorKind::kIo) return scanned.error();
    outcome.violation = "scan failed: " + scanned.error().message();
    return outcome;
  }
  outcome.status = scanned.value().status;

  if (outcome.status == LogStatus::kIncompleteTail) {
    if (!child_was_killed) {
      outcome.violation =
          "a writer that returned from every call and exited normally left an incomplete "
          "trailing record: " +
          scanned.value().detail + " (offset " + std::to_string(scanned.value().problem_offset) +
          ")";
      return outcome;
    }
    // Harness-owned recovery, which docs/PRD.md section 3 permits alongside the explicit
    // `recover --truncate-tail`. It discards only a structurally incomplete trailing
    // record (ADR-002).
    Result<RecoveryPlan> planned = PlanRecovery(path);
    if (!planned.ok()) return planned.error();
    Result<RecoveryOutcome> recovered = ApplyRecovery(path, planned.value(), *DefaultFileOps());
    if (!recovered.ok()) return recovered.error();
    outcome.recovered_incomplete_tail = true;

    Result<ScanReport> rescanned = ScanLog(path);
    if (!rescanned.ok()) return rescanned.error();
    if (!rescanned.value().clean()) {
      outcome.violation = "log was still " + std::string(LogStatusName(rescanned.value().status)) +
                          " after recovering its incomplete tail";
      return outcome;
    }
  } else if (outcome.status != LogStatus::kClean) {
    // Hard corruption: a complete record with a bad CRC, a full but invalid header, or a
    // sequence break. Nothing in the supported fault model produces it, and it is never
    // truncated away (ADR-002). Reported with its offset and left on disk.
    outcome.violation =
        "hard corruption after a controlled stop: " + std::string(LogStatusName(outcome.status)) +
        " at offset " + std::to_string(scanned.value().problem_offset) + " (" +
        scanned.value().detail + ")";
    return outcome;
  }

  // Read every record back: contiguous from 1, and byte-identical to the deterministic
  // content written for that sequence. This is the "no fabricated records" half of the
  // invariant, and it does not take the log's word for its own sequence numbers.
  Result<LogReader> opened = LogReader::Open(path);
  if (!opened.ok()) return opened.error();
  LogReader reader = std::move(opened).value();
  std::uint64_t expected = kFirstSequence;
  for (;;) {
    Result<std::optional<Record>> next = reader.Next();
    if (!next.ok()) {
      outcome.violation =
          "reading back record " + std::to_string(expected) + " failed: " + next.error().message();
      return outcome;
    }
    if (!next.value().has_value()) break;
    const Record& record = next.value().value();
    if (record.sequence != expected) {
      outcome.violation = "recovered sequence " + std::to_string(record.sequence) + " where " +
                          std::to_string(expected) + " was expected";
      return outcome;
    }
    if (record.payload != MakeDeterministicPayload(record.sequence, payload_bytes)) {
      outcome.violation = "payload of recovered record " + std::to_string(record.sequence) +
                          " does not match the deterministic content written for it";
      return outcome;
    }
    ++expected;
    ++outcome.recovered_records;
    outcome.recovered_last_sequence = record.sequence;
  }

  // The invariant from docs/PRD.md section 4:
  //
  //     acknowledged_sequences == prefix(recovered_sequences, len(acknowledged))
  //
  // Both sides are contiguous runs from 1 -- the loop above just proved it for the
  // recovered side, and acknowledgments are issued in order -- so prefix containment
  // reduces to this one comparison. The check is one-directional on purpose: records
  // recovered but never acknowledged are permitted (CPP-AC11).
  if (acknowledged_through > outcome.recovered_last_sequence) {
    outcome.violation =
        "lost acknowledged records: the supervisor observed durability through sequence " +
        std::to_string(acknowledged_through) + " but the recovered log ends at " +
        std::to_string(outcome.recovered_last_sequence);
  }
  return outcome;
}

// --- the child ---------------------------------------------------------------

int RunCrashChild(const std::filesystem::path& log, const CrashScenario& scenario, int frame_fd) {
  auto ops = std::make_shared<CheckpointFileOps>(scenario.checkpoint, scenario.short_write_bytes,
                                                 frame_fd);

  WriterOptions options;
  options.policy = scenario.policy;
  options.batch_size = scenario.batch_size;
  options.file_ops = ops;
  // allow_existing stays false: every scenario gets a fresh file, so Open cannot take
  // the resume-sync path and every barrier call belongs to a record this child wrote.

  Result<EventLogWriter> opened = EventLogWriter::Open(log, options);
  if (!opened.ok()) return FailChild(frame_fd, opened.error().message());
  EventLogWriter writer = std::move(opened).value();

  const std::uint64_t group = scenario.policy == SyncPolicy::kSyncBatch
                                  ? static_cast<std::uint64_t>(scenario.batch_size)
                                  : 1;
  std::vector<std::vector<std::byte>> payloads(static_cast<std::size_t>(group),
                                               std::vector<std::byte>(scenario.payload_bytes));
  std::vector<std::span<const std::byte>> spans;
  spans.reserve(static_cast<std::size_t>(group));

  std::uint64_t written = 0;
  std::uint64_t unit = 0;
  while (written < scenario.records) {
    ++unit;
    const std::uint64_t count = std::min<std::uint64_t>(group, scenario.records - written);
    const std::uint64_t first = written + 1;

    spans.clear();
    for (std::uint64_t i = 0; i < count; ++i) {
      const std::size_t slot = static_cast<std::size_t>(i);
      FillDeterministicPayload(first + i, payloads[slot]);
      spans.emplace_back(payloads[slot]);
    }

    if (scenario.checkpoint != Checkpoint::kNone && unit == scenario.kill_unit) ops->Arm();

    if (scenario.policy == SyncPolicy::kSyncBatch) {
      Result<BatchAppendResult> result = writer.AppendBatchAndSync(spans);
      if (!result.ok()) return FailChild(frame_fd, result.error().message());
    } else {
      Result<AppendResult> result = writer.AppendAndSync(spans[0]);
      if (!result.ok()) return FailChild(frame_fd, result.error().message());
    }

    // Reached only once the durability barrier has returned success for this unit, which
    // is the only thing that makes any of it durable (ADR-001, ADR-006).
    Status sent = SendFrame(
        frame_fd, "ack " + std::to_string(first) + " " + std::to_string(first + count - 1));
    if (!sent.ok()) return kChildHarnessFailure;

    if (unit == scenario.kill_unit && scenario.checkpoint == Checkpoint::kAfterAck) {
      Park(frame_fd, scenario.checkpoint);
    }
    written += count;
  }

  const std::uint64_t durable = writer.durable_through_sequence();
  Status closed = writer.Close();
  if (!closed.ok()) return FailChild(frame_fd, closed.error().message());
  Status done = SendFrame(frame_fd, "done " + std::to_string(durable));
  if (!done.ok()) return kChildHarnessFailure;
  return 0;
}

// --- the supervisor ----------------------------------------------------------

Result<CrashTestSummary> RunCrashTest(const CrashTestOptions& options) {
  if (options.workdir.empty()) {
    return Error(ErrorKind::kInvalidArgument, "crash-test needs a --workdir scratch root");
  }
  if (options.child_program.empty()) {
    return Error(ErrorKind::kInvalidArgument,
                 "crash-test needs the path of this executable to re-execute");
  }

  // One fresh subdirectory per invocation. Every fixture lives inside it, and only it is
  // removed afterwards: the scratch root itself belongs to the caller.
  ScratchDirectory scratch;
  Status created = scratch.Create(options.workdir, "run", "seed" + std::to_string(options.seed));
  if (!created.ok()) return created.error();
  std::error_code ec;

  CrashTestSummary summary;
  std::uint64_t rng = options.seed;
  for (std::uint64_t index = 0; index < options.runs; ++index) {
    const CrashScenario scenario = MakeScenario(rng, index, options.forced_checkpoint);
    const std::filesystem::path log = scratch.file("scenario-" + std::to_string(index) + ".log");

    Result<RunOutcome> run = RunOneScenario(scenario, log, options.child_program);
    if (!run.ok()) {
      scratch.Keep();
      return run.error();
    }
    const RunOutcome& outcome = run.value();

    ++summary.scenarios;
    ++summary.by_checkpoint[static_cast<std::size_t>(scenario.checkpoint)];
    if (outcome.killed) ++summary.killed;
    if (outcome.recovered_incomplete_tail) ++summary.incomplete_tails_recovered;
    summary.acknowledged_records += outcome.acknowledged_through;
    summary.recovered_records += outcome.recovered_records;
    if (outcome.recovered_last_sequence > outcome.acknowledged_through) {
      summary.extra_unacknowledged +=
          outcome.recovered_last_sequence - outcome.acknowledged_through;
    }

    if (outcome.upheld()) {
      // Checked, so the fixture goes. A failing one stays for inspection.
      std::filesystem::remove(log, ec);
    } else {
      ++summary.violations;
      summary.violation_details.push_back("run " + std::to_string(index) + " [" +
                                          EncodeScenario(scenario) + "]: " + outcome.violation);
      scratch.Keep();
    }
  }

  return summary;
}

}  // namespace eventlog
