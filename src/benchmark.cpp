#include "eventlog/benchmark.h"

#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/statfs.h>
#elif defined(__APPLE__)
#include <sys/mount.h>
#include <sys/param.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

#include "eventlog/json.h"
#include "eventlog/payload.h"
#include "eventlog/record.h"
#include "eventlog/scratch_dir.h"

namespace eventlog {
namespace {

using Clock = std::chrono::steady_clock;

double SecondsBetween(Clock::time_point from, Clock::time_point to) {
  return std::chrono::duration<double>(to - from).count();
}

double MicrosecondsBetween(Clock::time_point from, Clock::time_point to) {
  return std::chrono::duration<double, std::micro>(to - from).count();
}

std::string Fixed(double value, int places) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(places) << value;
  return out.str();
}

// --- environment -------------------------------------------------------------

// `utsname` and `rusage` each name both a struct and a function in scope, so these
// declarations need an elaborated specifier. Confining it to an alias keeps the
// declaration free of one: clang-format 14 and 17 disagree about `struct ::x y{}`, and
// the supported platform's formatter is 14. Same reason as PosixFileOps::FileSize.
using UtsName = struct ::utsname;

std::string DescribeOs() {
  UtsName info{};
  if (::uname(&info) != 0) return "unknown";
  return std::string(info.sysname) + " " + info.release + " " + info.machine;
}

std::string MachineArchitecture() {
  UtsName info{};
  if (::uname(&info) != 0) return "unknown";
  return info.machine;
}

// On x86 /proc/cpuinfo carries a "model name". On aarch64 it does not -- only numeric
// implementer and part identifiers -- so this reports those rather than inventing a model
// string, and says plainly when it found neither.
std::string DescribeCpu() {
#if defined(__linux__)
  std::ifstream cpuinfo("/proc/cpuinfo");
  if (cpuinfo) {
    std::string line;
    std::string implementer;
    std::string part;
    while (std::getline(cpuinfo, line)) {
      const std::size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      std::string key = line.substr(0, colon);
      std::string value = line.substr(colon + 1);
      while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
      if (key == "model name" && !value.empty()) return value;
      if (key == "CPU implementer" && implementer.empty()) implementer = value;
      if (key == "CPU part" && part.empty()) part = value;
    }
    if (!implementer.empty()) {
      return MachineArchitecture() + " (CPU implementer " + implementer + ", part " + part + ")";
    }
  }
  return MachineArchitecture() + " (no model name in /proc/cpuinfo)";
#else
  // macOS is an unsupported convenience path (ADR-004) and its results are never
  // publishable, so no platform-specific probing is carried for it.
  return MachineArchitecture() + " (CPU model not probed on this platform)";
#endif
}

std::string DescribeFilesystem(const std::filesystem::path& where) {
#if defined(__linux__)
  // `statfs` names both a struct and a syscall; the alias keeps the declaration free of
  // an elaborated specifier, which clang-format 14 and 17 format differently.
  using StatfsBuf = struct ::statfs;
  StatfsBuf info{};
  if (::statfs(where.c_str(), &info) != 0) return "unknown";
  struct Known {
    unsigned long magic;
    const char* name;
  };
  static constexpr Known kKnown[] = {
      {0xEF53, "ext2/ext3/ext4"}, {0x58465342, "xfs"}, {0x9123683E, "btrfs"}, {0x01021994, "tmpfs"},
      {0x794C7630, "overlay"},    {0x2FC12FC1, "zfs"}, {0x6969, "nfs"},       {0x4D44, "vfat"},
      {0x65735546, "fuse"},
  };
  for (const Known& candidate : kKnown) {
    if (static_cast<unsigned long>(info.f_type) == candidate.magic) return candidate.name;
  }
  std::ostringstream out;
  out << "unknown (f_type 0x" << std::hex << static_cast<unsigned long>(info.f_type) << ")";
  return out.str();
#elif defined(__APPLE__)
  using StatfsBuf = struct ::statfs;
  StatfsBuf info{};
  if (::statfs(where.c_str(), &info) != 0) return "unknown";
  return info.f_fstypename;
#else
  static_cast<void>(where);
  return "unknown";
#endif
}

std::uint64_t PhysicalMemoryBytes() {
  const long pages = ::sysconf(_SC_PHYS_PAGES);
  const long page_size = ::sysconf(_SC_PAGESIZE);
  if (pages <= 0 || page_size <= 0) return 0;
  return static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page_size);
}

