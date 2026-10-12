#include "eventlog/cli.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "eventlog/error.h"
#include "eventlog/file_ops.h"
#include "eventlog/payload.h"
#include "eventlog/reader.h"
#include "eventlog/recover.h"
#include "eventlog/writer.h"

namespace eventlog {
namespace {

constexpr std::uint64_t kMaxRecordsPerInvocation = 10'000'000;

void PrintUsage(std::ostream& out) {
  out << R"(eventlog - crash-safe append-only event log

Usage:
  eventlog append  --file PATH --records N --payload-bytes B
                   [--policy sync_each] [--batch-size N]
                   [--append-existing | --overwrite]
  eventlog verify  --file PATH [--json PATH]
  eventlog recover --file PATH [--truncate-tail]
  eventlog --help | --version

Commands:
  append    Append N deterministic synthetic records. Reports how many were
            acknowledged DURABLE, which happens only after a successful sync.
  verify    Read-only. Validates framing, CRC32C, and the sequence chain.
            Exit 0 clean, 1 incomplete trailing record, 2 hard corruption.
  recover   Reports a structurally incomplete trailing record. Truncates it only
            when --truncate-tail is given, and never touches hard corruption.

Durability: a record counts as durable only after this platform's barrier
()" << SyncPrimitiveName()
      << R"() returns success. A process kill is not a power failure.

Planned in later milestones: crash-test (M3), bench (M4), sync_batch (M2).
)";
}

// --- argument parsing --------------------------------------------------------

class Arguments {
 public:
  // Accepts only "--name value" for options and "--name" for flags. Anything else is an
  // error rather than a guess.
  Status Parse(int argc, char** argv, int first, const std::set<std::string>& options,
               const std::set<std::string>& flags) {
    for (int i = first; i < argc; ++i) {
      const std::string token(argv[i]);
      if (token.rfind("--", 0) != 0) {
        return Error(ErrorKind::kInvalidArgument,
                     "unexpected positional argument \"" + token + "\"");
      }
      const std::string name = token.substr(2);
      if (flags.count(name) > 0) {
        flags_.insert(name);
        continue;
      }
      if (options.count(name) == 0) {
        return Error(ErrorKind::kInvalidArgument, "unknown option \"" + token + "\"");
      }
      if (i + 1 >= argc) {
        return Error(ErrorKind::kInvalidArgument, token + " requires a value");
      }
      values_[name] = argv[++i];
    }
    return Status::Ok();
  }

  bool has_flag(std::string_view name) const { return flags_.count(std::string(name)) > 0; }
  bool has_value(std::string_view name) const { return values_.count(std::string(name)) > 0; }

  std::string value(std::string_view name) const {
    const auto it = values_.find(std::string(name));
    return it == values_.end() ? std::string() : it->second;
  }

  Result<std::uint64_t> required_u64(std::string_view name, std::uint64_t max) const {
    if (!has_value(name)) {
      return Error(ErrorKind::kInvalidArgument, "--" + std::string(name) + " is required");
    }
    const std::string raw = value(name);
    if (raw.empty() || raw.find_first_not_of("0123456789") != std::string::npos) {
      return Error(
          ErrorKind::kInvalidArgument,
          "--" + std::string(name) + " must be a non-negative integer, got \"" + raw + "\"");
    }
    std::uint64_t parsed = 0;
    for (const char digit : raw) {
      const std::uint64_t next = parsed * 10 + static_cast<std::uint64_t>(digit - '0');
      if (next < parsed || next > max) {
        return Error(ErrorKind::kLimitExceeded,
                     "--" + std::string(name) + " exceeds the limit of " + std::to_string(max));
      }
      parsed = next;
    }
    return parsed;
  }

  Result<std::filesystem::path> required_file() const {
    if (!has_value("file")) return Error(ErrorKind::kInvalidArgument, "--file is required");
    return std::filesystem::path(value("file"));
  }

