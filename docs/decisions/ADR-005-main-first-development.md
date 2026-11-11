# ADR-005 — Main-first development, no routine branches or pull requests

**Status:** accepted (2026-10-26), corrected 2026-11-10 · **Affects:** repository workflow,
verification discipline
· **Supersedes:** the feature-branch and pull-request bullets in
`docs/00_SHARED_AGENTIC_ENGINEERING_WORKFLOW.md` §7

## Context

The shared workflow §7 was written for a flow with feature branches, a protected `main`, and a PR
per task carrying scope, acceptance IDs and test evidence. On a single-owner repository with no
second reviewer, that PR is opened and merged by the same person who wrote the change: it adds
ceremony without adding a reviewer. Milestone 1 shipped on `feat/CPP-01-04-log-core` and the branch
never served a purpose beyond holding commits that were always going to land.

The one thing the PR flow did provide is a gate — a place where CI had to pass before code reached
`main`.

## Decision

Commit verified work directly to `main`. No routine feature branches, no routine pull requests.

The gate moves rather than disappearing: **`./scripts/verify.sh` must pass before every commit, and
`./scripts/verify-linux.sh` before every milestone commit.** With no PR to fail, local verification
is the only thing protecting `main`, so it is mandatory, not advisory. A known-broken intermediate
state is never committed; one larger verified commit is preferred to a sequence that leaves `main`
red in the middle.

Commit messages are a subject line and nothing else — no bodies. The detail a PR body would
have carried (scope, behavior, acceptance IDs, verification results, known limits, decisions
needing the owner) goes to `docs/STATUS.md` and the ADRs instead.

Short-lived branches remain available for a genuine exception — an experiment likely to be thrown
away, or a change the owner wants to read before it lands — and fast-forward back into `main`.

## Correction (2026-11-10)

This ADR originally said a commit message "carries what a PR body would have", relocating the
old §7 pull-request checklist into commit bodies. That was never the repository owner's
instruction — it was inferred from the PR requirement this ADR deletes, and it contradicted a
standing preference for short commit messages. Commit bodies are not used at all; the text above
reflects the corrected rule, and the existing history was rewritten to subject lines only.

## Consequences

- Less ceremony per change, and the history reads as a straight line of verified milestones.
- CI keeps a real job: it is the independent check that a commit builds and passes on Linux in a
  clean environment, now running *after* the commit rather than gating it. A red `main` is
  therefore possible in principle; the mandatory pre-commit gate is what makes it unlikely.
- `main` cannot be force-pushed and pushed history is not rewritten, so a bad commit is fixed by a
  follow-up commit or a revert, never by amending what is already published.
- Branch protection requiring a PR must stay **off** in repository settings, or this flow is
  blocked at the first commit.
