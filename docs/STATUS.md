# Current state

Milestone: M1 in progress | Active task: **milestone writeup, then M2 CPP-05**
Branch: `feat/CPP-01-04-log-core`
Last known good commit: the recovery/CLI commit on this branch
Status: verifying

## Done

CPP-04 recovery and the `append` / `verify` / `recover` CLI, completing the M1 feature set.

Recovery is two separate operations. The preflight is read-only, reports the exact byte range an
incomplete tail occupies, and changes nothing. Applying it truncates only that range, then syncs
the shortened length so a crash cannot resurrect the discarded tail. It refuses to act through a
symlink, takes the writer lock so it cannot truncate underneath a live writer, and re-checks the
file against the preflight so a stale plan cannot discard valid records.

Hard corruption is never truncated, including when the damaged record is last. Mid-log damage is
reported with its offset and left alone, because truncating there would silently discard the valid
records that follow it.

CLI with the PRD's exit codes: 0 clean, 1 incomplete tail, 2 hard corruption, 64 invalid arguments.
`verify` never opens the log for writing. `recover` prints what it would destroy before it destroys
anything. `append` refuses a nonempty file without an explicit flag, and `--overwrite` refuses to
act through a symlink.

## Evidence

`./scripts/verify.sh` PASS and `./scripts/verify-full.sh` PASS on 2026-10-12, macOS 15 / arm64,
Apple clang 17, APFS. 138 cases across 7 suites under ASan + UBSan.

`test_recovery` cuts a trailing record at **every** byte offset and asserts each cut is classified,
planned, and recovered identically, that surviving bytes are untouched, and that the result is a
log a writer will accept again. The slow sweep does this for a 4096-byte record: 4120 complete
scan / plan / recover / re-verify cycles.

Three scenarios assert the read-only guarantee by byte-comparing the file before and after repeated
`verify` and preflight runs. `cli_tests.sh` does the same with a SHA-256 of the file.

## Not done

Batching and CLI-driven fault injection (M2), the process-crash harness (M3), benchmarks (M4).

**The acknowledged-prefix invariant is argued but not yet demonstrated**: that needs the M3
supervisor. No throughput or latency figure appears anywhere in this repository.

## Known issues and limits

Scanning costs two `pread` calls per record; measure before M4 quotes verification throughput.

## Next action

Write up the milestone, then implement M2 CPP-05: `append_batch_and_sync` writing N records and
syncing once, acknowledging the batch only as a whole, reusing the ADR-003 terminal-failure path.
