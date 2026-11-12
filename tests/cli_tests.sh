#!/usr/bin/env bash
# CPP-AC12 / CPP-AC15, test CPP-T14: CLI exit codes, bounds, destructive-operation
# protection, and the read-only guarantee of `verify`.
#
# Usage: cli_tests.sh /path/to/eventlog
set -uo pipefail

EVENTLOG="${1:?usage: cli_tests.sh <path-to-eventlog-binary>}"
if [ ! -x "$EVENTLOG" ]; then
  echo "not executable: $EVENTLOG" >&2
  exit 1
fi

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/eventlog-cli-tests.XXXXXX")"
trap 'rm -rf "$WORKDIR"' EXIT INT TERM

PASS=0
FAIL=0

ok()   { PASS=$((PASS + 1)); printf '  PASS  %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL  %s\n          %s\n' "$1" "$2"; }

# Runs the binary, capturing stdout+stderr in LAST_OUTPUT and the code in LAST_CODE.
run() {
  LAST_OUTPUT="$("$EVENTLOG" "$@" 2>&1)"
  LAST_CODE=$?
  return 0
}

expect_code() {
  local label="$1" want="$2"
  shift 2
  run "$@"
  if [ "$LAST_CODE" -eq "$want" ]; then
    ok "$label"
  else
    bad "$label" "expected exit $want, got $LAST_CODE: $LAST_OUTPUT"
  fi
}

expect_output_contains() {
  local label="$1" needle="$2"
  case "$LAST_OUTPUT" in
    *"$needle"*) ok "$label" ;;
    *) bad "$label" "output did not contain '$needle': $LAST_OUTPUT" ;;
  esac
}

expect_output_lacks() {
  local label="$1" needle="$2"
  case "$LAST_OUTPUT" in
    *"$needle"*) bad "$label" "output should not have contained '$needle': $LAST_OUTPUT" ;;
    *) ok "$label" ;;
  esac
}

file_hash() {
  if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk '{print $1}'
  else sha256sum "$1" | awk '{print $1}'; fi
}

file_size() { wc -c < "$1" | tr -d ' '; }

echo "[suite] cli_tests ($EVENTLOG)"

# --- help and version --------------------------------------------------------
expect_code "help exits 0" 0 --help
expect_output_contains "help names the durability barrier" "durable"
expect_code "version exits 0" 0 --version
expect_code "no arguments is a usage error" 64
expect_code "unknown command is a usage error" 64 frobnicate

# --- argument validation (exit 64) -------------------------------------------
expect_code "append without --file is a usage error" 64 append --records 1 --payload-bytes 8
expect_code "append without --records is a usage error" 64 append --file "$WORKDIR/a.log" --payload-bytes 8
expect_code "unknown option is a usage error" 64 append --file "$WORKDIR/a.log" --records 1 --payload-bytes 8 --turbo
expect_code "option without a value is a usage error" 64 append --file "$WORKDIR/a.log" --records 1 --payload-bytes
expect_code "positional argument is a usage error" 64 verify --file "$WORKDIR/a.log" extra
expect_code "non-numeric --records is a usage error" 64 append --file "$WORKDIR/a.log" --records abc --payload-bytes 8
expect_code "negative --records is a usage error" 64 append --file "$WORKDIR/a.log" --records -5 --payload-bytes 8

# Bounds: payload above the 1 MiB format limit must be refused by the CLI.
expect_code "payload above 1 MiB is refused" 64 append --file "$WORKDIR/a.log" --records 1 --payload-bytes 1048577
expect_output_contains "payload bound names the limit" "1048576"
expect_code "absurd record count is refused" 64 append --file "$WORKDIR/a.log" --records 99999999999 --payload-bytes 8
expect_code "mutually exclusive flags are refused" 64 append --file "$WORKDIR/a.log" --records 1 --payload-bytes 8 --append-existing --overwrite

# A usage error must not have created anything.
if [ ! -e "$WORKDIR/a.log" ]; then ok "rejected invocations created no file"
else bad "rejected invocations created no file" "$WORKDIR/a.log exists"; fi

# --- append and verify -------------------------------------------------------
LOG="$WORKDIR/events.log"
expect_code "append 200 records succeeds" 0 append --file "$LOG" --records 200 --payload-bytes 64
expect_output_contains "append reports durable acknowledgements" "Durable acknowledged: 200"

expect_code "verify of a clean log exits 0" 0 verify --file "$LOG"
expect_output_contains "verify reports clean" "Status: clean"
expect_output_contains "verify reports the last sequence" "Last sequence: 200"

