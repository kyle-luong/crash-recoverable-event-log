# Product Requirements Document 03 — Crash Consistency Testing Tool: Crash-Safe Event Log & Performance Benchmark

**Language/platform:** C++20 / Linux (POSIX file APIs)  
**Type:** Compact storage component with crash/fault testing and performance comparisons  
**Status:** Implementation-ready specification; no code is assumed to exist  
**Estimated solo MVP:** 3–5 focused weeks with agent support; deeper testing may extend timeline  
**Priority:** Demonstrate C++ memory/resource ownership, persistent storage invariants, fault handling, and measurable throughput/latency tradeoffs  
**Shared agent workflow:** [`00_SHARED_AGENTIC_ENGINEERING_WORKFLOW.md`](00_SHARED_AGENTIC_ENGINEERING_WORKFLOW.md)

## 1. Product brief

**One-sentence pitch:** Implement a small append-only binary event log that survives interrupted writes according to an explicit recovery contract, then quantify the cost of stronger durability versus buffered/batched writes.

This is **not** a toy database with SQL, a distributed log service, or a generic key-value store. Its distinctive engineering question is: **How much throughput can batching recover while preserving precise guarantees about which records were acknowledged durable before a process crash?** The project includes a testing tool that systematically interrupts writes, corrupts controlled bytes, and checks recovered state against a supervisor-observed acknowledgment history.

**Primary users:** Students and engineers interested in storage systems, databases, infrastructure, quant systems, and C++ performance. The CLI demonstrates correctness and benchmarking without a server or network dependency. The library is written for one local process, one writer, and one log file at a time.

**Core demonstration:** Write records using `sync_each` and `sync_batch` policies. Inject a short write or terminate the writer after one of several explicit checkpoints. Restart, recover a maximal valid prefix or reject a fully formed corrupt record, and show that every **externally observed durability acknowledgment** is represented in recovered state. Benchmark the two durable policies on equal workloads and show real measured records/sec and p50/p95/p99 acknowledgment latency.

### Success criteria

- Record format is versioned, endian-stable, checksummed, bounded, and documented.
- Append path correctly handles short writes, `EINTR`, errors, and an explicit single-writer contract.
- A record is **acknowledged durable only after the relevant `fdatasync` completes successfully**. Batch acknowledgment must wait for the batch's `fdatasync`.
- Recovery reads exactly the valid prefix, truncates only a structurally incomplete trailing record, and rejects a complete record with checksum failure or a sequence gap; never silently accepts arbitrary corruption.
- A controlled supervisor can kill the writer at known checkpoints and independently track observed acknowledgments.
- Crash tests prove **all acknowledged sequence numbers are a prefix of recovered sequence numbers** for the supported process-crash/fault-injection model. Extra valid unacknowledged records are allowed.
- Performance comparison includes repeatable workload definitions, Release builds, multiple trials, and an honest baseline without making unmeasured speedup promises.
- Sanitizers and CI pass; the code is small enough for another engineer to understand without a giant framework.

### Important guarantee boundaries

- **Process crash ≠ power failure.** `SIGKILL` terminates a process but does not clear the kernel page cache or simulate sudden device power loss.
- `fdatasync` successful completion is the contract used to mark records durable **under ordinary Linux filesystem/storage semantics**; hardware faults, lying drives, filesystem bugs, and power-loss durability are outside the MVP's proof.
- Write may succeed while `fdatasync` fails; such writes are **not acknowledged** and their later persistence is uncertain. Stop further writes for that session and surface the error.
- Recovered log may include valid records that were written but never acknowledged before termination. **Never require exact equality between recovered records and acknowledged records.** Require acknowledged records to be a prefix of recovered state.
- Do not claim “zero lost data” without specifying the scope: the supported claim is “zero lost *externally acknowledged durable records in the tested process-crash and injected-write-fault scenarios*.”

## 2. Functional scope and exclusions

**MVP:** One local append-only log file; single writer; record encoding/decoding; checksums; `sync_each` and `sync_batch` policies; bounded payloads; advisory lock for cooperating writers; reader and recovery/truncation; injected short-write / syscall-failure behavior; controlled process termination; trace/result output; reliable benchmark suite; CLI; tests.

