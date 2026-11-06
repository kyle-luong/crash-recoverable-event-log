# ADR-006 — The batch is the acknowledgment unit, and batches are explicit

**Status:** accepted (2026-11-05) · **Affects:** acknowledgment contract, writer API, CLI
· **Implements:** PRD CPP-05 / CPP-AC06

## Context

`docs/PRD.md` §3 specifies `append_batch_and_sync`: write N sequential records, call the
durability barrier once, and only afterward acknowledge **the whole batch**. The CLI exposes this
as `append --policy sync_batch --batch-size N` over a stream of records.

That phrasing admits two very different implementations:

1. **Hidden buffering.** `AppendAndSync` accumulates records and syncs every N, returning a
   `durable_through_sequence` that lags the record just submitted. The writer holds
   accepted-but-unsynced state between calls.
2. **Explicit batches.** A separate `AppendBatchAndSync(payloads)` takes N payloads, writes them,
   syncs once, and acknowledges all N. The writer holds no pending state between calls; the CLI
   chunks its stream into batches.

Hidden buffering is the more conventional design and the more dangerous one here. It creates a
window in which records are accepted but not durable, and that window is only closed by something
outside the append path — `Close()`, a destructor, or an explicit flush. Three consequences:

- `Close()` has to decide whether to sync and acknowledge the partial batch or drop it. Either
  answer makes the acknowledgment boundary depend on how the process exits, which is precisely
  what the crash harness in M3 must be able to state without ambiguity.
- A writer destroyed on an error path would resolve the boundary in a destructor, where a failed
  sync cannot be reported at all.
- The externally observed acknowledgment — the thing M3's supervisor records as its oracle — would
  no longer line up with a single call returning.

## Decision

**The acknowledgment unit is the batch, and a batch is one call.**

`Result<BatchAppendResult> AppendBatchAndSync(std::span<const std::span<const std::byte>>)`
encodes all N records into one contiguous buffer, issues one `WriteFully`, issues one `SyncData`,
and only then advances `durable_through_sequence` to the batch's last sequence. The writer keeps
no accepted-but-unsynced records between calls, so there is no partial batch for `Close()` to
resolve and `Close()` keeps its current meaning: release the lock, sync nothing.

A streaming caller chunks its own records. `append --policy sync_batch --batch-size 32` over 100
records issues four batches — 32, 32, 32, 4 — and the final short batch is acknowledged exactly
like a full one. The ack boundary is always a call boundary and is therefore visible to the
caller, to the CLI output, and to M3's supervisor.

The policy and the method must agree: `AppendAndSync` requires `sync_each`, `AppendBatchAndSync`
requires `sync_batch`. A mismatch is rejected rather than quietly honored, so the policy label
printed with a result always describes what actually happened.

Failure handling is inherited unchanged from ADR-003 and applies to the batch as a unit:

| Failure | Acknowledged | Writer state |
|---|---|---|
| Write fails partway through the batch | **No record in the batch** | terminal |
| Sync fails after the whole batch was written | **No record in the batch** | terminal |

Records from a failed batch may still be present and complete on disk. That is permitted and
expected: the invariant is one-directional — every acknowledged record must be recovered, not the
reverse. A later `Open` resyncs the resumed prefix before reporting it durable (ADR-001), so those
records become durable under the new session's own barrier call rather than inheriting a claim.

## Consequences

- One `write(2)` and one `fdatasync(2)` per batch instead of N and N. The writer counts both, so
  the tradeoff is *verified by assertion* in the test suite rather than inferred: a 32-record
  batch is asserted to perform exactly one write and one sync. No timing claim is made here;
  measurement is M4.
- Batches are bounded before anything is allocated: at most `batch_size` records (itself capped at
  `kMaxBatchRecords`) and at most `kMaxBatchBytes` of encoded output. The batch buffer is sized
  from a total that was already checked, never from an attacker-chosen length.
- A larger batch means more unacknowledged work in flight to lose on a crash. That is the honest
  cost of the policy and is stated in the README rather than hidden.
- The writer is simpler: no pending-batch member, no flush path, no destructor-ordering question.

## Alternatives rejected

- **Hidden buffering with a flush on `Close()`.** Rejected for the reasons above: it makes the
  acknowledgment boundary a property of process teardown.
- **One `write(2)` per record with one sync per batch.** Keeps N write syscalls for no benefit;
  the single contiguous write is what makes the syscall-count tradeoff measurable.
- **Letting `AppendAndSync` work under `sync_batch` by syncing every call.** Would report results
  labeled `sync_batch` that were produced by `sync_each` behavior, corrupting the M4 comparison.