CLEAN_SIZE="$(file_size "$LOG")"
if [ "$CLEAN_SIZE" -eq $((200 * (24 + 64))) ]; then ok "file size matches the record format"
else bad "file size matches the record format" "expected $((200 * (24 + 64))), got $CLEAN_SIZE"; fi

# Appending to an existing log needs an explicit flag.
expect_code "append to an existing log is refused by default" 10 append --file "$LOG" --records 1 --payload-bytes 64
expect_output_contains "refusal suggests the explicit flags" "--append-existing"
expect_code "append --append-existing continues the log" 0 append --file "$LOG" --records 50 --payload-bytes 64 --append-existing
run verify --file "$LOG"
expect_output_contains "continued log reaches sequence 250" "Last sequence: 250"

# --- verify --json -----------------------------------------------------------
expect_code "verify --json writes a report" 0 verify --file "$LOG" --json "$WORKDIR/report.json"
if grep -q '"status": "clean"' "$WORKDIR/report.json" &&
   grep -q '"valid_records": 250' "$WORKDIR/report.json" &&
   grep -q '"durability_barrier"' "$WORKDIR/report.json"; then
  ok "json report contains status, counts, and the barrier"
else
  bad "json report contains status, counts, and the barrier" "$(cat "$WORKDIR/report.json")"
fi

# --- incomplete tail: verify is read-only, recover needs the flag ------------
TAIL="$WORKDIR/tail.log"
"$EVENTLOG" append --file "$TAIL" --records 10 --payload-bytes 32 >/dev/null 2>&1
FULL_SIZE="$(file_size "$TAIL")"
# Chop 20 bytes off the final record to make a structurally incomplete tail.
if command -v truncate >/dev/null 2>&1; then
  truncate -s $((FULL_SIZE - 20)) "$TAIL"
else
  dd if="$TAIL" of="$TAIL.cut" bs=1 count=$((FULL_SIZE - 20)) 2>/dev/null && mv "$TAIL.cut" "$TAIL"
fi

HASH_BEFORE="$(file_hash "$TAIL")"
expect_code "verify of an incomplete tail exits 1" 1 verify --file "$TAIL"
expect_output_contains "verify names the incomplete tail" "incomplete_tail"
expect_output_contains "verify points at the recover command" "--truncate-tail"

if [ "$(file_hash "$TAIL")" = "$HASH_BEFORE" ]; then ok "verify did not modify the file"
else bad "verify did not modify the file" "hash changed"; fi

expect_code "recover without --truncate-tail exits 1" 1 recover --file "$TAIL"
# Cutting 20 bytes off record 10 leaves the first 36 bytes of a 56-byte record behind,
# so that whole partial record is what recovery would discard - not just the 20 bytes.
TAIL_PREFIX=$((9 * (24 + 32)))
TAIL_DISCARD=$((FULL_SIZE - 20 - TAIL_PREFIX))
expect_output_contains "recover preflight says what it would discard" "Would discard $TAIL_DISCARD bytes"
expect_output_contains "recover preflight says nothing changed" "No change made"

if [ "$(file_hash "$TAIL")" = "$HASH_BEFORE" ]; then ok "recover preflight did not modify the file"
else bad "recover preflight did not modify the file" "hash changed"; fi

expect_code "recover --truncate-tail exits 0" 0 recover --file "$TAIL" --truncate-tail
expect_output_contains "recover reports the recovered status" "recovered_incomplete_tail"

RECOVERED_SIZE="$(file_size "$TAIL")"
if [ "$RECOVERED_SIZE" -eq $((9 * (24 + 32))) ]; then ok "recovery truncated to the valid prefix"
else bad "recovery truncated to the valid prefix" "expected $((9 * (24 + 32))), got $RECOVERED_SIZE"; fi

expect_code "verify after recovery exits 0" 0 verify --file "$TAIL"
expect_output_contains "recovered log has 9 records" "Valid records: 9"
expect_code "recover on a clean log exits 0" 0 recover --file "$TAIL"
expect_output_contains "recover on a clean log does nothing" "Nothing to recover"

# --- hard corruption: never truncated ---------------------------------------
CORRUPT="$WORKDIR/corrupt.log"
"$EVENTLOG" append --file "$CORRUPT" --records 5 --payload-bytes 32 >/dev/null 2>&1
# Flip a payload byte of the final record, leaving it structurally complete.
printf '\xff' | dd of="$CORRUPT" bs=1 seek=$((4 * (24 + 32) + 24 + 3)) conv=notrunc 2>/dev/null
HASH_CORRUPT="$(file_hash "$CORRUPT")"

