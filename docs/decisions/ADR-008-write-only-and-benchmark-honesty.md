# ADR-008 — A non-durable result has no durable field, and a report that cannot be quoted says so

**Status:** accepted (2026-11-12) · **Affects:** writer API, benchmark output, acknowledgment claim
· **Implements:** PRD CPP-08 / CPP-AC13, CPP-T16, CPP-T17

## Context

Two things arrive together in this milestone and share one risk: a number escaping the context that
makes it true.

`docs/PRD.md` §2 permits a `write_only` policy "to show raw throughput, but mark its returned result
as **accepted, not durable**. It must not expose the same durability acknowledgment status as
synchronized policies, and MUST NEVER be included in the acknowledged-prefix correctness claim."
§3 adds that it "can report `accepted_through_sequence` but never calls it durable". Until now the
policy existed in the enum and was refused at `Open`, which made those rules true by absence.

`docs/PRD.md` §5 and `benchmarks/README.md` then fix a benchmark method in detail: Release builds
with sanitizers off, Linux only, fresh per-trial files, equal workloads, at least five trials,
median and spread rather than the best run, percentiles at the policy's real acknowledgment
granularity, and no comparison computed before measurement. Every one of those is a rule about not
publishing a misleading figure.

Both risks have the same shape. A `write_only` result that merely *documents* its
`durable_through_sequence` as always zero is one careless read away from being quoted as durability.
A throughput table printed from a Debug sanitizer build looks exactly like a real one.

## Decision

**A non-durable result carries no durability field at all.**

`write_only` has its own method and its own type:

```cpp
struct AcceptResult { std::uint64_t sequence; std::uint64_t accepted_through_sequence; };
Result<AcceptResult> AppendWithoutSync(std::span<const std::byte> payload);
```

There is no `durable_through_sequence` on `AcceptResult` to misread, and the method is refused under
any other policy exactly as `AppendAndSync` and `AppendBatchAndSync` are refused under this one
(ADR-006's rule, that a result's shape describes how it was produced, extended to its type).

The policy never calls the barrier, anywhere:

- No `SyncData` per record, which is the definition of the policy.
- No `SyncData` when resuming an existing log either. The resume sync added in M2 exists so a
  session establishes its own durability claim rather than inheriting one (ADR-001); a policy that
  makes no claim has nothing to establish, and syncing there would make `write_only` call the
  barrier once, which is the one thing it is defined not to do.
- `durable_through_sequence()` therefore returns 0 for the writer's whole life, and
  `WriterStats::batches` — which counts *durable* acknowledgment units — stays 0 too.
- The directory fsync on file creation is unchanged: that is about the file existing, not about any
  record being durable.

A failed write is still terminal (ADR-003). The bytes written are byte-identical to what the
durable policies write for the same workload, because the policy changes when the barrier is called
and nothing else; the CLI test asserts that identity.

The crash harness continues to reject the policy outright (ADR-007), so it cannot enter the
acknowledged-prefix claim even by accident.

**A benchmark report states whether it may be quoted.**

Every report carries `publishable`, true only for a Release build with sanitizers off on Linux —
the conditions `benchmarks/README.md` steps 1 and 2 require — and a `publishable_note` naming each
reason it failed. The table prints that note as a `WARNING` banner before any number. A figure
cannot travel without its disqualification attached, so a Debug run cannot be mistaken for a result
and a macOS `F_FULLFSYNC` number cannot be set beside a Linux `fdatasync` one.

The rest of the method is enforced in the code rather than left to discipline:

| Rule | How it is enforced |
|---|---|
| Median and spread, never the best run | `Spread` carries median, min and max. There is no "best" accessor to reach for. |
| Percentiles are real observations | Nearest rank, no interpolation, so every figure reported is a latency that occurred. |
| At the policy's real ack granularity | One latency sample per acknowledgment unit: per record under `sync_each`, per batch under `sync_batch(N)`. |
| Equal workloads across policies | Byte counts are compared across trials and across policies; a mismatch fails the run instead of reporting it. |
| The policy did what it claims | Durable policies must show exactly one barrier call per acknowledgment; `write_only` must show none. Either mismatch fails the run. |
| Fresh file per trial | Each trial writes a new file and deletes it, so no trial inherits another's size or cache state and peak disk use is one trial. |
| Comparison only after measurement | The ratio against the baseline is computed from measured medians at report time. No target exists anywhere in the code. |
| `write_only` durable-ack latency is not applicable | Reported as `durable_ack_latency_applicable: false` with an explicit `latency_meaning`, never as a number in a durable column. |

Throughput is computed from time spent inside the append calls, not from the whole loop, so it
measures the log rather than the deterministic payload generator. Wall time is reported alongside
it, and the gap between them is the generator's share — visible rather than hidden in either
direction.

Environment capture reports what it found or says it does not know. `/proc/cpuinfo` has no
`model name` on aarch64, so the CPU field falls back to the implementer and part identifiers and
says so; `storage_class` is always `unknown`, because distinguishing an SSD from spinning or virtual
media needs queries this tool does not make and a plausible guess would silently change how every
number should be read.

## Consequences

- `eventlog append --policy write_only` works and prints `Accepted (NOT durable): N` with a line
  saying the barrier was never called. It never prints a durable acknowledgment count.
- `eventlog bench --out PATH` compares `sync_each`, `sync_batch(16)`, `sync_batch(64)` and
  `write_only` on one workload and writes a JSON report with its full environment and units. The
  report refuses to overwrite a nonempty file without `--overwrite` and will not write through a
  symlink.
- Bounds are checked before anything is written: one trial at most 4 GiB, a whole run at most
  16 GiB, trials at most 50, payload at most the format's 1 MiB.
- The benchmark is not a CI gate. It runs in the test suite only with tiny workloads, asserting
  syscall counts and report structure, never a duration — a timing assertion under sanitizers would
  be noise, and a timing assertion anywhere would make CI a performance test.
- **What this does not measure.** Nothing here speaks to power-loss persistence, to hardware that
  lies about flushing, or to any machine other than the one recorded in the report. A number
  measured in a virtual machine is a number about that virtual machine.

## Alternatives rejected

- **Keep `durable_through_sequence` on the `write_only` result and document it as always 0.** The
  field is the hazard. A reader who sees a familiar struct will read a familiar meaning.
- **One `Append` method that switches on the policy.** Then the return type cannot say whether a
  durability claim exists, and the caller's correctness depends on remembering which policy is set.
- **Sync once at `Close()` under `write_only` so the data survives.** It would make the policy's
  numbers meaningless and give it a durability property the PRD says it must not have.
- **Refuse to run `bench` outside a Release build.** The suite could then never exercise the
  benchmark at all, and an untested reporting path is worse than a clearly-labelled one.
- **Interpolated percentiles, or reporting the fastest trial.** Both publish figures that were never
  observed. `benchmarks/README.md` step 7 forbids the second explicitly; the first is the same error
  in smaller print.
- **Guessing the storage class from device names or rotational flags.** Wrong often enough to
  mislead, and the whole point of the environment block is that it is measured rather than inferred.
