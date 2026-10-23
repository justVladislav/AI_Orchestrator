# SQLite decomposition: review and implementation design

Status: phase 1 implemented; phase 2 below is a design, not implemented.
Reviewed on 2026-10-23. This document describes implementation boundaries and
acceptance criteria, not the completion of the Backend architecture refactor.

Paths below are relative to `First_AI_Agents/First_AI_Agents/` unless stated
otherwise. The [implementation guide](../../First_AI_Agents/First_AI_Agents/sourceFiles/infrastructure/sqlite/README.md)
describes the current ownership rules.

## Current state and review

Phase 1 moved statement/parameter/column primitives and Comparison persistence
out of `sourceFiles/infrastructure/SqliteRepositories.cpp`. The file decreased
from 15,728 to 15,327 lines. It still contains database opening, historical
migrations, feature mappings and most repository implementations. Moving 401
lines out is a first verified boundary, not resolution of the entire gap.

| Boundary | Current owner | Review result |
| --- | --- | --- |
| Native handle and shared mutex | `sqlite/SqliteConnection.hpp`, private `SqliteDatabase::Impl` | Same ownership and cleanup; repositories retain the shared database |
| Statements, bindings and primitive conversions | `sqlite/SqlitePrimitives.hpp/.cpp` | Moved bodies preserve finalization, numeric range checks and errors |
| Comparison SQL, row mapping and selection JSON | `sqlite/comparison/SqliteComparisonSessionRepository.cpp` | Same queries, column/binding order, locking and revision checks |
| Opening and schema upgrades | Remaining aggregate `.cpp` | Next extraction; schema remains 70 |
| Repository declarations | `includes/ai/infrastructure/SqliteRepositories.hpp` | Unchanged compatibility surface |

No blocking issue was found in the phase-1 move. This conclusion covers the
extraction, not every pre-existing storage behaviour or the unfinished Comparison
product features. Private headers are not exported as an include directory and
are not included by application services or the Qt adapter. Feature JSON stays
with the feature instead of entering the shared primitives header.

Verification already performed:

- The Comparison contracts passed against the original aggregate implementation
  before extraction. They cover reopen, optional values, filtering/order,
  shared ownership, conflicting revisions, failed bindings/writes, foreign-key
  failure recovery, corrupt selection JSON and concurrent revision winners.
- After extraction, Persistence, Comparison and BackendRuntime passed together
  in a Backend-only Debug build: 3/3. The final Comparison rerun also passed.
- The moved implementation bodies were compared with the originals. Database
  opening/migrations and all 59 raw SQL blocks retained their text and order.
- Hosted Windows CI run `39969521669` passed Debug and Release on public commit
  `3e61d07fc281492ca698e91fc646de15c52599b2`.

The same nine phase-1 files were synchronized into the primary local checkout
and committed there as `135ce5a`. Their normalized Git blobs match the tested
public change. That copy was not independently rebuilt after synchronization.
No GUI performance measurement, live profile database test, paid Run or new
release package is claimed by this review. New tests are contract tests, not
mutation-tested or benchmark evidence.

## Dependency direction and constraints

```text
Qt adapter / composition root
  -> application repository ports and existing public SQLite declarations
    -> feature-owned SQLite implementations
      -> private connection, statements and primitive conversions

SqliteDatabase::open
  -> private schema upgrade boundary
    -> SQLite primitives and historical schema validation
```

Continue with the existing `AIOrchestrator_Persistence` target and public
declarations. Do not introduce another database instance, mutex, connection pool,
Qt SQL dependency, service locator or generic repository framework. Repository
methods must retain their current lock scope, especially around transactions
and `sqlite3_changes()`. Do not change public method signatures or error codes.

Schema 70, applied migrations, foreign-key actions, indexes, triggers, stored
JSON, timestamp units and revision rules stay unchanged. Schema validation
currently uses pricing-domain validation; preserve that dependency in the schema
implementation instead of copying or weakening the rules.

This is not a UI latency fix. Profile UI/read-model refresh and locking costs
separately before changing their behaviour.

## Phase 2A: extract database lifecycle without rewriting it

Deliver one reviewable structural change before splitting the migration body.

1. Add `sourceFiles/infrastructure/sqlite/SqliteDatabase.cpp` to the Persistence
   target explicitly. Move the constructor, destructor, `open()` and `path()`
   there. At this intermediate checkpoint, `open()` still contains the entire
   existing migration body.