expect_code "verify of a corrupt record exits 2" 2 verify --file "$CORRUPT"
expect_output_contains "verify names hard corruption" "corrupt_complete_record"
expect_output_contains "verify says it is not auto-recoverable" "Not recoverable automatically"
expect_code "recover refuses hard corruption with exit 2" 2 recover --file "$CORRUPT"
expect_code "recover --truncate-tail still refuses hard corruption" 2 recover --file "$CORRUPT" --truncate-tail

if [ "$(file_hash "$CORRUPT")" = "$HASH_CORRUPT" ]; then ok "corrupt file was never modified"
else bad "corrupt file was never modified" "hash changed"; fi

# Appending to a damaged log is refused rather than burying the damage.
expect_code "append to a corrupt log is refused" 10 append --file "$CORRUPT" --records 1 --payload-bytes 32 --append-existing

# --- destructive-operation protection ---------------------------------------
TARGET="$WORKDIR/target.log"
"$EVENTLOG" append --file "$TARGET" --records 3 --payload-bytes 16 >/dev/null 2>&1
TARGET_HASH="$(file_hash "$TARGET")"
ln -s "$TARGET" "$WORKDIR/link.log" 2>/dev/null
if [ -L "$WORKDIR/link.log" ]; then
  expect_code "--overwrite refuses to act through a symlink" 10 append --file "$WORKDIR/link.log" --records 1 --payload-bytes 16 --overwrite
  expect_output_contains "symlink refusal says why" "symlink"
  if [ "$(file_hash "$TARGET")" = "$TARGET_HASH" ]; then ok "symlink target untouched"
  else bad "symlink target untouched" "hash changed"; fi
fi

# --overwrite on a real path replaces the log and restarts the sequence.
expect_code "--overwrite replaces an existing log" 0 append --file "$TARGET" --records 2 --payload-bytes 16 --overwrite
run verify --file "$TARGET"
expect_output_contains "overwritten log restarts at sequence 2" "Last sequence: 2"


# --- write_only: accepted, never durable (ADR-008) ---------------------------
expect_code "write_only appends without syncing" 0 append --file "$WORKDIR/wo.log" --records 20 --payload-bytes 8 --policy write_only
expect_output_contains "write_only reports acceptance" "Accepted (NOT durable): 20"
# It must not expose the same acknowledgment status as a synchronized policy.
expect_output_lacks "write_only claims no durable acknowledgement" "Durable acknowledged"
expect_output_contains "write_only says what it skipped" "never calls"
expect_output_contains "write_only costs no barrier calls" "Barrier calls: 0"
run verify --file "$WORKDIR/wo.log"
expect_output_contains "a write_only log is still a valid log" "Status: clean"
expect_output_contains "a write_only log holds every record" "Valid records: 20"
# The policy changes when the barrier is called, never what is written.
expect_code "sync_each writes the same workload as write_only" 0 append --file "$WORKDIR/wo-each.log" --records 20 --payload-bytes 8
if [ "$(file_hash "$WORKDIR/wo.log")" = "$(file_hash "$WORKDIR/wo-each.log")" ]; then
  ok "write_only and sync_each produce identical bytes"
else
  bad "write_only and sync_each produce identical bytes" "hashes differ"
fi
expect_code "write_only rejects a batch size" 10 append --file "$WORKDIR/wo2.log" --records 4 --payload-bytes 8 --policy write_only --batch-size 8
expect_code "unknown policy is a usage error" 64 append --file "$WORKDIR/c.log" --records 1 --payload-bytes 8 --policy turbo

# --- sync_batch (CPP-05 / CPP-AC06) ------------------------------------------
expect_code "sync_batch appends" 0 append --file "$WORKDIR/batch.log" --records 100 --payload-bytes 64 --policy sync_batch --batch-size 32
expect_output_contains "sync_batch acknowledges every record" "Durable acknowledged: 100"
expect_output_contains "sync_batch labels its batch size" "Policy: sync_batch(32)"
# 100 records in groups of 32 is four acknowledgment units, not one hundred.
expect_output_contains "sync_batch reports four acknowledgments" "Acknowledgments: 4"
expect_output_contains "sync_batch reports four barrier calls" "Barrier calls: 4"
run verify --file "$WORKDIR/batch.log"
expect_output_contains "sync_batch log verifies clean" "Status: clean"
expect_output_contains "sync_batch log holds every record" "Valid records: 100"