**Non-goals:** concurrent writers; cross-process transactions; replication; distributed consensus; arbitrary random-access updates; indexes; SQL; MVCC; compression; log compaction; `mmap` or `io_uring` as a required optimization; power-fail simulation; raw block-device testing; persistent queue semantics; security against malicious data; custom allocators without measured need; GUI/cloud.

**Optional diagnostic-only unsafe policy:** benchmark `write_only` without sync to show raw throughput, but mark its returned result as **accepted, not durable**. It must not expose the same durability acknowledgment status as synchronized policies, and MUST NEVER be included in the acknowledged-prefix correctness claim.

## 3. CLI and library contracts

Binary name: **`eventlog`**. Keep the public CLI intentionally compact:

```console
$ eventlog append --file ./data/events.log --records 10000 \
    --payload-bytes 128 --policy sync_batch --batch-size 32
Records appended: 10000  Durable acknowledged: 10000

$ eventlog verify --file ./data/events.log
Valid records: 10000   Last sequence: 10000   Status: clean

$ eventlog crash-test --runs 300 --seed 42 --workdir ./tmp-crash-tests
Scenarios: 300   Violations: 0   Incomplete tails recovered: 14

$ eventlog bench --payload-bytes 128 --records 20000 --trials 5 --out bench/results.json
Policy          records/sec       ack p50/p95/p99 (µs)
sync_each       [measured]        [measured]
sync_batch(32)  [measured]        [measured]
```

All output above is **illustrative**; never use placeholder outputs on a resume as achieved measurements. Benchmarks use temporary files and explicit cleanup. Refuse to overwrite an existing nonempty file unless `--append-existing` or `--overwrite` is explicitly supplied. The destructive operation `recover --truncate-tail` must require a specified file and produce a preflight finding before changing the file; for test fixtures, the harness uses a designated scratch root.

### CLI operations

| Command | Required behavior | Exit semantics |
|---|---|---|
| `append --file --records --payload-bytes --policy [--batch-size]` | Append synthetic deterministic records, report acknowledged count | `0` success, nonzero on I/O/contract error |
| `verify --file [--json PATH]` | Read only; validate framing, checksums, sequence | `0` clean; `1` incomplete trailing record; `2` hard corruption; `64` invalid args |
| `recover --file [--truncate-tail]` | Report structural incomplete tail; truncate only when explicitly requested | `0` clean/recovered; `2` unrecoverable full corruption; nonzero on I/O |
| `crash-test --runs --seed --workdir` | Write/kill/restart/check invariants using controlled local fixtures | `0` all supported invariants pass; `1` correctness failure; `3` harness failure |
| `bench --records --payload-bytes --trials --out` | Run comparable Release benchmarks in fresh files | `0` complete; nonzero on invalid results/IO |

**Fixed input bounds for v1:** record payload 0–1 MiB; record count and fixture storage bounded by CLI limits and available disk; refuse unreasonable allocations; sequence numbers start at 1 and increase by exactly 1. File formats must not rely on host struct packing, native alignment, or native endianness.

### Record layout v1 — normative binary format

Use fixed little-endian fields serialized explicitly:

| Field | Width | Semantics |
|---|---:|---|
| Magic | 4 bytes | ASCII `ELOG` |
| Version | uint16 | `1` |
| Flags | uint16 | `0` in MVP; unknown flag bits rejected |
| Sequence | uint64 | Monotonically increasing from 1 |
| Payload length | uint32 | 0–1,048,576 bytes |
| CRC32C | uint32 | Checksum over serialized magic/version/flags/sequence/length **plus payload**, excluding CRC field |
| Payload | variable | Arbitrary bytes, length defined above |

Header size: **24 bytes** (4+2+2+8+4+4). All numeric fields are little-endian; version and flags are checked *before* allocating payload. Reader must reject arithmetic overflow, impossible length, unknown version, invalid sequence, and corrupted checksum. CRC32C catches accidental corruption but is **not cryptographic authentication**. Include a known public CRC32C test vector, plus your own serialize/deserialize round-trip tests. The format becomes immutable in v1 once accepted; incompatible changes require a new version and migration decision.

