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

See the [project README](../../README.md) for the architecture overview, supported workflows,
known limitations and release packaging.

## Build and test

Prefer the unified repository preset from the repository root:

```powershell
cmake --fresh --preset x64-debug
cmake --build --preset x64-debug
ctest --preset x64-debug
```

For Backend-only work, presets are in `First_AI_Agents/CMakePresets.json`,
including the legacy `windows-vcpkg` configuration.

Ordinary tests are deterministic and network-free. Manual provider smoke tests
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
