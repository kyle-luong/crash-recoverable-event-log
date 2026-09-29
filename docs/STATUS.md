# Current state

Milestone: M1 in progress | Active task: **M1 CPP-04 (recovery) not started**
Branch: `feat/CPP-01-04-log-core`
Last known good commit: the writer/reader commit on this branch
Status: in progress

## Done

CPP-01 codec (previous commit), plus CPP-02 writer and CPP-03 reader. These two landed together
because they are mutually dependent: the writer scans an existing log to resume its sequence, and
the round-trip test for CPP-T06 needs both halves.

`sync_each` writer with an owning file descriptor, a short-write loop that retries only `EINTR` and
surfaces everything else with its byte offset, `O_APPEND` so a bookkeeping bug cannot overwrite
committed data, an advisory `flock` for cooperating writers, and a directory fsync when the file is
created so the first acknowledgment cannot outlive the file name.

The acknowledgment boundary is enforced in one place: `durable_through_sequence` advances only after
the durability barrier returns success. `accepted_through_sequence` tracks bytes handed to
`write(2)` and is never conflated with it. A failed write or sync is terminal for the writer
(ADR-003) rather than retried.

Log scanner reporting `clean` / `incomplete_tail` / `corrupt_complete_record` /
`sequence_violation` with a byte offset and a readable detail, plus a sequential `LogReader` for
reading payloads back. The scanner is the authority a writer consults before appending to an
existing log, and it refuses to append to anything that is not clean.

## Evidence

`./scripts/verify.sh` PASS on 2026-09-29, macOS 15 / arm64, Apple clang 17, APFS. 55 cases under
ASan + UBSan.

`test_writer` drives the writer through a `ScriptedFileOps` double: writes capped at 7 bytes to
force the short-write loop over 22 calls per record, injected `EINTR` rounds, `EIO`, and a failed
sync. The assertions are about what was *not* acknowledged. One case tears a record mid-write and
confirms the result is a recoverable incomplete tail with the valid prefix intact.

That double is the alternate `FileOps` implementation that justifies the interface existing, so the
boundary is not a speculative abstraction.

## Not done

Recovery and the CLI (rest of M1). Batching and CLI-driven fault injection (M2), crash harness
(M3), benchmarks (M4).

A damaged log can be *detected* but not yet repaired, and there is no way to do any of this from a
command line. `sync_batch` and `write_only` are rejected at `Open` so no path can report an
unimplemented policy as durable.

## Known issues and limits

Scanning costs two `pread` calls per record. Correct, and fine at these sizes, but it should be
measured before M4 quotes any verification throughput.

## Next action

Implement M1 CPP-04: a read-only recovery preflight that reports exactly which byte range an
incomplete tail occupies, and an opt-in truncation that removes only that range, syncs the
shortened length, and refuses to act through a symlink or on hard corruption.