**Record framing rule:** a completed record contains all 24 header bytes and exactly payload length bytes. If EOF occurs during the trailing header or payload, `verify` reports an incomplete tail. Recovery may truncate to the offset immediately after the last valid record, **but only by explicit `recover --truncate-tail` or harness-owned recovery**. A complete record whose CRC fails is a hard corruption error, including if it is last; do not silently discard it because it is inconvenient. A sequence gap or duplicate is also hard corruption.

### Writer API and acknowledgment contract

Suggested internal API (names illustrative; change only with documented reason):

```cpp
struct Record { std::uint64_t sequence; std::vector<std::byte> payload; };
struct AppendResult { std::uint64_t durable_through_sequence; };

class EventLogWriter {
public:
    static Result<EventLogWriter> Open(const std::filesystem::path& file);
    Result<AppendResult> append_and_sync(std::span<const std::byte> payload);
    Result<AppendResult> append_batch_and_sync(
        std::span<const std::span<const std::byte>> payloads);
    // No public 'durable' acknowledgment until sync success.
};
```

You need not introduce a generic `Result<T>` framework just for style; use a simple `std::expected`-like local type or explicit error return mechanism supported by C++20. Avoid exceptions escaping a C ABI or event loop; report I/O failures with syscall, errno, and context. `append_batch_and_sync` writes N sequential records then calls `fdatasync` once; only afterward does it acknowledge the **whole batch**. Per-record mode writes one and syncs one. A benchmark `write_only` mode can report `accepted_through_sequence` but never calls it durable.

For MVP, use one writer thread, an owning RAII file-descriptor wrapper, explicit `open` flags and permissions, and a Linux advisory `flock` or similar **cooperating writer** exclusion. Don't claim a global guarantee against noncooperating processes. Explicitly handle partial `write` with a loop; retry only `EINTR`; surface `EIO`, `ENOSPC`, `EBADF`, `fdatasync` failure and other errors; don't keep acknowledging after an uncertain synchronization failure.

### Architecture

```
CLI
 ├── log writer ── record codec ── FileOps / POSIX write/fdatasync
 │        └── policy: sync_each | sync_batch | write_only (diagnostic)
 ├── log reader / verifier ── record codec
 ├── recovery ── scan prefix + explicit ftruncate on incomplete tail
 ├── fault harness ── subprocess supervisor + injected FileOps failures
 └── benchmark runner ── fixed workload → JSON + table
```

Suggested modest code tree:

```
include/eventlog/record.h, writer.h, reader.h, faults.h
src/record.cpp, writer.cpp, reader.cpp, recover.cpp
src/file_ops.cpp           RAII fd + narrow injectable POSIX boundary
src/cli.cpp                argument parsing / commands
src/crash_harness.cpp      fork/exec or subprocess supervisor / checkpoints
src/benchmark.cpp          workload, timing, result JSON
tests/test_codec.cpp, test_recovery.cpp, test_faults.cpp
tests/test_crash_integration.cpp
benchmarks/README.md      reproducible methodology; measured results after run
CMakeLists.txt
scripts/verify.sh, verify-full.sh
```

Do not extract interfaces without a test or alternate concrete implementation. Here, a minimal `FileOps` boundary is justified: the test harness needs to inject short writes, `EINTR`, `EIO`, and failed `fdatasync` *without relying on impossible external OS timing*. The normal runtime uses ordinary POSIX operations.

## 4. Fault injection and independent correctness oracle

**Two distinct test mechanisms; never conflate them:**

1. **Injected I/O failures**: substitute a narrow `FileOps` implementation that can limit a `write` to K bytes, return `EINTR` once, fail after B bytes, or fail `fdatasync`. This tests error handling and on-disk partial record shape. These are **simulations**, not claims about actual power loss.
2. **Process termination tests**: a supervisor launches a child writer and can send `SIGKILL` at discrete **instrumented checkpoints**: before writing, midway through writing (after checkpoint-controlled short write), after write before sync, after sync before notifying supervisor, and after supervisor receives durable acknowledgment. It then reopens the log and verifies recovered prefix and supervisor-known acknowledgments. Uncontrolled kill timing is not needed for a first version.

