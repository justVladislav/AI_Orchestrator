#pragma once

#include "ai/application/agents/IAgentToolAuditRepository.hpp"
#include "ai/application/agents/IAgentToolSessionRepository.hpp"
#include "ai/application/artifacts/IArtifactRepository.hpp"
#include "ai/application/execution/IRunWarningRepository.hpp"
#include "ai/application/artifacts/IArtifactStore.hpp"
#include "ai/application/catalog/IExecutionProfileRepository.hpp"
#include "ai/application/catalog/IModelRepository.hpp"
#include "ai/application/agents/IModelExecutionRepository.hpp"
#include "ai/application/projects/IProjectRepository.hpp"
#include "ai/application/catalog/IProviderRepository.hpp"
#include "ai/application/planning/IRunDraftRepository.hpp"
#include "ai/application/execution/IRunRepository.hpp"
#include "ai/application/projects/IWorkspaceStateRepository.hpp"

#include <functional>
#include <mutex>
#include <unordered_map>

namespace ai::application {

// Thread-safe development/test adapter. It exercises the same persistence ports
// that the SQLite implementation will satisfy in a later stage.
class InMemoryProjectRepository final : public IProjectRepository {
public:
    common::Result<Project> insert(Project project) override;
    common::Result<Project> update(Project project) override;
    [[nodiscard]] common::Result<std::optional<Project>> findById(const ProjectId& id) const override;
    [[nodiscard]] common::Result<std::optional<Project>> findByRootPath(
        const std::filesystem::path& rootPath) const override;
    [[nodiscard]] common::Result<std::vector<Project>> listAll() const override;

private:
    mutable std::mutex mutex_;
    std::unordered_map<ProjectId, Project> projects_;
};

class InMemoryRunRepository final : public IRunRepository {
public:
    common::Result<Run> insertRun(Run run) override;
    common::Result<Run> insertRunWithStages(
        Run run,
        std::vector<RunStage> stages) override;
    // AG-17.7C: Run + stages + conversion record under one critical section (the in-memory
    // equivalent of SQLite's transaction). Task links are not stored by this adapter.
    common::Result<Run> insertRunWithStagesAndLegacyConversion(
        Run run,
        std::vector<RunStage> stages,
        LegacyConversionRecord record,
        const std::optional<RunId>& taskLinkSourceRunId) override;
    [[nodiscard]] common::Result<std::optional<LegacyConversionRecord>> findLegacyConversion(
        const RunId& runId) const override;
    [[nodiscard]] common::Result<std::vector<LegacyConversionIndexEntry>>
    listLegacyConversionIndex() const override;
    common::Result<Run> updateRun(Run run) override;
    [[nodiscard]] common::Result<std::optional<Run>> findRun(const RunId& id) const override;
    [[nodiscard]] common::Result<std::vector<Run>> listRuns(
        const std::optional<ProjectId>& projectId = std::nullopt) const override;
    [[nodiscard]] common::Result<std::vector<Run>> listUnfinishedRuns() const override;
    common::Result<void> replaceStages(const RunId& runId, std::vector<RunStage> stages) override;
    common::Result<RunStage> updateStage(RunStage stage) override;
    [[nodiscard]] common::Result<std::vector<RunStage>> listStages(const RunId& runId) const override;

private:
    mutable std::mutex mutex_;
    std::unordered_map<RunId, Run> runs_;
    std::unordered_map<RunId, std::vector<RunStage>> stages_;
    std::unordered_map<RunId, LegacyConversionRecord> legacyConversions_;
};

// Thread-safe development/test adapter for artifact metadata. Exercises the
// same IArtifactRepository port SqliteArtifactRepository satisfies.
class InMemoryArtifactRepository final : public IArtifactRepository {
public:
    common::Result<Artifact> insertArtifact(Artifact artifact) override;
    [[nodiscard]] common::Result<std::optional<Artifact>> findArtifact(
        const ArtifactId& id) const override;
    [[nodiscard]] common::Result<std::vector<Artifact>> listArtifacts(
        const RunId& runId) const override;

private:
    mutable std::mutex mutex_;
    std::unordered_map<ArtifactId, Artifact> artifacts_;
};

// Thread-safe development/test adapter for Run warnings; same identity rule as the SQLite adapter.
class InMemoryRunWarningRepository final : public IRunWarningRepository {
public:
    common::Result<bool> recordWarning(RunWarning warning) override;
    [[nodiscard]] common::Result<std::vector<RunWarning>> listWarnings(const RunId& runId) const override;

private:
    mutable std::mutex mutex_;
    std::vector<RunWarning> warnings_;
};

// Thread-safe in-memory IArtifactStore used by Application-layer tests so
// ArtifactService's own logic (sanitization, containment, service-level
// wiring) can be exercised without touching the real filesystem. It applies
// the exact same ArtifactPathPolicy rules FileArtifactStore uses, so a test
// that passes here is testing real path-safety behavior, not a shortcut.
class InMemoryArtifactStore final : public IArtifactStore {
public:
    common::Result<StoredArtifactFile> writeArtifact(
        const RunId& runId,
        std::string_view suggestedName,
        std::string_view content) override;
    common::Result<void> removeArtifact(
        const std::filesystem::path& relativePath) override;
    [[nodiscard]] common::Result<std::filesystem::path> resolveAbsolutePath(
        const std::filesystem::path& relativePath) const override;
    [[nodiscard]] common::Result<std::string> readArtifact(
        const std::filesystem::path& relativePath) const override;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> filesByRelativePath_;
    std::uint64_t counter_ {0};
};

// Thread-safe development/test adapter for the single active-project value.
class InMemoryWorkspaceStateRepository final : public IWorkspaceStateRepository {
public:
    [[nodiscard]] common::Result<std::optional<ProjectId>> activeProjectId() const override;
    common::Result<void> setActiveProjectId(const std::optional<ProjectId>& projectId) override;

private:
    mutable std::mutex mutex_;
    std::optional<ProjectId> activeProjectId_;
};

class InMemoryProviderRepository final : public IProviderRepository {
public:
    common::Result<Provider> insert(Provider provider) override;
    common::Result<Provider> update(Provider provider) override;
    [[nodiscard]] common::Result<std::optional<Provider>> findById(const ProviderId& id) const override;
    [[nodiscard]] common::Result<std::vector<Provider>> listAll() const override;

private:
    mutable std::mutex mutex_;
    std::unordered_map<ProviderId, Provider> providers_;
};

class InMemoryModelRepository final : public IModelRepository {
public:
    common::Result<Model> insert(Model model) override;
    common::Result<Model> update(Model model) override;
    [[nodiscard]] common::Result<std::optional<Model>> findById(const ModelId& id) const override;
    [[nodiscard]] common::Result<std::vector<Model>> listAll(
        const std::optional<ProviderId>& providerId = std::nullopt) const override;
    common::Result<void> setDefaultModel(const ProviderId& providerId, const ModelId& modelId) override;
    common::Result<void> remove(const ModelId& id) override;

private:
    mutable std::mutex mutex_;
    std::unordered_map<ModelId, Model> models_;
};

class InMemoryExecutionProfileRepository final : public IExecutionProfileRepository {
public:
    common::Result<ExecutionProfile> insert(ExecutionProfile profile) override;
    common::Result<ExecutionProfile> update(ExecutionProfile profile) override;
    [[nodiscard]] common::Result<std::optional<ExecutionProfile>> findById(
        const ExecutionProfileId& id) const override;
    [[nodiscard]] common::Result<std::vector<ExecutionProfile>> listAll() const override;
    common::Result<void> setDefaultProfile(const ExecutionProfileId& profileId) override;

private:
    mutable std::mutex mutex_;
    std::unordered_map<ExecutionProfileId, ExecutionProfile> profiles_;
};

// PC-3B: the same IRunDraftRepository contract as SqliteRunDraftRepository for drafts AND groups - atomic
// insertGroup (everything validated and checked before the first write), immutable provenance, a group that goes
// away with its last draft. Like the other in-memory stores it enforces no foreign keys: it never looks a project,
// provider or model up, which the SQLite adapter does.
class InMemoryRunDraftRepository final : public IRunDraftRepository {
public:
    common::Result<RunDraft> insert(RunDraft draft) override;
    common::Result<RunDraft> update(RunDraft draft) override;
    [[nodiscard]] common::Result<std::optional<RunDraft>> findById(const RunDraftId& id) const override;
    [[nodiscard]] common::Result<std::vector<RunDraft>> listAll() const override;
    common::Result<void> remove(const RunDraftId& id) override;
    common::Result<std::vector<RunDraft>> insertGroup(RunDraftGroup group, std::vector<RunDraft> drafts) override;
    [[nodiscard]] common::Result<std::optional<RunDraftGroup>> findGroupById(
        const RunDraftGroupId& id) const override;
    [[nodiscard]] common::Result<std::optional<RunDraftGroup>> findGroupByIdempotencyKey(
        const std::string& idempotencyKey) const override;
    [[nodiscard]] common::Result<std::vector<RunDraft>> listGroupDrafts(const RunDraftGroupId& id) const override;

