# AI Orchestrator Desktop

Qt 6 Quick/QML desktop composition for the sibling AI Orchestrator Backend.

QML communicates through `BackendBridge` controllers/models. Persistence,
provider calls, workspace access, and orchestration rules stay outside QML.

## Build and test

Verified Windows setup: Qt 6.11.1, MSVC 2022 x64, Ninja, and vcpkg. From the
repository root in a Visual Studio x64 developer environment:

```powershell
cmake --fresh --preset x64-debug
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
