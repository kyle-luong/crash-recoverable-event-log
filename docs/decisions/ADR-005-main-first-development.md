# ADR-005 — Main-first development, no routine branches or pull requests

**Status:** accepted (2026-10-26) · **Affects:** repository workflow, verification discipline
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

A commit message carries what a PR body would have: scope, behavior, acceptance IDs, the
verification command and its result, known limits, and any decision needing the owner.

Short-lived branches remain available for a genuine exception — an experiment likely to be thrown
away, or a change the owner wants to read before it lands — and fast-forward back into `main`.

## Consequences

- Less ceremony per change, and the history reads as a straight line of verified milestones.
- CI keeps a real job: it is the independent check that a commit builds and passes on Linux in a
  clean environment, now running *after* the commit rather than gating it. A red `main` is
  therefore possible in principle; the mandatory pre-commit gate is what makes it unlikely.
- `main` cannot be force-pushed and pushed history is not rewritten, so a bad commit is fixed by a
  follow-up commit or a revert, never by amending what is already published.
- Branch protection requiring a PR must stay **off** in repository settings, or this flow is
  blocked at the first commit.