    // Only for InMemoryFollowUpPlanRepository::confirmDrafts: the insertion of a group of the follow-up origin, which the
    // public insertGroup refuses. It accepts nothing else (`run_draft_group.invalid_group`). `whileLocked` (optional, must not
    // throw and must not call back into this repository) runs after the rows are in place and BEFORE this store's lock is
    // released, so the caller's own record of the same operation changes in the same critical section: no reader of the Draft
    // store can see the group without it.
    common::Result<std::vector<RunDraft>> insertConfirmedFollowUpGroup(
        RunDraftGroup group, std::vector<RunDraft> drafts, const std::function<void()>& whileLocked = {});

private:
    common::Result<std::vector<RunDraft>> insertGroupChecked(
        RunDraftGroup group, std::vector<RunDraft> drafts, const std::function<void()>& whileLocked = {});

    mutable std::mutex mutex_;
    std::unordered_map<RunDraftId, RunDraft> drafts_;
    std::unordered_map<RunDraftGroupId, RunDraftGroup> groups_;
};

class InMemoryModelExecutionRepository final : public IModelExecutionRepository {
public:
    common::Result<ModelExecution> insertExecution(ModelExecution value) override;
    common::Result<ModelExecution> updateExecution(ModelExecution value) override;
    [[nodiscard]] common::Result<std::optional<ModelExecution>> findExecution(const ModelExecutionId& id) const override;
    [[nodiscard]] common::Result<std::vector<ModelExecution>> listExecutions(ModelExecutionSource source, const std::string& sourceId) const override;
    [[nodiscard]] common::Result<std::vector<ModelExecution>> listProjectExecutions(const ProjectId& projectId) const override;
    [[nodiscard]] common::Result<std::vector<ModelExecution>> listUnfinishedExecutions() const override;
    common::Result<QualityEvaluation> insertEvaluation(QualityEvaluation value) override;
    [[nodiscard]] common::Result<std::optional<QualityEvaluation>> findEvaluation(const QualityEvaluationId& id) const override;
    [[nodiscard]] common::Result<std::vector<QualityEvaluation>> listEvaluations(const ModelExecutionId& executionId) const override;

private:
    mutable std::mutex mutex_;
    std::unordered_map<ModelExecutionId, ModelExecution> executions_;
    std::unordered_map<QualityEvaluationId, QualityEvaluation> evaluations_;
};

// Thread-safe in-memory adapter for agent tool sessions and provider turns.
class InMemoryAgentToolSessionRepository final : public IAgentToolSessionRepository {
public:
    common::Result<AgentToolSession> insertSession(AgentToolSession session) override;
    common::Result<AgentToolSession> updateSession(AgentToolSession session) override;
    [[nodiscard]] common::Result<std::optional<AgentToolSession>> findSession(
        const std::string& sessionId) const override;
    [[nodiscard]] common::Result<std::vector<AgentToolSession>> listForRun(
        const std::string& runId) const override;
    common::Result<AgentProviderTurn> insertTurn(AgentProviderTurn turn) override;
    [[nodiscard]] common::Result<std::vector<AgentProviderTurn>> listTurns(
        const std::string& sessionId) const override;
    [[nodiscard]] common::Result<std::size_t> interruptActiveSessions() override;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, AgentToolSession> sessions_;
    std::unordered_map<std::string, std::vector<AgentProviderTurn>> turns_; // keyed by sessionId
};

// Thread-safe in-memory adapter for tool call audit records.
class InMemoryAgentToolAuditRepository final : public IAgentToolAuditRepository {
public:
    common::Result<void> insert(const AgentToolAuditRecord& record) override;
    common::Result<void> update(const AgentToolAuditRecord& record) override;
    [[nodiscard]] common::Result<std::optional<AgentToolAuditRecord>> find(
        const std::string& auditId) const override;
    [[nodiscard]] common::Result<std::vector<AgentToolAuditRecord>> listForSession(
        const std::string& sessionId) const override;
    [[nodiscard]] common::Result<std::size_t> interruptRunning(
        const std::string& sessionId) override;
    [[nodiscard]] common::Result<std::size_t> interruptAllRunning() override;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, AgentToolAuditRecord> records_;
};

} // namespace ai::application