**Supervisor acknowledgment protocol:** child sends `(sequence, durable=true)` to supervisor **only after successful `fdatasync`**. Parent records the acknowledgment as an independent oracle (e.g., to memory before initiating the kill); then it kills child and reopens file. To avoid ambiguous test bookkeeping, parent does not kill while reading a partially received acknowledgment frame unless that exact ambiguity is the scenario under test. When an ack was sent but not observed, the record may be present; this is permitted. The recovery invariant is:

```
acknowledged_sequences == prefix(recovered_sequences, len(acknowledged_sequences))
```

In addition, `recovered_sequences` must be contiguous from 1 with valid CRCs and payloads; no fabricated records. Injected I/O error before ack must never lead to an acknowledgement for that record. For `write_only` there are no durable acknowledgments to compare.

**Controlled corruption:** flip one byte of a fully formed header/payload in a fixture; `verify` must detect CRC or structural failure and **refuse automatic truncation**. An incomplete trailing record is recoverable; mid-log complete corruption is not repaired in the MVP.

**Output result classes:** `clean`, `recovered_incomplete_tail`, `corrupt_complete_record`, `sequence_violation`, `io_error`, `unknown_durability`, `test_harness_error`. Make uncertain states explicit.

## 5. Acceptance criteria and test matrix

| ID | Criterion | Required evidence |
|---|---|---|
| CPP-AC01 | Encode/decode exact v1 format independent of host padding/endian | Known vectors + round-trip tests |
| CPP-AC02 | Reject invalid magic/version/flags/length/CRC/sequence | Negative parser tests |
| CPP-AC03 | Append handles short write and interrupted system calls | Injected `FileOps` tests |
| CPP-AC04 | Single writer is enforced for cooperating processes | Second writer reports lock contention |
| CPP-AC05 | `sync_each` only acknowledges after successful sync | Failure before sync never acknowledged |
| CPP-AC06 | `sync_batch(N)` acknowledges a batch together after successful sync | Trace and ack assertions |
| CPP-AC07 | Sync failure stops durable writes and surfaces uncertainty | Failed `fdatasync` test |
| CPP-AC08 | Incomplete trailing record identified and truncated only when requested | EOF-mid-header/payload recovery tests |
| CPP-AC09 | Fully framed corrupt record or sequence gap rejected, not silently dropped | Corruption/sequence tests |
| CPP-AC10 | Process-crash recovery preserves every supervisor-observed durable acknowledgment | Controlled kill matrix |
| CPP-AC11 | Extra valid unacknowledged records accepted as possible | Test explicitly covers ack/recovery inequality |
| CPP-AC12 | CLI bounds and destructive operation protection work | Negative CLI tests, scratch roots |
| CPP-AC13 | Performance policies compared with reproducible workloads and statistics | Documented Release benchmark JSON |
| CPP-AC14 | No leak/UB findings in exercised code paths | ASan/UBSan, warnings, clean valgrind optional |
| CPP-AC15 | `verify` is read-only; `recover` requires explicit action to truncate | Before/after file checksum test |
| CPP-AC16 | README communicates real guarantee boundaries honestly | Manual review against this PRD |

### Detailed tests

| Test ID | Level | Test |
|---|---|---|
| CPP-T01 | Unit | Encode record lengths 0, 1, 128, 1 MiB; round-trip exact bytes |
| CPP-T02 | Unit | Reject 1 MiB+1, invalid CRC, truncated header, invalid flags/version |
| CPP-T03 | Unit | Sequence gap/duplicate and integer overflow rejection |
| CPP-T04 | Unit | Short `write` and `EINTR` retried correctly; `EIO` propagated |
| CPP-T05 | Unit | Failed `fdatasync` causes no durability ack and stops writer |
| CPP-T06 | Integration | Append 1,000 records and read back exact payload and order |
| CPP-T07 | Integration | Concurrent second cooperating writer rejected by lock |
| CPP-T08 | Recovery | Cut tail at every possible byte offset of one record; recover valid prefix |
| CPP-T09 | Recovery | Flip byte in complete final record; error rather than truncation |
| CPP-T10 | Recovery | Corrupt middle record; stop with explicit location |
| CPP-T11 | Crash | Kill after write before sync; prefix guarantee holds regardless of extras |
| CPP-T12 | Crash | Kill after sync before ack observed; recovered extra allowed |
| CPP-T13 | Crash | Kill after ack observed; every acknowledged record recovered |
| CPP-T14 | CLI | Read-only verify unchanged; explicit truncate only on recover command |
| CPP-T15 | Quality | Debug sanitizer tests, resource cleanup, bounded allocations |
| CPP-T16 | Bench | Fixed payload/record counts under all policies; multiple repeated trials |
| CPP-T17 | Benchmark correctness | Release output data includes compiler, CPU, filesystem, batch size, sync policy, trial count, and units |