std::uint64_t PeakResidentBytes() {
  using Rusage = struct ::rusage;
  Rusage usage{};
  if (::getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#if defined(__APPLE__)
  // Darwin reports bytes; Linux reports kilobytes.
  return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
  return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024u;
#endif
}

std::string CompilerDescription() {
#if defined(__VERSION__)
  return __VERSION__;
#else
  return "unknown";
#endif
}

// --- one trial ---------------------------------------------------------------

Result<TrialResult> RunTrial(const PolicySpec& spec, const BenchmarkOptions& options,
                             const std::filesystem::path& log) {
  WriterOptions writer_options;
  writer_options.policy = spec.policy;
  writer_options.batch_size = spec.batch_size;

  Result<EventLogWriter> opened = EventLogWriter::Open(log, writer_options);
  if (!opened.ok()) return opened.error();
  EventLogWriter writer = std::move(opened).value();

  const std::uint64_t group = spec.group();
  std::vector<std::vector<std::byte>> payloads(static_cast<std::size_t>(group),
                                               std::vector<std::byte>(options.payload_bytes));
  std::vector<std::span<const std::byte>> spans;
  spans.reserve(static_cast<std::size_t>(group));

  std::vector<double> latencies;
  latencies.reserve(static_cast<std::size_t>((options.records + group - 1) / group));

  double append_seconds = 0;
  std::uint64_t written = 0;
  const Clock::time_point loop_start = Clock::now();

  while (written < options.records) {
    const std::uint64_t count = std::min<std::uint64_t>(group, options.records - written);
    const std::uint64_t first = written + 1;

    // Generated outside the timed window: throughput should measure the log, not the
    // payload generator. The difference between append_seconds and wall_seconds is what
    // this costs, and both are reported.
    spans.clear();
    for (std::uint64_t i = 0; i < count; ++i) {
      const std::size_t slot = static_cast<std::size_t>(i);
      FillDeterministicPayload(first + i, payloads[slot]);
      spans.emplace_back(payloads[slot]);
    }

    const Clock::time_point before = Clock::now();
    switch (spec.policy) {
      case SyncPolicy::kSyncEach: {
        Result<AppendResult> result = writer.AppendAndSync(spans[0]);
        if (!result.ok()) return result.error();
        break;
      }
      case SyncPolicy::kSyncBatch: {
        Result<BatchAppendResult> result = writer.AppendBatchAndSync(spans);
        if (!result.ok()) return result.error();
        break;
      }
      case SyncPolicy::kWriteOnly: {
        Result<AcceptResult> result = writer.AppendWithoutSync(spans[0]);
        if (!result.ok()) return result.error();
        break;
      }
    }
    const Clock::time_point after = Clock::now();

    const double elapsed_us = MicrosecondsBetween(before, after);
    latencies.push_back(elapsed_us);
    append_seconds += elapsed_us / 1e6;
    written += count;
  }

  const Clock::time_point loop_end = Clock::now();

  TrialResult trial;
  trial.records = written;
  trial.acknowledgment_units = latencies.size();
  trial.bytes_written = writer.file_offset();
  trial.write_calls = writer.stats().write_calls;
  trial.sync_calls = writer.stats().sync_calls;
  trial.append_seconds = append_seconds;
  trial.wall_seconds = SecondsBetween(loop_start, loop_end);
  trial.records_per_second =
      append_seconds > 0 ? static_cast<double>(written) / append_seconds : 0.0;

  std::sort(latencies.begin(), latencies.end());
  trial.ack_p50_us = PercentileOfSorted(latencies, 0.50);
  trial.ack_p95_us = PercentileOfSorted(latencies, 0.95);
  trial.ack_p99_us = PercentileOfSorted(latencies, 0.99);
  trial.ack_max_us = latencies.empty() ? 0.0 : latencies.back();

  // Under a durable policy every acknowledgment cost exactly one barrier call. Checked
  // rather than assumed, because a policy that quietly synced less would look faster.
  if (spec.durable() && trial.sync_calls != trial.acknowledgment_units) {
    return Error(ErrorKind::kInternal, spec.label() + " made " + std::to_string(trial.sync_calls) +
                                           " barrier calls for " +
                                           std::to_string(trial.acknowledgment_units) +
                                           " acknowledgments");
  }
  if (!spec.durable() && trial.sync_calls != 0) {
    return Error(ErrorKind::kInternal, spec.label() + " called the durability barrier");
  }

  Status closed = writer.Close();
  if (!closed.ok()) return closed.error();
  return trial;
}

}  // namespace

