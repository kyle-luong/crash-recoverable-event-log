# ADR-003 — A failed sync ends the writer session

**Status:** accepted (2026-09-22) · **Affects:** writer state machine, CPP-AC07

## Context

`write(2)` can succeed while the subsequent sync fails. PRD §1 requires that such records are not
acknowledged, that the error is surfaced, and that the writer stops. The open question was whether
a failed sync may be retried in place.

## Decision

A failed `SyncData` moves the writer to a terminal `unknown_durability` state. Every later call
on that writer fails immediately with the original error as context; the writer never retries the
sync and never acknowledges anything again. Recovering requires closing the writer, running
`verify`/`recover`, and opening a fresh writer.

## Rationale

On Linux a failed `fsync`/`fdatasync` may discard the dirty page it failed to write, and the error
is reported once and then cleared. A retry can therefore return success while the data is
permanently gone — the retry would manufacture a false durable acknowledgment, which is the single
failure this project exists to rule out. Treating the session as poisoned is the only state that
does not lie.

## Consequences

- `durable_through_sequence` stops advancing at the last successfully synced record, which is
  exactly the value the crash-test oracle compares against.
- Records written but not synced are left on disk. They are legal unacknowledged extras: recovery
  may surface them, and the acknowledged-prefix invariant still holds.
- Callers must distinguish "this record failed" from "durability is unknown from here on". The
  error kind `unknown_durability` carries that meaning and is one of the PRD's required result
  classes.
