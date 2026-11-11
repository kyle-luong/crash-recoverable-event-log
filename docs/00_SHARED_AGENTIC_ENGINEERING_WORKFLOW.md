# Shared PRD — Lean Agentic Engineering Workflow

**Status:** Implementation-ready specification (not implemented)  
**Applies to:** Process & File Activity Inspector (C); HTTP Fault Injection & Testing Framework (Go); Crash-Safe Event Log & Performance Benchmark (C++)  
**Owner:** Solo developer  
**Last specified:** 2026-11-09  
**Authority:** Each project's PRD defines product behavior; this file defines how code is planned, developed, checked, reviewed, and merged.

## 0. Outcome and operating principles

**Objective:** Enable a primary Claude Code session to complete substantial, well-bounded engineering tasks with minimal handholding, high correctness, truthful performance claims, and low recurring cost. A new agent session must be able to recover its task context from the repository without depending on prior chat history.

**Core rule:** Automate execution and verification; retain human authority over scope, architecture, credentials, and merging. Never exchange test quality for passing CI.

Priority order: (1) correctness and safety, (2) understanding and reproducibility, (3) usable scope, (4) performance where relevant, (5) implementation speed, (6) orchestration sophistication. Avoid agent count, generated LOC, commits/day, or token volume as success metrics.

Requirement labels:

- **MUST:** completion gate for the MVP or repository workflow.
- **SHOULD:** recommended; may defer when it adds more maintenance than value.
- **LATER:** explicit extension; not required for MVP.

### Minimum viable setup

Use **ChatGPT** to refine PRDs and architectural tradeoffs; **Claude Code** (existing subscription) as the main planner/implementer; **local deterministic test commands** as verifier; **fresh-context Claude or Codex** as independent reviewer on substantial patches; **GitHub** for commits, PRs, CI, and manual merge. Use one coding session and one active feature branch by default. No custom orchestration server, job queue, background task daemon, or extra subscription is required.

A compact lifecycle:

```
PRD / acceptance criteria
        ↓
Validate task specification → inspect repo → plan bounded task → human approval IF material design change
        ↓
Implement + add tests → run verification → fix (bounded attempts)
        ↓
Fresh-context review (substantial change) → repair + reverify
        ↓
Commit → GitHub PR + evidence → human review → manual merge
        ↓
Update STATUS + capture any reusable lesson
```

## 1. Tool allocation, responsibilities, and cost

| Component                     | Default responsibility                                                                | Use condition                                                                         |
| ----------------------------- | ------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------- |
| ChatGPT Plus                  | PRD development; threat/failure-model discussion; milestone prioritization            | During planning or after design blockers; use included access first                   |
| Claude Code                   | Repository investigation, task plan, code, test, debug, docs, commits, PR preparation | Default main agent; use existing subscription allowance                               |
| Claude Code Plan Mode         | Inspect and propose architecture before changes                                       | Tasks touching public interfaces, persistent data, concurrency, safety, or >3 modules |
| Local tests/scripts           | Mechanical evidence that acceptance criteria hold                                     | Every significant code change; never replace with AI assertions                       |
| Fresh Claude session/subagent | Narrow investigation or independent, evidence-based code review                       | Nontrivial diff, cross-cutting behavior, or uncertainty about correctness             |
| Codex (if accessible)         | Cross-model independent review of difficult C/C++/Go changes                          | Optional; use when its independent review justifies time/cost                         |
| GitHub Actions                | Clean-room build/test verification on PR and main                                     | Mandatory after the first working milestone                                           |
| Git worktrees                 | Isolate genuinely independent parallel tasks                                          | Only if two non-overlapping tasks can be tested and integrated separately             |
| OpenRouter + OpenCode         | Alternative model execution after comparing real task outcomes and costs              | **Do not enable by default**                                                          |

**Tooling adoption rule:** add a new tool only when a concrete problem recurs at least twice and the tool's expected benefit can be measured. Examples: repeated stale-context handoffs, repository-search bottlenecks, or conflicting parallel tasks.

## 2. Repo structure and source of truth

Each project lives in its **own repository**. Commit a copy of the relevant project PRD as `docs/PRD.md` and this shared workflow as `docs/00_SHARED_AGENTIC_ENGINEERING_WORKFLOW.md` to preserve PRD cross-links. A shorter `docs/WORKFLOW.md` is optional later; do not maintain two conflicting copies. Minimal initial structure:

```
repo/
├── CLAUDE.md                 # <= ~80 lines: navigation and non-negotiable rules
├── README.md                 # what, build, run, example, limitations
├── docs/
│   ├── PRD.md                # project specification and acceptance criteria
│   ├── STATUS.md             # one active task, evidence, next action, known blockers
│   └── 00_SHARED_AGENTIC_ENGINEERING_WORKFLOW.md  # shared process
├── src/                      # C/C++ (Go may use package root + internal/)
├── tests/
├── scripts/
│   ├── verify.sh             # stable, quick authoritative command
│   └── verify-full.sh        # slower integration/stress/bench checks as appropriate
├── .github/workflows/ci.yml
└── .gitignore
```

Avoid creating more folders until they contain real code. For Go, prefer idiomatic `cmd/`, `internal/`, or package-root layout **only as justified by package boundaries**. Build artifacts, logs, temporary fault-test directories, captured payloads, and benchmark outputs must be gitignored by default. Preserve compact canonical examples in `tests/fixtures/` and `docs/benchmarks/` as appropriate.

**Source-of-truth hierarchy:** explicit PRD acceptance criteria > agreed architectural decision/contract > tests that faithfully express these > implementation > informal notes. If tests contradict approved requirements, stop and resolve the contradiction; never silently rewrite requirements to match code.

### Suggested `CLAUDE.md` (copy per repository, customize verification command)

```md
# Agent instructions

Read docs/PRD.md and docs/STATUS.md before changing code.
Inspect existing code and tests before planning work.
Keep each change within the active task's acceptance criteria and non-goals.
Run ./scripts/verify.sh; use ./scripts/verify-full.sh before significant PRs.
Never weaken/remove tests or change acceptance criteria solely to get a pass.
Treat filesystem paths, networks, process execution, and crash injection as dangerous:
use only designated local fixtures and scratch directories by default.
Keep main untouched; work on a task branch; do not force-push or merge.
Human approval required for architecture/format changes, new dependencies,
credential use, remote/destructive operations, and scope expansion.
Before handoff, update docs/STATUS.md with changes, exact verification,
failures, next step, and commit/branch identifiers.
```

### `docs/STATUS.md`: minimal resumable handoff

```md
# Current state

Milestone: M1 | Active task: GO-04 (from docs/PRD.md)
Branch: feat/go-04-state-oracle
Last known good commit: <sha or NONE>
Status: in progress / blocked / ready for review
Done: [specific accepted behavior and tests]
Evidence: ./scripts/verify.sh PASS at <date>, <commands/results>
Not done: [small concrete list]
Known issues / failed approaches: [specific diagnosis, not a transcript]
Next action: [one actionable sentence]
Human decision needed: [none or explicit question]
```

The file should usually stay under one screen. For lengthy work, link a task document `docs/tasks/GO-04.md` and keep STATUS succinct. Add `docs/decisions/ADR-001.md` only for durable decisions that change interfaces, data formats, or fundamental guarantees. Do not generate a document for every small choice.

## 3. Task contract and handoff rules

Every task meant to occupy more than one focused coding session has a small contract:

```
Task ID and title:
Why / user-visible outcome:
In scope:
Out of scope:
Dependencies and affected modules:
Acceptance criteria (IDs from PRD):
Failure cases / risks:
How to verify (exact command + observable result):
Decisions needing human approval:
Estimated size: small (hours) / medium (1–2 days) / large (split)
```

### Specification-first gate (required before substantial implementation)

**Project PRD = product contract; feature/task specification = one bounded behavior; implementation plan = how to build it.** These are logically distinct even when a small task uses one Markdown file. For a tiny bug fix, a precise issue plus regression acceptance test is enough. For a normal feature, use a brief section in the task description or a short `docs/features/TASK-ID.md` when work will span sessions. For a new protocol, data format, durability guarantee, or complex subsystem, use a persistent feature specification and approved technical plan. **Do not create a feature file by default for every trivial change.**

Before implementation, the primary agent **MUST** check the task specification for:

