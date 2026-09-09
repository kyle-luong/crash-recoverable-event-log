# Agent instructions

Read `docs/PRD.md`, `docs/STATUS.md`, and `docs/PLAN.md` before changing code.
Inspect existing code and tests before planning work.
Keep each change within the active task's acceptance criteria and non-goals (`docs/PLAN.md`).

## Verify

- `./scripts/verify.sh` — format check, `-Werror` build, ASan+UBSan Debug, full CTest suite. Run it
  for every change.
- `./scripts/verify-full.sh` — adds the Release build and the slow exhaustive recovery sweep. Run it
  before a significant PR or milestone commit.

Never weaken or remove a test, and never change an acceptance criterion, to get a pass. If a test
contradicts `docs/PRD.md`, stop and resolve the contradiction (workflow §2 source-of-truth order).

## Non-negotiable domain rules

- A record is **durable only after `FileOps::SyncData` returns success** (ADR-001). Never report
  `durable_through_sequence` ahead of that. `accepted` ≠ `durable`; keep them separate in code,
  output, and prose.
- A failed sync is **terminal** for that writer (ADR-003). Never retry a sync; never acknowledge
  after one fails.
- Recovery truncates **only** a structurally incomplete trailing record, and only when explicitly
  requested (ADR-002). A complete record with a bad CRC, an invalid full header, or a sequence
  gap is hard corruption — report it with its offset; never silently discard it.
- Validate payload length against both the 1 MiB bound and the bytes remaining in the file
  **before** allocating any payload buffer.
- `SIGKILL` is not power loss, and `verify` is read-only. Do not let either claim drift in docs.
- Binary format v1 is frozen. An incompatible change needs a new version number and human approval.

## Safety

Treat filesystem paths, process execution, and crash injection as dangerous: write only inside
designated scratch directories, never follow a symlink for a destructive operation, and refuse to
overwrite a nonempty file without an explicit flag.

Keep `main` untouched; work on a task branch; do not force-push or merge.
Human approval required for architecture or format changes, new dependencies, credential use,
remote or destructive operations, and scope expansion.

Before handoff, update `docs/STATUS.md` with what changed, the exact verification run, failures,
the next step, and the branch and commit identifiers.

## Code conventions

C++20, no third-party dependencies. RAII for every OS resource; no raw `close`/`free` in business
logic. Errors return `Result<T>`/`Status` carrying syscall name, `errno`, and byte offset — no
exceptions for control flow, no bare `bool`. Explicit little-endian load/store helpers only; never
`memcpy` or `reinterpret_cast` a struct to or from the file format.
