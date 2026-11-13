# Milestone plan

Derived from `docs/PRD.md` §7. Each milestone is **independently verifiable**: it has one command
that passes or fails on its own, without reference to later work. A milestone is `done` only when
its command passes from a clean build directory **on Linux** — the supported platform
([ADR-004](decisions/ADR-004-linux-is-the-supported-platform.md)). In practice that means
`./scripts/verify-linux.sh`; a macOS pass is a convenience check, never the evidence.

| M | PRD tasks | Deliverable | Verification command | State |
|---|---|---|---|---|
| **M0** | CPP-00 | CMake C++20 scaffold, warnings, sanitizer Debug path, Release target, format check, CI, docs | `./scripts/verify.sh` on a clean checkout | done |
| **M1** | CPP-01…04 | Record codec + CRC32C, `sync_each` writer with advisory lock, log scanner, incomplete-tail recovery, `append`/`verify`/`recover` CLI | `./scripts/verify-linux.sh` | done |
| **M2** | CPP-05, CPP-06 | `append_batch_and_sync` with whole-batch ack boundaries (ADR-006); shared `FileOps` fault injection (short write, `EINTR`, `EIO`, failed sync) driving batch and recovery cases | `./scripts/verify-linux.sh` | done |
| **M3** | CPP-07 | Subprocess supervisor, `SIGKILL` at instrumented checkpoints, independent ack oracle, acknowledged-prefix invariant over many runs (ADR-007) | `eventlog crash-test --runs 300 --seed 42 --workdir <scratch>` exits 0, inside `./scripts/verify-linux.sh` | done |
| **M4** | CPP-08 | Release benchmark: workload generator, records/sec, p50/p95/p99 durable-ack latency, environment capture, JSON output; `write_only` as the non-durable baseline (ADR-008) | `eventlog bench --trials 5 --out out/results.json`, reviewed against `benchmarks/README.md` methodology | done |
| **M5** | CPP-09 | Harden and release: read-path fault injection, regular-file requirement, report-file guards, library hardening assertions, README re-verified line by line | `./scripts/verify-linux.sh` + manual README review against PRD §5 | done |

M2 and M3 are sequenced before M4 deliberately, per PRD §7: "never introduce [batch optimization]
before the acknowledgment model is tested."

## Acceptance traceability

Only criteria claimed by a completed milestone are listed as covered. Everything else is `todo` and
must not be described as satisfied.

| PRD ID | Covered by | Milestone |
|---|---|---|
| CPP-AC01 | `test_crc32c` (4 published vectors + chaining), `test_codec` round-trips at 0/1/128/1 MiB, fixed-byte golden-vector assertion | M1 |
| CPP-AC02 | `test_codec` rejections: bad magic, version 0/2, any unknown flag bit, length 1 MiB+1, truncated header, CRC mismatch | M1 |
| CPP-AC03 | `test_writer` short-write and `EINTR` injection via `FileOps` double; `EIO` propagation | M1 |
| CPP-AC04 | `test_writer` second-writer `flock` contention | M1 |
| CPP-AC05 | `test_writer` no ack when the write fails and when the sync fails; directory-entry sync asserted for a created log and for one that already existed empty, plus its injected failure (R4) | M1 |
| CPP-AC06 | `test_batch` whole-batch acknowledgment, short final batch, failed batch write and failed batch sync acknowledging nothing; `cli_tests.sh` batch counts | M2 |
| CPP-AC07 | `test_writer` failed-sync terminal state (ADR-003); `test_batch` reconfirms it at batch granularity | M1, M2 |
| CPP-AC08 | `test_recovery` tail cut at **every** byte offset of a record; truncation only with explicit opt-in; stale plan rejected by length and by file identity | M1 |
| CPP-AC09 | `test_recovery` byte flip in final record, byte flip in middle record, sequence gap and duplicate | M1 |
| CPP-AC10 | `test_crash` oracle negative controls (lost acknowledgment, flipped byte in a complete record, sequence gap, fabricated payload, tail from a clean exit); `cli_tests.sh` kill matrix at all six checkpoints, plus a stand-in child whose false acknowledgment must be reported | M3 |
| CPP-AC11 | `test_crash` accepts records recovered but never acknowledged; `cli_tests.sh` requires a nonzero extra count at `after_write_before_sync` and `after_sync_before_ack`, and zero at `before_write` and `after_ack` | M3 |
| CPP-AC12 | `cli_tests.sh` bounds, overwrite refusal, symlink refusal; extended in M5 to the `crash-test` and `bench` scratch roots, both report paths (`verify --json`, `bench --out`), and the refusal of a directory or named pipe as a log; `test_reader` covers the file-type refusal directly | M1, M5 |
| CPP-AC13 | `test_bench` percentile and spread statistics, per-policy barrier-call counts, equal-workload checks, bounds, and the publishability stamp; `cli_tests.sh` report contents and overwrite/symlink guards; `benchmarks/RESULTS.md` holds a measured Release run with its full environment | M4 |
| CPP-AC14 | ASan + UBSan Debug build is the default `verify.sh` path, with `detect_stack_use_after_return`, LeakSanitizer on Linux and `_GLIBCXX_ASSERTIONS`; `-Werror` under both GCC 12 (Linux) and Apple clang; every command exercised under it, including the crash harness's child processes | M1, M5 |
| CPP-AC15 | `cli_tests.sh` file-hash-unchanged check across `verify` and `recover` without the flag | M1 |
| CPP-AC16 | README guarantee-boundaries section, reviewed against PRD §1 and §5 in M5 by re-running every documented command on Linux Release and diffing its output against the file line by line | M5 |

## Scope boundaries

**M1.** In scope: the record format, one durable policy (`sync_each`), reading and verifying a log,
recovering a structurally incomplete trailing record, and the three CLI commands that exercise
those paths.

**M2.** In scope: `sync_batch` with whole-batch acknowledgment, the batch bounds, and promoting the
fault-injection double to a shared fixture that drives batch and recovery cases.

**M3.** In scope: the subprocess supervisor, the six instrumented checkpoints, the in-memory
acknowledgment oracle, harness-owned recovery of a torn tail, and the `crash-test` CLI command
(ADR-007). The two PRD fault mechanisms stay separate: injected `FileOps` failures (M2) test error
handling in process, and process termination tests the acknowledgment boundary across a death.
Controlled kill timing only — a parked child killed at a known point, not a race.

**M4.** In scope: the `bench` command, the four-policy comparison fixed by
`benchmarks/README.md` step 6, the statistics it reports, the environment it captures, and
`write_only` as the non-durable baseline that comparison needs (ADR-008). A report states whether
it may be quoted; `write_only` carries no durability field at all and stays excluded from the
acknowledged-prefix claim.

**M5.** In scope: closing the gaps a crash-safety tool should not ship with. The read path now goes
through the injectable `FileOps` boundary, so an I/O error while reading is provably reported as an
I/O error rather than as corruption. A log must be a regular file, checked on the descriptor, with
`O_NONBLOCK` on every open so a named pipe is refused instead of blocking forever. Both commands
that write a report refuse a symlink and refuse to replace existing content without `--overwrite`.
The instrumented build gains `detect_stack_use_after_return` and bounds-checked containers. The
README's every command was re-run and diffed against the file.

Out of scope, by decision: concurrent writers, mid-log repair, and compaction. Benchmarks are
reports, not CI gates: the suite exercises the benchmark with tiny workloads and asserts syscall
counts and report structure, never a duration.