// --- statistics --------------------------------------------------------------

double PercentileOfSorted(const std::vector<double>& sorted, double fraction) {
  if (sorted.empty()) return 0.0;
  if (fraction <= 0.0) return sorted.front();
  if (fraction >= 1.0) return sorted.back();
  // Nearest rank, so the figure reported is a latency that actually occurred.
  const double rank = fraction * static_cast<double>(sorted.size());
  std::size_t index = static_cast<std::size_t>(rank);
  if (rank > static_cast<double>(index)) ++index;
  if (index == 0) index = 1;
  if (index > sorted.size()) index = sorted.size();
  return sorted[index - 1];
}

Spread Summarize(std::vector<double> values) {
  Spread spread;
  if (values.empty()) return spread;
  std::sort(values.begin(), values.end());
  spread.min = values.front();
  spread.max = values.back();
  const std::size_t middle = values.size() / 2;
  spread.median =
      values.size() % 2 == 1 ? values[middle] : (values[middle - 1] + values[middle]) / 2.0;
  return spread;
}

// --- environment -------------------------------------------------------------

bool IsPublishable(std::string_view build_type, bool sanitizers, bool supported_platform) {
  return build_type == "Release" && !sanitizers && supported_platform;
}

Environment CaptureEnvironment(const std::filesystem::path& scratch) {
  Environment environment;
  environment.os = DescribeOs();
  environment.cpu = DescribeCpu();
  const long online = ::sysconf(_SC_NPROCESSORS_ONLN);
  environment.cpu_count = online > 0 ? static_cast<std::uint64_t>(online) : 0;
  environment.ram_bytes = PhysicalMemoryBytes();
  environment.filesystem = DescribeFilesystem(scratch);
  environment.storage_class = "unknown";
  environment.compiler = CompilerDescription();
#if defined(EVENTLOG_BUILD_TYPE)
  environment.build_type = EVENTLOG_BUILD_TYPE;
#else
  environment.build_type = "unknown";
#endif
#if defined(EVENTLOG_SANITIZERS_ON)
  environment.sanitizers = true;
#else
  environment.sanitizers = false;
#endif
  environment.durability_barrier = SyncPrimitiveName();
  double load[3] = {0, 0, 0};
  if (::getloadavg(load, 3) >= 1) environment.load_average_1m = load[0];

#if defined(__linux__)
  const bool supported_platform = true;
#else
  const bool supported_platform = false;
#endif
  const bool release = environment.build_type == "Release";
  environment.publishable =
      IsPublishable(environment.build_type, environment.sanitizers, supported_platform);
  if (environment.publishable) {
    environment.publishable_note =
        "Release build, sanitizers off, on the supported platform; see benchmarks/README.md";
  } else {
    environment.publishable_note = "NOT publishable:";
    if (!release) environment.publishable_note += " build type is " + environment.build_type + ";";
    if (environment.sanitizers) environment.publishable_note += " sanitizers are on;";
    if (!supported_platform) {
      environment.publishable_note +=
          " not Linux, whose fdatasync is the barrier these guarantees are written against;";
    }
    environment.publishable_note += " these numbers must not be quoted";
  }
  return environment;
}

// --- policies ----------------------------------------------------------------

std::string PolicySpec::label() const {
  if (policy == SyncPolicy::kSyncBatch) {
    return std::string(SyncPolicyName(policy)) + "(" + std::to_string(batch_size) + ")";
  }
  return std::string(SyncPolicyName(policy));
}

std::uint64_t PolicySpec::group() const {
  return policy == SyncPolicy::kSyncBatch ? static_cast<std::uint64_t>(batch_size) : 1;
}

std::vector<PolicySpec> DefaultPolicySet() {
  return {PolicySpec{SyncPolicy::kSyncEach, 1}, PolicySpec{SyncPolicy::kSyncBatch, 16},
          PolicySpec{SyncPolicy::kSyncBatch, 64}, PolicySpec{SyncPolicy::kWriteOnly, 1}};
}

// --- the run -----------------------------------------------------------------

