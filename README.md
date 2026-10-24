# AI Orchestrator

A C++20 and Qt Quick desktop application for AI-assisted software work: plan a task, coordinate model workers, inspect proposed changes, verify the combined result, and explicitly approve what reaches your project.

Version 1.0 is primarily validated for **C++ and Qt/QML projects on Windows using MSVC x64 and CMake presets**. Other language and build ecosystems are not release-validated.

Your workspace stays under your control. A model response is a proposal, not permission to modify files.

Version **1.0.1** improves window resizing and page loading, virtualizes the project-memory list, and reorganizes SQLite persistence into focused modules. See the [changelog](CHANGELOG.md) for details.

## Features

- **Multi-model workflows:** OpenAI and Anthropic adapters, execution profiles, task planning and bounded worker scheduling. A deterministic Mock path supports development and tests.
- **Next-run drafts:** turn a goal and an existing Run's outcome into a model-proposed plan, review it, then confirm creation of saved drafts. You choose the execution configuration and launch each Run explicitly.
- **Reviewable changes:** file-level proposals, diffs, recorded verification findings, approval/rejection and explicit Apply.
- **Project readiness:** selected CMake preset checks, automatic child-process MSVC/Windows SDK environment setup, and persisted readiness that is reused while configuration inputs remain unchanged.
- **Persistent work:** projects, Runs, stages, chats, artifacts, proposals and project memory backed by SQLite and local file stores.
- **Usage visibility:** provider turns, token usage, estimated cost and configured execution budgets.
- **Desktop tools:** provider/model management, credentials, project context, chats, experimental comparisons and Run diagnostics.

## Download and launch

