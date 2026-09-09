# ADR-002 — Recovery classifies damage by byte-prefix, not by convenience

**Status:** accepted (2026-09-08) · **Affects:** recovery semantics, `verify`/`recover` exit codes

## Context

`docs/PRD.md` requires that a structurally incomplete trailing record be recoverable by explicit
truncation, and that a fully formed record failing CRC be a hard error "including if it is last".
It does not say which of those two rules governs a trailing record whose 24 header bytes are all
present but whose magic, version, flags, or payload length are invalid. Picking the lenient reading
would let a damaged file be silently shortened.

## Decision

A log is scanned strictly left to right from offset 0 with an expected sequence counter starting
at 1. At each position, with `remaining = file_size - offset`:

| Condition | Classification | Recoverable by truncation |
|---|---|---|
| `remaining == 0` | `clean` | — |
| `0 < remaining < 24` | `incomplete_tail` (partial header) | yes, with explicit opt-in |
| 24 header bytes present, header structurally invalid | `corrupt_complete_record` | **no** |
| `remaining < 24 + payload_length` | `incomplete_tail` (partial payload) | yes, with explicit opt-in |
| full record present, CRC32C mismatch | `corrupt_complete_record` | **no** |
| full record present, `sequence != expected` | `sequence_violation` | **no** |

Truncation targets the offset immediately after the last fully valid record, and only ever
discards a trailing `incomplete_tail`. It happens only under `recover --truncate-tail`, after a
read-only preflight has printed the byte range to be discarded.

## Rationale

The dividing line is the supported fault model, not the shape of the damage. With one writer, a
`SIGKILL`, and a bounded `write(2)` retry loop, the bytes on disk are always a **strict byte-prefix**
of the encoded record: the kernel copies from the caller's buffer, so termination can only stop the
loop between calls, never scramble bytes already accepted. Therefore:

- a prefix shorter than 24 bytes cannot contain a full header, so "full header" and "incomplete
  header" are decidable without guessing; and
- a full 24-byte header that is *invalid* cannot have been produced by the supported model. It is
  evidence of damage from outside that model (media error, unrelated writer, deliberate corruption),
  which the PRD says must never be silently accepted.

This also means CRC validation of a trailing record is not a substitute for the length check: the
length must be validated against both the 1 MiB bound and the bytes actually remaining in the file
*before* any payload buffer is allocated, so a hostile or damaged length cannot cause a large
allocation or an out-of-bounds read.

## Consequences

- `verify` is strictly read-only and exits `0` clean, `1` incomplete tail, `2` hard corruption,
  `64` bad arguments — a caller can distinguish "needs recovery" from "needs a human".
- Mid-log corruption is reported with its byte offset and is not repaired in the MVP. Salvaging a
  prefix of a corrupt-in-the-middle log is deliberately not offered, because it would discard valid
  records after the damage without the operator's knowledge.
- A writer refuses to open a log that is not `clean`; recovery is a separate, explicit step.
- Under real power loss a torn sector *could* produce a full-but-garbage header. This decision
  classifies that correctly (hard corruption) but the project does not claim to test it.