1. **Observable behavior:** inputs, outputs, success/failure statuses, and any required side effects.
2. **Preconditions and postconditions:** what must already be true and what will be true after execution.
3. **Invariants and forbidden behavior:** what must never happen, especially during crashes, retries, or concurrent operations.
4. **Failure/edge cases and scope:** resource limits, cancellation, malformed input, and explicit non-goals.
5. **Acceptance evidence:** a named test or inspection for each meaningful requirement, including at least one negative/failure case.
6. **Consistency and feasibility:** no contradictory guarantees, unavailable dependencies, unverifiable requirements, or unjustified implementation constraints.

**Gate outcome:** `spec-ready` if the above is testable and consistent; `blocked` with the exact contradiction or unresolved design choice otherwise. For consequential contract changes, human approval is needed _before_ implementation. A fresh-context specification review is recommended for nontrivial concurrency, data-durability, or external-facing semantics; routine tasks do not require a separate review model.

A proportional feature contract (use only the relevant sections):

```md
# TASK-ID: short outcome

PRD criteria: [GO-AC05, GO-AC07]
Behavior: [what an observer sees]
Preconditions / postconditions: [inputs and guaranteed results]
Invariants / forbidden behavior: [conditions that remain true]
Failure cases / non-goals: [explicit boundaries]
Acceptance evidence:

- GO-AC05 -> TestLostResponseAfterCommit (integration)
- GO-AC07 -> TestSafeRetryNoDuplicate (state oracle)
  Implementation plan: [agent fills in after inspecting repository]
  Open decisions: [none or explicit human decision needed]
```

**Traceability:** Substantial feature changes **MUST** identify the relevant PRD acceptance IDs and their verifying test(s) in the task file or PR. Keep this as a short mapping, not a duplicate project-wide spreadsheet. Reviewers must challenge the tests' ability to prove the requirement—not just check that a test exists.

**Changing specifications:** If implementation reveals a necessary requirement or guarantee change, stop work on the affected behavior. Describe the existing contract, proposed revision, reason, affected acceptance tests, and compatibility risks. Obtain human approval for material semantic/scope changes; update the PRD/feature spec, then the tests, then resume implementation. Never silently relax requirements or tests to match generated code. Small clarifications that preserve behavior can be edited directly with a short note in the task or PR.

**Maximum task size:** aim for one independently reviewable change that takes a human less than ~15 minutes to assess. If a task will likely exceed ~2 days or touch many unrelated areas, split it by independently testable behavior. Do not split one correctness invariant across competing agents unless integration ownership is unambiguous.

**Task states:** `todo → in-progress → verifying → review → done`; `blocked` is separate. Only a test-supported `done` is complete. Resume by reading `CLAUDE.md`, `docs/STATUS.md`, `docs/PRD.md`, `git status`, latest commits, then relevant code/tests. The new session should **not** trust a stale status note over the current repository.

**Long-running sessions:** persist status before finishing or when context is near exhaustion. Prefer a new clean session with repository context over forcing giant chat histories. Claude Code can resume conversations; repo-state recovery remains the authoritative fallback. Do not store secrets or whole agent transcripts in handoffs.

## 4. End-to-end task protocol (agent execution)

1. **Specify and validate:** Select one task ID; check observable requirements, pre/postconditions, invariants, failure cases, non-goals, and acceptance evidence. Mark `spec-ready` or stop on contradictions. Never implement an unapproved broad idea.
2. **Inspect:** Read current source, tests, build system, and interfaces. Report any contradiction with the PRD before editing.
3. **Plan:** Propose touched files, data/control flow, expected failures, verification commands, and rollback. Obtain approval for schema/protocol/architecture changes or dependency additions; routine choices need no approval.
4. **Implement:** Work on a feature branch. Write the minimal change and tests. Avoid speculative helper layers, service abstractions, frameworks, or extra features.
5. **Verify:** Run formatter/linter, compile, unit tests, key integrations and fault tests. The same `verify.sh` should run locally and in CI. Run full verification at milestones/PRs.
6. **Repair:** Investigate the failing assumption, fix root cause, rerun checks. Maximum **two autonomous repair cycles per distinct persistent failure**; if still failing, report evidence and escalate rather than looping or deleting tests.
7. **Review:** For substantial changes, obtain a fresh-context review with only the task contract, PRD section, diff, and test evidence. Reviewer reports _actionable defects_ with file/line, impact, and reproduction—not stylistic preferences.
8. **Reverify:** Apply substantive findings and rerun appropriate checks. A review does not replace deterministic verification.
9. **Deliver:** Commit logically, open a PR with concise summary, exact tests run, risks/limits, and links to benchmark artifacts when relevant. User merges manually after CI passes.
10. **Learn:** Update `STATUS`, and if a real failure reveals a recurring issue, improve a specific test or short instruction. Avoid accumulating generic advice.

