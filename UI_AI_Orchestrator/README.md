# AI Orchestrator Desktop

Qt 6 Quick/QML desktop composition for the sibling AI Orchestrator Backend.

QML communicates through `BackendBridge` controllers/models. Persistence,
provider calls, workspace access, and orchestration rules stay outside QML.

## Cost and recovery views in 1.0.2

Overview uses `ProjectCostController` for the recorded-token metric, monetary
composition, provider rows, and separate compaction coverage. A failed monetary
read clears the result and closes stale provider details. Coverage can be
unavailable while the successfully read monetary result remains visible.

Chat reads its cost header and model rows from one recorded-usage snapshot.
Project memory has independent history, unfinished-protocol, operation-outcome,
and startup-recovery views. The recovery view is read-only and covers the owning
database, not only the selected project. QML displays the Backend's states and
formatted projections; it does not infer saved accounting from diagnostic text.

See the [1.0.2 guide](../docs/releases/1.0.2.md) for the user-facing checks and
known limits. The intermittent memory-history layout test issue remains open.

## Build and test

Local Windows setup: Qt 6.11.1, MSVC x64, Ninja, and vcpkg; hosted CI uses Qt
6.10.3. From the
repository root in a Visual Studio x64 developer environment:

```powershell
cmake --preset x64-debug
cmake --build --preset x64-debug
ctest --preset x64-debug
```

For standalone UI composition, use the same commands from this directory; its
`CMakePresets.json` resolves the sibling Backend through a relative path.

For QML edits, also build/run `all_qmllint` and inspect its output: warnings may
not produce a nonzero exit code. Run the real-QML smoke tests for component,
binding, and lifecycle coverage.

## Source structure

```text
backend/       Qt adapter controllers and models
qml/           compiled QML module, pages, components, Theme and navigation
tests/         Qt adapter and real-QML smoke coverage
assets/        source assets
importedcontent/ optional generated/design input; do not edit casually
```

See the [project README](../README.md) for the architecture overview, supported workflows,
known limitations, data/credential handling and release packaging.