 private:
  std::map<std::string, std::string> values_;
  std::set<std::string> flags_;
};

int Fail(const Error& error, int code) {
  std::cerr << "error: " << error.message() << "\n";
  return code;
}

int FailUsage(const Error& error) {
  std::cerr << "error: " << error.message() << "\n\n";
  PrintUsage(std::cerr);
  return kExitUsage;
}

std::string JsonEscape(std::string_view in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (const char c : in) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out += "\\u00";
          const char* digits = "0123456789abcdef";
          out += digits[(static_cast<unsigned char>(c) >> 4) & 0xF];
          out += digits[static_cast<unsigned char>(c) & 0xF];
        } else {
          out += c;
        }
    }
  }
  return out;
}

// --- append ------------------------------------------------------------------

int CommandAppend(int argc, char** argv) {
  Arguments args;
  Status parsed =
      args.Parse(argc, argv, 2, {"file", "records", "payload-bytes", "policy", "batch-size"},
                 {"append-existing", "overwrite"});
  if (!parsed.ok()) return FailUsage(parsed.error());

  Result<std::filesystem::path> file = args.required_file();
  if (!file.ok()) return FailUsage(file.error());
  Result<std::uint64_t> records = args.required_u64("records", kMaxRecordsPerInvocation);
  if (!records.ok()) return FailUsage(records.error());
  Result<std::uint64_t> payload_bytes = args.required_u64("payload-bytes", kMaxPayloadBytes);
  if (!payload_bytes.ok()) return FailUsage(payload_bytes.error());

  WriterOptions options;
  if (args.has_value("policy")) {
    Result<SyncPolicy> policy = ParseSyncPolicy(args.value("policy"));
    if (!policy.ok()) return FailUsage(policy.error());
    options.policy = policy.value();
  }
  if (args.has_value("batch-size")) {
    Result<std::uint64_t> batch = args.required_u64("batch-size", 1u << 20);
    if (!batch.ok()) return FailUsage(batch.error());
    options.batch_size = static_cast<std::size_t>(batch.value());
  }

  if (args.has_flag("append-existing") && args.has_flag("overwrite")) {
    return FailUsage(Error(ErrorKind::kInvalidArgument,
                           "--append-existing and --overwrite are mutually exclusive"));
  }
  options.allow_existing = args.has_flag("append-existing");

  if (args.has_flag("overwrite")) {
    // Destructive, so it refuses to act through a symlink (docs/PRD.md section 6).
    std::error_code ec;
    if (std::filesystem::is_symlink(file.value(), ec)) {
      return Fail(Error(ErrorKind::kInvalidArgument,
                        file.value().string() + " is a symlink; refusing to overwrite through it"),
                  kExitRuntimeError);
    }
    std::filesystem::remove(file.value(), ec);
    if (ec) {
      return Fail(Error(ErrorKind::kIo, "could not remove " + file.value().string(), ec.value()),
                  kExitRuntimeError);
    }
  }

  Result<EventLogWriter> opened = EventLogWriter::Open(file.value(), options);
  if (!opened.ok()) return Fail(opened.error(), kExitRuntimeError);
  EventLogWriter writer = std::move(opened).value();

  const std::uint64_t first_sequence = writer.accepted_through_sequence() + 1;
  const std::size_t size = static_cast<std::size_t>(payload_bytes.value());
  std::vector<std::byte> payload(size);
  std::uint64_t appended = 0;

  for (std::uint64_t i = 0; i < records.value(); ++i) {
    FillDeterministicPayload(first_sequence + i, payload);
    Result<AppendResult> result = writer.AppendAndSync(payload);
    if (!result.ok()) {
      std::cerr << "error: " << result.error().message() << "\n";
      std::cerr << "Records appended: " << appended
                << "  Durable acknowledged: " << writer.durable_through_sequence() << "\n";
      // Close is still attempted so the lock is released and errors are not hidden.
      Status closed = writer.Close();
      if (!closed.ok()) std::cerr << "error: " << closed.error().message() << "\n";
      return kExitRuntimeError;
    }
    ++appended;
  }

  const std::uint64_t durable = writer.durable_through_sequence();
  const std::uint64_t bytes = writer.file_offset();
  Status closed = writer.Close();
  if (!closed.ok()) return Fail(closed.error(), kExitRuntimeError);

  std::cout << "Records appended: " << appended << "  Durable acknowledged: " << durable << "\n";
  std::cout << "Policy: " << SyncPolicyName(options.policy)
            << "  Durability barrier: " << SyncPrimitiveName() << "  File size: " << bytes
            << " bytes\n";
  return kExitOk;
}

