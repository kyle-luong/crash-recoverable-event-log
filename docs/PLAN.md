# Milestone plan

Derived from `docs/PRD.md` §7. Each milestone is **independently verifiable**: it has one command
that passes or fails on its own, without reference to later work. A milestone is `done` only when
its command passes from a clean build directory.

| M | PRD tasks | Deliverable | Verification command | State |
|---|---|---|---|---|
| **M0** | CPP-00 | CMake C++20 scaffold, warnings, sanitizer Debug path, Release target, format check, CI, docs | `./scripts/verify.sh` on a clean checkout | done |
| **M1** | CPP-01…04 | Record codec + CRC32C, `sync_each` writer with advisory lock, log scanner, incomplete-tail recovery, `append`/`verify`/`recover` CLI | `./scripts/verify.sh`  + `./scripts/verify-full.sh` | todo |
| **M2** | CPP-05, CPP-06 | `append_batch_and_sync` with whole-batch ack boundaries; scriptable `FileOps` fault injection (short write, `EINTR`, `EIO`, failed sync) driving recovery cases | `./scripts/verify.sh` + new `test_faults` suite | todo |
| **M3** | CPP-07 | Subprocess supervisor, `SIGKILL` at instrumented checkpoints, independent ack oracle, acknowledged-prefix invariant over many runs | `eventlog crash-test --runs 300 --seed 42 --workdir <scratch>` exits 0 | todo |
| **M4** | CPP-08 | Release benchmark: workload generator, records/sec, p50/p95/p99 durable-ack latency, environment capture, JSON output | `eventlog bench --trials 5 --out out/results.json`, reviewed against `benchmarks/README.md` methodology | todo |
| **M5** | CPP-09 | Harden and release: full sanitizer sweep, CLI bounds and destructive-op guards, honest README with measured sample output, PR | `./scripts/verify-full.sh` + manual README review against PRD §5 | todo |

M2 and M3 are sequenced before M4 deliberately, per PRD §7: "never introduce [batch optimization]
before the acknowledgment model is tested."

## Acceptance traceability

The verifying test named for each criterion is the one that milestone must deliver. Nothing below
is satisfied yet: only M0 is complete, so every row is `planned`.

| PRD ID | Covered by | Milestone |
|---|---|---|
| CPP-AC01 | `test_crc32c` (4 published vectors + chaining), `test_codec` round-trips at 0/1/128/1 MiB, fixed-byte golden-vector assertion | M1 (planned) |
| CPP-AC02 | `test_codec` rejections: bad magic, version 0/2, any unknown flag bit, length 1 MiB+1, truncated header, CRC mismatch | M1 (planned) |
| CPP-AC03 | `test_writer` short-write and `EINTR` injection via `FileOps` double; `EIO` propagation | M1 (planned) |
| CPP-AC04 | `test_writer` second-writer `flock` contention | M1 (planned) |
| CPP-AC05 | `test_writer` no ack when the write fails and when the sync fails | M1 (planned) |
| CPP-AC06 | — | M2 (planned) |
| CPP-AC07 | `test_writer` failed-sync terminal state (ADR-003) | M1 (planned) |
| CPP-AC08 | `test_recovery` tail cut at **every** byte offset of a record; truncation only with explicit opt-in | M1 (planned) |
| CPP-AC09 | `test_recovery` byte flip in final record, byte flip in middle record, sequence gap and duplicate | M1 (planned) |
| CPP-AC10, AC11 | — | M3 (planned) |
| CPP-AC12 | `cli_tests.sh` bounds, overwrite refusal, symlink refusal | M1 (planned) |
| CPP-AC13 | — | M4 (planned) |
| CPP-AC14 | ASan + UBSan Debug build is the default `verify.sh` path; `-Werror` | M1 (planned) |
| CPP-AC15 | `cli_tests.sh` file-hash-unchanged check across `verify` and `recover` without the flag | M1 (planned) |
| CPP-AC16 | README guarantee-boundaries section, reviewed against PRD §1 | M5 (planned) |

## Milestone 1 scope boundary (next task)

In scope: the record format, one durable policy (`sync_each`), reading and verifying a log,
recovering a structurally incomplete trailing record, and the three CLI commands that exercise
those paths.

Out of scope, by decision: batching, the crash supervisor, benchmarks, `write_only`, concurrent
writers, mid-log repair, and compaction. `SyncPolicy::kSyncBatch` and `kWriteOnly` exist in the
enum but are rejected at `Open` so that no code path can report an unimplemented policy's result
as durable.
