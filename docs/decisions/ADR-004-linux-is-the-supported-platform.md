# ADR-004 — Linux is the supported platform; macOS is a convenience path

**Status:** accepted (2026-10-26) · **Affects:** durability guarantee, verification evidence,
benchmark comparability · **Amends:** ADR-001

## Context

ADR-001 established one `FileOps::SyncData` boundary mapped per platform, and listed
"Linux-only, develop in a VM/container" as an alternative rejected *for now*, to be reconsidered
if the cost of the second platform ever rose. Two things have changed:

1. `docs/PRD.md` §0 names the target as "C++20 / Linux (POSIX file APIs)" and writes every
   durability acceptance criterion in terms of `fdatasync(2)`. A macOS result therefore never
   satisfies CPP-AC06 or CPP-AC07 literally, only by analogy.
2. The repository owner has Docker available, which removes the objection that a Linux run would
   add a required virtualization dependency to the local loop. Linux verification is now cheap.

The upcoming milestones are also where the second platform stops being free. M3 injects process
crashes; M4 publishes timing numbers. Both would otherwise need a per-platform story — and a
`F_FULLFSYNC` number sitting next to an `fdatasync` number invites exactly the comparison ADR-001
says is not supported.

## Decision

**Linux is the only supported platform.** Concretely:

- `./scripts/verify-linux.sh` runs the full suite in a Linux container. Only a Linux result is
  verification evidence; `docs/STATUS.md` records the Linux run, and a macOS run is noted as a
  convenience check or not at all.
- All measured numbers (M4) come from Linux. No macOS timing is published, so the
  non-comparability of the two barriers stops being a thing a reader has to be warned about.
- macOS remains an **unsupported convenience path** for the fast local edit loop. It is kept only
  while it stays cheap: today that is the `F_FULLFSYNC` mapping in `src/file_ops.cpp` and the
  LeakSanitizer guard in `scripts/verify.sh`, roughly fifteen lines in total. If a future milestone
  would need materially more than that — a second crash-injection mechanism, a second timing
  story — the macOS path is dropped rather than grown.

The guarantee itself does not change. `SyncData` is still the single barrier, still mapped as
ADR-001 specifies, and the acknowledgment rule is still that no record is durable until it returns
success. What changes is which platform's result *counts*, and that macOS is no longer described as
a supported target anywhere.

## Consequences

- The PRD's named primitive is the one actually exercised by the result we report. The analogy
  step disappears from the durability argument.
- Local iteration stays fast on the dev host, with the verification gate moved to the container.
  The container run is slower, so `verify.sh` on macOS remains useful as a first filter — but a
  macOS pass alone is never grounds to commit.
- One platform's numbers means M4 needs no cross-platform comparability caveat.
- Risk accepted: a macOS-only regression can land, because the convenience path is not gated. It
  is unsupported, so this is a cost to the dev loop, never to a stated guarantee.
- `docs/PRD.md` needs no change; this decision moves the repository *toward* it, not away.

## Alternatives rejected

- **Keep both platforms as equals.** Means a per-platform story for crash injection and
  benchmarks, and a permanent comparability warning, to support a platform nothing is deployed on.
- **Drop macOS entirely now.** Would cost the fast local loop for no gain while the macOS code is
  still about fifteen lines. Revisit when that stops being true.
