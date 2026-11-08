# Changelog

## 1.0.2 - 2026-11-08

### Workspace and verification

- Add per-project output-folder Preview, Approve, Revoke and recheck actions. New Manifest Runs, project context and follow-up planning respect approved exclusions; already-started work retains its recorded scope.
- Show recorded workspace exclusions, scope drift and post-capture provenance diagnostics in Run Details. Approvals do not sandbox external build tools and are not yet consumed by the execution-profile path.
- Preserve the user's CMake presets and confirm the Ninja generator from configuration/cache evidence; refuse unsupported or unconfirmed generators instead of silently choosing another one.
- Decode baseline paths as UTF-8 and report the offending file in baseline-limit diagnostics. Contain C++ Run-execution exceptions while preserving finalization, usage, cancellation priority and valid UTF-8 diagnostics; abort/assert/SEH failures are outside this boundary.
- Normalize supported Planner deferred-read conflicts and align repair feedback while retaining ownership and verification gates.

### Run and provider diagnostics

- Preserve bounded provider-turn metadata and make a lost turn-record write visible in Worker/Planner diagnostics and Manifest/Profile reports.
- Explain why a bounded verification repair did not open a session, without changing the original acceptance verdict. Keep diagnostic truncation on UTF-8 character boundaries.
- Resend trusted OpenAI session instructions on every request, including tool-result turns.
- Distinguish structured OpenAI exhausted-credit/quota refusals from temporary rate limiting in both provider paths, and veto transport retries for those durable refusals. Configured fallback policy remains unchanged.

### Cost estimates and project memory

- Update the untouched Sol catalog seed and retain custom pricing. Stored historical amounts are not rewritten.
- Preserve price completeness in chat, comparison, compaction history, and project estimates; clear unavailable reads instead of showing stale totals.
- Include stored memory-compaction amounts in project cost estimates, with a separate execution/compaction breakdown.
- Retain reported usage from a successful response received before cancellation. Expose failed or unconfirmed persistence separately from confirmed saved accounting.
- Add SQLite write-certainty outcomes and connection quarantine. An accounting fallback is admitted only after non-commit and a healthy connection are established; it does not replace the primary failure.
- Require a confirmed new durable intent before dispatching a compaction request. Final records close matching intents transactionally. Owning-database startup recovery inspects unfinished protocols without resending provider calls or creating speculative financial entries.
- Show unfinished-protocol coverage and database startup-recovery details independently from monetary completeness and history-read errors.

### Upgrade and verification notes

- Database schema advances from the published 1.0.1 package's **70** to **75**. A verified pre-migration database backup is required by the application. Make a separate backup of the entire profile before updating; older executables may not read an upgraded database.
- SQLite 3.44.0 or newer is required. Release dependencies use SQLite 3.53.1; Windows MSVC x64 remains the distributed target.
- The complete product stack passed 64 Debug and 64 Release tests locally and in each repository's PR CI before release preparation. The release verification attachment records the checks performed on the exact 1.0.2 tree.
- An intermittent memory-history layout test failure remains unresolved. No claim is made that these successful runs establish long-term stability, provider billing/delivery accuracy, power-loss recovery, or a clean-machine GUI acceptance result.
- See the [1.0.2 upgrade and testing guide](docs/releases/1.0.2.md) before testing against existing data.

### Maintenance

- Group Qt-free application services/contracts and desktop presentation components by responsibility. The release retains the existing layer boundaries rather than moving orchestration or SQL into QML.

## 1.0.1 - 2026-10-24

### UI performance

- Pages are created on their first visit and retained afterwards. Selection, entered text and page state survive navigation.
- Hidden pages retain their last geometry and scale instead of recalculating their entire contents on every window resize. They catch up when shown again.
- Automatic interface scaling follows frame updates instead of waiting for a 100 ms resize pause. The existing scale formula, element sizing, Interface scale setting and 720 x 480 minimum window size are preserved.
- The project-memory segment list creates delegates for the visible area and a cache margin instead of creating the entire list. Full summary text remains in the model.
- Navigation icons use a fixed decoded resolution, avoiding image reloads whenever the interface scale changes.
- Navigation from Projects and Chats to Runs supports loading the destination page on demand.

### Internal structure and maintenance

- Split the large SQLite persistence implementation into database/schema support and focused repositories for catalog, projects, settings, conversations, planning, execution, verification, proposals, agents and comparisons.
- Split public SQLite declarations into module headers while retaining SqliteRepositories.hpp as a compatibility facade. Database schema, migration order and persistence contracts are preserved.
- Added focused persistence contract coverage and corrected a concurrent budget test to account for already-admitted responses completing after the turn limit is reached.
- Removed unused archived implementation snapshots and assets; refreshed architecture comments and documentation.
- Clarified the next-run draft workflow and the experimental status of Comparison in the README.

### Upgrade and verification notes

- Database schema remains **70**, the same as 1.0.0. Back up the application-data folder and extract the update into a new directory.
- Lazy loading moves page-construction work from application startup to the first visit. A hidden page may still need a layout update when reopened after a scale change.
- Resize improvements were checked locally, including manual acceptance on the developer's machine. Results depend on the page, data volume, build configuration and hardware; they are not a fixed frame-rate guarantee.
- The portable executable remains unsigned. Supported real-provider adapters and project-verification toolchain requirements are unchanged.

## 1.0.0 - 2026-10-22

- Initial public release of AI Orchestrator for Windows x64: reviewed multi-model workflows, project readiness, proposals and explicit Apply, persistent Runs/chats/project memory, and next-run planning drafts.
- OpenAI and Anthropic execution adapters, a deterministic Mock workflow, and release-validated MSVC x64/CMake-preset verification for C++ and Qt/QML projects.