### Benchmark methodology (mandatory before resume claims)

1. Build `Release` (`-O2` or `-O3`, no sanitizers), and record compiler version, CPU, RAM, filesystem type, OS, storage class, workload, and machine load.
2. Use fresh per-trial files in a dedicated scratch directory; ensure equal payload contents and record counts. Suggested workloads: 64, 512, and 4096-byte payloads; 10,000–100,000 records per trial subject to time/disk budget.
3. Compare: `sync_each`, `sync_batch(16)`, `sync_batch(64)`, and optionally `write_only` with explicit **not durable** label. Record acknowledged write throughput (records/sec), total wall time, p50/p95/p99 **durable ack latency** at the policy's actual ack granularity, bytes written, and peak RSS if available. For `write_only`, report only accepted throughput/latency and mark durable-ack latency **not applicable**.
4. Do at least five trials per data point when feasible; show median and spread (e.g., min/max or interquartile range). Avoid cherry-picking best result, and explain OS cache effects; do not claim comparison to real power-loss persistence.
5. Compare to the `sync_each` baseline and compute observed improvement only **after** measurement. A 20–50% gain is not presumed; batching may have a much larger or smaller effect depending on disk, fsync semantics, and workload. Never assert a target speedup is an achieved result.
6. Benchmarks are reports, not CI pass/fail gates. CI remains deterministic and lightweight; raw output lives in ignored `out/` and a curated reproducible example may be committed under `benchmarks/` once actually run.

**Performance learning goals:** number of syscalls per batch, effects of buffering, allocations per record, wall-clock vs CPU time, acknowledgement granularity, and tail latency under sync-heavy work. Do not add a custom memory pool until profiling demonstrates allocation cost is important.

## 6. Failure, security, and lifecycle decisions

| Failure/edge | Expected behavior |
|---|---|
| Disk full or syscall error during record | Stop, report errno/offset; no false durability ack |
| `fdatasync` error | Transition writer to failed/uncertain; require reopen/recovery |
| Crash in middle of trailing header/payload | Verify incomplete tail; explicit recovery truncates to last valid offset |
| Fully formed record with checksum mismatch | Stop as hard corruption; no silent truncation |
| Invalid/hostile length | Reject without huge allocation or OOB read |
| Existing log version unsupported | Report unsupported, preserve bytes |
| Writer lock already held | Fail fast; no concurrent append attempt |
| Reader while writer active | MVP disallows or requires documented snapshot boundary; don't claim consistent live reads |
| Process killed after `write` before `fdatasync` | Unacknowledged records may or may not be recovered; invariant applies to acknowledged only |
| Process killed after `fdatasync` | Record durable under declared process-crash/OS assumptions, whether supervisor saw ack or not |
| Temporary fixture path | Only write/truncate within scratch dir for crash tests; reject symlink target tricks when destructive operations are requested |
| Benchmark resource exhaustion | Bound total bytes/time; clean scratch data even if interrupted |

**Safety:** Fault harness must operate on owned temporary fixture paths only, never on arbitrary production log files. Destructive recovery requires explicit command/flag and prints what will be changed. Avoid following user-provided symlinks to files outside the configured test root. Run as ordinary unprivileged user. No root, privileged direct I/O, or external services required.

## 7. Milestones and agent-ready tasks