2. Move `supportedSchemaVersion`, `followUpPlanRequestsObjectsSql()` and
   `followUpConfirmNeedsGroupTriggerSql()` with it into its anonymous namespace.
   Both SQL helpers belong to schema ownership; the second is not optional.
   Preserve their SQL and call positions in steps 66 and 67.
3. Move the existing `pathToUtf8` / `pathFromUtf8` pair unchanged into
   `SqlitePrimitives.hpp/.cpp`, adding `<filesystem>` there. Both lifecycle and
   remaining repositories use the same conversion. Remove the aggregate's local
   definitions and import the private functions explicitly. Do not introduce
   ANSI `.string()` conversions or duplicate these implementations.
4. Keep `SqliteConnection.hpp` as the single definition of `Impl`; the constructor
   and destructor stay out of the public header. Retain the old aggregate for
   repositories only. Do not make new implementation files include a `.cpp`.
5. Preserve the existing open sequence: path validation, directory creation,
   UTF-8 path, READWRITE/CREATE/FULLMUTEX, extended result codes, 5-second busy
   timeout, foreign keys, WAL, synchronous NORMAL, version inspection and
   migrations, then publication of the shared database. This order also affects
   failure precedence and side effects; do not rearrange it during extraction.
6. Preserve failed-open handle cleanup and all early-return cleanup. An error
   must not expose a partially opened `SqliteDatabase` to callers.

Use explicit includes. Migration code also uses `ProviderCatalogTypes.hpp`
(`LongContextPricing`, its checks and diagnostics), containers and local schema
inspection lambdas. Inventory these dependencies before removal; do not move
unrelated feature row readers merely to satisfy compilation.

## Phase 2B: isolate schema upgrades behind one private function

Only after 2A passes, add:

```text
sourceFiles/infrastructure/sqlite/
  SqliteDatabase.cpp       # filesystem, native open, PRAGMAs and lifecycle
  SqliteSchema.hpp         # private upgrade declaration
  SqliteSchema.cpp         # version check, historical steps and schema helpers
```

The private declaration is:

```cpp
namespace ai::infrastructure::sqlite_detail {
common::Result<void> ensureSchema(sqlite3* handle);
}
```

Contract: the handle is non-null, configured and not yet published to
repositories. The caller retains sole ownership. `ensureSchema` neither opens
nor closes it, acquires no repository lock and returns the original error.
This function is not for invoking migrations on a database already in use.

Move version inspection, newer-schema rejection, schema-only helpers and all
existing migration branches into `SqliteSchema.cpp`. Keep the version statement
scope: it must finalize before a migration transaction begins. Keep
`supportedSchemaVersion = 70` private to the schema owner.

Mechanical edits allowed inside the moved body:

- `impl->handle` becomes the handle parameter.
- Outer `Result<shared_ptr<SqliteDatabase>>::failure(...)` returns become
  `Result<void>::failure(...)`; keep the error object/message unchanged.
- Successful completion returns `Result<void>::ok()`. The lifecycle code checks
  that result before constructing the shared database.

Do not rewrite nested lambdas' different Result types, version guards, SQL,
branches or recovery logic. Do not wrap all upgrades in a new transaction, add
an automatic retry or replace historical branches with a numbered loop. The
existing checks include column/shape inspection, pricing validation, rollback,
foreign-key checks and refusal to discard paid follow-up evidence. Preserve the
special v66/v67 table/trigger repair ordering and non-empty legacy-table refusal.

Keep the migration code together for this step. Splitting SQL by historical
version is a later decision; it must not produce dozens of files before a stable
schema boundary exists. Do not change schema, package version or release assets.

## Acceptance and tests for phase 2

Reuse the existing suites and add only missing behavioural coverage. First run
any new lifecycle/error contracts against phase 1, before moving implementation.
Tests must use public entry points and synthetic temporary databases, not private
headers, the user's profile database or native handle access added to product API.

| Case | Required observation |
| --- | --- |
| Fresh database and reopen | Schema 70, same schema objects/defaults; persisted values survive reopen |
| Already-current database | Reopen preserves rows, revisions and readiness; does not reset data |
| Existing upgrade fixtures | Existing old-schema, v66/v67 and v67-v70 tests still pass with the same refusal/recovery rules |
| Newer schema | `migration.unsupported_version`; future-version marker and sentinel rows are preserved |
| Empty path / invalid parent | Same `persistence.empty_path` / `persistence.directory_failed`; no crash |
| Unicode database location | A UTF-8 round-trip works in a Cyrillic/CJK directory and after reopen |
| Failed migration | Original failure is returned, failed step does not advance its version; after removing a synthetic blocker, reopen succeeds |
| Shared lifetime and writes | Comparison and another repository continue sharing one connection; revision conflict and transaction tests remain valid |

