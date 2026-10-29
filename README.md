# eventlog — a crash-safe append-only event log

A small C++20 append-only binary log that survives interrupted writes according to an **explicit
recovery contract**, built to answer one question honestly: *which records can you prove were
durable before the process died?*

This is not a database, a queue, or a distributed log. It is one local file, one writer, a
checksummed record format, and a recovery path that refuses to guess.

**Status:** milestone M1 complete — record format, `sync_each` durable writer, log verification, and
incomplete-tail recovery. Batching, the process-crash harness, and benchmarks are specified and not
yet built; see [docs/PLAN.md](docs/PLAN.md). No performance numbers are claimed anywhere in this
repository, because none have been measured yet.

## Build

**Linux is the supported platform** — it is the one whose `fdatasync(2)` contract this project's
guarantees are written against ([ADR-004](docs/decisions/ADR-004-linux-is-the-supported-platform.md)).
Requires CMake 3.20+ and a C++20 compiler. No third-party dependencies.

```sh
./scripts/verify.sh        # format check, -Werror build, ASan+UBSan, all tests
./scripts/verify-full.sh   # the above plus the exhaustive recovery sweep and a Release build
./scripts/verify-linux.sh  # verify-full.sh inside a Linux container — the result that counts
```

`verify-linux.sh` mounts this repository read-only and builds in a container-local directory, so it
cannot modify your working tree. It needs a running Docker daemon.

macOS builds and passes as a convenience for the local edit loop, but it is not a supported target:
no guarantee is stated for it and no timing from it is ever published.

The binary lands at `build/debug-asan/eventlog` (or `build/release/eventlog`).

## Use

Real output from a Release build on Linux (Debian 12, aarch64, ext4), captured by
`scripts/verify-linux.sh`'s toolchain. File sizes are deterministic, so yours will match.

```console
$ eventlog append --file demo/events.log --records 1000 --payload-bytes 128
Records appended: 1000  Durable acknowledged: 1000
Policy: sync_each  Durability barrier: fdatasync  File size: 152000 bytes

$ eventlog verify --file demo/events.log
Valid records: 1000   Last sequence: 1000   Status: clean
```

Now simulate an interrupted trailing write by cutting 40 bytes off the end of the file:

```console
$ eventlog verify --file demo/events.log
Valid records: 999   Last sequence: 999   Status: incomplete_tail
Detail: trailing partial payload for sequence 1000: 88 of 128 bytes (offset 151848)
Recoverable: 112 trailing bytes form a partial record.
Run: eventlog recover --file demo/events.log --truncate-tail
```

`verify` is read-only and exited `1`. Recovery requires a second, explicit step, and tells you what
it would destroy before it does anything:

```console
$ eventlog recover --file demo/events.log
Status: incomplete_tail   Valid records: 999   Last sequence: 999
Detail: trailing partial payload for sequence 1000: 88 of 128 bytes (offset 151848)
Would discard 112 bytes: offsets 151848..151960 (one structurally incomplete trailing record).
No change made. Re-run with --truncate-tail to apply.

$ eventlog recover --file demo/events.log --truncate-tail
Status: incomplete_tail   Valid records: 999   Last sequence: 999
Detail: trailing partial payload for sequence 1000: 88 of 128 bytes (offset 151848)
Would discard 112 bytes: offsets 151848..151960 (one structurally incomplete trailing record).
Status: recovered_incomplete_tail   Discarded: 112 bytes   Valid records: 999   Last sequence: 999

$ eventlog verify --file demo/events.log
Valid records: 999   Last sequence: 999   Status: clean
```

Note that 112 bytes were discarded, not the 40 that were cut: the whole partial record goes, because
a record is the unit of validity.

### Exit codes

| Code | Meaning |
|---:|---|
| 0 | clean, or recovery applied |
| 1 | `verify`/`recover`: structurally incomplete trailing record (recoverable) |
| 2 | hard corruption, or an unsupported format version — nothing was modified |
| 10 | I/O or contract error |
| 64 | invalid arguments |

## What is actually guaranteed

This is the part most storage write-ups get wrong, so it is stated plainly.

**The acknowledgment contract.** A record is reported durable *only* after the platform's durability
barrier returns success for it. Until then it is `accepted`, which means the bytes were handed to
`write(2)` and nothing more. The two counters are separate in the API, in the CLI output, and in the
tests.

**The barrier is `fdatasync(2)`** on the supported platform. All durability goes through a single
`FileOps::SyncData` boundary ([ADR-001](docs/decisions/ADR-001-durability-primitive.md)), which maps
to `fdatasync(2)` on Linux, `fcntl(F_FULLFSYNC)` on macOS, and `fsync(2)` on other POSIX systems.
Only the Linux mapping is a supported guarantee, and only Linux numbers will ever be published, so
there is no cross-platform comparison to mistrust.