Result<BenchmarkReport> RunBenchmark(const BenchmarkOptions& options) {
  if (options.records == 0) {
    return Error(ErrorKind::kInvalidArgument, "a benchmark needs at least one record");
  }
  if (options.trials == 0 || options.trials > kMaxBenchmarkTrials) {
    return Error(ErrorKind::kInvalidArgument,
                 "--trials must be between 1 and " + std::to_string(kMaxBenchmarkTrials));
  }
  if (options.payload_bytes > kMaxPayloadBytes) {
    return Error(ErrorKind::kLimitExceeded, "payload is " + std::to_string(options.payload_bytes) +
                                                " bytes; the v1 limit is " +
                                                std::to_string(kMaxPayloadBytes));
  }

  std::vector<PolicySpec> policies =
      options.policies.empty() ? DefaultPolicySet() : options.policies;

  // Bounded before anything is written, and bounded twice: one trial's file is the peak
  // disk use, while the total is what the whole run will move (docs/PRD.md section 6).
  const std::uint64_t record_bytes =
      static_cast<std::uint64_t>(kHeaderSize) + options.payload_bytes;
  const std::uint64_t per_trial = options.records * record_bytes;
  if (per_trial > kMaxBenchmarkBytesPerTrial) {
    return Error(ErrorKind::kLimitExceeded, "one trial would write " + std::to_string(per_trial) +
                                                " bytes; the limit is " +
                                                std::to_string(kMaxBenchmarkBytesPerTrial));
  }
  const std::uint64_t total = per_trial * policies.size() * options.trials;
  if (total > kMaxBenchmarkBytesTotal) {
    return Error(ErrorKind::kLimitExceeded, "this run would write " + std::to_string(total) +
                                                " bytes across " + std::to_string(policies.size()) +
                                                " policies and " + std::to_string(options.trials) +
                                                " trials; the limit is " +
                                                std::to_string(kMaxBenchmarkBytesTotal));
  }

  ScratchDirectory scratch;
  Status created = scratch.Create(options.workdir, "bench", std::to_string(options.records) + "r");
  if (!created.ok()) return created.error();

  BenchmarkReport report;
  report.environment = CaptureEnvironment(scratch.path());
  report.records = options.records;
  report.payload_bytes = options.payload_bytes;
  report.trials = options.trials;

  const Clock::time_point started = Clock::now();
  std::error_code ec;

  for (const PolicySpec& spec : policies) {
    PolicyResult result;
    result.spec = spec;
    std::vector<double> throughput;
    std::vector<double> p50;
    std::vector<double> p95;
    std::vector<double> p99;

    for (std::uint64_t trial = 0; trial < options.trials; ++trial) {
      // A fresh file per trial, so no trial inherits another's page cache state or file
      // size (benchmarks/README.md step 4).
      const std::filesystem::path log =
          scratch.file(spec.label() + "-trial" + std::to_string(trial) + ".log");
      Result<TrialResult> measured = RunTrial(spec, options, log);
      if (!measured.ok()) return measured.error();
      const TrialResult& value = measured.value();

      if (result.bytes_written == 0) {
        result.bytes_written = value.bytes_written;
      } else if (result.bytes_written != value.bytes_written) {
        return Error(ErrorKind::kInternal,
                     spec.label() + " wrote " + std::to_string(value.bytes_written) +
                         " bytes in one trial and " + std::to_string(result.bytes_written) +
                         " in another; the workload was not equal");
      }

      throughput.push_back(value.records_per_second);
      p50.push_back(value.ack_p50_us);
      p95.push_back(value.ack_p95_us);
      p99.push_back(value.ack_p99_us);
      result.trials.push_back(value);
      report.total_bytes_written += value.bytes_written;

      // One trial's file at a time: the bound above is then the real peak disk use.
      std::filesystem::remove(log, ec);
    }

    result.records_per_second = Summarize(std::move(throughput));
    result.ack_p50_us = Summarize(std::move(p50));
    result.ack_p95_us = Summarize(std::move(p95));
    result.ack_p99_us = Summarize(std::move(p99));
    result.peak_rss_bytes = PeakResidentBytes();
    report.policies.push_back(std::move(result));
  }

  // Every policy must have written exactly the same bytes. They differ only in when the
  // barrier is called, so an unequal byte count means the comparison is not one.
  for (const PolicyResult& policy : report.policies) {
    if (policy.bytes_written != report.policies.front().bytes_written) {
      return Error(ErrorKind::kInternal,
                   policy.spec.label() + " wrote " + std::to_string(policy.bytes_written) +
                       " bytes where " + report.policies.front().spec.label() + " wrote " +
                       std::to_string(report.policies.front().bytes_written));
    }
  }

  report.total_seconds = SecondsBetween(started, Clock::now());
  return report;
}

// --- reporting ---------------------------------------------------------------

