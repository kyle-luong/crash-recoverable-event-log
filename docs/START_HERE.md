# Three Systems Engineering PRDs — Start Here

**Prepared:** November 9, 2026  
**Status:** Planning documents only. No application code has been built, tested, or deployed.

## What is in this bundle?

| Document | Purpose | Language | Typical scoped MVP |
|---|---|---|---|
| `01_PROCESS_FILE_ACTIVITY_INSPECTOR_PRD.md` | Command-centered filesystem activity inspector + comparison of run outputs | C17 / Linux | 1–2 focused weeks |
| `02_HTTP_FAULT_INJECTION_FRAMEWORK_PRD.md` | Concurrent fault-injection and state-oracle testing of HTTP retries/idempotency | Go | 2–4 focused weeks |
| `03_CRASH_SAFE_EVENT_LOG_PRD.md` | Crash-consistent event log, controlled fault testing, durability/performance benchmark | C++20 / Linux | 3–5 focused weeks |
| `00_SHARED_AGENTIC_ENGINEERING_WORKFLOW.md` | Lean Claude Code–first task lifecycle, verifiers, review, CI, handoffs, safety, tooling decisions | All | Reuse across repositories |

These estimates are indicative for an experienced learner with agent assistance and a tightly bounded MVP. They are not promised delivery times or schedules.

## Recommended build order

**First:** C inspector (learn Linux process & file APIs, produce an immediately useful tool).  
**Second:** Go HTTP fault tester (flagship general-SWE project: networking, concurrency, deterministic injected decisions, automatic correctness checks).  
**Third:** C++ event log (deepest engineering work: resource ownership, persistent file formats, errors, crash recovery, and performance).

The three repos are deliberately independent. Do not force them into a monorepo or invent shared code just for aesthetic consistency.

## Specification-first development rule

Before Claude Code implements a **substantial** task, validate the requirements, invariants, failure cases, and acceptance evidence. Use a short task contract or `docs/features/TASK-ID.md` only when a feature needs durable detail; don't create extra paperwork for tiny fixes. Track relevant PRD acceptance IDs to tests, and obtain approval before changing a material contract. See **“Specification-first gate”** in the shared workflow for the exact template and stop conditions.

## How to start with Claude Code

1. Create a **new** Git repository for the selected project.
2. Copy its PRD to `docs/PRD.md`; copy the shared workflow to `docs/00_SHARED_AGENTIC_ENGINEERING_WORKFLOW.md` so the PRD link remains valid (or rename it and update that link).
3. Create `CLAUDE.md` and `docs/STATUS.md` from the templates in the shared workflow.
4. Ask Claude Code to implement **only task `C-00`, `GO-00`, or `CPP-00`** using the first-task prompt at the end of the relevant PRD.
5. Establish and run `./scripts/verify.sh` before implementing product features.
6. Work one bounded task at a time. Commit genuine milestones directly to `main` after local verification passes (shared workflow §7); use clean-context review for significant changes.

**The PRD is the plan, not a mandate to implement every future idea.** `MUST` acceptance criteria are for the scoped MVP; `LATER` sections are deliberately excluded unless you decide to expand the project.

## What matters for job applications

| Project | Demonstrates | Best demo evidence |
|---|---|---|
| C inspector | Linux system calls, robust process/resource handling, useful developer tooling | A command-impact report that catches an unexpected file change; clear handling of monitoring limits |
| Go tester | HTTP/networking, concurrent test orchestration, idempotency and failure semantics | Unsafe fixture duplicates side effect while safe fixture passes under the same controlled lost-response test |
| C++ log | Memory/resource ownership, binary serialization, error handling, storage correctness, benchmarking | Process-crash acknowledgment invariant + reproducible throughput-versus-sync-policy results |

**Never fabricate numeric results.** Example figures and benchmark ranges in the PRDs are planning illustrations; insert measured numbers only after executing and documenting experiments.

## Key decisions already resolved

- Keep C, Go, C++ as the implementation languages; shell/JSON/YAML for supporting scripts/configuration is fine.
- Make each project **narrow, useful, and technically distinctive**, not a large generic platform.
- Standardize correctness, tests, formatters, warnings, sanitizer/race checks, basic CI, and a concise README.
- Do not add mandatory Kubernetes, Terraform, a dashboard, extensive architecture diagrams, cloud deployment, or custom agent orchestrator.
- Claude Code is the default executor; local tests are the verifier; optional Codex/fresh-context Claude reviews substantive changes; human retains release and remote-push control.
- Favor honest specifications of mechanisms and failure guarantees over impressive but unjustified claims.

## How to read the PRDs

Within each PRD: (1) product outcome and non-goals, (2) CLI and observable behavior, (3) architecture and data contracts, (4) edge/failure model, (5) acceptance criteria and test cases, (6) milestone task graph, (7) definition of done, and (8) starter agent prompt. The shared workflow supplies the cross-project tools, review, handoff, PR, and safety policies.

## Research / authoritative references

Agentic-workflow capabilities and practices were cross-checked against Claude Code documentation (https://code.claude.com/docs/en/common-workflows, https://code.claude.com/docs/en/settings, https://code.claude.com/docs/en/sub-agents, https://code.claude.com/docs/en/hooks), GitHub Actions documentation (https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax), and OpenAI's harness-engineering discussion (https://openai.com/index/harness-engineering/). These sources inform the workflow; implementation details still must be verified by building and testing the actual code.
