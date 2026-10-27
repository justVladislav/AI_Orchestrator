#pragma once

#include "ai/application/artifacts/IArtifactRepository.hpp"
#include "ai/application/IExecutionManifestRepository.hpp"
#include "ai/application/IManifestTaskExecutionRepository.hpp"
#include "ai/application/IManifestWorkLedgerRepository.hpp"
#include "ai/application/IRunRepository.hpp"
#include "ai/application/IRunWarningRepository.hpp"
#include "ai/application/IRunWorkspaceScopeRepository.hpp"
#include "ai/common/Result.hpp"
#include "ai/infrastructure/sqlite/SqliteDatabase.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ai::infrastructure {

/** SQLite implementation of run/stage persistence with optimistic revisions. */
class SqliteRunRepository final : public application::IRunRepository {
public:
    explicit SqliteRunRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<application::Run> insertRun(application::Run run) override;
    common::Result<application::Run> insertRunWithStages(
        application::Run run,
        std::vector<application::RunStage> stages) override;
    common::Result<application::Run> insertRunWithStagesAndCopiedTaskLinks(
        application::Run run,
        std::vector<application::RunStage> stages,
        const application::RunId& taskLinkSourceRunId) override;
    common::Result<application::Run> insertRunWithStagesAndTaskLink(
        application::Run run,
        std::vector<application::RunStage> stages,
        const application::TaskId& taskId) override;
    // AG-17.7C: Run + stages + assignment snapshot + copied task links + the immutable
    // conversion record in ONE transaction.
    common::Result<application::Run> insertRunWithStagesAndLegacyConversion(
        application::Run run,
        std::vector<application::RunStage> stages,
        application::LegacyConversionRecord record,
        const std::optional<application::RunId>& taskLinkSourceRunId) override;
    [[nodiscard]] common::Result<std::optional<application::LegacyConversionRecord>>
    findLegacyConversion(const application::RunId& runId) const override;
    [[nodiscard]] common::Result<std::vector<application::LegacyConversionIndexEntry>>
    listLegacyConversionIndex() const override;
    common::Result<application::Run> updateRun(application::Run run) override;
    [[nodiscard]] common::Result<std::optional<application::Run>> findRun(
        const application::RunId& id) const override;
    [[nodiscard]] common::Result<std::vector<application::Run>> listTerminalRunsPage(
        const application::ProjectId& projectId, const std::vector<std::string>& workflowIds,
        const std::optional<application::RunPageCursor>& after, std::size_t limit) const override;
    [[nodiscard]] common::Result<std::vector<application::Run>> listRuns(
        const std::optional<application::ProjectId>& projectId = std::nullopt) const override;
    [[nodiscard]] common::Result<std::vector<application::Run>> listUnfinishedRuns() const override;
    common::Result<void> replaceStages(
        const application::RunId& runId,
        std::vector<application::RunStage> stages) override;
    common::Result<application::RunStage> updateStage(application::RunStage stage) override;
    [[nodiscard]] common::Result<std::vector<application::RunStage>> listStages(
        const application::RunId& runId) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** Immutable JSON snapshot of the accepted, validated execution contract. */
class SqliteExecutionManifestRepository final
    : public application::IExecutionManifestRepository {
public:
    explicit SqliteExecutionManifestRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<application::ExecutionManifest> insert(
        application::ExecutionManifest manifest) override;
    [[nodiscard]] common::Result<std::optional<application::ExecutionManifest>> findForRun(
        const application::RunId& runId) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

class SqliteManifestTaskExecutionRepository final
    : public application::IManifestTaskExecutionRepository {
public:
    explicit SqliteManifestTaskExecutionRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<void> upsert(
        const application::RunId& runId, const std::string& taskId,
        const application::ManifestTaskState& state) override;
    [[nodiscard]] common::Result<std::vector<application::ManifestTaskExecutionRecord>> listForRun(
        const application::RunId& runId) const override;

    common::Result<void> upsertNoChanges(
        const application::RunId& runId, const std::string& taskId,
        const application::ManifestWorkerNoChanges& evidence) override;
    [[nodiscard]] common::Result<std::vector<application::ManifestTaskNoChangesRecord>>
    listNoChangesForRun(const application::RunId& runId) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

class SqliteManifestWorkLedgerRepository final
    : public application::IManifestWorkLedgerRepository {
public:
    explicit SqliteManifestWorkLedgerRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<void> insertEntry(application::ManifestWorkLedgerEntry entry) override;
    [[nodiscard]] common::Result<std::optional<application::ManifestWorkLedgerEntry>> findEntry(
        const application::RunId& runId) const override;
    [[nodiscard]] common::Result<std::vector<application::ManifestWorkLedgerEntry>>
    listRecentForProject(const application::ProjectId& projectId, std::size_t limit) const override;

    common::Result<void> appendTransition(
        application::ManifestWorkLedgerReviewTransition transition) override;
    [[nodiscard]] common::Result<std::vector<application::ManifestWorkLedgerReviewTransition>>
    listTransitionsForRun(const application::RunId& runId) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of the AG-15 frozen workspace scope + expansions. */
class SqliteRunWorkspaceScopeRepository final
    : public application::IRunWorkspaceScopeRepository {
public:
    explicit SqliteRunWorkspaceScopeRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<void> freeze(application::RunWorkspaceScope scope) override;
    [[nodiscard]] common::Result<std::optional<application::RunWorkspaceScope>> findForRun(
        const application::RunId& runId) const override;

    common::Result<void> recordExpansion(
        application::RunWorkspaceScopeExpansion expansion) override;
    [[nodiscard]] common::Result<std::vector<application::RunWorkspaceScopeExpansion>>
    listExpansionsForRun(const application::RunId& runId) const override;

    common::Result<void> recordDiagnosticRead(
        application::RunWorkspaceScopeDiagnosticRead grant) override;
    [[nodiscard]] common::Result<std::vector<application::RunWorkspaceScopeDiagnosticRead>>
    listDiagnosticReadsForRun(const application::RunId& runId) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of the artifact metadata persistence port. */
class SqliteArtifactRepository final : public application::IArtifactRepository {
public:
    explicit SqliteArtifactRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<application::Artifact> insertArtifact(application::Artifact artifact) override;
    [[nodiscard]] common::Result<std::optional<application::Artifact>> findArtifact(
        const application::ArtifactId& id) const override;
    [[nodiscard]] common::Result<std::vector<application::Artifact>> listArtifacts(
        const application::RunId& runId) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of the Run warning port (table run_warnings, schema step 68). */
class SqliteRunWarningRepository final : public application::IRunWarningRepository {
public:
    explicit SqliteRunWarningRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<bool> recordWarning(application::RunWarning warning) override;
    [[nodiscard]] common::Result<std::vector<application::RunWarning>> listWarnings(
        const application::RunId& runId) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

} // namespace ai::infrastructure
