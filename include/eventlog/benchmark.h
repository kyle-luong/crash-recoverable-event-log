#ifndef EVENTLOG_BENCHMARK_H_
#define EVENTLOG_BENCHMARK_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "eventlog/error.h"
#include "eventlog/writer.h"

namespace eventlog {

// Median with the range around it. `benchmarks/README.md` step 7 requires median and
// spread, never the best run, so there is no "best" accessor here to reach for.
struct Spread {
  double median = 0;
  double min = 0;
  double max = 0;
};

Spread Summarize(std::vector<double> values);

// Nearest-rank percentile of an already-sorted sample: the smallest observed value at or
// above the requested fraction of the sample. No interpolation, so every reported figure
// is a measurement that actually occurred.
double PercentileOfSorted(const std::vector<double>& sorted, double fraction);

// What the machine was, captured rather than assumed (CPP-T17). Every field is either
// measured or says it is unknown; nothing is guessed from a plausible default.
struct Environment {
  std::string os;
  std::string cpu;
  std::uint64_t cpu_count = 0;
  std::uint64_t ram_bytes = 0;
  std::string filesystem;  // of the scratch directory actually written
  // "unknown" unless something measured it. Distinguishing an SSD from spinning media or
  // from a virtual disk needs privileged or host-side queries this tool does not make,
  // and a guess here would silently change how every number should be read.
  std::string storage_class;
  std::string compiler;
  std::string build_type;
  bool sanitizers = false;
  std::string durability_barrier;
  double load_average_1m = 0;
  // True only for a Release build with sanitizers off, on the supported platform. A
  // sanitizer or Debug build is never a throughput measurement, and a macOS barrier is a
  // different and stronger one (`benchmarks/README.md` steps 1 and 2). A report that
  // states this cannot be quoted by accident.
  bool publishable = false;
  std::string publishable_note;
};

// The rule behind `Environment::publishable`, as a function of its inputs rather than of
// the build that happens to be running: `benchmarks/README.md` steps 1 and 2 require a
// Release build with sanitizers off, on the supported platform. Exposed so the whole
// truth table can be checked, since a test suite can only ever observe its own build.
bool IsPublishable(std::string_view build_type, bool sanitizers, bool supported_platform);

Environment CaptureEnvironment(const std::filesystem::path& scratch);

struct PolicySpec {
  SyncPolicy policy = SyncPolicy::kSyncEach;
  std::size_t batch_size = 1;

  // "sync_each", "sync_batch(16)", "write_only".
  std::string label() const;
  // How many records share one acknowledgment, or one acceptance under write_only.
  std::uint64_t group() const;
  bool durable() const { return policy != SyncPolicy::kWriteOnly; }
};

// sync_each, sync_batch(16), sync_batch(64), write_only — the comparison set fixed by
// `benchmarks/README.md` step 6.
std::vector<PolicySpec> DefaultPolicySet();

struct TrialResult {
  std::uint64_t records = 0;
  std::uint64_t acknowledgment_units = 0;
  std::uint64_t bytes_written = 0;
  std::uint64_t write_calls = 0;
  std::uint64_t sync_calls = 0;
  // Time inside the append calls only. Throughput is computed from this, so it measures
  // the log rather than the payload generator.
  double append_seconds = 0;
  // The whole loop, including generating each record's deterministic payload. The gap
  // between the two is the generator's share and is reported rather than hidden.
  double wall_seconds = 0;
  double records_per_second = 0;
  double ack_p50_us = 0;
  double ack_p95_us = 0;
  double ack_p99_us = 0;
  double ack_max_us = 0;
};

struct PolicyResult {
  PolicySpec spec;
  std::vector<TrialResult> trials;
  Spread records_per_second;
  Spread ack_p50_us;
  Spread ack_p95_us;
  Spread ack_p99_us;
  // Identical across trials and across policies; the benchmark fails if it is not, since
  // unequal workloads would make the comparison meaningless.
  std::uint64_t bytes_written = 0;
  // Process-wide peak as reported by getrusage, not a per-trial delta.
  std::uint64_t peak_rss_bytes = 0;
};

struct BenchmarkOptions {
  std::uint64_t records = 20000;
  std::size_t payload_bytes = 128;
  std::uint64_t trials = 5;
  // Scratch root. One fresh subdirectory per invocation; one trial's file exists at a
  // time, so peak disk use is one trial's worth.
  std::filesystem::path workdir;
  // Empty means DefaultPolicySet().
  std::vector<PolicySpec> policies;
};

struct BenchmarkReport {
  Environment environment;
  std::uint64_t records = 0;
  std::size_t payload_bytes = 0;
  std::uint64_t trials = 0;
  std::vector<PolicyResult> policies;
  double total_seconds = 0;
  std::uint64_t total_bytes_written = 0;
};

// Bounds, checked before anything is written (docs/PRD.md section 6).
inline constexpr std::uint64_t kMaxBenchmarkTrials = 50;
inline constexpr std::uint64_t kMaxBenchmarkBytesPerTrial = 4ull << 30;
inline constexpr std::uint64_t kMaxBenchmarkBytesTotal = 16ull << 30;

Result<BenchmarkReport> RunBenchmark(const BenchmarkOptions& options);

Status WriteBenchmarkJson(const std::filesystem::path& path, const BenchmarkReport& report);
void PrintBenchmarkTable(std::ostream& out, const BenchmarkReport& report);

}  // namespace eventlog

#endif  // EVENTLOG_BENCHMARK_H_
