# ADR-001 — One `SyncData` boundary, mapped per platform

**Status:** accepted (2026-09-08), amended by [ADR-004](ADR-004-linux-is-the-supported-platform.md)
(2026-10-26) · **Affects:** durability guarantee, benchmark comparability

## Context

`docs/PRD.md` defines the acknowledgment contract in terms of one syscall: a record is durable
only after "the relevant `fdatasync` completes successfully". `fdatasync(2)` is Linux/POSIX.
Development happens on macOS/arm64, where it is not available in the public API, and where plain
`fsync(2)` flushes the filesystem's cache to the device but does **not** force the device to flush
its own volatile write cache. Using `fsync` on macOS and calling the result equivalent to Linux
`fdatasync` would overstate the guarantee.

## Decision

All durability goes through a single narrow operation, `FileOps::SyncData(fd)`:

| Platform | Call | Notes |
|---|---|---|
| Linux | `fdatasync(fd)` | Exactly the PRD contract. Skips inode metadata not needed to read the data back. |
| macOS | `fcntl(fd, F_FULLFSYNC)` | Forces the drive to flush its write cache — stronger than `fsync`, and much slower. |
| macOS fallback | `fsync(fd)` | Only when the filesystem rejects `F_FULLFSYNC` with `ENOTSUP`/`EOPNOTSUPP`/`EINVAL`. The weaker guarantee is recorded in the result. |
| Other POSIX | `fsync(fd)` | Conservative default. |

The acknowledgment rule itself is unchanged and platform-independent: no record is reported durable
until this call returns success. A failure makes the writer's durability state **uncertain**, not
failed-and-retryable — see ADR-003.

Separately, a log that holds no records yet syncs the containing directory once, before the first
record can be acknowledged, so the file's directory entry is durable too. Two cases need it: a file
this `Open` created, and a file that already existed empty — in the second, nothing else in that
file's lifetime would ever sync the entry.

## Consequences

- The guarantee is honest on both platforms, but **not identical** on both. `F_FULLFSYNC` is a
  stronger barrier than `fdatasync`, so macOS numbers will be slower and are not comparable to
  Linux numbers.
- Every benchmark and crash-test result records OS, filesystem, and which primitive was used.
  Cross-platform throughput comparison is explicitly not supported.
- CI runs on Linux, so the PRD's named primitive is the one exercised by automated verification.
- Still out of scope, per PRD §1: hardware that lies about flushing, filesystem bugs, and real
  power loss. `SIGKILL` does not clear the kernel page cache and is not a power-failure test.
- **Amended 2026-11-12.** The directory sync had been conditioned on this `Open` having created the
  file, which skipped a log that already existed empty — `touch log && eventlog append`. The rule
  stated above is what the code now does. The guarantee it supports is unchanged; only its trigger
  was too narrow, and it could only have mattered under power loss, which is out of scope anyway.

## Alternatives rejected

- **`fsync` everywhere.** Simpler, but weaker than the PRD contract on Linux (and misleading on
  macOS) for no measured benefit.
- **Linux-only, develop in a VM/container.** Would match the PRD exactly, but adds a required
  virtualization dependency to every local `verify.sh` run. Rejected for now; reconsider if the
  macOS path ever diverges in behavior rather than only in cost.
  **Later accepted** — see [ADR-004](ADR-004-linux-is-the-supported-platform.md). The mapping table
  above still stands and `SyncData` is unchanged; what changed is that only a Linux result counts
  as evidence, macOS is no longer a supported target, and no macOS timing is published.