# The two policies must produce identical bytes: they differ only in when the barrier
# is called, never in what is written.
expect_code "sync_each writes the same workload" 0 append --file "$WORKDIR/each.log" --records 100 --payload-bytes 64
if [ "$(file_hash "$WORKDIR/batch.log")" = "$(file_hash "$WORKDIR/each.log")" ]; then
  ok "sync_batch and sync_each produce identical bytes"
else
  bad "sync_batch and sync_each produce identical bytes" "hashes differ"
fi
run append --file "$WORKDIR/each.log2" --records 100 --payload-bytes 64
expect_output_contains "sync_each costs one barrier call per record" "Barrier calls: 100"

expect_code "sync_batch rejects batch-size 1" 10 append --file "$WORKDIR/b1.log" --records 4 --payload-bytes 8 --policy sync_batch --batch-size 1
expect_code "sync_batch rejects an oversized batch-size" 10 append --file "$WORKDIR/b2.log" --records 4 --payload-bytes 8 --policy sync_batch --batch-size 99999
expect_code "an over-large batch buffer is a usage error" 64 append --file "$WORKDIR/b3.log" --records 4 --payload-bytes 1048576 --policy sync_batch --batch-size 4000
expect_code "sync_batch resumes an existing log" 0 append --file "$WORKDIR/batch.log" --records 20 --payload-bytes 64 --policy sync_batch --batch-size 8 --append-existing
run verify --file "$WORKDIR/batch.log"
expect_output_contains "resumed sync_batch log verifies clean" "Valid records: 120"

# --- crash-test (CPP-07 / CPP-AC10 / CPP-AC11, tests CPP-T11 to CPP-T13) -----
#
# The supervisor is its own assertion engine: it reports violations and exits nonzero. So
# these cases do two things. They check that it exits 0, and they check the breakdown it
# prints, because a checkpoint that silently did nothing would otherwise pass. What proves
# the oracle can fail at all is test_crash.cpp, which feeds it broken logs directly.

CRASH="$WORKDIR/crash"

expect_code "crash-test without --workdir is a usage error" 64 crash-test --runs 2
expect_code "crash-test rejects an unknown --scenario" 64 crash-test --runs 2 --workdir "$CRASH" --scenario after_lunch
expect_code "crash-test rejects --runs 0" 64 crash-test --runs 0 --workdir "$CRASH"
expect_code "crash-test rejects a non-numeric --seed" 64 crash-test --runs 2 --seed xyz --workdir "$CRASH"
expect_code "crash-child rejects a malformed scenario spec" 64 crash-child "$CRASH/x.log" "records=1,at=none"
expect_code "crash-child rejects a missing scenario spec" 64 crash-child "$CRASH/x.log"

# An uninterrupted writer must leave exactly what it acknowledged: no extras, no losses.
expect_code "crash-test control runs pass" 0 crash-test --runs 6 --seed 11 --workdir "$CRASH" --scenario none
expect_output_contains "control runs report no violations" "Violations: 0"
expect_output_contains "control runs kill nothing" "Killed: 0"
expect_output_contains "control runs recover exactly what was acknowledged" "Extra unacknowledged: 0"

# Killed before any byte of the unit reached the file: again nothing extra to recover.
expect_code "kill before the write passes" 0 crash-test --runs 6 --seed 11 --workdir "$CRASH" --scenario before_write
expect_output_contains "kill before the write reports no violations" "Violations: 0"
expect_output_contains "kill before the write leaves no extra records" "Extra unacknowledged: 0"
expect_output_contains "kill before the write killed every child" "Killed: 6"

# A controlled partial write leaves a torn trailing record every time. Asserting the count
# is what distinguishes "the tail path was exercised" from "the checkpoint did nothing".
expect_code "kill mid-write passes" 0 crash-test --runs 6 --seed 11 --workdir "$CRASH" --scenario mid_write
expect_output_contains "kill mid-write reports no violations" "Violations: 0"
expect_output_contains "every mid-write run left a recoverable tail" "Incomplete tails recovered: 6"

# CPP-T11: written but not synced, so not acknowledged. Whatever is recovered is extra.
expect_code "kill after the write before the sync passes" 0 crash-test --runs 6 --seed 11 --workdir "$CRASH" --scenario after_write_before_sync
expect_output_contains "kill before the sync reports no violations" "Violations: 0"
expect_output_contains "a completed write leaves no torn record" "Incomplete tails recovered: 0"
expect_output_lacks "kill before the sync recovers records it never acknowledged" "Extra unacknowledged: 0"