| Phase/task | Depends on | Deliverable | Acceptance gate |
|---|---|---|---|
| **M0 CPP-00 — scaffold** | None | CMake, C++20 toolchain, formatter, tests, CI, PRD/CLAUDE/STATUS, verify scripts | Fresh checkout builds and basic tests pass |
| **M1 CPP-01 — codec** | CPP-00 | Explicit little-endian v1 encode/decode, CRC32C | CPP-AC01/02, CPP-T01–03 |
| **M1 CPP-02 — writer** | CPP-01 | Owning fd, short-write loop, sync_each, lock | CPP-AC03–05, CPP-T04–07 |
| **M1 CPP-03 — reader** | CPP-02 | Verify file, report complete/incomplete/corrupt records | CPP-AC08/09/15 |
| **M2 CPP-04 — recovery** | CPP-03 | Read-only preflight, opt-in incomplete-tail truncation | CPP-T08–10, CPP-T14 |
| **M2 CPP-05 — batch policy** | CPP-02/04 | `append_batch_and_sync`, ack boundaries, `fdatasync` errors | CPP-AC06/07, CPP-T05 |
| **M2 CPP-06 — fault injection** | CPP-05 | Narrow `FileOps` simulation for partial writes, errors | CPP-T04/05, recovery fault cases |
| **M3 CPP-07 — process crash harness** | CPP-06 | Supervisor checkpoints, `SIGKILL`, independent acks, oracle | CPP-AC10/11, CPP-T11–13 |
| **M3 CPP-08 — benchmark** | CPP-05 | Workload generator, Release benchmark, JSON metrics | CPP-AC13, CPP-T16/17 |
| **M3 CPP-09 — harden/release** | All | Sanitizers, docs, safety bounds, sample benchmark report, PR | CPP-AC12/14/16, clean CI |

**Prioritize correctness over optimization:** if behind schedule, finish a trustworthy `sync_each` baseline, reader/recovery, and fault tests first. Batch optimization is important to the intended resume signal, but never introduce it before the acknowledgment model is tested.

### Definition of done

- Binary format v1 is specified, stable, and tested by fixed vectors and negative cases.
- A writer using either durable policy acknowledges only after successful `fdatasync`.
- The controlled crash harness demonstrates the acknowledged-prefix invariant, including after-sync-before-ack ambiguity.
- Corrupted complete records are rejected; structurally incomplete tail recovery is only performed with explicit instruction.
- CI and sanitizer tests pass; no race-prone concurrency is introduced merely to raise complexity.
- Release benchmark compares multiple sync/batch policies with recorded environment, complete raw metrics, and honest interpretation.
- README includes concrete append/verify/recover/crash-test/bench commands, limitations, and real sample output clearly labeled measured or illustrative.
- Independent review scrutinizes file-format/acknowledgment correctness, integer/bounds handling, and crash harness assumptions; human merges.

## 8. Future extensions (not part of MVP)

Multiple producer threads with a single writer queue; `writev`/vectored I/O; block alignment; group commit with latency targets; storage preallocation; memory-mapped reader; failure injection around directory/file creation; more realistic power-cut simulation on a throwaway loopback filesystem with careful host isolation; checksummed segment files; compaction; network replication; integration as a backend for another project. Add only in response to performance measurements or a genuine desired use case.

## 9. First Claude Code task and implementation prompt

> Read `docs/PRD.md`, `docs/STATUS.md`, and current repository state. Implement CPP-00 only: smallest CMake C++20 project, clang-format, warning configuration, Debug sanitizer test path, Release build target, canonical `./scripts/verify.sh`, GitHub Actions, and placeholder CLI/tests. Do not implement record logging yet. Record exact checks and provide a bounded plan for CPP-01. For any implementation, distinguish durable `fdatasync` acknowledgments from buffered writes and distinguish `SIGKILL` from power loss.

**Implementation references:** Linux/POSIX `open(2)`, `write(2)`, `fdatasync(2)`, `fsync(2)`, `ftruncate(2)`, `flock(2)`, `kill(2)`, `waitpid(2)`, and the C++ Core Guidelines (RAII/resource ownership). Consult the actual target filesystem semantics and document the limits of what the test harness proves.
