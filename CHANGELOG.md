# Changelog

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