Do not claim coverage of every historical version because a few selected
upgrade fixtures pass. Where failure injection is needed, construct an explicit
fixture that makes one existing migration fail. Assert that step's atomicity;
earlier successfully committed upgrades may remain applied. Do not require the
whole multi-version upgrade to roll back as one transaction.

For structural verification compare the moved SQL literals, concatenated SQL
fragments, helper bodies, version guards, diagnostic codes and meaningful
statement scopes against the pre-move revision. Comparing only raw-string
blocks misses migrations assembled from ordinary strings and C++ data checks.

Build the affected targets incrementally from the primary checkout. Run:

```powershell
cmake --build out/build/x64-debug --target AIOrchestrator_Persistence_tests AIOrchestrator_SqliteComparison_tests AIOrchestrator_BackendRuntime_tests ProjectReadiness_tests VerificationScratch_tests
ctest --test-dir out/build/x64-debug -R '^(AIOrchestrator_Persistence_tests|AIOrchestrator_SqliteComparison_tests|AIOrchestrator_BackendRuntime_tests|ProjectReadiness_tests|VerificationScratch_tests)$' --output-on-failure
```

Use the configured MSVC developer environment. Check `CMAKE_HOME_DIRECTORY` in
the selected build directory; it must identify the intended source checkout.
Use an existing equivalent build directory if named differently; never silently
test another source tree. Record the exact revision, targets, commands and
results. The existing Persistence runner contains multiple sequential groups;
report a failing early group rather than treating later groups as executed.

After both structural changes, rely on one normal PR CI matrix for broader
Debug/Release coverage. Do not rebuild or repack the stable release for a source
reorganization, or repeat full builds solely for documentation changes. A failing
test, changed behaviour or new unresolved concern can justify additional checks.

## Subsequent feature extraction order

| Batch | Responsibility / examples | Boundary to preserve |
| --- | --- | --- |
| 3 | `catalog/`: providers, models and execution profiles | Pricing checks, profile/roster mapping and foreign keys |
| 4 | `projects/`, `settings/`: projects/tasks, workspace state, settings, scratch and readiness | Path encoding, readiness proof and revision rules |
| 5 | `conversations/`, `memory/`: chat and project memory | Message ordering, JSON and compaction transactions |
| 6 | `planning/`: drafts, Outcome Brief and follow-up requests | Cross-table atomic draft confirmation and immutable paid evidence |
| 7 | `execution/`, `verification/`: Runs, Manifest, work ledger, scope, continuation, bootstrap and handoff | Roster snapshots, state transitions, transaction/lock boundaries |
| 8 | `proposals/`, `agents/`: changes, approvals, review, tool sessions/audit; remaining model executions | Apply audit, batched reads, execution history and recovery |

These are responsibility groups, not a command to move a batch in one commit.
Extract one coherent repository or transactional aggregate plus its own row/JSON
helpers. Shared roster mapping used by profiles, Runs and drafts needs a small
private feature helper, not promotion into `SqlitePrimitives`. Never split a
transaction across separately locking repository calls. Keep helpers private
until multiple consumers actually need them. Inventory callers before choosing
whether a helper is shared or belongs to a single feature.

After implementation ownership is clear, split the aggregate declaration header
into feature entry headers and retain the old header as a compatibility facade.
Do that separately from moving implementation. The broader application layer can
then be grouped around workflows, verification, proposals, providers, planning,
conversations and memory with explicit CMake ownership. Audit include/target
dependencies first; a folder move alone does not establish an architectural
boundary or make other toolchains supported.

## Handoff and delivery procedure

The next agent's task is phase 2A, followed by 2B only after 2A's checks pass.
Do not start the later feature batches, UI changes or toolchain expansion as part
of that task. Read this design, the SQLite guide, current CMake sources and the
actual implementation before editing; line numbers are only navigation aids.

Work in the primary local checkout first. Preserve unrelated dirty files and
stage explicit paths. Commit 2A and 2B separately, with their verification results;
update the status above to describe what actually passed. Report any failing
behaviour discovered during extraction separately instead of silently fixing it
inside a structural move. No concurrent agent should edit the same storage files.

The private archive and clean public repository have different Git histories.
Transfer only reviewed patches/files to the public branch; never point the old
checkout at the public remote or push its branch ancestry there. Verify the
public base and compare the synchronized content before pushing. Follow the
owner's authorization for push, PR and merge; a refactor task does not authorize
merging, retagging, publishing a release or touching the live database.
