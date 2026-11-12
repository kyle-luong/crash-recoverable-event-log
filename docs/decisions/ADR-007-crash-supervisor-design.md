# ADR-007 — The supervisor kills a parked child and checks it against its own record

**Status:** accepted (2026-11-12) · **Affects:** crash harness, acknowledgment claim, CLI
· **Implements:** PRD CPP-07 / CPP-AC10, CPP-AC11

## Context

`docs/PRD.md` §4 requires a supervisor that launches a child writer, sends `SIGKILL` at discrete
instrumented checkpoints, and then reopens the log to check recovered state against the
acknowledgments it observed itself. Until this exists, the acknowledged-prefix invariant is argued
but not demonstrated; it is the claim the rest of the project is built to support.

Three questions had to be settled before any of it could be written.

**Who issues the kill, and when?** The PRD names five checkpoints — before writing, midway through
a write, after the write before the sync, after the sync before notifying the supervisor, and after
the supervisor has the acknowledgment. A kill scheduled by elapsed time would hit a different point
on every run and on every machine, so a run that passed would not say which state it tested. The
PRD also says explicitly that uncontrolled kill timing is not needed for a first version.

**What is the oracle?** The invariant compares acknowledgments against recovered records. If the
acknowledgment side were derived from the log, the check would compare the log against itself and
pass for any log.

**What counts as a violation rather than an expected outcome?** A crash leaves states that are
correct but not clean: a torn trailing record, or valid records that were never acknowledged.
Treating those as failures would make the harness useless; treating a genuinely corrupt record as
recoverable would make it dishonest.

## Decision

**The child parks at its checkpoint and the supervisor kills it there.**

The child announces the checkpoint on its frame channel and then calls `pause()` in a loop.
`SIGKILL` is uncatchable, so the child never resumes. The supervisor reads the frame, which tells
it exactly what state the child is in, and only then sends the signal. Kill timing is therefore
exact rather than probabilistic, and the same seed replays the same matrix.

The two checkpoints inside the I/O path are reached through a `FileOps` decorator that the child
arms immediately before the append call to be interrupted — so it cannot fire on `Open`'s own
syscalls or on an earlier record. `mid_write` performs a *real* partial write and then parks,
leaving a strict byte-prefix of a record on disk, which is the only tail shape a single writer
stopped between syscalls can produce (ADR-002). The generated matrix never places that stop on a
record boundary, because a stop between two whole records leaves no torn record and would quietly
be a different scenario.

**The oracle is the supervisor's own memory, written before the kill.** The child emits
`ack <first> <last>` only after the durability barrier has returned success for that unit, which
under `sync_batch` is the whole batch (ADR-006). The supervisor records the range in memory as the
frame arrives — before the kill that a later frame may trigger — and nothing read back out of the
log ever feeds it. Frames are single short lines well below `PIPE_BUF`, so one arrives whole or not
at all, and a trailing partial line is discarded rather than acted on: half an acknowledgment frame
is not an acknowledgment, which is the bookkeeping ambiguity the PRD tells us to avoid.

**The invariant is checked in one direction.** Every acknowledged record must be present in the
recovered log; records recovered but never acknowledged are permitted, counted, and reported. Both
sides are contiguous runs from 1 — the read-back loop proves it for the recovered side, and
acknowledgments are issued in order — so prefix containment reduces to
`acknowledged_through <= recovered_last_sequence`.

Recovered records are not taken at face value. Each one is read back and compared byte for byte
against the deterministic payload for its sequence number, so a record that is structurally
perfect but is not the record that was written is caught.

| Recovered state | Verdict |
|---|---|
| Clean, acknowledged ⊑ recovered | upheld |
| Valid records beyond the last acknowledgment | upheld, counted as extra |
| Incomplete trailing record, child was killed | recovered by the harness, then upheld |
| Incomplete trailing record, child exited normally | **violation** |
| Complete record with a bad CRC, invalid header, or sequence break | **violation**, file left alone |
| Acknowledged sequence missing from the log | **violation** |
| Payload that does not match its sequence | **violation** |

Harness-owned recovery of an incomplete tail is what `docs/PRD.md` §3 permits alongside the
explicit `recover --truncate-tail`. Hard corruption is never truncated (ADR-002): nothing in the
supported fault model produces it, so it is reported with its offset and left on disk.

`write_only` is rejected by the scenario parser. It produces no durable acknowledgment, so it has
nothing to compare, and `docs/PRD.md` §2 says it MUST NEVER appear in the acknowledged-prefix
claim. Excluding it at the parser keeps that true by construction rather than by convention.

## Consequences

- `eventlog crash-test --runs 300 --seed 42 --workdir <scratch>` exits 0 on all supported
  invariants, 1 on a correctness violation, 3 when the harness itself could not run. Scenarios
  cycle through all six checkpoints, so every one is exercised even at a small `--runs`, and the
  seed varies only the workload around them.
- A violation report carries the scenario spec that produced it, which is also the child's command
  line, so a failing run can be replayed by hand. The failing run's log is kept; passing fixtures
  are removed.
- The supervisor can fail. Its oracle is tested directly against hand-damaged logs in
  `tests/test_crash.cpp` — a lost acknowledgment, a flipped byte in a complete record, a sequence
  gap, a fabricated payload, a tail from a clean exit — and the path from a detected violation to a
  nonzero exit is tested through the CLI with a stand-in child that claims an acknowledgment it
  never earned. Without those, `Violations: 0` would be unfalsifiable.
- Only the supervisor's own subdirectory of `--workdir` is written or removed; the scratch root
  itself belongs to the caller, and a symlinked root is refused. On Linux the child also gets
  `PR_SET_PDEATHSIG`, so a supervisor that dies cannot leave a parked process holding a lock.
- **What this does not prove.** `SIGKILL` ends a process; it does not clear the kernel page cache
  and it is not a power failure. The supported claim is zero lost externally acknowledged durable
  records *in the tested process-crash and injected-write-fault scenarios*, under ordinary Linux
  filesystem semantics (ADR-001, ADR-004). Nothing here speaks to hardware faults, lying drives, or
  power loss.

## Alternatives rejected

- **The child kills itself at the checkpoint.** Simpler by a pipe, but then there is no supervisor:
  the PRD wants the parent to record the acknowledgment and *then* initiate the kill, which is what
  makes the oracle independent of the process being tested.
- **Killing after a timeout or a fixed delay.** Hits a different state on every machine, so a pass
  would not identify which checkpoint it covered.
- **Comparing recovered records against the log's own last sequence.** Compares the log with
  itself; passes for any log, including an empty one.
- **Requiring recovered == acknowledged.** Contradicts `docs/PRD.md` §1 and §4, which permit extra
  valid unacknowledged records explicitly. The equality is required only of a writer that was never
  interrupted, where it is checked.
- **Driving the checkpoints from the in-process `FileOps` double instead of a subprocess.** That is
  mechanism 1 of PRD §4 and already exists (M2). It cannot test process death, and the PRD says the
  two mechanisms must never be conflated.