**Stop/escalate immediately** if: the PRD is contradictory, a test would require live external destructive calls, production credentials are needed, the action touches data outside sandbox/workspace, repeated repairs regress other tests, a critical invariant cannot be explained, or performance claims cannot be reproduced.

## 5. Delegation and independence

Default **one primary Claude Code agent**; verifier is a script, not another model. A read-only subagent can inspect an API, review alternatives, or summarize a large subsystem in separate context without editing files. Use a fresh reviewer after a substantive change, particularly code involving atomics, concurrent HTTP, process signals, or durable writes. A reviewer should not be asked to grade its own implementation blindly.

Parallelize only when tasks are independent by interface and file ownership (e.g., a documentation update versus independent benchmark harness). Each agent gets its own worktree/branch and an explicit output contract. One integrator owns merges and cross-feature invariants. Never assign two agents simultaneous edits to the same core module. Use worktrees only if measurable cycle-time savings outweigh setup and conflict costs.

Codex is optional for second opinions and not required for every small patch. If unavailable, use a _fresh_ Claude session or another human review. Do not claim cross-model review happened unless it actually did.

## 6. Automated checks and quality bars

| Frequency               | Required checks                                                                 | Notes                                               |
| ----------------------- | ------------------------------------------------------------------------------- | --------------------------------------------------- |
| Every edit batch        | Format, compile, targeted unit test                                             | Fast feedback; agent repairs autonomously           |
| Every task completion   | `./scripts/verify.sh`; key negative test                                        | No unchecked task completion                        |
| Before significant PR   | `./scripts/verify-full.sh`; sanitizers/race/fault integration                   | May take longer; results recorded                   |
| Before claiming speedup | Dedicated release benchmark vs baseline, fixed environment, multiple runs       | Never measure sanitizer builds as normal throughput |
| In PR CI                | Clean checkout, build, fast tests, static checks; race/sanitizer as appropriate | `GITHUB_TOKEN` read-only; no secret-dependent tests |

**C:** compiler warnings (`-Wall -Wextra -Wpedantic -Werror` where practical), `clang-format`, AddressSanitizer/UBSan, integration tests that run actual child processes. **Go:** `gofmt`, `go vet ./...`, `go test ./...`, `go test -race ./...`, timeouts and resource cleanup. **C++:** CMake Debug+ASan/UBSan, warnings, unit/recovery tests, separate Release benchmark; use RAII, explicit ownership, no unnecessary hand-written allocators.

Avoid arbitrary test-count or coverage quotas; use acceptance coverage: all documented invariants, boundary cases, and deliberate failure conditions must be tested. Flaky tests are defects, not acceptable background noise. If CI is flaky, isolate the environmental cause or remove unstable test from gating while retaining an explicit tracked issue; do not weaken its assertions silently.

## 7. CI and Git policy

- **Main-first (trunk-based).** Commit verified work directly to `main`. No routine feature branches
  and no routine pull requests. `./scripts/verify.sh` must pass before every commit — on a
  trunk-based flow the local verification gate *is* the protection for `main`, so it is not
  optional. Use a short-lived branch only for a genuine exception (an experiment likely to be
  thrown away, or a change needing review before it reaches `main`), and fast-forward it back.
- Commits represent genuine milestones or coherent changes; no contrived commit splitting.
- Because `main` is the integration point, never commit a known-broken intermediate state. Prefer
  one larger verified commit over a sequence that leaves `main` red in the middle.
- Workflow uses `push` to `main` and `pull_request`, with no credentials for internet APIs. Set
  `permissions: contents: read` by default. CI remains the independent check that the commit builds
  and passes on the primary platform in a clean environment.
- Do not force-push `main` or rewrite pushed history.
- **Commit messages are a subject line and nothing else.** No bodies. A subject is under ten
  words, descriptive, with no conventional-commit prefix and no tool name. Scope, behavior,
  acceptance IDs, verification results, known limits and decisions belong in `docs/STATUS.md` and
  the ADRs, where they stay editable and are read on purpose — a commit body is none of those
  things, and on a rewritten or rebased history it is the first thing to go stale.
