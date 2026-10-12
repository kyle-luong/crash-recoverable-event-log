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

# --- unimplemented commands are honest about it -----------------------------
expect_code "crash-test reports it is not implemented" 64 crash-test --runs 1
expect_output_contains "crash-test names its milestone" "M3"
expect_code "bench reports it is not implemented" 64 bench --records 1
expect_output_contains "bench names its milestone" "M4"
expect_code "sync_batch policy is rejected" 10 append --file "$WORKDIR/b.log" --records 1 --payload-bytes 8 --policy sync_batch
expect_code "unknown policy is a usage error" 64 append --file "$WORKDIR/c.log" --records 1 --payload-bytes 8 --policy turbo

# --- edge cases --------------------------------------------------------------
expect_code "zero records succeeds" 0 append --file "$WORKDIR/zero.log" --records 0 --payload-bytes 8
expect_code "zero-length payloads succeed" 0 append --file "$WORKDIR/empty-payload.log" --records 4 --payload-bytes 0
run verify --file "$WORKDIR/empty-payload.log"
expect_output_contains "header-only records verify clean" "Status: clean"
expect_code "verify of a missing file fails" 10 verify --file "$WORKDIR/nope.log"
expect_code "max payload size is accepted" 0 append --file "$WORKDIR/big.log" --records 1 --payload-bytes 1048576

echo "[suite] cli_tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