// --- verify ------------------------------------------------------------------

Status WriteVerifyJson(const std::filesystem::path& path, const std::filesystem::path& log,
                       const ScanReport& report) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    return Error(ErrorKind::kIo, "could not open " + path.string() + " for writing");
  }
  out << "{\n"
      << "  \"file\": \"" << JsonEscape(log.string()) << "\",\n"
      << "  \"status\": \"" << LogStatusName(report.status) << "\",\n"
      << "  \"valid_records\": " << report.valid_records << ",\n"
      << "  \"last_sequence\": " << report.last_sequence << ",\n"
      << "  \"valid_prefix_bytes\": " << report.valid_prefix_bytes << ",\n"
      << "  \"file_size\": " << report.file_size << ",\n"
      << "  \"trailing_bytes\": " << report.trailing_bytes() << ",\n"
      << "  \"problem_offset\": " << report.problem_offset << ",\n"
      << "  \"detail\": \"" << JsonEscape(report.detail) << "\",\n"
      << "  \"format_version\": " << kFormatVersion << ",\n"
      << "  \"durability_barrier\": \"" << JsonEscape(SyncPrimitiveName()) << "\"\n"
      << "}\n";
  out.flush();
  if (!out) return Error(ErrorKind::kIo, "failed while writing " + path.string());
  return Status::Ok();
}

int CommandVerify(int argc, char** argv) {
  Arguments args;
  Status parsed = args.Parse(argc, argv, 2, {"file", "json"}, {});
  if (!parsed.ok()) return FailUsage(parsed.error());

  Result<std::filesystem::path> file = args.required_file();
  if (!file.ok()) return FailUsage(file.error());

  // Strictly read-only: the log is never opened for writing and never locked.
  Result<ScanReport> scanned = ScanLog(file.value());
  if (!scanned.ok()) {
    const int code = scanned.error().kind() == ErrorKind::kUnsupportedVersion ? kExitHardCorruption
                                                                              : kExitRuntimeError;
    if (scanned.error().kind() == ErrorKind::kUnsupportedVersion) {
      std::cerr << "error: " << scanned.error().message() << "\nFile left unchanged.\n";
      return code;
    }
    return Fail(scanned.error(), code);
  }
  const ScanReport& report = scanned.value();

  std::cout << "Valid records: " << report.valid_records
            << "   Last sequence: " << report.last_sequence
            << "   Status: " << LogStatusName(report.status) << "\n";
  if (!report.clean()) {
    std::cout << "Detail: " << report.detail << " (offset " << report.problem_offset << ")\n";
    if (report.status == LogStatus::kIncompleteTail) {
      std::cout << "Recoverable: " << report.trailing_bytes()
                << " trailing bytes form a partial record.\n"
                << "Run: eventlog recover --file " << file.value().string() << " --truncate-tail\n";
    } else {
      std::cout << "Not recoverable automatically: a complete record is damaged. "
                << "The file was not modified.\n";
    }
  }

  if (args.has_value("json")) {
    Status written = WriteVerifyJson(args.value("json"), file.value(), report);
    if (!written.ok()) return Fail(written.error(), kExitRuntimeError);
  }

  return LogStatusExitCode(report.status);
}

