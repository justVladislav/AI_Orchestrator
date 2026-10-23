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
- `projects/SqliteProjectRepository.cpp` and `projects/SqliteTaskRepository.cpp` own the
  project and task table queries with their row mapping/binding (batch 4, first part):
  the duplicate id / duplicate root path mapping, the exact root path lookup, the task
  listing order and the task-to-run links. Workspace state, settings, scratch settings and
  project readiness are still in the aggregate.
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
- The remaining repositories remain in `../SqliteRepositories.cpp` during this
  incremental decomposition. That is transitional code, not the destination for
  new unrelated features.

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
acceptance checks. Phases 1, 2A and 2B are complete; the steps below are not
implemented yet.

1. Extract additional feature-owned repositories and their row/JSON mappings in
   small independently verified changes, retaining public compatibility.
2. Gradually replace the aggregate public declaration header with feature entry
   headers and a compatibility facade; do not combine that API/include migration
   with an unverified storage behaviour change.

Folder layout is only part of modularity: keep dependencies and tests owned by
the feature instead of only moving filenames.
