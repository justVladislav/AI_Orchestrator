# AI Orchestrator Backend

C++20 Backend for the AI Orchestrator desktop: provider-neutral model access,
application services, audited agent tools, change proposals, SQLite persistence,
and durable workflow execution.

## Main capabilities

- OpenAI, Anthropic, and deterministic Mock provider paths.
- Projects, Runs, Stages, Tasks, Artifacts, Chats, Comparison, profiles, drafts,
  analytics, and project memory.
- Audited read/write workspace tools with bounded policies.
- Immutable reviewable change proposals; no automatic apply.
- Inspect-only Manifest planning, evidence-backed Planner no-op, immutable task
  DAGs, bounded scheduling, exact owned paths, and persisted task state.
- SQLite schema/recovery and Qt-free Backend composition used by the desktop.
- Project cost composition of recorded executions and stored compactions, with
  price completeness separate from unfinished-protocol coverage.
- Durable compaction admission, typed persistence certainty, cancellation usage,
  and owner-only startup recovery that does not repeat provider requests.

See the [project README](../../README.md) for the architecture overview, supported workflows,
known limitations and release packaging.

## Persistence implementation

SQLite implementations live in focused repository modules. The 1.0.2 database
uses schema 75 and requires SQLite 3.44.0 or newer. See the
[SQLite implementation boundaries](sourceFiles/infrastructure/sqlite/README.md)
for ownership of the shared connection, low-level SQL primitives, feature
repositories and the checks required for structural moves.

The [decomposition review and next-step design](../../docs/architecture/sqlite-decomposition.md)
records the completed first extraction, its verification limits, the database
lifecycle/schema split and the subsequent feature boundaries.

## Build and test

Prefer the unified repository preset from the repository root:

```powershell
cmake --preset x64-debug
cmake --build --preset x64-debug
ctest --preset x64-debug
```

For Backend-only work, presets are in `First_AI_Agents/CMakePresets.json`,
including the legacy `windows-vcpkg` configuration.

Ordinary tests use deterministic fakes and temporary storage; some transport tests
use local loopback HTTP, and Windows crash tests use controlled child processes.
They make no paid provider calls. Manual provider smoke tests
may make billable calls and must be launched only with explicit credentials and
intent.

## Credentials

Production providers use the platform credential boundary or environment-based
manual smoke-test configuration. Never commit, log, persist, or place API keys
in prompts/artifacts.

Common manual environment names include `OPENAI_API_KEY` and
`ANTHROPIC_API_KEY`; optional model overrides are documented near the relevant
provider/smoke-test code.

## Safety notes

- Provider/model output is untrusted data.
- Backend-authored capability, workspace policy, Manifest ownership, hashes, and
  budgets are authoritative.
- Rejected proposals remain rejected and auditable.
- `ReadyForReview` is not approval and is not proof of product acceptance.
- Applying a proposal always requires explicit user authorization.