**Resuming a log re-establishes durability rather than assuming it.** Opening an existing log scans
it, which proves its records are present and complete — not that any of them was ever synced. So
`Open` issues one barrier call before reporting the existing prefix as durable. A writer killed
between `write(2)` and its sync leaves records that a scan cannot distinguish from synced ones, and
inheriting that claim would be exactly the conflation this project exists to avoid.

**A failed sync ends the writer session** ([ADR-003](docs/decisions/ADR-003-sync-failure-is-terminal.md)).
On Linux a failed `fsync` can discard the dirty page and report the error only once, so retrying
could return success over permanently lost data. The writer refuses to acknowledge anything after
that point rather than manufacture a false guarantee.

**Recovery truncates one specific thing** ([ADR-002](docs/decisions/ADR-002-recovery-contract.md)):
a trailing record that is a strict byte-prefix, and only when you ask with `--truncate-tail`. A
complete record with a bad checksum, a full-but-invalid header, or a sequence gap is reported with
its byte offset and left untouched — including when it is the last record in the file.

### What is *not* guaranteed

- **A process crash is not a power failure.** `SIGKILL` stops a process; it does not clear the
  kernel page cache or cut power to a drive mid-write. Nothing here has been tested against real
  power loss, and the upcoming crash harness will not change that.
- **Hardware that lies is out of scope.** Drives that acknowledge a cache flush without performing
  one, filesystem bugs, and media failure are outside what any of this proves.
- **CRC32C is not authentication.** It detects accidental corruption. A deliberately crafted file
  can carry a valid checksum.
- **Cooperating writers only.** The advisory `flock` excludes other processes that also take the
  lock. A process that ignores it is not prevented from writing.
- **No live-reader consistency claim.** Reading a log while a writer is appending is not a supported
  mode in this milestone.

The honest form of the claim this project aims to support is: *zero lost externally acknowledged
durable records, in the tested process-crash and injected-write-fault scenarios.* The crash harness
that will test it is milestone M3 and does not exist yet, so that claim is **not yet substantiated**.

## Record format v1

Frozen. 24-byte header, all integers explicit little-endian, no struct packing or host endianness
anywhere in the serialization path.

| Offset | Width | Field |
|---:|---:|---|
| 0 | 4 | magic `ELOG` |
| 4 | 2 | version (`1`) |
| 6 | 2 | flags (`0`; any set bit is rejected) |
| 8 | 8 | sequence (from 1, step exactly 1) |
| 16 | 4 | payload length (0 … 1,048,576) |
| 20 | 4 | CRC32C |
| 24 | … | payload |

CRC32C is deliberately the last header field, so the checksummed range is the contiguous header
prefix `[0, 20)` followed by the payload. The checksum is verified against published CRC-32C vectors
(canonical check value `0xE3069283`), not merely against itself, and the codec is pinned by
fixed golden byte vectors generated by an independent implementation.

Payload length is validated against both the 1 MiB bound and the bytes actually remaining in the
file *before* any buffer is allocated from it.

### Deterministic payloads

`append` generates payloads from a SplitMix64 stream keyed by sequence number and stored
little-endian, so `(sequence, size)` yields identical bytes on any platform and a shorter payload is
a prefix of a longer one. This makes appends reproducible and lets a verifier regenerate the expected
bytes for any record.

## Layout

```
include/eventlog/   public headers (error, record, crc32c, file_ops, writer, reader, recover)
src/                implementation; file_ops.cpp is the only POSIX boundary
tests/              6 C++ suites plus a CLI integration suite (138 cases)
docs/PRD.md         the product contract
docs/PLAN.md        milestones, with the verification command for each
docs/STATUS.md      current state and next action
docs/decisions/     ADRs for the three decisions that fix a guarantee
```

`FileOps` is the one extracted interface, and it exists because the fault tests have to produce
short writes, `EINTR`, `EIO`, and a failed sync without depending on OS timing. Normal runtime uses
`PosixFileOps`, which does nothing but call the syscalls.

## Testing

```
test_scaffold  error/result types, syscall context in messages, scratch-dir discipline
test_crc32c    published CRC-32C vectors, chaining, all 2048 single-bit flips of a 256-byte block
test_codec     golden byte vectors, round-trips at 0/1/128/1 MiB, every rejection path
test_writer    short writes, EINTR retry, EIO, failed sync, lock contention, policy guards
test_reader    1000-record round-trip byte-for-byte, scan report accuracy
test_recovery  every byte offset of a trailing record, mid-log corruption, sequence violations
cli_tests      exit codes, argument bounds, read-only verify, destructive-operation guards
```

The recovery suite cuts a trailing record at **every** byte offset and checks that each cut is
classified, planned, and recovered identically: 4119 complete recover cycles for a 4096-byte
record in the slow sweep, plus every offset of a 32-byte and a header-only record in the fast one. Tests
write only inside per-test scratch directories that are removed even when a case fails.