# CPP-T12 and CPP-AC11: durable, but the supervisor was never told. The records are
# allowed to come back, and must not be counted as acknowledged.
expect_code "kill after the sync before the ack passes" 0 crash-test --runs 6 --seed 11 --workdir "$CRASH" --scenario after_sync_before_ack
expect_output_contains "kill before the ack reports no violations" "Violations: 0"
expect_output_lacks "kill before the ack recovers an unacknowledged record" "Extra unacknowledged: 0"

# CPP-T13: the acknowledgment was observed, so every record it covered must be present.
expect_code "kill after the ack was observed passes" 0 crash-test --runs 6 --seed 11 --workdir "$CRASH" --scenario after_ack
expect_output_contains "kill after the ack reports no violations" "Violations: 0"
expect_output_contains "kill after the ack loses nothing" "Extra unacknowledged: 0"

# The mixed sweep cycles every checkpoint, so a small --runs still covers all six.
expect_code "the mixed crash-test sweep passes" 0 crash-test --runs 12 --seed 42 --workdir "$CRASH"
expect_output_contains "the sweep reports no violations" "Violations: 0"
expect_output_contains "the sweep exercises every checkpoint" "none 2  before_write 2  mid_write 2  after_write_before_sync 2  after_sync_before_ack 2  after_ack 2"
# A torn record comes from mid_write and from nothing else: every other checkpoint stops
# either before the write starts or after it finished.
expect_output_contains "only the mid-write runs left tails" "Incomplete tails recovered: 2"
expect_output_contains "the sweep names the process-crash model" "not power loss"

# One seed, one matrix: a failing run can be replayed.
run crash-test --runs 12 --seed 42 --workdir "$CRASH"
CRASH_REPEAT="$LAST_OUTPUT"
run crash-test --runs 12 --seed 42 --workdir "$CRASH"
if [ "$CRASH_REPEAT" = "$LAST_OUTPUT" ]; then ok "the same seed produces the same matrix"
else bad "the same seed produces the same matrix" "output differed between runs"; fi

# Fixtures are the harness's own and are cleaned up when every run passes.
if [ -z "$(ls -A "$CRASH")" ]; then ok "passing runs leave no fixtures behind"
else bad "passing runs leave no fixtures behind" "$(ls -A "$CRASH")"; fi

# The harness creates and deletes files, so it refuses a scratch root it cannot trust.
ln -s "$CRASH" "$WORKDIR/crash-link" 2>/dev/null
if [ -L "$WORKDIR/crash-link" ]; then
  expect_code "crash-test refuses a symlinked workdir" 3 crash-test --runs 2 --workdir "$WORKDIR/crash-link"
  expect_output_contains "symlinked workdir refusal says why" "symlink"
fi

# A supervisor that cannot fail proves nothing. Substituting a writer that claims a
# durable acknowledgment for a record it never wrote must produce a reported violation and
# a nonzero exit, end to end.
FAKE_CHILD="$(dirname "$0")/fake_crash_child.sh"
FAKECRASH="$WORKDIR/crash-fake"
expect_code "a falsely acknowledged record is a violation" 1 crash-test --runs 1 --workdir "$FAKECRASH" --child-program "$FAKE_CHILD"
expect_output_contains "the violation names the lost acknowledgment" "lost acknowledged records"
expect_output_contains "the violation reports the count" "Violations: 1"
expect_output_contains "the violation names the scenario to replay" "records="
if [ -n "$(ls -A "$FAKECRASH")" ]; then ok "a failing run keeps its fixtures"
else bad "a failing run keeps its fixtures" "$FAKECRASH is empty"; fi

# The acceptance gate from docs/PLAN.md. Slow, so it runs with the exhaustive sweep.
if [ "${EVENTLOG_SLOW_TESTS:-0}" = "1" ]; then
  expect_code "the 300-scenario acceptance gate passes" 0 crash-test --runs 300 --seed 42 --workdir "$CRASH"
  expect_output_contains "the gate ran 300 scenarios" "Scenarios: 300"
  expect_output_contains "the gate reports no violations" "Violations: 0"
fi

# --- bench (CPP-AC13, tests CPP-T16 and CPP-T17) -----------------------------
#
# No case here asserts a throughput or latency figure: this suite runs under sanitizers,
# where such a number would be meaningless. What is checked is that the report says what
# it measured and disqualifies itself when it cannot be quoted.