namespace {

void WriteSpread(std::ostream& out, const char* name, const Spread& spread, const char* indent) {
  out << indent << "\"" << name << "\": {\"median\": " << Fixed(spread.median, 3)
      << ", \"min\": " << Fixed(spread.min, 3) << ", \"max\": " << Fixed(spread.max, 3) << "}";
}

}  // namespace

Status WriteBenchmarkJson(const std::filesystem::path& path, const BenchmarkReport& report) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    return Error(ErrorKind::kIo, "could not open " + path.string() + " for writing");
  }

  const Environment& env = report.environment;
  out << "{\n";
  out << "  \"schema\": \"eventlog-benchmark-v1\",\n";
  out << "  \"publishable\": " << (env.publishable ? "true" : "false") << ",\n";
  out << "  \"publishable_note\": \"" << JsonEscape(env.publishable_note) << "\",\n";
  out << "  \"environment\": {\n";
  out << "    \"os\": \"" << JsonEscape(env.os) << "\",\n";
  out << "    \"cpu\": \"" << JsonEscape(env.cpu) << "\",\n";
  out << "    \"cpu_count\": " << env.cpu_count << ",\n";
  out << "    \"ram_bytes\": " << env.ram_bytes << ",\n";
  out << "    \"filesystem\": \"" << JsonEscape(env.filesystem) << "\",\n";
  out << "    \"storage_class\": \"" << JsonEscape(env.storage_class) << "\",\n";
  out << "    \"compiler\": \"" << JsonEscape(env.compiler) << "\",\n";
  out << "    \"build_type\": \"" << JsonEscape(env.build_type) << "\",\n";
  out << "    \"sanitizers\": " << (env.sanitizers ? "true" : "false") << ",\n";
  out << "    \"durability_barrier\": \"" << JsonEscape(env.durability_barrier) << "\",\n";
  out << "    \"load_average_1m\": " << Fixed(env.load_average_1m, 2) << "\n";
  out << "  },\n";
  out << "  \"workload\": {\n";
  out << "    \"records\": " << report.records << ",\n";
  out << "    \"payload_bytes\": " << report.payload_bytes << ",\n";
  out << "    \"record_bytes\": " << (kHeaderSize + report.payload_bytes) << ",\n";
  out << "    \"trials\": " << report.trials << ",\n";
  out << "    \"payload_source\": \"deterministic SplitMix64 keyed by sequence number\",\n";
  out << "    \"file_per_trial\": true\n";
  out << "  },\n";
  out << "  \"units\": {\n";
  out << "    \"latency\": \"microseconds\",\n";
  out << "    \"throughput\": \"records per second, over time spent inside append calls\",\n";
  out << "    \"wall_seconds\": \"seconds, including deterministic payload generation\",\n";
  out << "    \"bytes\": \"bytes\",\n";
  out << "    \"peak_rss_bytes\": \"bytes, process-wide peak from getrusage\",\n";
  out << "    \"percentile_method\": \"nearest rank, no interpolation\"\n";
  out << "  },\n";
  out << "  \"policies\": [\n";

  for (std::size_t i = 0; i < report.policies.size(); ++i) {
    const PolicyResult& policy = report.policies[i];
    out << "    {\n";
    out << "      \"label\": \"" << JsonEscape(policy.spec.label()) << "\",\n";
    out << "      \"policy\": \"" << SyncPolicyName(policy.spec.policy) << "\",\n";
    out << "      \"batch_size\": " << policy.spec.batch_size << ",\n";
    out << "      \"records_per_acknowledgment\": " << policy.spec.group() << ",\n";
    out << "      \"acknowledgment\": \""
        << (policy.spec.durable() ? "durable" : "accepted_not_durable") << "\",\n";
    out << "      \"durable_ack_latency_applicable\": "
        << (policy.spec.durable() ? "true" : "false") << ",\n";
    if (!policy.spec.durable()) {
      out << "      \"latency_meaning\": \"time to accept a record; this policy never calls the "
             "durability barrier, so durable-ack latency does not apply\",\n";
    }
    out << "      \"bytes_written\": " << policy.bytes_written << ",\n";
    out << "      \"peak_rss_bytes\": " << policy.peak_rss_bytes << ",\n";
    WriteSpread(out, "records_per_second", policy.records_per_second, "      ");
    out << ",\n";
    out << "      \"ack_latency_us\": {\n";
    WriteSpread(out, "p50", policy.ack_p50_us, "        ");
    out << ",\n";
    WriteSpread(out, "p95", policy.ack_p95_us, "        ");
    out << ",\n";
    WriteSpread(out, "p99", policy.ack_p99_us, "        ");
    out << "\n      },\n";
    out << "      \"trials\": [\n";
    for (std::size_t t = 0; t < policy.trials.size(); ++t) {
      const TrialResult& trial = policy.trials[t];
      out << "        {\"records\": " << trial.records
          << ", \"acknowledgment_units\": " << trial.acknowledgment_units
          << ", \"bytes_written\": " << trial.bytes_written
          << ", \"write_calls\": " << trial.write_calls << ", \"sync_calls\": " << trial.sync_calls
          << ", \"append_seconds\": " << Fixed(trial.append_seconds, 6)
          << ", \"wall_seconds\": " << Fixed(trial.wall_seconds, 6)
          << ", \"records_per_second\": " << Fixed(trial.records_per_second, 3)
          << ", \"ack_p50_us\": " << Fixed(trial.ack_p50_us, 3)
          << ", \"ack_p95_us\": " << Fixed(trial.ack_p95_us, 3)
          << ", \"ack_p99_us\": " << Fixed(trial.ack_p99_us, 3)
          << ", \"ack_max_us\": " << Fixed(trial.ack_max_us, 3) << "}";
      out << (t + 1 < policy.trials.size() ? ",\n" : "\n");
    }
    out << "      ]\n";
    out << "    }" << (i + 1 < report.policies.size() ? ",\n" : "\n");
  }
  out << "  ],\n";

  // Computed only now, from measurements, never from a target (benchmarks/README step 9).
  out << "  \"comparison\": {\n";
  out << "    \"baseline\": \""
      << JsonEscape(report.policies.empty() ? std::string() : report.policies.front().spec.label())
      << "\",\n";
  out << "    \"records_per_second_ratio_vs_baseline\": {\n";
  const double baseline =
      report.policies.empty() ? 0.0 : report.policies.front().records_per_second.median;
  for (std::size_t i = 0; i < report.policies.size(); ++i) {
    const double ratio = baseline > 0 ? report.policies[i].records_per_second.median / baseline : 0;
    out << "      \"" << JsonEscape(report.policies[i].spec.label()) << "\": " << Fixed(ratio, 3)
        << (i + 1 < report.policies.size() ? ",\n" : "\n");
  }
  out << "    }\n";
  out << "  },\n";
  out << "  \"total_seconds\": " << Fixed(report.total_seconds, 3) << ",\n";
  out << "  \"total_bytes_written\": " << report.total_bytes_written << "\n";
  out << "}\n";

  out.flush();
  if (!out) return Error(ErrorKind::kIo, "failed while writing " + path.string());
  return Status::Ok();
}

