# eventlog — a crash-safe append-only event log

A small C++20 append-only binary log that aims to survive interrupted writes according to an
explicit recovery contract, and then to quantify what stronger durability actually costs.

**Status: scaffold only.** No record format, writer, reader, or recovery exists yet. This commit
establishes the toolchain, warnings, sanitizers, test harness, verification scripts, and CI. The
CLI lists its intended commands and refuses all of them.

Read [docs/PRD.md](docs/PRD.md) for the product contract and [docs/PLAN.md](docs/PLAN.md) for the
milestones and the verification command that gates each one.

## Build and verify

Requires CMake 3.20+ and a C++20 compiler. No third-party dependencies.

```sh
./scripts/verify.sh        # format check, -Werror build, ASan+UBSan, tests
./scripts/verify-full.sh   # the above plus a Release build
```

## Guarantees, stated up front

These boundaries are fixed before the code exists, so that no claim can drift to fit a result:

- A record will be reported durable **only** after the platform's durability barrier returns
  success for it — `fdatasync(2)` on Linux, `fcntl(F_FULLFSYNC)` on macOS
  ([ADR-001](docs/decisions/ADR-001-durability-primitive.md)). Bytes handed to `write(2)` are
  *accepted*, not durable.
- Recovery will truncate **only** a trailing record that is a strict byte-prefix, and only when
  explicitly asked ([ADR-002](docs/decisions/ADR-002-recovery-contract.md)). A complete record with
  a bad checksum is an error, not something to quietly discard.
- **A process crash is not a power failure.** `SIGKILL` does not clear the kernel page cache or cut
  power to a drive. Power-loss durability is out of scope and will not be claimed.
- CRC32C detects accidental corruption. It is not authentication.

No performance number will appear in this repository until it has been measured by the procedure in
[benchmarks/README.md](benchmarks/README.md).