Get a Windows portable binary ZIP from [Releases](https://github.com/justVladislav/AI_Orchestrator/releases).
Check the release label: a release candidate is marked as a pre-release. GitHub's automatic **Source code** archives are not executable packages.

1. Download `AIOrchestrator-<version>-win-x64.zip` and its `.sha256` file.
2. Compare the checksum with `Get-FileHash <zip-path> -Algorithm SHA256`.
3. Extract the **whole archive** into a new folder.
4. Launch `appUI_AI_Orchestrator.exe` from the extracted folder.
5. Check **Settings > Diagnostics > Application version**.

The portable application targets Windows 10 1809 or later / Windows 11, x64. It includes its Qt and C++ runtime libraries; you do not need Qt or Visual Studio just to open it. The executable is not code-signed, so Windows may show a SmartScreen warning.

Build tools and project-specific dependencies are needed separately when a Run must configure, build or lint your project.

## Try without an API key (Mock)

1. Create a project pointing to a disposable working folder.
2. Open **New Run**, leave **Run with real providers** unchecked, and enter a small test task.
3. Start the Run and inspect its stages and results. No provider/model setup or API key is required for this path.

Mock uses deterministic simulated responses, not a real AI model, and makes no billable provider requests. It demonstrates the workflow; it does not establish that a requested coding task or a real project build succeeds. Chat and provider connection tests are separate from this Mock path.

## First real-provider workflow

1. In **Providers and Models**, add a provider, save its API credential, add a model and enable the provider/model you intend to use.
2. Create a project pointing to an existing working folder. Prefer a version-controlled project with a clean, understood baseline.
3. Choose an execution profile and model configuration. Enable real-provider execution only when you intend to make billable API calls.
4. Describe a small, verifiable change and start a Run. Inspect progress, planning diagnostics and any setup errors.
5. Review the proposed file changes and verification results. Approve and **Apply** only the changes you want.

Real-provider calls use your own account and may incur charges. Configured prices and displayed estimates are not a substitute for your provider's billing records.

A successful build or lint check is not proof that every requested behaviour is correct. Review the diff and exercise the changed feature.

### Plan the next Runs

1. From a completed Run, open **Plan next Run...**, describe the next goal, choose the planning model and how to split the work, and select any available file excerpts to include.
2. Review the quote and included context before authorising the paid planning call.
3. Review the returned plan, task boundaries, dependencies and any contracts. Confirm it to save the proposed **Drafts**.
4. Open a draft in **New Run**, complete or review its lead/worker models, budgets and other settings, then start it yourself. An existing roster template can supply previously chosen settings.

The model generates a plan; drafts are saved only after your confirmation. Planning never starts a Run or applies changes automatically. Providers, models and budgets for execution remain user choices. Failed Runs offer the corresponding **Plan recovery...** action.

### Preparing a project for verification

Version 1.0 supports **MSVC x64 projects using CMake presets**. Install the project's compiler, Windows SDK, Qt kit if applicable, and other dependencies.

In the project card, use **Verification setup** to select a configure/build preset pair and check it. The Backend performs a real configure and confirms the target catalog. If several pairs are available, select one explicitly. The preset's build directory must be inside the project source directory.

A successful result is stored and reused. Configuration/preset/toolchain changes or removal of the build directory invalidate it; ordinary QML/C++ source edits do not. Readiness does not replace verification of the exact proposed changes before Apply.

For a verification folder, choose a short, private, writable path in **Settings > Data and privacy > Verification folder**. On Windows the root is limited to 64 characters to leave room for external build tools. It must not overlap the project or use links/junctions.

The application prepares MSVC/SDK environment variables only for its child build processes. It neither installs an SDK nor changes your global environment. Creating a Qt Quick/Widgets project from a template remains a user/IDE step in 1.0.

## Data, credentials and updates

On Windows, application data normally lives at:

```text
%APPDATA%/AIOrchestrator/AIOrchestrator/
  orchestrator.sqlite
  artifacts/
  proposals/
```

The actual location is resolved through Qt's Windows profile APIs, not by overriding the `APPDATA` environment variable. API credentials are stored in **Windows Credential Manager** for the current Windows account.

Runs send selected prompts and project context to the chosen external provider. Logs, proposals and artifacts may contain project content; inspect and redact them before sharing.

This version has no built-in telemetry or automatic update checker. Application-owned API requests are made through the provider adapters. This is not a network-isolation guarantee: build tools and project dependencies can make their own connections.

To update:

1. Close all application instances.
2. Back up the entire application-data folder, including the database and adjacent artifact/proposal stores.
3. Extract the new version into a **new folder**, not over the old version.
4. Start it and keep the backup until you have checked your projects and history.

Database migrations run on startup. A database upgraded by a newer application is not automatically compatible with an older executable. Versions 1.0.0 and 1.0.1 both use schema **70**; this update adds no database migration.

## Architecture

```text
Qt Quick / QML desktop
  -> Qt controllers and read models (BackendBridge)
  -> Qt-free application services and workflow runners
  -> provider adapters, audited workspace tools, SQLite and file stores
```

The Backend owns scope, task dependencies, budgets, proposal validation and Apply decisions. QML presents state and sends commands; it does not own provider credentials or database access. Model output is untrusted input, not authority to bypass workspace or review policies.

The normal change workflow is planning -> bounded worker execution -> proposals -> combined candidate verification -> human review -> Apply. Runs and attempts retain their diagnostic history; a failed attempt is not overwritten by a later Retry.

### Repository layout

```text
First_AI_Agents/              Qt-free Backend, provider adapters, persistence and tests
UI_AI_Orchestrator/           Qt adapter, QML desktop, assets and UI tests
cmake/                       Shared version and test/build helpers
packaging/                   Portable ZIP, dependency notices and source bundles
.github/workflows/           Windows Debug/Release CI
out/                         Local generated builds/artifacts (ignored by Git)
```

The role files under `First_AI_Agents/First_AI_Agents/prompts/` are functional project inputs, not disposable development notes.

## Build from source

### Prerequisites

- Windows x64 and Visual Studio C++ build tools with MSVC and Windows SDK.
- Qt 6 with the MSVC x64 kit and Qt Quick/Quick Controls modules.
- CMake, Ninja, Git and vcpkg.

The local 1.0 build used Qt **6.11.1**, MSVC **19.51** and CMake **4.4.3**. The Windows CI workflow uses Qt **6.10.3** and its hosted MSVC toolchain. The dependency baseline is pinned in [First_AI_Agents/vcpkg.json](First_AI_Agents/vcpkg.json).

Run the commands below in a **Visual Studio x64 developer terminal**. Automatic environment preparation inside the application is for project-verification child processes, not this source-build shell.

```powershell
git clone https://github.com/justVladislav/AI_Orchestrator.git
cd AI_Orchestrator

# Set these to your installed tools; override the machine-specific preset defaults.
$qtPrefix = 'C:/Qt/6.11.1/msvc2022_64'
$vcpkgRoot = 'C:/c++_libs_tools/vcpkg'

cmake --preset x64-release "-DCMAKE_PREFIX_PATH=$qtPrefix" "-DCMAKE_TOOLCHAIN_FILE=$vcpkgRoot/scripts/buildsystems/vcpkg.cmake"
cmake --build --preset x64-release --parallel 4
ctest --test-dir out/build/x64-release --output-on-failure
```

The built desktop executable is `out/build/x64-release/UI_AI_Orchestrator/appUI_AI_Orchestrator.exe`. Keep its deployed runtime files beside it; copying only the EXE is not a portable distribution.

For Debug, substitute `x64-debug`; `ctest --preset x64-debug` is also provided. See the [Backend README](First_AI_Agents/First_AI_Agents/README.md) and [desktop README](UI_AI_Orchestrator/README.md) for subsystem work and standalone configurations. Other platform presets exist, but Windows x64 is the distributed and release-validated 1.0 target.

## Tests

Ordinary tests are deterministic and do not call paid providers. The suite covers workflow/planner validation, workspace boundaries, proposal review/Apply, persistence, readiness, provider contracts and real-QML loading.

Release test executables retain their `assert()` checks; a dedicated canary detects disabled assertions. The [Windows CI workflow](.github/workflows/windows-ci.yml) builds and tests Debug and Release. Check [Actions](https://github.com/justVladislav/AI_Orchestrator/actions) for the result of the specific commit you use.

Optional real-toolchain tests can be enabled with `AIO_TEST_QT_PREFIX` (MSVC Qt kit) and `AIO_TEST_QT_MINGW_PREFIX` (unsupported-kit refusal). They invoke local tools, not provider APIs.

For QML changes, also inspect the `all_qmllint` output and run the real-QML tests. Fix a reproduced defect with a regression test; do not relax scope or verification guards to obtain a green result.

## Release packaging

Use a clean tracked checkout and a completed Release build. The product version comes from [cmake/AIOrchestratorVersion.cmake](cmake/AIOrchestratorVersion.cmake); a stable release has an empty prerelease suffix.

Packaging currently targets Qt 6.11.1 and the pinned curl/SQLite/zlib dependencies. It needs their matching local sources, the Qt SPDX inventories and the compiler's redistributable CRT. If Qt's source `.tag` is a tree hash rather than a commit, supply a Git checkout of the matching upstream commit through `-QtDeclarativeSourceRoot`.

1. Run `packaging/New-SourceBundle.ps1` with `-SourceRoot`, `-BuildDir`, `-OutDir` and `-Version`.
2. Run `packaging/New-PortableZip.ps1` with the same source/build paths, an output directory, and `-SourceBundleDir` pointing to the first output.
3. Publish the binary ZIP/hash **and all three companion source archives/hashes** together. `SOURCE-INFO.txt` identifies the corresponding source.
4. Retain the build provenance and verification report with the release.

Run scripts with process-only `-ExecutionPolicy Bypass` if your shell policy requires it; do not change the machine-wide policy.

The package includes application/dependency licence texts, Qt attribution/SPDX information and build metadata. Packaging checks imports, app-local CRT, forbidden data/debug file types, matching source hashes and every file after ZIP extraction.

The manifest/parser checks are in [Test-ReleasePackaging.ps1](packaging/verify/Test-ReleasePackaging.ps1). The [portable guide](packaging/README-portable.txt) and [clean-machine checklist](packaging/verify/CLEAN-MACHINE-CHECKLIST.md) describe launch and verification. The clean-profile script refuses an existing profile database; do not bypass it on an account holding real data.

## Known limits in 1.0

- Comparison is experimental and partially implemented. Response synthesis and per-result retry are unavailable in 1.0. Automated scores use deterministic text heuristics and should not be interpreted as proof of answer correctness or model quality.
- Real-provider execution implements OpenAI and Anthropic adapters only. The `openai_compatible` and `local` provider kinds may be stored, but their execution adapters are not implemented in 1.0.
- Project verification supports MSVC x64/CMake presets, not MinGW or ARM. Qt project-template generation and a full verification-profile editor are not included.
- The portable executable is unsigned; there is no installer.
- Large single-worker tasks can exceed a provider's output/turn limits. A failed model attempt is not proof that a proposed feature was delivered.
- Verification uses the configured build/lint/test actions; it is not an automatic interactive GUI acceptance test.
- Startup/database-loading and provider-switch responsiveness have further optimisation work.
- Unicode and long-path fixes cover reproduced scenarios; not every legacy path conversion has been audited or changed.
- Some multi-worker/provider recovery scenarios have automated coverage but are not independently live-accepted in every configuration.

## Reporting issues and contributing

For suspected security vulnerabilities, follow [SECURITY.md](SECURITY.md) instead of posting details publicly.

Open an [issue](https://github.com/justVladislav/AI_Orchestrator/issues) with the application version, Windows/toolchain details, reproduction steps, expected behaviour and exact error code. For a Run failure, include its ID and relevant **redacted** diagnostics. Never post API credentials, a private database or an unreviewed project archive.

Keep QML presentation separate from Backend policy. Add focused regression coverage for behaviour changes, document any compatibility/migration impact, and keep generated reports, credentials and local build artifacts out of Git.

## Licence

Copyright (C) 2026 **justVladislav**. Original project material is licensed under **GPL-3.0-only**; see [LICENSE](LICENSE) and [COPYRIGHT](COPYRIGHT).

Third-party code and bundled libraries keep their own copyrights and terms; see [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt) and the package's `LICENCES/` folder. The application's licence does not relicense Qt, Microsoft runtime components or other dependencies.

Distribute the application and dependency source archives alongside binary downloads with the same access permissions. GitHub's automatic application-source archive alone does not include the dependency sources.

The software is provided without warranty to the extent permitted by applicable law; the full licence governs.