void PrintBenchmarkTable(std::ostream& out, const BenchmarkReport& report) {
  if (!report.environment.publishable) {
    out << "WARNING: " << report.environment.publishable_note << "\n\n";
  }

  out << "Policy          records/sec       ack p50/p95/p99 (us)        vs baseline\n";
  const double baseline =
      report.policies.empty() ? 0.0 : report.policies.front().records_per_second.median;
  bool any_accepted_only = false;
  for (const PolicyResult& policy : report.policies) {
    const double ratio = baseline > 0 ? policy.records_per_second.median / baseline : 0.0;
    std::string label = policy.spec.label();
    if (label.size() < 15) label.resize(15, ' ');
    std::string throughput = Fixed(policy.records_per_second.median, 1);
    if (throughput.size() < 17) throughput.resize(17, ' ');
    std::string latency = Fixed(policy.ack_p50_us.median, 1) + " / " +
                          Fixed(policy.ack_p95_us.median, 1) + " / " +
                          Fixed(policy.ack_p99_us.median, 1);
    if (!policy.spec.durable()) {
      latency += " *";
      any_accepted_only = true;
    }
    if (latency.size() < 27) latency.resize(27, ' ');
    out << label << " " << throughput << " " << latency << " " << Fixed(ratio, 2) << "x\n";
  }

  if (any_accepted_only) {
    out << "* accepted, not durable: time to accept a record. This policy never calls the "
           "barrier,\n  so durable-ack latency does not apply to it.\n";
  }
  out << "Median of " << report.trials << " trials, " << report.records << " records of "
      << report.payload_bytes
      << " payload bytes each. Barrier: " << report.environment.durability_barrier
      << ". Build: " << report.environment.build_type
      << (report.environment.sanitizers ? " with sanitizers" : "") << ".\n";
}

}  // namespace eventlog
