# eventlog — a crash-safe append-only event log

A small C++20 append-only binary log that survives interrupted writes according to an **explicit
recovery contract**, built to answer one question honestly: *which records can you prove were
durable before the process died?*

This is not a database, a queue, or a distributed log. It is one local file, one writer, a
checksummed record format, and a recovery path that refuses to guess.

**Status: feature-complete.** All five milestones in [docs/PLAN.md](docs/PLAN.md) are done and all
16 acceptance criteria have named test evidence — the record format, the `sync_each` and
`sync_batch` writers, log verification and incomplete-tail recovery, fault injection over the
syscall boundary in both directions, the process-crash harness that kills a writer at instrumented
checkpoints and checks the recovered log against independently observed acknowledgments, and a
Release benchmark comparing the durable policies against a non-durable baseline.

Verified by `./scripts/verify-linux.sh` on Linux — 10 suites, 290 cases, three phases (fast,
exhaustive, Release), 0 failures — and independently by GitHub Actions on a clean x86_64 runner.
Measured performance figures, with the machine that produced them, are in
[benchmarks/RESULTS.md](benchmarks/RESULTS.md); every number in this repository comes from a run
recorded there, and a report built with sanitizers or off the supported platform marks itself
unpublishable rather than looking like a result.

Nothing further is planned. The remaining ideas in PRD §8 — threads, vectored I/O, group commit,
`mmap`, compaction, replication — are deliberately not built, and should wait for a measurement or
a real use case to ask for them.

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

Every block below is **real output**, re-captured from a Release build on Linux (Debian 12,
aarch64, ext4) using `scripts/verify-linux.sh`'s toolchain, and checked line by line against this
file. Nothing here is illustrative. File sizes and offsets are deterministic, so yours will match
exactly; throughput figures are the one thing that will not, which is why they live in
[benchmarks/RESULTS.md](benchmarks/RESULTS.md) with the machine that produced them.

```console
$ eventlog append --file demo/events.log --records 1000 --payload-bytes 128
Records appended: 1000  Durable acknowledged: 1000
Policy: sync_each  Durability barrier: fdatasync  File size: 152000 bytes
Acknowledgments: 1000  Barrier calls: 1000  Write calls: 1000

$ eventlog verify --file demo/events.log
Valid records: 1000   Last sequence: 1000   Status: clean
```

The same workload under `sync_batch`, which writes and acknowledges 32 records at a time:

```console
$ eventlog append --file demo/batched.log --records 1000 --payload-bytes 128 \
      --policy sync_batch --batch-size 32
Records appended: 1000  Durable acknowledged: 1000
Policy: sync_batch(32)  Durability barrier: fdatasync  File size: 152000 bytes
Acknowledgments: 32  Barrier calls: 32  Write calls: 32
```

Same bytes on disk, 32 barrier calls instead of 1000. The last batch holds 8 records, not 32, and
is acknowledged exactly like a full one.

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

Prove the acknowledgment contract against actual process death:

```console
$ eventlog crash-test --runs 300 --seed 42 --workdir ./tmp-crash-tests
Scenarios: 300   Violations: 0   Incomplete tails recovered: 50
Checkpoints:  none 50  before_write 50  mid_write 50  after_write_before_sync 50  after_sync_before_ack 50  after_ack 50
Killed: 250   Acknowledged records: 1029   Recovered records: 1290   Extra unacknowledged: 261
Seed: 42   Durability barrier: fdatasync   Model: process kill, not power loss
```

Measured output from a Linux run, not a sample. Each scenario spawns a writer, stops it with
`SIGKILL` at one instrumented point, recovers the log, and checks it against the acknowledgments
this supervisor saw. *Extra unacknowledged* is the interesting column: 261 valid records came back
that nobody was ever told were durable. That is correct and expected — the invariant is
one-directional. The reverse, a single acknowledged record missing from a recovered log, is a
violation and fails the run.

Compare what durability costs:

```console
$ eventlog bench --records 10000 --payload-bytes 64 --trials 5 --out out/results-64.json
Policy          records/sec       ack p50/p95/p99 (us)        vs baseline
sync_each       934.9             1254.0 / 2963.6 / 3495.9    1.00x
sync_batch(16)  13466.5           1361.2 / 3105.9 / 5163.7    14.40x
sync_batch(64)  32808.7           1509.6 / 3421.6 / 4773.1    35.09x
write_only      2598153.0         0.4 / 0.4 / 0.7 *           2779.06x
* accepted, not durable: time to accept a record. This policy never calls the barrier,
  so durable-ack latency does not apply to it.
Median of 5 trials, 10000 records of 64 payload bytes each. Barrier: fdatasync. Build: Release.
```

A real Release run on Linux, reproduced from `benchmarks/results-64.json`. Read
[benchmarks/RESULTS.md](benchmarks/RESULTS.md) before taking any of those multipliers seriously:
the durable policies varied by up to 4.3× between trials on that machine, which is a virtual one.
A build with sanitizers on, or off Linux, prints a `WARNING` banner above the table and stamps
`"publishable": false` into the JSON instead of looking like this.

### Exit codes

| Code | Meaning |
|---:|---|
| 0 | clean, or recovery applied |
| 1 | `verify`/`recover`: structurally incomplete trailing record (recoverable) |
| 2 | hard corruption, or an unsupported format version — nothing was modified |
| 10 | I/O or contract error |
| 64 | invalid arguments |

`crash-test` uses its own small set, because "what went wrong" matters more there than where:

| Code | Meaning |
|---:|---|
| 0 | every supported invariant held |
| 1 | a correctness violation, reported with the scenario that produced it |
| 3 | the harness itself could not run |

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

**A batch is acknowledged as a unit or not at all**
([ADR-006](docs/decisions/ADR-006-batch-acknowledgment-boundary.md)). `sync_batch` writes N records
as one `write(2)`, calls the barrier once, and only then acknowledges all N. If either call fails,
no record in the batch is acknowledged. Records from a failed batch may still be on disk, complete
and valid — that is permitted, because the invariant runs one way only: every acknowledged record
must be recoverable, never the reverse.

Batches are explicit calls rather than hidden buffering, so the acknowledgment boundary is always a
call boundary. Nothing is left accepted-but-unsynced between calls, which means `Close()` has no
partial batch to resolve and the boundary never depends on how the process exits.

**What batching costs you.** A larger batch is more unacknowledged work in flight to lose if the
process dies. What it buys is syscalls, and that part is checked by assertion rather than asserted
in prose: for 32 records, `sync_batch(32)` performs exactly one write and one barrier call where
`sync_each` performs thirty-two of each, and both produce byte-identical files.

Whether fewer barrier calls translate into proportionally more throughput is a measurement, not a
deduction — and measured, they do not. On the machine recorded in
[benchmarks/RESULTS.md](benchmarks/RESULTS.md), batching bought between 9× and 85× the `sync_each`
throughput, which is large but short of the batch size: amortising a barrier is not removing it.
Per-acknowledgment latency did not improve and often worsened, because one acknowledgment now
covers N records. Those runs also varied by up to 4.3× between trials of the same workload, so they
bound an order of magnitude rather than pin a multiplier, and the results file says so before it
shows a number.

**`write_only` is the floor, not a policy.** It never calls the barrier, reports
`Accepted (NOT durable)`, and carries no durable sequence in its result type at all
([ADR-008](docs/decisions/ADR-008-write-only-and-benchmark-honesty.md)). It exists to measure what
durability costs and is excluded from the acknowledged-prefix claim by construction — the crash
harness refuses to run it. Do not store anything with it.

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
  power loss, and the crash harness does not change that — it kills processes, which is a strictly
  weaker fault than losing power.
- **Hardware that lies is out of scope.** Drives that acknowledge a cache flush without performing
  one, filesystem bugs, and media failure are outside what any of this proves.