- Do not run privileged network requests or arbitrary untrusted shell scripts in CI; use local
  controlled fixtures and temp dirs.
- Do not auto-merge/deploy. Ask before the first remote push of a repository.
- **One primary platform per project.** The PRD names it; it is the platform whose syscall contract
  the acceptance criteria describe and the only one whose verification result counts as evidence. A
  convenience path on a second platform is allowed while it stays cheap, must never widen or restate
  a guarantee, and is not a supported target.

Example `.github/workflows/ci.yml` (adapt job name and dependencies):

```yaml
name: CI
on:
  pull_request:
  push:
    branches: [main]
permissions:
  contents: read
jobs:
  verify:
    runs-on: ubuntu-latest
    timeout-minutes: 15
    steps:
      - uses: actions/checkout@v4
      - name: Verify project
        run: ./scripts/verify.sh
```

For a real repository, pin third-party GitHub Actions by immutable SHA when feasible and add toolchain installation/version pinning as required. Keep test artifacts small and avoid uploading captured request payloads with secrets.

## 8. Claude Code configuration and native features

**Use now:** a short `CLAUDE.md`; optional `.claude/settings.json` with narrowly scoped approval rules for the project's `verify.sh`, `git status`, and reading files. Claude Code supports project/shared settings, project-local overrides, plan mode, subagents, skills, hooks, and worktree-based workflows. _Configuration is not isolation_: never approve untrusted execution merely because a command matches an allow rule.

**Keep permissions conservative:** no unrestricted shell or filesystem access by default; deny reading private `.env`/credential files; require interactive approval for recursive deletes, shell pipelines from the internet, edits outside repository, remote targets, secret use, `git push`, and GitHub write actions. Use OS-level sandboxing for untrusted script execution—not only agent permission text. Confirm available permission semantics against installed Claude Code version rather than copying broad allow rules.

**Use later, only when justified:** one reusable skill such as `verify-and-handoff` when you repeat it often; a `PreToolUse` safety hook only if recurrent destructive actions remain a problem; optional custom read-only reviewer subagent. Do not create elaborate planner/coder/tester/reviewer agents for these repositories. A local `./scripts/verify.sh` is a more transparent enforcement mechanism than a complex hook chain.

Example `.claude/settings.json` **illustrative** permissions (test against installed version):

```json
{
  "permissions": {
    "allow": ["Bash(./scripts/verify.sh)", "Bash(git status *)"],
    "deny": ["Read(./.env)", "Read(./.env.*)"]
  }
}
```

Do not assume this protects against all shell variants. Escalate to a restricted container/user for dangerous workloads. `/status` and `claude doctor` can help inspect loaded settings. Reference: https://code.claude.com/docs/en/settings and https://code.claude.com/docs/en/permissions.

## 9. Adoption decisions for optional ecosystem tools

| Tool/category                         | Current decision                                                                  | Trigger to reconsider                                                                          |
| ------------------------------------- | --------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------- |
| T3 Code / Paseo                       | Skip initially: UI/session supervision duplicates default single-session workflow | >3 concurrent autonomous sessions frequently need supervision                                  |
| Compound Engineering / GSD            | Skip initially: PRD + STATUS + tests are enough                                   | Repeated lost context or multi-milestone plan drift after several projects                     |
| OpenRouter + OpenCode                 | Skip initially: use existing subscription allowances                              | Measurable tasks-per-dollar benefit after real A/B trials                                      |
| Serena / Morph                        | Skip initially: built-in search and code navigation should suffice                | Large repo or measurable search/edit bottleneck (>20k LOC, repetitive mapping time)            |
| Symphony / issue-driven orchestration | Skip initially: queue is not the bottleneck                                       | >5 independent ready issues, robust CI, and proven safe unattended merges are **not** required |
| Cloud sandbox                         | Optional safety upgrade, not an agent framework                                   | Need to run untrusted scripts or unattended destructive/fault tests safely                     |
| Additional review model               | Optional: fresh context first                                                     | Repeated serious bugs missed by single-model review justify cost                               |

Each adoption experiment should compare task completion time, verified defects, human minutes, and incremental cost across a small set of comparable real tasks. Reject tools that add coordination burden without measurably improving one of these.

