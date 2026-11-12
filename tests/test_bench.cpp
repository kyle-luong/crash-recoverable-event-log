// CPP-AC13 and tests CPP-T16, CPP-T17: the benchmark's statistics, its environment
// capture, and the guards that keep a number from being quoted out of context.
//
// Nothing here asserts a throughput or latency figure. The suite runs under sanitizers,
// where any such number would be meaningless, so what it checks is that the measurement
// machinery reports what it measured -- the right number of trials, the right syscall
// counts per policy, equal workloads across policies, and a report that says plainly when
// it must not be published.

#include "eventlog/benchmark.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "eventlog/record.h"
#include "eventlog/scratch_dir.h"
#include "test_support.h"

namespace eventlog {
namespace {

std::string ReadWholeFile(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) throw test::AssertionFailure("could not read " + path.string());
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

bool Mentions(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

// --- statistics --------------------------------------------------------------

TEST(PercentilesPickAnObservedValue) {
  std::vector<double> sorted;
  for (int i = 1; i <= 100; ++i) sorted.push_back(static_cast<double>(i));

  // Nearest rank, so every reported figure is a sample that actually occurred: no
  // interpolated latency that nothing ever measured.
  EL_CHECK_EQ(PercentileOfSorted(sorted, 0.50), 50.0);
  EL_CHECK_EQ(PercentileOfSorted(sorted, 0.95), 95.0);
  EL_CHECK_EQ(PercentileOfSorted(sorted, 0.99), 99.0);
  EL_CHECK_EQ(PercentileOfSorted(sorted, 1.0), 100.0);
  EL_CHECK_EQ(PercentileOfSorted(sorted, 0.0), 1.0);
}

TEST(PercentilesHandleTinyAndEmptySamples) {
  EL_CHECK_EQ(PercentileOfSorted({}, 0.5), 0.0);
  EL_CHECK_EQ(PercentileOfSorted({7.5}, 0.5), 7.5);
  EL_CHECK_EQ(PercentileOfSorted({7.5}, 0.99), 7.5);
  // Two samples: p50 must still be one of them, not their mean.
  EL_CHECK_EQ(PercentileOfSorted({2.0, 8.0}, 0.5), 2.0);
  EL_CHECK_EQ(PercentileOfSorted({2.0, 8.0}, 0.95), 8.0);
}

TEST(SummarizeReportsMedianAndRangeNotTheBestRun) {
  const Spread odd = Summarize({5.0, 1.0, 3.0});
  EL_CHECK_EQ(odd.median, 3.0);
  EL_CHECK_EQ(odd.min, 1.0);
  EL_CHECK_EQ(odd.max, 5.0);

  const Spread even = Summarize({4.0, 1.0, 3.0, 2.0});
  EL_CHECK_EQ(even.median, 2.5);
  EL_CHECK_EQ(even.min, 1.0);
  EL_CHECK_EQ(even.max, 4.0);

  const Spread none = Summarize({});
  EL_CHECK_EQ(none.median, 0.0);
}

// --- policy set --------------------------------------------------------------

TEST(DefaultPolicySetIsTheDocumentedComparison) {
  // benchmarks/README.md step 6 fixes this set. A quieter default would make published
  // numbers incomparable with the method the repository committed to.
  const std::vector<PolicySpec> policies = DefaultPolicySet();
  EL_CHECK_EQ(policies.size(), std::size_t{4});
  EL_CHECK_EQ(policies[0].label(), std::string("sync_each"));
  EL_CHECK_EQ(policies[1].label(), std::string("sync_batch(16)"));
  EL_CHECK_EQ(policies[2].label(), std::string("sync_batch(64)"));
  EL_CHECK_EQ(policies[3].label(), std::string("write_only"));

  EL_CHECK(policies[0].durable());
  EL_CHECK(policies[1].durable());
  EL_CHECK(!policies[3].durable());
  EL_CHECK_EQ(policies[0].group(), std::uint64_t{1});
  EL_CHECK_EQ(policies[2].group(), std::uint64_t{64});
  EL_CHECK_EQ(policies[3].group(), std::uint64_t{1});
}

// --- environment -------------------------------------------------------------

// The rule itself, over every combination of its inputs. A suite can only observe the
// build it was compiled into -- this one runs under sanitizers in two phases and in
// Release in a third -- so asserting the ambient build's answer would test one row of the
// table and silently invert in the other phase.
TEST(PublishabilityNeedsReleaseSanitizersOffAndTheSupportedPlatform) {
  EL_CHECK(IsPublishable("Release", false, true));
  EL_CHECK(!IsPublishable("Release", true, true));
  EL_CHECK(!IsPublishable("Release", false, false));
  EL_CHECK(!IsPublishable("Release", true, false));
  EL_CHECK(!IsPublishable("Debug", false, true));
  EL_CHECK(!IsPublishable("Debug", true, true));
  EL_CHECK(!IsPublishable("Debug", false, false));
  EL_CHECK(!IsPublishable("RelWithDebInfo", false, true));
  EL_CHECK(!IsPublishable("", false, true));
}

TEST(EnvironmentIsCapturedAndNeverGuessed) {
  const test::TempDir dir("bench-environment");
  const Environment environment = CaptureEnvironment(dir.path());

  EL_CHECK(!environment.os.empty());
  EL_CHECK(!environment.cpu.empty());
  EL_CHECK(!environment.filesystem.empty());
  EL_CHECK(!environment.compiler.empty());
  EL_CHECK(!environment.durability_barrier.empty());
  EL_CHECK(!environment.build_type.empty());
  EL_CHECK(environment.cpu_count > 0);
  // Storage class needs a privileged or host-side query this tool does not make, so it
  // must say unknown rather than name a plausible medium.
  EL_CHECK_EQ(environment.storage_class, std::string("unknown"));

  // Whatever build this is, the captured flag must agree with the rule and the note must
  // say which way it went.
#if defined(EVENTLOG_SANITIZERS_ON)
  EL_CHECK(environment.sanitizers);
#else
  EL_CHECK(!environment.sanitizers);
#endif
#if defined(__linux__)
  const bool supported_platform = true;
#else
  const bool supported_platform = false;
#endif
  EL_CHECK_EQ(environment.publishable,
              IsPublishable(environment.build_type, environment.sanitizers, supported_platform));
  EL_CHECK(!environment.publishable_note.empty());
  if (environment.publishable) {
    EL_CHECK(Mentions(environment.publishable_note, "Release"));
    EL_CHECK(!Mentions(environment.publishable_note, "NOT publishable"));
  } else {
    EL_CHECK(Mentions(environment.publishable_note, "NOT publishable"));
    EL_CHECK(Mentions(environment.publishable_note, "must not be quoted"));
  }
}

// --- the run -----------------------------------------------------------------

TEST(EveryPolicyRunsEveryTrialOnAnEqualWorkload) {
  const test::TempDir dir("bench-run");
  BenchmarkOptions options;
  options.records = 8;
  options.payload_bytes = 16;
  options.trials = 2;
  options.workdir = dir.path();

  const BenchmarkReport report = EL_EXPECT_OK(RunBenchmark(options));
  EL_CHECK_EQ(report.policies.size(), std::size_t{4});
  EL_CHECK_EQ(report.records, std::uint64_t{8});
  EL_CHECK_EQ(report.trials, std::uint64_t{2});

  const std::uint64_t expected_bytes = 8 * (kHeaderSize + 16);
  for (const PolicyResult& policy : report.policies) {
    EL_CHECK_EQ(policy.trials.size(), std::size_t{2});
    // Equal payload contents and record counts across policies: the policies differ only
    // in when the barrier is called, never in what is written.
    EL_CHECK_EQ(policy.bytes_written, expected_bytes);
    for (const TrialResult& trial : policy.trials) {
      EL_CHECK_EQ(trial.records, std::uint64_t{8});
      EL_CHECK_EQ(trial.bytes_written, expected_bytes);
      // One latency sample per acknowledgment unit, at the policy's real granularity. A
      // batch larger than the workload still forms one short unit, acknowledged whole.
      const std::uint64_t group = policy.spec.group();
      EL_CHECK_EQ(trial.acknowledgment_units, (8 + group - 1) / group);
      EL_CHECK(trial.wall_seconds >= trial.append_seconds);
    }
  }
}

// The syscall counts are what make the tradeoff a fact rather than a story, and the
// benchmark refuses to report a durable policy that quietly synced less often.
TEST(BarrierCallsMatchEachPolicysAcknowledgmentGranularity) {
  const test::TempDir dir("bench-syscalls");
  BenchmarkOptions options;
  options.records = 64;
  options.payload_bytes = 0;
  options.trials = 1;
  options.workdir = dir.path();

  const BenchmarkReport report = EL_EXPECT_OK(RunBenchmark(options));
  EL_CHECK_EQ(report.policies[0].trials[0].sync_calls, std::uint64_t{64});  // sync_each
  EL_CHECK_EQ(report.policies[1].trials[0].sync_calls, std::uint64_t{4});   // sync_batch(16)
  EL_CHECK_EQ(report.policies[2].trials[0].sync_calls, std::uint64_t{1});   // sync_batch(64)
  EL_CHECK_EQ(report.policies[3].trials[0].sync_calls, std::uint64_t{0});   // write_only
  EL_CHECK_EQ(report.policies[1].trials[0].write_calls, std::uint64_t{4});
  EL_CHECK_EQ(report.policies[3].trials[0].acknowledgment_units, std::uint64_t{64});
}

TEST(BenchmarkBoundsAreCheckedBeforeAnythingIsWritten) {
  const test::TempDir dir("bench-bounds");
  BenchmarkOptions options;
  options.records = 4;
  options.payload_bytes = 8;
  options.trials = 1;
  options.workdir = dir.path();

  BenchmarkOptions no_records = options;
  no_records.records = 0;
  EL_EXPECT_ERROR(RunBenchmark(no_records), ErrorKind::kInvalidArgument);

  BenchmarkOptions no_trials = options;
  no_trials.trials = 0;
  EL_EXPECT_ERROR(RunBenchmark(no_trials), ErrorKind::kInvalidArgument);

  BenchmarkOptions too_many_trials = options;
  too_many_trials.trials = kMaxBenchmarkTrials + 1;
  EL_EXPECT_ERROR(RunBenchmark(too_many_trials), ErrorKind::kInvalidArgument);

  BenchmarkOptions huge_payload = options;
  huge_payload.payload_bytes = kMaxPayloadBytes + 1;
  EL_EXPECT_ERROR(RunBenchmark(huge_payload), ErrorKind::kLimitExceeded);

  // One trial's file is the peak disk use, and the whole run is bounded separately.
  BenchmarkOptions huge_trial = options;
  huge_trial.records = 1'000'000;
  huge_trial.payload_bytes = 1 << 20;
  EL_EXPECT_ERROR(RunBenchmark(huge_trial), ErrorKind::kLimitExceeded);

  BenchmarkOptions huge_total = options;
  huge_total.records = 40'000;
  huge_total.payload_bytes = 1 << 20;
  huge_total.trials = 5;
  EL_EXPECT_ERROR(RunBenchmark(huge_total), ErrorKind::kLimitExceeded);

  // A rejected run must not have left fixtures behind.
  EL_CHECK(std::filesystem::is_empty(dir.path()));
}

TEST(BenchmarkCleansUpItsFixtures) {
  const test::TempDir dir("bench-cleanup");
  BenchmarkOptions options;
  options.records = 4;
  options.payload_bytes = 8;
  options.trials = 1;
  options.workdir = dir.path();
  options.policies = {PolicySpec{SyncPolicy::kSyncEach, 1}};

  EL_EXPECT_OK(RunBenchmark(options));
  EL_CHECK(std::filesystem::is_empty(dir.path()));
}

// --- the report --------------------------------------------------------------

// CPP-T17: the recorded output has to carry enough context that a reader can tell what
// was measured and on what. A number without its environment is not a result.
TEST(JsonReportCarriesEveryFieldAResultNeeds) {
  const test::TempDir dir("bench-json");
  BenchmarkOptions options;
  options.records = 4;
  options.payload_bytes = 32;
  options.trials = 2;
  options.workdir = dir.path();

  const BenchmarkReport report = EL_EXPECT_OK(RunBenchmark(options));
  const auto json = dir.file("results.json");
  EL_EXPECT_STATUS_OK(WriteBenchmarkJson(json, report));
  const std::string text = ReadWholeFile(json);

  for (const std::string_view key :
       {"\"compiler\"", "\"cpu\"", "\"filesystem\"", "\"os\"", "\"build_type\"", "\"sanitizers\"",
        "\"durability_barrier\"", "\"batch_size\"", "\"policy\"", "\"trials\"", "\"units\"",
        "\"records_per_second\"", "\"ack_latency_us\"", "\"percentile_method\"",
        "\"storage_class\"", "\"comparison\"", "\"baseline\"", "\"publishable_note\""}) {
    if (!Mentions(text, key)) EL_FAIL("report is missing " + std::string(key));
  }

  // Units are named, never implied.
  EL_CHECK(Mentions(text, "microseconds"));
  EL_CHECK(Mentions(text, "records per second"));
  // write_only must not carry the same acknowledgment status as a synchronized policy.
  EL_CHECK(Mentions(text, "\"accepted_not_durable\""));
  EL_CHECK(Mentions(text, "\"durable_ack_latency_applicable\": false"));
  // Whichever build produced it, the report states its own publishability.
  EL_CHECK(Mentions(
      text, report.environment.publishable ? "\"publishable\": true" : "\"publishable\": false"));
}

// Both branches, from a hand-built report, so the warning is tested in every build rather
// than only in the one that happens to be disqualified.
TEST(TableWarnsOnlyWhenTheReportCannotBeQuoted) {
  BenchmarkReport report;
  report.records = 10;
  report.trials = 3;
  report.payload_bytes = 64;
  PolicyResult policy;
  policy.spec = PolicySpec{SyncPolicy::kSyncEach, 1};
  policy.records_per_second = Spread{100.0, 90.0, 110.0};
  report.policies.push_back(policy);
  PolicyResult accepted;
  accepted.spec = PolicySpec{SyncPolicy::kWriteOnly, 1};
  accepted.records_per_second = Spread{9000.0, 8900.0, 9100.0};
  report.policies.push_back(accepted);

  report.environment.durability_barrier = "fdatasync";
  report.environment.build_type = "Debug";
  report.environment.sanitizers = true;
  report.environment.publishable = false;
  report.environment.publishable_note =
      "NOT publishable: sanitizers are on; these numbers "
      "must not be quoted";
  std::ostringstream disqualified;
  PrintBenchmarkTable(disqualified, report);
  EL_CHECK(Mentions(disqualified.str(), "WARNING"));
  EL_CHECK(Mentions(disqualified.str(), "must not be quoted"));
  // The accepted-only policy is marked in the table too, not just in the JSON.
  EL_CHECK(Mentions(disqualified.str(), "accepted, not durable"));
  EL_CHECK(Mentions(disqualified.str(), "Median of 3 trials"));

  report.environment.build_type = "Release";
  report.environment.sanitizers = false;
  report.environment.publishable = true;
  report.environment.publishable_note = "Release build, sanitizers off";
  std::ostringstream publishable;
  PrintBenchmarkTable(publishable, report);
  EL_CHECK(!Mentions(publishable.str(), "WARNING"));
  EL_CHECK(Mentions(publishable.str(), "vs baseline"));
}

// --- the shared scratch directory -------------------------------------------

TEST(ScratchDirectoryCreatesAndRemovesOnlyWhatItMade) {
  const test::TempDir dir("bench-scratch");
  std::filesystem::path created;
  {
    ScratchDirectory scratch;
    EL_EXPECT_STATUS_OK(scratch.Create(dir.path(), "probe", "one"));
    created = scratch.path();
    EL_CHECK(std::filesystem::is_directory(created));
    test::WriteFileBytes(scratch.file("fixture.log"), {std::byte{1}});
  }
  EL_CHECK(!std::filesystem::exists(created));
  // The caller's root is the caller's.
  EL_CHECK(std::filesystem::is_directory(dir.path()));
}

TEST(ScratchDirectoryKeepsEvidenceWhenAsked) {
  const test::TempDir dir("bench-scratch-keep");
  std::filesystem::path created;
  {
    ScratchDirectory scratch;
    EL_EXPECT_STATUS_OK(scratch.Create(dir.path(), "probe", "two"));
    created = scratch.path();
    scratch.Keep();
  }
  EL_CHECK(std::filesystem::is_directory(created));
}

TEST(ScratchDirectoryRefusesASymlinkedRoot) {
  const test::TempDir dir("bench-scratch-symlink");
  const auto target = dir.file("real");
  std::filesystem::create_directory(target);
  const auto link = dir.file("link");
  std::error_code ec;
  std::filesystem::create_directory_symlink(target, link, ec);
  if (ec) return;  // a filesystem without symlinks has nothing to test here

  ScratchDirectory scratch;
  const Status created = scratch.Create(link, "probe", "three");
  EL_CHECK(!created.ok());
  EL_CHECK_EQ(created.error().kind(), ErrorKind::kInvalidArgument);
  EL_CHECK(Mentions(created.error().message(), "symlink"));
  EL_CHECK(std::filesystem::is_empty(target));
}

}  // namespace
}  // namespace eventlog