BENCH="$WORKDIR/bench"
expect_code "bench without --out is a usage error" 64 bench --records 4
expect_code "bench rejects more trials than the limit" 64 bench --out "$BENCH/r.json" --records 4 --trials 999
expect_code "bench rejects a payload above the format limit" 64 bench --out "$BENCH/r.json" --records 4 --payload-bytes 1048577
# Bounded before a byte is written, so an over-budget request fails immediately.
expect_code "bench refuses an over-budget run" 64 bench --out "$BENCH/r.json" --records 1000000 --payload-bytes 1048576 --trials 5
expect_output_contains "the budget refusal names the limit" "the limit is"

expect_code "bench runs and writes a report" 0 bench --out "$BENCH/results.json" --records 40 --payload-bytes 32 --trials 2
BENCH_OUTPUT="$LAST_OUTPUT"
expect_output_contains "bench prints a comparison table" "vs baseline"
expect_output_contains "bench compares every documented policy" "sync_batch(64)"
expect_output_contains "bench labels the accepted-only policy" "accepted, not durable"
expect_output_contains "bench states the trial count it summarised" "Median of 2 trials"

# This suite runs against both a sanitizer build and a Release one, so the assertion is
# that the table and the JSON agree about publishability -- not that either answer is the
# one this build happens to give.
if grep -q '"publishable": false' "$BENCH/results.json"; then
  case "$BENCH_OUTPUT" in
    *"must not be quoted"*) ok "a disqualified build warns above its own numbers" ;;
    *) bad "a disqualified build warns above its own numbers" "no warning in: $BENCH_OUTPUT" ;;
  esac
else
  case "$BENCH_OUTPUT" in
    *WARNING*) bad "a publishable build prints no warning" "unexpected warning: $BENCH_OUTPUT" ;;
    *) ok "a publishable build prints no warning" ;;
  esac
fi

if grep -q '"publishable"' "$BENCH/results.json" &&
   grep -q '"publishable_note"' "$BENCH/results.json" &&
   grep -q '"compiler"' "$BENCH/results.json" &&
   grep -q '"filesystem"' "$BENCH/results.json" &&
   grep -q '"storage_class": "unknown"' "$BENCH/results.json" &&
   grep -q '"percentile_method"' "$BENCH/results.json" &&
   grep -q '"accepted_not_durable"' "$BENCH/results.json" &&
   grep -q '"durable_ack_latency_applicable": false' "$BENCH/results.json"; then
  ok "the json report carries its environment, units, and durability status"
else
  bad "the json report carries its environment, units, and durability status" "$(head -30 "$BENCH/results.json")"
fi

# A report is a file worth not clobbering by accident.
expect_code "bench refuses to overwrite an existing report" 64 bench --out "$BENCH/results.json" --records 4 --payload-bytes 8 --trials 1
expect_output_contains "the overwrite refusal names the flag" "--overwrite"
expect_code "bench --overwrite replaces a report" 0 bench --out "$BENCH/results.json" --records 4 --payload-bytes 8 --trials 1 --overwrite

ln -s "$BENCH/results.json" "$BENCH/report-link.json" 2>/dev/null
if [ -L "$BENCH/report-link.json" ]; then
  expect_code "bench refuses to write through a symlink" 10 bench --out "$BENCH/report-link.json" --records 4 --payload-bytes 8 --trials 1
  expect_output_contains "the symlink refusal says why" "symlink"
fi

# One trial's file at a time, and none left over.
if [ -z "$(find "$BENCH" -maxdepth 1 -name 'bench-*' -print -quit)" ]; then
  ok "bench leaves no scratch fixtures behind"
else
  bad "bench leaves no scratch fixtures behind" "$(ls -A "$BENCH")"
fi

# --- edge cases --------------------------------------------------------------
expect_code "zero records succeeds" 0 append --file "$WORKDIR/zero.log" --records 0 --payload-bytes 8
expect_code "zero-length payloads succeed" 0 append --file "$WORKDIR/empty-payload.log" --records 4 --payload-bytes 0
run verify --file "$WORKDIR/empty-payload.log"
expect_output_contains "header-only records verify clean" "Status: clean"
expect_code "verify of a missing file fails" 10 verify --file "$WORKDIR/nope.log"
expect_code "max payload size is accepted" 0 append --file "$WORKDIR/big.log" --records 1 --payload-bytes 1048576

echo "[suite] cli_tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