## 10. Metrics for workflow health

Record a short line per completed PR in `docs/STATUS.md` or a lightweight `docs/WORKLOG.tsv` once there are enough tasks:

```
TaskID | start/end | accepted? | human_minutes | repair_cycles | CI_failures | escaped_defects | optional_cost_usd
```

Review after 5–10 tasks. The aim is **verified work per hour of human attention**, not commits, models, lines, or test-count inflation. Record failures honestly and translate recurring patterns into one regression test or small instruction update.

## 11. Implementation roadmap

**Day 0 (initial repo):** Commit PRD, `CLAUDE.md`, README stub, `STATUS.md`, compiler/toolchain config, `verify.sh`, CI. Verify clean clone runs the initial check. Avoid skills/hooks/multiple agents.

**First feature (one task):** Use one Claude Code session to implement a narrow accepted capability. Capture handoff. Obtain independent review before merging the first significant code change. Record the exact commands/results.

**After first milestone:** Add `verify-full.sh` for slower fault/stress/sanitizer checks; one smoke benchmark where relevant. Tighten only safety practices shown necessary by real defects.

**After ~5 reviewed tasks:** Inspect failures, CI flakes, human overhead, and subscription usage. Consider a single review/handoff skill, Codex independent review, or an isolated test runner only if they solve an observed problem.

**After first finished project:** Copy the small workflow template into the next repository; don't build a centralized orchestration platform.

## 12. Concrete prompts for the primary agent

**Bootstrap**

> Inspect this empty repository and `docs/PRD.md`. Create only the minimal repo scaffold, `CLAUDE.md`, `docs/STATUS.md`, formatter/build/test scripts, and a GitHub Actions job. Do not implement product features. Ensure `./scripts/verify.sh` passes on a clean checkout. Summarize the files and commands created.

**Plan a task**

> Read `CLAUDE.md`, `docs/STATUS.md`, and PRD acceptance criteria for TASK-ID. First validate that the specification defines observable behavior, invariants, failure cases, and acceptance evidence; report gaps or contradictions. Inspect relevant code and tests, then produce a bounded implementation plan: touched files, interfaces, exact verification, rollback, and decisions requiring approval. Do not implement until the specification is ready.

**Autonomous implementation**

> Implement approved TASK-ID, including meaningful tests for success, edge cases, and failures. Run `./scripts/verify.sh`, repair root causes (maximum two repeated-failure cycles), update status, and provide exact evidence. Do not weaken tests or expand scope. If blocked, explain the minimal decision needed.

**Independent review**

> Review this diff independently against TASK-ID and relevant PRD requirements. Check both specification coverage (requirements-to-tests) and implementation correctness, including untested edge cases. Report only concrete correctness/security/resource/concurrency/performance defects or missing critical tests, with file location, impact, and a reproduction path. Do not edit code. Distinguish verified issues from hypotheses.

**Before PR**

> Re-run required verification; show its exact results. Prepare a reviewable PR summary with acceptance IDs, design decisions, limitations, benchmark methodology, and relevant test evidence. Do not merge or deploy; ask for human approval for consequential changes.

**Fresh-session recovery**

> Inspect `CLAUDE.md`, `docs/STATUS.md`, `docs/PRD.md`, `git status`, and recent commits. Reconcile written status with actual repository state, identify the active task, run the smallest relevant verification, and continue only from the last known good state. Escalate any contradiction.

## 13. Documentation and external grounding

The workflow favors repository knowledge as the durable record, bounded execution plans, deterministic verification, and humans directing rather than manually performing every code edit. This reflects the documented principles in OpenAI's February 2026 harness-engineering write-up (https://openai.com/index/harness-engineering/). Claude Code's native configuration, subagent, skills, hook, and worktree functionality is documented at https://code.claude.com/docs/en/settings, https://code.claude.com/docs/en/sub-agents, https://code.claude.com/docs/en/skills, https://code.claude.com/docs/en/hooks, and https://code.claude.com/docs/en/common-workflows. GitHub Actions and branch protection are documented at https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax and https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-protected-branches/about-protected-branches.

These sources support the _capabilities and general practices_, not guaranteed productivity gains for these personal projects. Tool interfaces and subscription access can change; verify details against the installed versions and your entitlements before copying configuration.