- **CRC32C is not authentication.** It detects accidental corruption. A deliberately crafted file
  can carry a valid checksum.
- **Cooperating writers only.** The advisory `flock` excludes other processes that also take the
  lock. A process that ignores it is not prevented from writing.
- **No live-reader consistency claim.** Reading a log while a writer is appending is not a supported
  mode in this milestone.

The honest form of the claim this project supports is: *zero lost externally acknowledged durable
records, in the tested process-crash and injected-write-fault scenarios.* That is now demonstrated
rather than argued. `eventlog crash-test` spawns a writer, stops it with `SIGKILL` at one of six
instrumented points, recovers the log, and checks it against the acknowledgments the supervisor
recorded in its own memory before issuing the kill — 300 scenarios per acceptance run, every
checkpoint exercised ([ADR-007](docs/decisions/ADR-007-crash-supervisor-design.md)). The scope of
the claim is exactly the words above: process crashes and injected write faults, under ordinary
Linux filesystem semantics, and nothing wider.

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
include/eventlog/   public headers: the format (record, crc32c, endian), the log
                    (writer, reader, recover, payload), the POSIX boundary
                    (file_ops, scratch_dir), the tools (crash_test, benchmark, cli)
src/                implementation. All of the log's own I/O goes through
                    file_ops.cpp; the crash harness also uses fork/exec/kill and the
                    benchmark reads the environment, neither of which touch a log
tests/              9 C++ suites plus a CLI integration suite
benchmarks/         the method, and the measured results it governs
docs/PRD.md         the product contract
docs/PLAN.md        milestones, with the verification command for each
docs/STATUS.md      current state and next action
docs/decisions/     ADRs for the decisions that fix a guarantee or the workflow
```

`FileOps` is the one extracted interface, and it exists because the fault tests have to produce
short writes, `EINTR`, `EIO`, a failed sync and a failed read without depending on OS timing. Normal
runtime uses `PosixFileOps`, which does nothing but call the syscalls. Everything that writes or
reads a log goes through it, including the scanner — so "the disk failed" and "the data is corrupt"
are different verdicts with a test each, rather than two names for whichever one the code reached
first.

## Testing

```
test_scaffold  error/result types, syscall context in messages, scratch-dir discipline
test_crc32c    published CRC-32C vectors, chaining, all 2048 single-bit flips of a 256-byte block
test_codec     golden byte vectors, round-trips at 0/1/128/1 MiB, every rejection path
test_writer    short writes, EINTR retry, EIO, failed sync, lock contention, policy
               guards, and write_only's accepted-never-durable contract
test_batch     whole-batch acknowledgment, failed batch write and sync, syscall counts
test_reader    1000-record round-trip byte-for-byte, scan report accuracy, injected
               read faults, and the refusal of anything that is not a regular file
test_recovery  every byte offset of a trailing record, mid-log corruption, sequence violations
test_crash     the acknowledged-prefix oracle, against logs damaged on purpose
test_bench     percentiles and spread, per-policy barrier counts, equal workloads,
               bounds, and the stamp that disqualifies an unpublishable report
cli_tests      exit codes, argument bounds, read-only verify, destructive-operation
               guards, the kill matrix at all six crash-test checkpoints, and the
               contents of a benchmark report
```

The recovery suite cuts a trailing record at **every** byte offset and checks that each cut is
classified, planned, and recovered identically: 4119 complete recover cycles for a 4096-byte
record in the slow sweep, plus every offset of a 32-byte and a header-only record in the fast one. Tests
write only inside per-test scratch directories that are removed even when a case fails.

`test_crash` exists because a supervisor that reports `Violations: 0` is worth nothing unless its
check can fail. It feeds the oracle logs that break each invariant on purpose — a missing
acknowledged record, a flipped byte in a complete record, a sequence gap, a payload that is valid
but is not the one that was written, a torn tail left by a writer that exited normally — and
requires it to say so. The path from a detected violation to a nonzero exit is covered separately,
by running `crash-test` against a stand-in child that claims an acknowledgment it never earned.
