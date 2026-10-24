# SQLite persistence implementation boundaries

This directory is private to the `AIOrchestrator_Persistence` target. Application
services and the Qt adapter use repository interfaces; they must not include
these implementation headers or access native SQLite handles.

## Ownership

- `SqliteConnection.hpp` defines the private `SqliteDatabase::Impl`. One database
  object owns one native connection and one mutex. Repositories retain that
  object through `shared_ptr`; extracting a repository must not open a separate
  connection or introduce a per-repository mutex.
- `SqlitePrimitives.hpp/.cpp` own statement finalization, error conversion,
  parameter binding and primitive column/time conversions. They must not contain
  feature DTOs, feature SQL, schema definitions or repository-specific JSON.
- `comparison/SqliteComparisonSessionRepository.cpp` owns all Comparison session
  queries, column order and selection JSON encoding/decoding. Its public class
  remains declared in `includes/ai/infrastructure/SqliteRepositories.hpp` for
  source compatibility.
- `catalog/SqliteProviderRepository.cpp`, `catalog/SqliteModelRepository.cpp` and
  `catalog/SqliteExecutionProfileRepository.cpp` own the provider, model and
  execution profile table queries with their row mapping/binding (batch 3). The
  model file also owns the price columns, the long-context tier check on every
  read, the default-model transaction and the in-use check on removal. The profile
  file owns the roster replacement and default-profile transactions and the
  deterministic roster read order. Their public classes stay declared in
  `includes/ai/infrastructure/SqliteRepositories.hpp`.
- `projects/` (batch 4) holds `SqliteProjectRepository`, `SqliteTaskRepository`,
  `SqliteWorkspaceStateRepository` and `SqliteProjectReadinessRepository`: the duplicate id /
  duplicate root path mapping, the exact root path lookup, the task listing order and the
  task-to-run links, the active project singleton, and the per-project readiness record
  with its upsert. The readiness configure mechanism lives in the application layer and is
  not part of this directory.
- `conversations/` (batch 5) holds `SqliteChatRepository` (conversations, messages, the
  prepared-turn transaction and the project context attachment writes),
  `SqliteChatMemoryRepository`, `SqliteProjectMemoryRepository` (including the bounded
  recent page read under one lock) and `SqliteProjectMemoryCompactionRepository` (the atomic
  commit that removes the source segments, inserts the result and records the provenance).
- `planning/` (batch 6) holds `SqliteRunDraftRepository`, `SqliteOutcomeBriefRepository` and
  `SqliteFollowUpPlanRepository`. The run draft group storage that the follow-up confirm also
  calls is declared in the private `planning/SqliteRunDraftGroupStorage.hpp`.
- `roster/SqliteRosterMapping.hpp/.cpp` (the JSON string list and the roster assignment row shared
  by Runs and run drafts) and `execution/SqliteRunStatusMapping.hpp/.cpp` (the stored Run status,
  shared by the Run reader, the work ledger and the Outcome Brief reader) are small private
  helpers; batch 7 will add the Run code to `execution/`.
- `settings/` (batch 4) holds `SqliteApplicationSettingsRepository` (the optimistic-revision
  update and the enum mapping) and `SqliteVerificationScratchSettingRepository` (the
  singleton upsert and the path encoding). Permission and safety checks of the scratch
  folder stay in the service.
- `catalog/SqliteProfileMapping.hpp/.cpp` hold the mapping that execution profiles
  share with the run draft code still in the aggregate: the provider budget override
  JSON and the narrow assignment row. It is a private feature helper, not a
  primitive.
- `SqliteDatabase.cpp` owns `SqliteDatabase` construction, `open()` and `path()`:
  filesystem preparation, native open and PRAGMAs, then one call to `ensureSchema`.
  `pathToUtf8` / `pathFromUtf8` live in the primitives.
- `SqliteSchema.hpp/.cpp` own the private `ensureSchema(sqlite3*)`: the version
  check, the newer-schema refusal and all historical migration steps, plus
  `supportedSchemaVersion` and the follow-up SQL helpers. It is called only from
  `open()`, on a handle that is not yet published and that the caller closes.
- `execution/` (batch 7) holds the Run, execution manifest, workspace scope, manifest task
  execution, work ledger, artifact and run warning repositories. `verification/` (batch 7) holds
  the manifest verification continuation, manual handoff and baseline normalization repositories
  with the verification environment and bootstrap repositories. `agents/` (batch 8) holds the model
  execution (with quality evaluations) and the agent tool session and audit repositories;
  `proposals/` (batch 8) holds the change proposal, approval and proposal review repositories.
  Each file is the class with the helpers only it uses, copied from the former aggregate.
- `SqliteRepositories.cpp` no longer exists: every repository implementation lives in its
  feature directory. The public declarations are split into per-module headers under
  `includes/ai/infrastructure/sqlite/` (`SqliteDatabase.hpp` plus `catalog/`, `comparison/`,
  `projects/`, `settings/`, `conversations/`, `planning/`, `execution/`, `verification/`,
  `agents/` and `proposals/`, grouped like the implementations here). The old
  `includes/ai/infrastructure/SqliteRepositories.hpp` is now a compatibility facade that
  includes all of them, so the earlier statements that a class "stays declared" there still
  hold through that include. Consumers are not migrated yet, and the private
  `SqliteConnection.hpp` still includes the facade.

The CMake target lists these translation units explicitly. The internal directory
is not an exported include path. Do not turn the primitives header into another
aggregate of every feature's persistence implementation.

## Behaviour that a structural move must preserve

Repository methods hold the shared mutex across their complete operation,
including multi-statement transactions and the inspection of `sqlite3_changes`.
Keep connection ownership, statement lifetime, SQL, binding/column order, public
interfaces and observable error codes unchanged when extracting code.

Do not amend applied migrations, increment the schema version, or change
serialization formats merely to reorganise source files. Migration extraction
and changes in database behaviour are separate review scopes.

## Verification

`AIOrchestrator_SqliteComparison_tests` exercises the public repository contract:
round-trip/reopen and shared ownership, ordering/filtering, revision conflicts,
failed writes, malformed persisted selections and concurrent revision winners.
It was first run against the original monolithic implementation. It intentionally
does not include the private headers above.

Run that target together with `AIOrchestrator_Persistence_tests` and
`AIOrchestrator_BackendRuntime_tests` after an extraction. The broader suites
protect migrations, persistence transactions/recovery and the composition root.
These are provider-free tests using temporary databases, not the user's profile.

## Next decomposition steps

The [review and implementation design](../../../../../docs/architecture/sqlite-decomposition.md)
defines the next agent's scope, private interfaces, migration invariants and
acceptance checks. The implementation split (phases 1, 2A and 2B and batches 3-8) and the
split of the public declaration header into module headers behind a compatibility facade
are complete. What is left is optional include migration:

1. Move consumers and `SqliteConnection.hpp` from the facade to the module headers, and
   drop the leftover monolith `using` declarations in the implementations; do not combine
   that include migration with an unverified storage behaviour change.

Folder layout is only part of modularity: keep dependencies and tests owned by
the feature instead of only moving filenames.
