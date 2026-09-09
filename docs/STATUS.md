# Current state

Milestone: **M0 complete** | Active task: M1 CPP-01 (record codec) not started
Branch: `main`
Last known good commit: the scaffold commit on `main`
Status: ready for the first feature task

## Done

CPP-00 scaffold only. CMake C++20 project with a strict warning set promoted to errors, a Debug
build with AddressSanitizer and UndefinedBehaviorSanitizer configured to fail rather than log, a
Release target, `clang-format` enforcement, a dependency-free test harness, `./scripts/verify.sh`
and `./scripts/verify-full.sh`, and a GitHub Actions job that runs the same script CI and local
development share.

Specification validated against `docs/PRD.md` before any implementation
(`docs/SPEC_VALIDATION.md`): outcome `spec-ready` with four recorded resolutions, two of which
became ADRs because they fix a guarantee rather than an implementation detail.

Placeholder CLI listing the intended commands. Every one exits with a usage error naming the
milestone that will implement it, so nothing can be mistaken for working.

## Evidence

`./scripts/verify.sh` PASS on 2026-09-09, macOS 15 / arm64, Apple clang 17, CMake 4.4, APFS.
`test_scaffold` covers 8 cases over the error and result types and the scratch-directory
discipline, so a passing verification means something before any log logic exists.

## Not done

Everything in M1 onward: record codec, writer, reader, recovery, CLI, fault injection, crash
harness, benchmarks. See `docs/PLAN.md`.

## Known issues and limits

`fdatasync` is unavailable on the macOS development host, so the durability barrier is a documented
per-platform mapping (ADR-001) and CI on Linux is what exercises the primitive the PRD names.
Benchmark numbers will therefore not be comparable across the two platforms.

`clang-format` versions differ between the macOS toolchain and the Ubuntu CI image, so the CI
format check is advisory and the local check in `verify.sh` is the enforcing one.

## Next action

Implement M1 CPP-01: explicit little-endian v1 encode/decode with CRC32C, verified against published
CRC-32C vectors and fixed golden byte vectors, plus the negative parser cases in CPP-T02 and CPP-T03.

## Human decision needed

None.