// --- recover -----------------------------------------------------------------

int CommandRecover(int argc, char** argv) {
  Arguments args;
  Status parsed = args.Parse(argc, argv, 2, {"file"}, {"truncate-tail"});
  if (!parsed.ok()) return FailUsage(parsed.error());

  Result<std::filesystem::path> file = args.required_file();
  if (!file.ok()) return FailUsage(file.error());

  Result<RecoveryPlan> planned = PlanRecovery(file.value());
  if (!planned.ok()) {
    const bool unsupported = planned.error().kind() == ErrorKind::kUnsupportedVersion;
    return Fail(planned.error(), unsupported ? kExitHardCorruption : kExitRuntimeError);
  }
  const RecoveryPlan& plan = planned.value();

  std::cout << "Status: " << LogStatusName(plan.scan.status)
            << "   Valid records: " << plan.scan.valid_records
            << "   Last sequence: " << plan.scan.last_sequence << "\n";

  if (plan.scan.status == LogStatus::kClean) {
    std::cout << "Nothing to recover. No change made.\n";
    return kExitOk;
  }

  if (!plan.truncation_required) {
    // Hard corruption. Report where it is and change nothing (ADR-002).
    std::cout << "Detail: " << plan.scan.detail << " (offset " << plan.scan.problem_offset << ")\n"
              << "A complete record is damaged, so automatic recovery would discard data "
                 "silently.\nThe file was not modified. Valid prefix ends at byte "
              << plan.scan.valid_prefix_bytes << ".\n";
    return kExitHardCorruption;
  }

  // Preflight finding, printed before any change (docs/PRD.md section 3).
  std::cout << "Detail: " << plan.scan.detail << " (offset " << plan.scan.problem_offset << ")\n"
            << "Would discard " << plan.bytes_to_discard << " bytes: offsets " << plan.truncate_to
            << ".." << plan.scan.file_size << " (one structurally incomplete trailing record).\n";

  if (!args.has_flag("truncate-tail")) {
    std::cout << "No change made. Re-run with --truncate-tail to apply.\n";
    return kExitIncompleteTail;
  }

  Result<RecoveryOutcome> outcome = ApplyRecovery(file.value(), plan, *DefaultFileOps());
  if (!outcome.ok()) return Fail(outcome.error(), kExitRuntimeError);

  std::cout << "Status: recovered_incomplete_tail   Discarded: " << outcome.value().bytes_discarded
            << " bytes   Valid records: " << outcome.value().valid_records
            << "   Last sequence: " << outcome.value().last_sequence << "\n";
  return kExitOk;
}

int NotYetImplemented(std::string_view command, std::string_view milestone) {
  std::cerr << "error: " << command << " is planned for milestone " << milestone
            << " and is not implemented yet.\n"
            << "See docs/PLAN.md for the milestone plan.\n";
  return kExitUsage;
}

}  // namespace

int RunCli(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage(std::cerr);
    return kExitUsage;
  }

  const std::string_view command(argv[1]);
  if (command == "--help" || command == "-h" || command == "help") {
    PrintUsage(std::cout);
    return kExitOk;
  }
  if (command == "--version") {
    std::cout << "eventlog 0.1.0 (record format v" << kFormatVersion << ", durability barrier "
              << SyncPrimitiveName() << ")\n";
    return kExitOk;
  }
  if (command == "append") return CommandAppend(argc, argv);
  if (command == "verify") return CommandVerify(argc, argv);
  if (command == "recover") return CommandRecover(argc, argv);
  if (command == "crash-test") return NotYetImplemented("crash-test", "M3");
  if (command == "bench") return NotYetImplemented("bench", "M4");

  std::cerr << "error: unknown command \"" << command << "\"\n\n";
  PrintUsage(std::cerr);
  return kExitUsage;
}

}  // namespace eventlog
