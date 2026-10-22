#include "ai/application/ModelExecutionService.hpp"
#include "ai/application/InMemoryRepositories.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>

namespace {

using namespace ai::application;
using namespace ai::infrastructure;
namespace fs = std::filesystem;

void requireExecution(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct ExecutionWorkspace {
    fs::path root {fs::temp_directory_path() / ("ai-orchestrator-execution-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
    fs::path database {root / "orchestrator.sqlite3"};
    ~ExecutionWorkspace() { std::error_code ignored; fs::remove_all(root, ignored); }
};

void testModelExecutionsAndQualityPersist()
{
    ExecutionWorkspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    requireExecution(opened.success(), "Could not open execution database");

    SqliteProjectRepository projects(opened.value());
    SqliteProviderRepository providers(opened.value());
    SqliteModelRepository models(opened.value());
    SqliteModelExecutionRepository executions(opened.value());
    SqliteComparisonSessionRepository comparisons(opened.value());
    const auto now = std::chrono::system_clock::now();

    Project project {"project-1", "Execution project", "Context", workspace.root / "project", ProjectStatus::Active, now, now};
    requireExecution(projects.insert(project).success(), "Could not insert execution project");
    Provider provider {"provider-1", "OpenAI", "openai", "https://api.openai.com", true, std::nullopt, now, now};
    requireExecution(providers.insert(provider).success(), "Could not insert execution provider");
    Model model {"model-1", provider.id, "gpt-test", "GPT Test", 128000, 4096, 100, 200, 0, 0, true, true, now, now};
    requireExecution(models.insert(model).success(), "Could not insert execution model");

    ModelExecution second;
    second.id = "execution-2";
    second.source = ModelExecutionSource::Comparison;
    second.sourceId = "comparison-1";
    second.projectId = project.id;
    second.providerId = provider.id;
    second.modelId = model.id;
    second.role = "reviewer";
    second.position = 2;
    second.attempt = 3;
    second.status = ModelExecutionStatus::Completed;
    second.requestHash = "request-sha256";
    second.contextHash = "context-sha256";
    second.outputContent = std::string(2'048, 'x') + "\nDurable full comparison result";
    second.outputPreview = second.outputContent.substr(0, 1'000);
    second.inputTokens = 120;
    second.outputTokens = 80;
    second.cacheCreationInputTokens = 30;
    second.cacheReadInputTokens = 40;
    second.cacheCreationDuration.known = true;
    second.cacheCreationDuration.fiveMinuteTokens = 10;
    second.cacheCreationDuration.oneHourTokens = 20;
    second.costMicros = 42;
    // MW-P2: proves the honest "estimated" flag round-trips - a false value
    // specifically, since the field's default is true and a naive binding
    // bug could easily leave it silently stuck at the default.
    second.costPriceKnown = false;
    second.latencyMs = 1500;
    second.createdAt = now;
    second.startedAt = now;
    second.finishedAt = now;
    requireExecution(executions.insertExecution(second).success(), "Could not insert completed model execution");

    auto invalidDuration = second;
    invalidDuration.id = "execution-invalid-cache-duration";
    invalidDuration.position = 9;
    invalidDuration.cacheCreationDuration.oneHourTokens = 19;
    const auto rejectedSqliteDuration = executions.insertExecution(invalidDuration);
    requireExecution(
        !rejectedSqliteDuration.success()
            && rejectedSqliteDuration.error()->code
                == "cache_duration.partition_total_mismatch",
        "SQLite execution repository accepted an invalid cache-duration partition");
    InMemoryModelExecutionRepository inMemoryExecutions;
    const auto rejectedInMemoryDuration = inMemoryExecutions.insertExecution(invalidDuration);
    requireExecution(
        !rejectedInMemoryDuration.success()
            && rejectedInMemoryDuration.error()->code
                == "cache_duration.partition_total_mismatch",
        "In-memory execution repository accepted an invalid cache-duration partition");

    auto hiddenDuration = second;
    hiddenDuration.id = "execution-hidden-cache-duration";
    hiddenDuration.position = 11;
    hiddenDuration.cacheCreationDuration.known = false;
    const auto rejectedSqliteHidden = executions.insertExecution(hiddenDuration);
    const auto rejectedInMemoryHidden = inMemoryExecutions.insertExecution(hiddenDuration);
    requireExecution(
        !rejectedSqliteHidden.success()
            && rejectedSqliteHidden.error()->code == "cache_duration.detail_without_partition"
            && !rejectedInMemoryHidden.success()
            && rejectedInMemoryHidden.error()->code
                == "cache_duration.detail_without_partition",
        "Execution repositories accepted hidden duration detail");

    auto invalidCacheTotal = second;
    invalidCacheTotal.id = "execution-invalid-cache-total";
    invalidCacheTotal.position = 10;
    invalidCacheTotal.inputTokens = 60;
    const auto rejectedSqliteTotal = executions.insertExecution(invalidCacheTotal);
    const auto rejectedInMemoryTotal = inMemoryExecutions.insertExecution(invalidCacheTotal);
    requireExecution(
        !rejectedSqliteTotal.success()
            && rejectedSqliteTotal.error()->code == "model_execution.invalid_cache_usage"
            && !rejectedInMemoryTotal.success()
            && rejectedInMemoryTotal.error()->code == "model_execution.invalid_cache_usage",
        "Execution repositories accepted cache subsets larger than input tokens");

    auto retry = second;
    retry.id = "execution-2-retry";
    retry.attempt = 4;
    retry.status = ModelExecutionStatus::Failed;
    retry.errorCode = "provider.timeout";
    retry.errorMessage = "Retry timed out";
    retry.outputContent.clear();
    retry.outputPreview.clear();
    retry.inputTokens = 0;
    retry.outputTokens = 0;
    retry.costMicros = 0;
    retry.cacheCreationInputTokens = 0;
    retry.cacheReadInputTokens = 0;
    retry.cacheCreationDuration = {};
    // A retry/fallback keeps the logical stage position and differs only by
    // attempt. Schema v13 must admit both rows; schema v12 rejected this
    // insert before the fallback provider could be called.
    requireExecution(executions.insertExecution(retry).success(),
        "Could not insert a second attempt for the same execution position");

    auto first = second;
    first.id = "execution-1";
    first.position = 0;
    first.status = ModelExecutionStatus::Failed;
    first.errorCode = "provider.timeout";
    first.errorMessage = "Timed out";
    requireExecution(executions.insertExecution(first).success(), "Could not insert failed model execution");

    ComparisonSession comparison;
    comparison.id = "comparison-1";
    comparison.projectId = project.id;
    comparison.title = "Persisted comparison";
    comparison.prompt = "Compare the candidate outputs";
    comparison.status = ComparisonSessionStatus::Running;
    comparison.requestHash = "request-sha256";
    comparison.contextHash = "context-sha256";
    comparison.createdAt = now;
    comparison.startedAt = now;
    requireExecution(comparisons.insertSession(comparison).success(),
        "Could not insert comparison session");
    comparison.preferredExecutionId = second.id;
    comparison.synthesisSelection = {second.id};
    comparison.revision = 1;
    requireExecution(comparisons.updateSession(comparison).success(),
        "Could not update comparison selection");

    QualityEvaluation automated {"evaluation-1", second.id, QualityEvaluatorKind::Automated, std::nullopt, std::nullopt, "acceptance-v1", 8750, R"({"tests":90,"coverage":85})", "Acceptance checks passed", "8 of 9 checks", now};
    requireExecution(executions.insertEvaluation(automated).success(), "Could not insert automated quality evaluation");
    QualityEvaluation judge {"evaluation-2", second.id, QualityEvaluatorKind::BlindedModel, provider.id, model.id, "rubric-v2", 8100, R"({"correctness":82,"clarity":80})", "Provider identity was hidden", "Rubric evidence", now + std::chrono::milliseconds(1)};
    requireExecution(executions.insertEvaluation(judge).success(), "Could not insert blinded quality evaluation");

    const auto ordered = executions.listExecutions(ModelExecutionSource::Comparison, "comparison-1");
    requireExecution(ordered.success() && ordered.value().size() == 3, "Comparison execution list did not round-trip");
    requireExecution(ordered.value()[0].id == first.id
            && ordered.value()[1].id == second.id
            && ordered.value()[2].id == retry.id,
        "Executions are not ordered by position and attempt creation order");
    const auto projectRows = executions.listProjectExecutions(project.id);
    requireExecution(projectRows.success() && projectRows.value().size() == 3, "Project execution filter failed");

    const auto reopened = SqliteDatabase::open(workspace.database);
    requireExecution(reopened.success(), "Could not reopen execution database");
    SqliteModelExecutionRepository reopenedExecutions(reopened.value());
    SqliteComparisonSessionRepository reopenedComparisons(reopened.value());
    const auto persisted = reopenedExecutions.findExecution(second.id);
    requireExecution(persisted.success() && persisted.value().has_value(), "Model execution did not survive reopen");
    requireExecution(persisted.value()->inputTokens == 120 && persisted.value()->costMicros == 42
            && persisted.value()->cacheCreationInputTokens == 30
            && persisted.value()->cacheReadInputTokens == 40
            && persisted.value()->cacheCreationDuration.known
            && persisted.value()->cacheCreationDuration.fiveMinuteTokens == 10
            && persisted.value()->cacheCreationDuration.oneHourTokens == 20,
        "Duration-aware execution usage changed after reopen");
    requireExecution(persisted.value()->costPriceKnown == false,
        "costPriceKnown did not survive reopen");
    requireExecution(persisted.value()->attempt == 3,
        "Model execution attempt number did not survive reopen");
    requireExecution(persisted.value()->outputContent == second.outputContent
            && persisted.value()->outputPreview.size() == 1'000,
        "Full execution output and compact preview did not survive reopen");
    const auto evaluations = reopenedExecutions.listEvaluations(second.id);
    requireExecution(evaluations.success() && evaluations.value().size() == 2, "Quality evaluations did not survive reopen");
    requireExecution(evaluations.value()[0].scoreBasisPoints == 8750 && evaluations.value()[1].evaluatorKind == QualityEvaluatorKind::BlindedModel, "Quality evaluation data changed after reopen");
    const auto persistedComparison = reopenedComparisons.findSession(comparison.id);
    requireExecution(persistedComparison.success() && persistedComparison.value().has_value(),
        "Comparison session did not survive reopen");
    requireExecution(persistedComparison.value()->preferredExecutionId == second.id
            && persistedComparison.value()->synthesisSelection.size() == 1
            && persistedComparison.value()->revision == 1,
        "Comparison session selection changed after reopen");

    auto updated = *persisted.value();
    updated.status = ModelExecutionStatus::Partial;
    updated.outputTokens = 91;
    requireExecution(reopenedExecutions.updateExecution(updated).success(), "Could not update model execution outcome");
    const auto updatedRead = reopenedExecutions.findExecution(updated.id);
    requireExecution(updatedRead.success() && updatedRead.value()->status == ModelExecutionStatus::Partial && updatedRead.value()->outputTokens == 91, "Updated execution did not round-trip");

    auto invalidScore = automated;
    invalidScore.id = "evaluation-invalid";
    invalidScore.scoreBasisPoints = 10001;
    requireExecution(!reopenedExecutions.insertEvaluation(invalidScore).success(), "SQLite accepted a quality score above 100 percent");
}

// Regression test for the intermittent "Quality evaluation did not survive
// restart" failure in AIOrchestrator_BackendRuntime_tests
// (testModelExecutionLifecycleAndRecoveryAcrossRestart). Root cause: quality
// evaluation ids are random UUIDs (generateUuid(), IdGenerator.cpp), not
// time-ordered, and listEvaluations() used to sort
// "ORDER BY created_at_ms,id". Two evaluations for the same execution
// created back-to-back in real code (or in that test) can land in the same
// millisecond, so created_at_ms ties and the random id string decided the
// order - roughly a coin flip, independent of actual insertion order. This
// reproduces the tie deterministically (both evaluations share one
// timestamp) instead of depending on real-world timing luck.
void testQualityEvaluationsOrderedByInsertionWhenTimestampsTie()
{
    ExecutionWorkspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    requireExecution(opened.success(), "Could not open database for tie-ordering test");

    SqliteProjectRepository projects(opened.value());
    SqliteProviderRepository providers(opened.value());
    SqliteModelRepository models(opened.value());
    SqliteModelExecutionRepository executions(opened.value());
    const auto now = std::chrono::system_clock::now();

    Project project {"project-tie", "Tie project", "Context", workspace.root / "project", ProjectStatus::Active, now, now};
    requireExecution(projects.insert(project).success(), "Could not insert tie-test project");
    Provider provider {"provider-tie", "OpenAI", "openai", "https://api.openai.com", true, std::nullopt, now, now};
    requireExecution(providers.insert(provider).success(), "Could not insert tie-test provider");
    Model model {"model-tie", provider.id, "gpt-test", "GPT Test", 128000, 4096, 100, 200, 0, 0, true, true, now, now};
    requireExecution(models.insert(model).success(), "Could not insert tie-test model");

    ModelExecution execution;
    execution.id = "execution-tie";
    execution.source = ModelExecutionSource::Comparison;
    execution.sourceId = "comparison-tie";
    execution.projectId = project.id;
    execution.providerId = provider.id;
    execution.modelId = model.id;
    execution.role = "reviewer";
    execution.position = 0;
    execution.status = ModelExecutionStatus::Completed;
    execution.requestHash = "request-tie";
    execution.contextHash = "context-tie";
    execution.createdAt = now;
    execution.startedAt = now;
    execution.finishedAt = now;
    requireExecution(executions.insertExecution(execution).success(), "Could not insert tie-test execution");

    // Both evaluations share the exact same created_at_ms. Ids are chosen so
    // a lexicographic id tiebreak ("zzz..." before "aaa...") would put them
    // in the WRONG order relative to insertion - this deterministically
    // fails under the old "ORDER BY created_at_ms,id" and passes under the
    // fixed "ORDER BY created_at_ms,rowid".
    QualityEvaluation firstInserted {"zzz-inserted-first", execution.id, QualityEvaluatorKind::Automated,
        std::nullopt, std::nullopt, "acceptance-v1", 9100, "{}", "First by insertion order", "evidence-1", now};
    requireExecution(executions.insertEvaluation(firstInserted).success(),
        "Could not insert first tied evaluation");
    QualityEvaluation secondInserted {"aaa-inserted-second", execution.id, QualityEvaluatorKind::Automated,
        std::nullopt, std::nullopt, "acceptance-v1", 8600, "{}", "Second by insertion order", "evidence-2", now};
    requireExecution(executions.insertEvaluation(secondInserted).success(),
        "Could not insert second tied evaluation");

    const auto evaluations = executions.listEvaluations(execution.id);
    requireExecution(evaluations.success() && evaluations.value().size() == 2,
        "Tied-timestamp evaluations did not round-trip");
    requireExecution(evaluations.value()[0].id == "zzz-inserted-first"
            && evaluations.value()[1].id == "aaa-inserted-second",
        "Evaluations with a tied created_at_ms must come back in insertion order, "
        "not sorted by their (random, unordered) id string");
}

void testV12MigrationAdmitsRetryAtSamePosition()
{
    ExecutionWorkspace workspace;
    const auto now = std::chrono::system_clock::now();
    ModelExecution firstAttempt;

    {
        const auto opened = SqliteDatabase::open(workspace.database);
        requireExecution(opened.success(), "Could not create pre-migration execution database");
        SqliteProjectRepository projects(opened.value());
        SqliteProviderRepository providers(opened.value());
        SqliteModelRepository models(opened.value());
        SqliteModelExecutionRepository executions(opened.value());

        Project project {"migration-project", "Migration project", "Context",
            workspace.root / "project", ProjectStatus::Active, now, now};
        requireExecution(projects.insert(project).success(),
            "Could not insert migration project");
        Provider provider {"migration-provider", "OpenAI", "openai",
            "https://api.openai.com", true, std::nullopt, now, now};
        requireExecution(providers.insert(provider).success(),
            "Could not insert migration provider");
        Model model {"migration-model", provider.id, "gpt-test", "GPT Test",
            128000, 8192, 0, 0, 0, 0, true, true, now, now};
        requireExecution(models.insert(model).success(),
            "Could not insert migration model");

        firstAttempt.id = "migration-attempt-1";
        firstAttempt.source = ModelExecutionSource::Run;
        firstAttempt.sourceId = "migration-run";
        firstAttempt.projectId = project.id;
        firstAttempt.providerId = provider.id;
        firstAttempt.modelId = model.id;
        firstAttempt.role = "architect";
        firstAttempt.position = 0;
        firstAttempt.attempt = 1;
        firstAttempt.status = ModelExecutionStatus::Failed;
        firstAttempt.requestHash = "request";
        firstAttempt.contextHash = "context";
        firstAttempt.errorCode = "provider.timeout";
        firstAttempt.errorMessage = "Timed out";
        firstAttempt.createdAt = now;
        requireExecution(executions.insertExecution(firstAttempt).success(),
            "Could not insert first migration attempt");
    }

    // Recreate the schema-v12 defect on an otherwise complete database: the
    // old uniqueness ignored attempt. Setting user_version to 12 makes the
    // next production open exercise the real v13 migration path.
    sqlite3* raw = nullptr;
    requireExecution(sqlite3_open_v2(
        workspace.database.string().c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
        "Could not open raw pre-migration database");
    const int downgraded = sqlite3_exec(raw,
        "CREATE UNIQUE INDEX model_executions_v12_position_unique "
        "ON model_executions(source_type,source_id,position);"
        "PRAGMA user_version = 12;",
        nullptr, nullptr, nullptr);
    sqlite3_close_v2(raw);
    requireExecution(downgraded == SQLITE_OK,
        "Could not prepare schema-v12 retry fixture");

    const auto migrated = SqliteDatabase::open(workspace.database);
    requireExecution(migrated.success(), "Schema-v13 retry migration failed");
    SqliteModelExecutionRepository executions(migrated.value());
    auto retry = firstAttempt;
    retry.id = "migration-attempt-2";
    retry.attempt = 2;
    retry.errorMessage = "Second timeout";
    retry.createdAt = now + std::chrono::milliseconds(1);
    requireExecution(executions.insertExecution(retry).success(),
        "Schema-v13 migration still rejects a retry at the same position");
    const auto attempts = executions.listExecutions(
        ModelExecutionSource::Run, firstAttempt.sourceId);
    requireExecution(attempts.success() && attempts.value().size() == 2
            && attempts.value()[0].attempt == 1
            && attempts.value()[1].attempt == 2,
        "Schema-v13 migration did not preserve and order both attempts");
}

void testV45ToV46CostPriceKnownMigrationDefaultsAndRestart()
{
    ExecutionWorkspace workspace;
    const auto now = std::chrono::system_clock::now();

    {
        const auto opened = SqliteDatabase::open(workspace.database);
        requireExecution(opened.success(), "Could not create v45 cost-price-known fixture");
        SqliteProjectRepository projects(opened.value());
        SqliteProviderRepository providers(opened.value());
        SqliteModelRepository models(opened.value());
        SqliteModelExecutionRepository executions(opened.value());

        Project project {"cost-known-project", "Cost known project", "Context",
            workspace.root / "project", ProjectStatus::Active, now, now};
        requireExecution(projects.insert(project).success(), "Could not seed cost-known project");
        Provider provider {"cost-known-provider", "OpenAI", "openai",
            "https://api.openai.com", true, std::nullopt, now, now};
        requireExecution(providers.insert(provider).success(), "Could not seed cost-known provider");
        Model model {"cost-known-model", provider.id, "gpt-test", "GPT Test",
            128000, 4096, 100, 200, 0, 0, true, true, now, now};
        requireExecution(models.insert(model).success(), "Could not seed cost-known model");

        ModelExecution execution;
        execution.id = "cost-known-execution";
        execution.source = ModelExecutionSource::Run;
        execution.sourceId = "cost-known-run";
        execution.projectId = project.id;
        execution.providerId = provider.id;
        execution.modelId = model.id;
        execution.role = "architect";
        execution.position = 0;
        execution.attempt = 1;
        execution.status = ModelExecutionStatus::Completed;
        execution.requestHash = "request";
        execution.contextHash = "context";
        execution.costMicros = 7;
        execution.createdAt = now;
        requireExecution(executions.insertExecution(execution).success(),
            "Could not seed cost-known execution");
    }

    sqlite3* handle = nullptr;
    requireExecution(sqlite3_open_v2(
        workspace.database.string().c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
        "Could not open cost-known fixture through native sqlite3");
    const int rewound = sqlite3_exec(handle,
        "ALTER TABLE model_executions DROP COLUMN cost_price_known;"
        "PRAGMA user_version = 45;",
        nullptr, nullptr, nullptr);
    sqlite3_close_v2(handle);
    requireExecution(rewound == SQLITE_OK, "Could not rewind cost-known fixture to v45");

    const auto migrated = SqliteDatabase::open(workspace.database);
    requireExecution(migrated.success(), "Could not migrate cost-known fixture v45->v46");
    SqliteModelExecutionRepository executions(migrated.value());
    const auto found = executions.findExecution("cost-known-execution");
    requireExecution(found.success() && found.value().has_value()
            && found.value()->costPriceKnown == true,
        "v45 execution did not receive the truthful default costPriceKnown=true");

    // A real false value must still round-trip through the migrated column,
    // not just the migration's own default.
    auto updated = *found.value();
    updated.costPriceKnown = false;
    const auto persisted = executions.updateExecution(updated);
    requireExecution(persisted.success() && persisted.value().costPriceKnown == false,
        "Migrated v46 database did not persist a real costPriceKnown=false value");

    const auto reopened = SqliteDatabase::open(workspace.database);
    requireExecution(reopened.success(), "Migrated v46 cost-known database did not reopen");
}

// Proves SqliteModelRepository::remove()'s ON DELETE RESTRICT enforcement:
// a model with recorded execution history cannot be deleted, but an unused
// sibling model can. ProviderCatalogService::deleteModel()'s own rules
// (not-found, default-cannot-be-deleted) are covered against InMemory
// repositories in WorkspaceAndCatalogTests.cpp - this is the repository-level
// proof that the foreign key itself, not just service-layer logic, blocks
// orphaning a real execution row.
void testModelDeletionEnforcesExecutionHistory()
{
    ExecutionWorkspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    requireExecution(opened.success(), "Could not open model-deletion test database");

    SqliteProjectRepository projects(opened.value());
    SqliteProviderRepository providers(opened.value());
    SqliteModelRepository models(opened.value());
    SqliteModelExecutionRepository executions(opened.value());
    const auto now = std::chrono::system_clock::now();

    Project project {"deletion-project", "Model deletion project", "Context",
        workspace.root / "project", ProjectStatus::Active, now, now};
    requireExecution(projects.insert(project).success(), "Could not insert deletion project");
    Provider provider {"deletion-provider", "OpenAI", "openai",
        "https://api.openai.com", true, std::nullopt, now, now};
    requireExecution(providers.insert(provider).success(), "Could not insert deletion provider");

    Model usedModel {"deletion-model-used", provider.id, "gpt-used", "GPT Used",
        128000, 4096, 100, 200, 0, 0, true, false, now, now};
    requireExecution(models.insert(usedModel).success(), "Could not insert used model");
    Model unusedModel {"deletion-model-unused", provider.id, "gpt-unused", "GPT Unused",
        128000, 4096, 100, 200, 0, 0, true, false, now, now};
    requireExecution(models.insert(unusedModel).success(), "Could not insert unused model");

    ModelExecution execution;
    execution.id = "deletion-execution";
    execution.source = ModelExecutionSource::Run;
    execution.sourceId = "deletion-run";
    execution.projectId = project.id;
    execution.providerId = provider.id;
    execution.modelId = usedModel.id;
    execution.role = "architect";
    execution.position = 0;
    execution.attempt = 1;
    execution.status = ModelExecutionStatus::Completed;
    execution.requestHash = "request-sha256";
    execution.contextHash = "context-sha256";
    execution.outputContent = "Deletion test output";
    execution.outputPreview = execution.outputContent;
    execution.inputTokens = 10;
    execution.outputTokens = 5;
    execution.costMicros = 1;
    execution.latencyMs = 100;
    execution.createdAt = now;
    execution.startedAt = now;
    execution.finishedAt = now;
    requireExecution(executions.insertExecution(execution).success(),
        "Could not insert deletion-test model execution");

    const auto blocked = models.remove(usedModel.id);
    requireExecution(!blocked.success() && blocked.error()->code == "model.in_use",
        "Deleting a model with execution history must fail with model.in_use");

    const auto removed = models.remove(unusedModel.id);
    requireExecution(removed.success(), "Deleting an unused model must succeed");
    const auto goneCheck = models.findById(unusedModel.id);
    requireExecution(goneCheck.success() && !goneCheck.value().has_value(),
        "Deleted model must no longer be findable");

    const auto missing = models.remove(unusedModel.id);
    requireExecution(!missing.success() && missing.error()->code == "model.not_found",
        "Deleting an already-deleted model must fail with model.not_found");
}

// AG-10: ModelExecutionService::reconcileRunExecutions() (RunManager's
// same-process safety net, see RunManager::finishRun) must close every
// non-terminal row for exactly the requested run, leave every already-
// terminal row (any run) completely untouched, and never invent usage.
void testReconcileRunExecutionsClosesOnlyNonTerminalRowsForThatRun()
{
    ExecutionWorkspace workspace;
    const auto opened = SqliteDatabase::open(workspace.database);
    requireExecution(opened.success(), "Could not open reconciliation test database");

    SqliteProjectRepository projects(opened.value());
    SqliteProviderRepository providers(opened.value());
    SqliteModelRepository models(opened.value());
    SqliteModelExecutionRepository executions(opened.value());
    ModelExecutionService service(executions);
    const auto now = std::chrono::system_clock::now();

    Project project {"reconcile-project", "Reconciliation project", "Context",
        workspace.root / "project", ProjectStatus::Active, now, now};
    requireExecution(projects.insert(project).success(), "Could not insert reconciliation project");
    Provider provider {"reconcile-provider", "OpenAI", "openai",
        "https://api.openai.com", true, std::nullopt, now, now};
    requireExecution(providers.insert(provider).success(), "Could not insert reconciliation provider");
    Model model {"reconcile-model", provider.id, "gpt-reconcile", "GPT Reconcile",
        128000, 4096, 100, 200, 0, 0, true, true, now, now};
    requireExecution(models.insert(model).success(), "Could not insert reconciliation model");

    // position is part of model_executions' UNIQUE(source_type, source_id,
    // position) constraint, so sibling rows for the same run need distinct
    // positions - matching how real per-task executions never share a slot.
    auto makeExecution = [&](const std::string& id, const std::string& runId,
                              std::uint32_t position, ModelExecutionStatus status) {
        ModelExecution value;
        value.id = id;
        value.source = ModelExecutionSource::Run;
        value.sourceId = runId;
        value.projectId = project.id;
        value.providerId = provider.id;
        value.modelId = model.id;
        value.role = "component-worker";
        value.position = position;
        value.attempt = 1;
        value.status = status;
        value.requestHash = "request-sha256";
        value.contextHash = "context-sha256";
        value.createdAt = now;
        if (status != ModelExecutionStatus::Pending) value.startedAt = now;
        if (status == ModelExecutionStatus::Completed) {
            value.finishedAt = now;
            value.inputTokens = 111;
            value.outputTokens = 222;
            value.costMicros = 333;
        }
        return value;
    };

    // run-dangling: one Running row (position 0) that should be reconciled...
    const auto danglingRunning = makeExecution("dangling-running", "run-dangling", 0,
        ModelExecutionStatus::Running);
    requireExecution(executions.insertExecution(danglingRunning).success(),
        "Could not insert dangling Running execution");
    // ...and one already-Completed sibling task (position 1) for the SAME
    // run that must stay exactly as-is.
    const auto danglingCompleted = makeExecution("dangling-completed", "run-dangling", 1,
        ModelExecutionStatus::Completed);
    requireExecution(executions.insertExecution(danglingCompleted).success(),
        "Could not insert already-terminal execution for the dangling run");
    // run-other: a Running row for a DIFFERENT run must never be touched -
    // reconcileRunExecutions() is scoped to exactly one run.
    const auto otherRunning = makeExecution("other-running", "run-other", 0,
        ModelExecutionStatus::Running);
    requireExecution(executions.insertExecution(otherRunning).success(),
        "Could not insert unrelated-run Running execution");

    const auto reconciled = service.reconcileRunExecutions("run-dangling");
    requireExecution(reconciled.success(), "reconcileRunExecutions failed");

    const auto closedRow = executions.findExecution("dangling-running");
    requireExecution(closedRow.success() && closedRow.value().has_value(),
        "Dangling execution disappeared");
    requireExecution(closedRow.value()->status == ModelExecutionStatus::Failed,
        "Dangling execution was not closed to Failed");
    requireExecution(closedRow.value()->errorCode == "model_execution.orphaned",
        "Reconciled execution did not use the stable AG-10 error code");
    requireExecution(
        closedRow.value()->inputTokens == 0 && closedRow.value()->outputTokens == 0
            && closedRow.value()->costMicros == 0,
        "Reconciliation must never invent usage");
    requireExecution(closedRow.value()->finishedAt.has_value(),
        "Reconciled execution must record a finish time");

    const auto untouchedCompleted = executions.findExecution("dangling-completed");
    requireExecution(untouchedCompleted.success() && untouchedCompleted.value().has_value(),
        "Already-terminal execution disappeared");
    requireExecution(untouchedCompleted.value()->status == ModelExecutionStatus::Completed,
        "Already-terminal execution must never be reopened/reclassified");
    requireExecution(untouchedCompleted.value()->inputTokens == 111,
        "Already-terminal execution's real usage must be untouched");

    const auto untouchedOther = executions.findExecution("other-running");
    requireExecution(untouchedOther.success() && untouchedOther.value().has_value(),
        "Unrelated-run execution disappeared");
    requireExecution(untouchedOther.value()->status == ModelExecutionStatus::Running,
        "reconcileRunExecutions() must not touch a different run's execution");

    // Idempotent: reconciling the same run again is a safe no-op for the row
    // it already closed.
    const auto reconciledAgain = service.reconcileRunExecutions("run-dangling");
    requireExecution(reconciledAgain.success(), "Repeated reconcileRunExecutions must succeed");
    const auto stillClosed = executions.findExecution("dangling-running");
    requireExecution(stillClosed.success() && stillClosed.value().has_value()
            && stillClosed.value()->status == ModelExecutionStatus::Failed,
        "Repeated reconciliation must not change an already-terminal row");
}

} // namespace

int runModelExecutionRepositoryTests()
{
    testModelExecutionsAndQualityPersist();
    testQualityEvaluationsOrderedByInsertionWhenTimestampsTie();
    testV12MigrationAdmitsRetryAtSamePosition();
    testV45ToV46CostPriceKnownMigrationDefaultsAndRestart();
    testModelDeletionEnforcesExecutionHistory();
    testReconcileRunExecutionsClosesOnlyNonTerminalRowsForThatRun();
    return 0;
}
