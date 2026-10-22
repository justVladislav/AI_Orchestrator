#pragma once

#include "ai/application/IAgentToolAuditRepository.hpp"
#include "ai/application/IAgentToolSessionRepository.hpp"
#include "ai/application/IApplicationSettingsRepository.hpp"
#include "ai/application/IArtifactRepository.hpp"
#include "ai/application/IRunWarningRepository.hpp"
#include "ai/application/IProjectReadinessRepository.hpp"
#include "ai/application/IVerificationScratchSettingRepository.hpp"
#include "ai/application/IChangeApprovalRepository.hpp"
#include "ai/application/IChangeProposalRepository.hpp"
#include "ai/application/IChatMemoryRepository.hpp"
#include "ai/application/IChatRepository.hpp"
#include "ai/application/IComparisonSessionRepository.hpp"
#include "ai/application/IExecutionProfileRepository.hpp"
#include "ai/application/IExecutionManifestRepository.hpp"
#include "ai/application/IFollowUpPlanRequestRepository.hpp"
#include "ai/application/IManifestTaskExecutionRepository.hpp"
#include "ai/application/IManifestVerificationBaselineNormalizationRepository.hpp"
#include "ai/application/IManifestVerificationContinuationRepository.hpp"
#include "ai/application/IManifestVerificationManualHandoffRepository.hpp"
#include "ai/application/IManifestWorkLedgerRepository.hpp"
#include "ai/application/IModelRepository.hpp"
#include "ai/application/IModelExecutionRepository.hpp"
#include "ai/application/IOutcomeBriefRepository.hpp"
#include "ai/application/IProjectMemoryCompactionRepository.hpp"
#include "ai/application/IProjectMemoryRepository.hpp"
#include "ai/application/IProjectRepository.hpp"
#include "ai/application/IProposalReviewRepository.hpp"
#include "ai/application/IRunWorkspaceScopeRepository.hpp"
#include "ai/application/ITaskRepository.hpp"
#include "ai/application/IProviderRepository.hpp"
#include "ai/application/IRunDraftRepository.hpp"
#include "ai/application/IRunRepository.hpp"
#include "ai/application/IVerificationBootstrapRepository.hpp"
#include "ai/application/IVerificationEnvironmentRepository.hpp"
#include "ai/application/IWorkspaceStateRepository.hpp"
#include "ai/common/Result.hpp"

#include <filesystem>
#include <memory>
#include <unordered_map>

namespace ai::infrastructure {

/**
 * Shared SQLite connection and migration boundary.
 *
 * Opening is explicit and fallible so desktop/CLI composition roots can show a
 * controlled persistence error instead of silently replacing durable storage
 * with an in-memory repository. Repository instances share this object and its
 * lock, which also keeps multi-statement stage replacement atomic.
 */
class SqliteDatabase final {
public:
    [[nodiscard]] static common::Result<std::shared_ptr<SqliteDatabase>> open(
        const std::filesystem::path& databasePath);

    ~SqliteDatabase();

    SqliteDatabase(const SqliteDatabase&) = delete;
    SqliteDatabase& operator=(const SqliteDatabase&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept;

private:
    struct Impl;

    explicit SqliteDatabase(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;

    friend class SqliteProjectRepository;
    friend class SqliteTaskRepository;
    friend class SqliteRunRepository;
    friend class SqliteArtifactRepository;
    friend class SqliteRunWarningRepository;
    friend class SqliteVerificationScratchSettingRepository;
    friend class SqliteProjectReadinessRepository;
    friend class SqliteWorkspaceStateRepository;
    friend class SqliteProviderRepository;
    friend class SqliteModelRepository;
    friend class SqliteExecutionProfileRepository;
    friend class SqliteExecutionManifestRepository;
    friend class SqliteManifestTaskExecutionRepository;
    friend class SqliteManifestWorkLedgerRepository;
    friend class SqliteManifestVerificationContinuationRepository;
    friend class SqliteManifestVerificationManualHandoffRepository;
    friend class SqliteManifestVerificationBaselineNormalizationRepository;
    friend class SqliteRunWorkspaceScopeRepository;
    friend class SqliteVerificationEnvironmentRepository;
    friend class SqliteVerificationBootstrapRepository;
    friend class SqliteRunDraftRepository;
    friend class SqliteOutcomeBriefRepository;
    friend class SqliteFollowUpPlanRepository;
    friend class SqliteChatRepository;
    friend class SqliteChatMemoryRepository;
    friend class SqliteProjectMemoryRepository;
    friend class SqliteProjectMemoryCompactionRepository;
    friend class SqliteModelExecutionRepository;
    friend class SqliteComparisonSessionRepository;
    friend class SqliteApplicationSettingsRepository;
    friend class SqliteChangeProposalRepository;
    friend class SqliteChangeApprovalRepository;
    friend class SqliteProposalReviewRepository;
    friend class SqliteAgentToolSessionRepository;
    friend class SqliteAgentToolAuditRepository;
};

class SqliteTaskRepository final : public application::ITaskRepository {
public:
    explicit SqliteTaskRepository(std::shared_ptr<SqliteDatabase> database);
    common::Result<application::ProjectTask> insert(application::ProjectTask task) override;
    common::Result<application::ProjectTask> update(application::ProjectTask task) override;
    [[nodiscard]] common::Result<std::optional<application::ProjectTask>> findById(
        const application::TaskId& id) const override;
    [[nodiscard]] common::Result<std::vector<application::ProjectTask>> list(
        const std::optional<application::ProjectId>& projectId = std::nullopt) const override;
    common::Result<void> linkRun(
        const application::TaskId& taskId, const application::RunId& runId) override;
    [[nodiscard]] common::Result<std::vector<application::RunId>> linkedRuns(
        const application::TaskId& taskId) const override;
private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of the project persistence port. */
class SqliteProjectRepository final : public application::IProjectRepository {
public:
    explicit SqliteProjectRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<application::Project> insert(application::Project project) override;
    common::Result<application::Project> update(application::Project project) override;
    [[nodiscard]] common::Result<std::optional<application::Project>> findById(
        const application::ProjectId& id) const override;
    [[nodiscard]] common::Result<std::optional<application::Project>> findByRootPath(
        const std::filesystem::path& rootPath) const override;
    [[nodiscard]] common::Result<std::vector<application::Project>> listAll() const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

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

/** SQLite implementation of MW-P5.1's durable verification-continuation checkpoint. */
class SqliteManifestVerificationContinuationRepository final
    : public application::IManifestVerificationContinuationRepository {
public:
    explicit SqliteManifestVerificationContinuationRepository(
        std::shared_ptr<SqliteDatabase> database);

    common::Result<void> insert(
        application::ManifestVerificationContinuationCheckpoint checkpoint) override;
    [[nodiscard]] common::Result<
        std::optional<application::ManifestVerificationContinuationCheckpoint>>
    findBySourceRun(const application::RunId& sourceRunId) const override;
    common::Result<void> insertAttribution(
        application::ManifestVerificationAttributionCheckpoint checkpoint) override;
    [[nodiscard]] common::Result<
        std::optional<application::ManifestVerificationAttributionCheckpoint>>
    findAttributionBySourceRun(const application::RunId& sourceRunId) const override;
    common::Result<void> insertInput(
        application::ManifestVerificationContinuationInput input) override;
    [[nodiscard]] common::Result<
        std::optional<application::ManifestVerificationContinuationInput>>
    findInputBySourceRun(const application::RunId& sourceRunId) const override;
    [[nodiscard]] common::Result<
        std::optional<application::ManifestVerificationContinuationInput>>
    findInputByChildRun(const application::RunId& childRunId) const override;
    [[nodiscard]] common::Result<std::vector<application::ManifestVerificationContinuationInput>>
    listInputs() const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of QML-P4's immutable manual-handoff contract + attempts. */
class SqliteManifestVerificationManualHandoffRepository final
    : public application::IManifestVerificationManualHandoffRepository {
public:
    explicit SqliteManifestVerificationManualHandoffRepository(
        std::shared_ptr<SqliteDatabase> database);

    common::Result<void> insertHandoff(
        application::ManifestVerificationManualHandoff handoff) override;
    [[nodiscard]] common::Result<
        std::optional<application::ManifestVerificationManualHandoff>>
    findHandoffByAnchorRun(const application::RunId& verificationAnchorRunId) const override;
    [[nodiscard]] common::Result<
        std::optional<application::ManifestVerificationManualHandoff>>
    findHandoffById(const std::string& handoffId) const override;
    [[nodiscard]] common::Result<
        std::optional<application::ManifestVerificationManualHandoff>>
    findHandoffByMemberProposalId(const application::ChangeSetId& proposalId) const override;
    [[nodiscard]] common::Result<std::vector<application::ManifestVerificationManualHandoff>>
    listAllHandoffs() const override;
    common::Result<void> discardHandoff(const std::string& handoffId) override;

    common::Result<void> insertAttempt(
        application::ManifestVerificationManualHandoffAttempt attempt) override;
    [[nodiscard]] common::Result<
        std::optional<application::ManifestVerificationManualHandoffAttempt>>
    findAttemptByRunId(const application::RunId& attemptRunId) const override;
    [[nodiscard]] common::Result<
        std::optional<application::ManifestVerificationManualHandoffAttempt>>
    findAttemptByFingerprint(
        const std::string& handoffId,
        const std::string& editedSnapshotFingerprint) const override;
    [[nodiscard]] common::Result<
        std::vector<application::ManifestVerificationManualHandoffAttempt>>
    listAttemptsForHandoff(const std::string& handoffId) const override;
    [[nodiscard]] common::Result<
        std::vector<application::ManifestVerificationManualHandoffAttempt>>
    listAllAttempts() const override;

    common::Result<void> insertAttemptResult(
        application::ManifestVerificationManualHandoffAttemptResult result) override;
    [[nodiscard]] common::Result<
        std::optional<application::ManifestVerificationManualHandoffAttemptResult>>
    findAttemptResultByAttemptId(const std::string& attemptId) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of MW-P5.6D's immutable legacy-baseline-drift normalization proof. */
class SqliteManifestVerificationBaselineNormalizationRepository final
    : public application::IManifestVerificationBaselineNormalizationRepository {
public:
    explicit SqliteManifestVerificationBaselineNormalizationRepository(
        std::shared_ptr<SqliteDatabase> database);

    common::Result<void> insert(
        application::ManifestVerificationBaselineNormalization normalization) override;
    [[nodiscard]] common::Result<
        std::optional<application::ManifestVerificationBaselineNormalization>>
    findBySourceRun(const application::RunId& sourceRunId) const override;

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

/** SQLite implementation of the AG-18.1 verification environment snapshot. */
class SqliteVerificationEnvironmentRepository final
    : public application::IVerificationEnvironmentRepository {
public:
    explicit SqliteVerificationEnvironmentRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<void> recordSnapshot(
        application::VerificationEnvironmentSnapshot snapshot) override;
    [[nodiscard]] common::Result<std::optional<application::VerificationEnvironmentSnapshot>>
    findForRun(const application::RunId& runId) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of the AG-18.3 bootstrap/action-required lifecycle. */
class SqliteVerificationBootstrapRepository final
    : public application::IVerificationBootstrapRepository {
public:
    explicit SqliteVerificationBootstrapRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<void> recordDecision(
        application::VerificationBootstrapDecision decision) override;
    [[nodiscard]] common::Result<std::optional<application::VerificationBootstrapDecision>>
    findDecisionForRun(const application::RunId& runId) const override;
    common::Result<void> appendTransition(
        application::VerificationBootstrapTransition transition) override;
    [[nodiscard]] common::Result<std::vector<application::VerificationBootstrapTransition>>
    listTransitionsForRun(const application::RunId& runId) const override;
    common::Result<void> recordRetryLink(
        application::VerificationBootstrapRetryLink link) override;
    [[nodiscard]] common::Result<std::optional<application::VerificationBootstrapRetryLink>>
    findRetryLinkForPredecessor(const application::RunId& predecessorRunId) const override;

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

/** SQLite implementation of the verification-folder setting port (table verification_scratch_setting, schema step 69). */
class SqliteVerificationScratchSettingRepository final : public application::IVerificationScratchSettingRepository {
public:
    explicit SqliteVerificationScratchSettingRepository(std::shared_ptr<SqliteDatabase> database);

    [[nodiscard]] common::Result<std::optional<std::filesystem::path>> load() const override;
    common::Result<void> store(const std::optional<std::filesystem::path>& root) override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of the per-project readiness port (table project_verification_readiness, schema step 70). */
class SqliteProjectReadinessRepository final : public application::IProjectReadinessRepository {
public:
    explicit SqliteProjectReadinessRepository(std::shared_ptr<SqliteDatabase> database);

    [[nodiscard]] common::Result<std::optional<application::ProjectReadinessRecord>> find(
        const application::ProjectId& projectId) const override;
    common::Result<void> upsert(const application::ProjectReadinessRecord& record) override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of the single active-project workspace state port. */
class SqliteWorkspaceStateRepository final : public application::IWorkspaceStateRepository {
public:
    explicit SqliteWorkspaceStateRepository(std::shared_ptr<SqliteDatabase> database);

    [[nodiscard]] common::Result<std::optional<application::ProjectId>> activeProjectId() const override;
    common::Result<void> setActiveProjectId(
        const std::optional<application::ProjectId>& projectId) override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of the provider catalog's provider persistence port. */
class SqliteProviderRepository final : public application::IProviderRepository {
public:
    explicit SqliteProviderRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<application::Provider> insert(application::Provider provider) override;
    common::Result<application::Provider> update(application::Provider provider) override;
    [[nodiscard]] common::Result<std::optional<application::Provider>> findById(
        const application::ProviderId& id) const override;
    [[nodiscard]] common::Result<std::vector<application::Provider>> listAll() const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of the provider catalog's model persistence port. */
class SqliteModelRepository final : public application::IModelRepository {
public:
    explicit SqliteModelRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<application::Model> insert(application::Model model) override;
    common::Result<application::Model> update(application::Model model) override;
    [[nodiscard]] common::Result<std::optional<application::Model>> findById(
        const application::ModelId& id) const override;
    [[nodiscard]] common::Result<std::vector<application::Model>> listAll(
        const std::optional<application::ProviderId>& providerId = std::nullopt) const override;
    common::Result<void> setDefaultModel(
        const application::ProviderId& providerId,
        const application::ModelId& modelId) override;
    common::Result<void> remove(const application::ModelId& id) override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of execution profiles and their assignment lists. */
class SqliteExecutionProfileRepository final : public application::IExecutionProfileRepository {
public:
    explicit SqliteExecutionProfileRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<application::ExecutionProfile> insert(application::ExecutionProfile profile) override;
    common::Result<application::ExecutionProfile> update(application::ExecutionProfile profile) override;
    [[nodiscard]] common::Result<std::optional<application::ExecutionProfile>> findById(
        const application::ExecutionProfileId& id) const override;
    [[nodiscard]] common::Result<std::vector<application::ExecutionProfile>> listAll() const override;
    common::Result<void> setDefaultProfile(const application::ExecutionProfileId& profileId) override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

class SqliteRunDraftRepository final : public application::IRunDraftRepository {
public:
    explicit SqliteRunDraftRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<application::RunDraft> insert(application::RunDraft draft) override;
    common::Result<application::RunDraft> update(application::RunDraft draft) override;
    [[nodiscard]] common::Result<std::optional<application::RunDraft>> findById(
        const application::RunDraftId& id) const override;
    [[nodiscard]] common::Result<std::vector<application::RunDraft>> listAll() const override;
    // Also removes the draft's group in the same transaction when it was the group's last draft (PC-3B).
    common::Result<void> remove(const application::RunDraftId& id) override;
    // PC-3B: the group row and every draft with its roster rows in ONE transaction (schema v62).
    common::Result<std::vector<application::RunDraft>> insertGroup(
        application::RunDraftGroup group, std::vector<application::RunDraft> drafts) override;
    [[nodiscard]] common::Result<std::optional<application::RunDraftGroup>> findGroupById(
        const application::RunDraftGroupId& id) const override;
    [[nodiscard]] common::Result<std::optional<application::RunDraftGroup>> findGroupByIdempotencyKey(
        const std::string& idempotencyKey) const override;
    [[nodiscard]] common::Result<std::vector<application::RunDraft>> listGroupDrafts(
        const application::RunDraftGroupId& id) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

// PC-4B: the append-only revisions of the Outcome Brief and the derived head pointer (schema v64). The write gate is
// validateOutcomeBrief; a revision and its head row are written in ONE transaction, and every read passes the gate again.
class SqliteOutcomeBriefRepository final : public application::IOutcomeBriefRepository {
public:
    explicit SqliteOutcomeBriefRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<application::OutcomeBriefAppendResult> append(
        application::OutcomeBrief brief, application::OutcomeBriefExpectedHead expected) override;
    [[nodiscard]] common::Result<std::optional<application::OutcomeBriefRevision>> findHead(
        const application::RunId& runId) const override;
    [[nodiscard]] common::Result<std::optional<application::OutcomeBriefRevision>> findRevision(
        const application::OutcomeBriefRevisionId& id) const override;
    [[nodiscard]] common::Result<std::vector<application::OutcomeBriefRevision>> listRevisions(
        const application::RunId& runId, std::size_t limit) const override;
    [[nodiscard]] common::Result<std::vector<application::OutcomeBriefRevision>> listHeads(
        const application::ProjectId& projectId, std::size_t limit) const override;
    [[nodiscard]] common::Result<std::optional<application::OutcomeBriefHeadVersion>> findHeadVersionForAppend(
        const application::RunId& runId) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

// PC-5B: the durable follow-up plan request (schema v66) and the atomic Confirm of its plan. Every rule is the shared one of
// FollowUpPlanRequest.hpp; this adapter stores what those functions decide. A transition is a compare-and-set written in ONE
// transaction (BEGIN IMMEDIATE), and the Confirm writes the Draft group, its Drafts and drafts_saved in ONE transaction. The
// Run authority is read from `runs` inside the transaction and again on every read; the schema triggers keep the identity
// and the quote immutable and the states on the agreed edges even for a writer that bypasses this class.
class SqliteFollowUpPlanRepository final : public application::IFollowUpPlanRequestRepository,
                                           public application::IFollowUpPlanConfirmPort {
public:
    explicit SqliteFollowUpPlanRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<application::FollowUpInsertResult> insertQuoted(application::FollowUpPlanRequest request) override;
    common::Result<application::FollowUpPlanRequest> transition(
        const application::FollowUpPlanRequestId& id,
        application::FollowUpTransition transition,
        std::chrono::system_clock::time_point now) override;
    [[nodiscard]] common::Result<std::optional<application::FollowUpPlanRequest>> findById(
        const application::FollowUpPlanRequestId& id) const override;
    [[nodiscard]] common::Result<std::optional<application::FollowUpPlanRequest>> findOpenForSourceRun(
        const application::RunId& runId) const override;
    [[nodiscard]] common::Result<std::optional<application::FollowUpPlanRequest>> findLatestForSourceRun(
        const application::RunId& runId) const override;
    [[nodiscard]] common::Result<std::vector<application::FollowUpPlanRequest>> listForProject(
        const application::ProjectId& projectId, std::size_t limit) const override;
    [[nodiscard]] common::Result<std::vector<application::FollowUpPlanRequest>> listByState(
        application::FollowUpPlanRequestState state, std::size_t limit) const override;
    [[nodiscard]] common::Result<std::vector<application::FollowUpPlanRequest>> listByStateAfter(
        application::FollowUpPlanRequestState state, std::size_t limit,
        const std::optional<application::FollowUpStateCursor>& after) const override;

    common::Result<application::FollowUpConfirmResult> confirmDrafts(
        const application::FollowUpPlanRequestId& requestId,
        const std::string& expectedPlanFingerprint,
        application::RunDraftGroup group,
        std::vector<application::RunDraft> drafts,
        std::chrono::system_clock::time_point now) override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

class SqliteChatRepository final : public application::IChatRepository {
public:
    explicit SqliteChatRepository(std::shared_ptr<SqliteDatabase> database);
    common::Result<application::ChatConversation> insertConversation(application::ChatConversation) override;
    common::Result<application::ChatConversation> updateConversation(application::ChatConversation) override;
    common::Result<void> removeConversation(const application::ConversationId&) override;
    [[nodiscard]] common::Result<std::optional<application::ChatConversation>> findConversation(const application::ConversationId&) const override;
    [[nodiscard]] common::Result<std::vector<application::ChatConversation>> listConversations() const override;
    common::Result<application::PreparedChatTurn> insertPreparedTurn(
        application::ChatConversation, application::PreparedChatTurn) override;
    common::Result<application::ChatMessage> insertMessage(application::ChatMessage) override;
    common::Result<application::ChatMessage> updateMessage(application::ChatMessage) override;
    [[nodiscard]] common::Result<std::optional<application::ChatMessage>> findMessage(const application::ChatMessageId&) const override;
    [[nodiscard]] common::Result<std::vector<application::ChatMessage>> listMessages(const application::ConversationId&) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

class SqliteChatMemoryRepository final : public application::IChatMemoryRepository {
public:
    explicit SqliteChatMemoryRepository(std::shared_ptr<SqliteDatabase> database);
    common::Result<application::ChatMemorySegment> insertMemorySegment(application::ChatMemorySegment) override;
    [[nodiscard]] common::Result<std::vector<application::ChatMemorySegment>> listMemorySegments(
        const application::ConversationId&) const override;
    [[nodiscard]] common::Result<std::optional<application::ChatMemorySegment>> findMemorySegment(
        const application::ChatMemorySegmentId&) const override;
    common::Result<void> removeMemorySegments(const application::ConversationId&) override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

class SqliteProjectMemoryRepository final : public application::IProjectMemoryRepository {
public:
    explicit SqliteProjectMemoryRepository(std::shared_ptr<SqliteDatabase> database);
    common::Result<application::ProjectMemorySegment> insertMemorySegment(application::ProjectMemorySegment) override;
    [[nodiscard]] common::Result<std::vector<application::ProjectMemorySegment>> listMemorySegments(
        const application::ProjectId&) const override;
    [[nodiscard]] common::Result<application::ProjectMemoryRecentPage> listRecentMemorySegments(
        const application::ProjectId&, std::size_t limit) const override;
    [[nodiscard]] common::Result<std::optional<application::ProjectMemorySegment>> findMemorySegment(
        const application::ProjectMemorySegmentId&) const override;
    common::Result<void> removeMemorySegments(const application::ProjectId&) override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of the AG-11 compaction provenance port. */
class SqliteProjectMemoryCompactionRepository final
    : public application::IProjectMemoryCompactionRepository {
public:
    explicit SqliteProjectMemoryCompactionRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<void> insertCompactionRecord(
        const application::ProjectMemoryCompactionRecord& record) override;
    common::Result<void> commitSuccessfulCompaction(
        const application::ProjectMemoryCompactionRecord& record,
        application::ProjectMemorySegment resultSegment) override;

    [[nodiscard]] common::Result<std::vector<application::ProjectMemoryCompactionRecord>>
    listCompactionRecords(const application::ProjectId& projectId) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

class SqliteModelExecutionRepository final : public application::IModelExecutionRepository {
public:
    explicit SqliteModelExecutionRepository(std::shared_ptr<SqliteDatabase> database);
    common::Result<application::ModelExecution> insertExecution(application::ModelExecution value) override;
    common::Result<application::ModelExecution> updateExecution(application::ModelExecution value) override;
    [[nodiscard]] common::Result<std::optional<application::ModelExecution>> findExecution(const application::ModelExecutionId& id) const override;
    [[nodiscard]] common::Result<std::vector<application::ModelExecution>> listExecutions(application::ModelExecutionSource source, const std::string& sourceId) const override;
    [[nodiscard]] common::Result<std::vector<application::ModelExecution>> listProjectExecutions(const application::ProjectId& projectId) const override;
    [[nodiscard]] common::Result<std::vector<application::ModelExecution>> listUnfinishedExecutions() const override;
    common::Result<application::QualityEvaluation> insertEvaluation(application::QualityEvaluation value) override;
    [[nodiscard]] common::Result<std::optional<application::QualityEvaluation>> findEvaluation(const application::QualityEvaluationId& id) const override;
    [[nodiscard]] common::Result<std::vector<application::QualityEvaluation>> listEvaluations(const application::ModelExecutionId& executionId) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

class SqliteComparisonSessionRepository final
    : public application::IComparisonSessionRepository {
public:
    explicit SqliteComparisonSessionRepository(std::shared_ptr<SqliteDatabase> database);
    common::Result<application::ComparisonSession> insertSession(
        application::ComparisonSession value) override;
    common::Result<application::ComparisonSession> updateSession(
        application::ComparisonSession value) override;
    [[nodiscard]] common::Result<std::optional<application::ComparisonSession>> findSession(
        const application::ComparisonSessionId& id) const override;
    [[nodiscard]] common::Result<std::vector<application::ComparisonSession>> listSessions(
        const std::optional<application::ProjectId>& projectId = std::nullopt) const override;
    [[nodiscard]] common::Result<std::vector<application::ComparisonSession>>
    listUnfinishedSessions() const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

class SqliteApplicationSettingsRepository final
    : public application::IApplicationSettingsRepository {
public:
    explicit SqliteApplicationSettingsRepository(
        std::shared_ptr<SqliteDatabase> database);
    [[nodiscard]] common::Result<application::ApplicationSettings> load() const override;
    common::Result<application::ApplicationSettings> save(
        application::ApplicationSettings value) override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of the Stage 1 change-proposal persistence port. */
class SqliteChangeProposalRepository final : public application::IChangeProposalRepository {
public:
    explicit SqliteChangeProposalRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<void> insertChangeSet(
        const application::ProposedChangeSet& changeSet) override;

    common::Result<application::ProposedChangeSet> updateChangeSetState(
        const application::ChangeSetId& id,
        application::ChangeSetState newState) override;

    common::Result<void> markMissingEvidence(
        const application::ChangeSetId& id, std::string code, std::string message) override;

    [[nodiscard]] common::Result<std::optional<application::ProposedChangeSet>> findChangeSet(
        const application::ChangeSetId& id) const override;

    [[nodiscard]] common::Result<std::vector<application::ProposedChangeSet>> listForRun(
        const application::RunId& runId) const override;

    [[nodiscard]] common::Result<std::vector<application::ProposedChangeSet>> listForProject(
        const application::ProjectId& projectId) const override;

    [[nodiscard]] common::Result<std::vector<application::ProposedFileChange>> listChanges(
        const application::ChangeSetId& changeSetId) const override;

    [[nodiscard]] common::Result<std::vector<application::ValidationFinding>> listValidationFindings(
        const application::ChangeSetId& changeSetId) const override;

    [[nodiscard]] common::Result<
        std::unordered_map<application::ChangeSetId, std::vector<application::ProposedFileChange>>>
    listChangesForSets(const std::vector<application::ChangeSetId>& changeSetIds) const override;

    [[nodiscard]] common::Result<
        std::unordered_map<application::ChangeSetId, std::vector<application::ValidationFinding>>>
    listValidationFindingsForSets(
        const std::vector<application::ChangeSetId>& changeSetIds) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of the change-set approval audit trail. */
class SqliteChangeApprovalRepository final : public application::IChangeApprovalRepository {
public:
    explicit SqliteChangeApprovalRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<void> recordApproval(
        const application::ApprovalRecord& record) override;

    [[nodiscard]] common::Result<std::optional<application::ApprovalRecord>> findApproval(
        const application::ChangeSetId& changeSetId) const override;

    common::Result<void> recordApplyOutcome(
        const application::ChangeSetId& changeSetId,
        const std::string& result,
        const std::optional<std::string>& errorCode,
        const std::vector<application::ApplyHashEntry>& hashLog,
        const std::optional<std::string>& postApplyWorkspaceFingerprint) override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of the durable human proposal-rejection port (AG-07). */
class SqliteProposalReviewRepository final : public application::IProposalReviewRepository {
public:
    explicit SqliteProposalReviewRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<void> recordRejection(
        const application::ProposalReviewRejection& record) override;

    [[nodiscard]] common::Result<std::optional<application::ProposalReviewRejection>>
    findRejection(const application::ChangeSetId& changeSetId) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of agent tool session and provider turn persistence. */
class SqliteAgentToolSessionRepository final : public application::IAgentToolSessionRepository {
public:
    explicit SqliteAgentToolSessionRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<application::AgentToolSession> insertSession(
        application::AgentToolSession session) override;
    common::Result<application::AgentToolSession> updateSession(
        application::AgentToolSession session) override;
    [[nodiscard]] common::Result<std::optional<application::AgentToolSession>> findSession(
        const std::string& sessionId) const override;
    [[nodiscard]] common::Result<std::vector<application::AgentToolSession>> listForRun(
        const std::string& runId) const override;
    common::Result<application::AgentProviderTurn> insertTurn(
        application::AgentProviderTurn turn) override;
    [[nodiscard]] common::Result<std::vector<application::AgentProviderTurn>> listTurns(
        const std::string& sessionId) const override;
    [[nodiscard]] common::Result<std::size_t> interruptActiveSessions() override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of tool-call audit records (agent_tool_calls table). */
class SqliteAgentToolAuditRepository final : public application::IAgentToolAuditRepository {
public:
    explicit SqliteAgentToolAuditRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<void> insert(
        const application::AgentToolAuditRecord& record) override;
    common::Result<void> update(
        const application::AgentToolAuditRecord& record) override;
    [[nodiscard]] common::Result<std::optional<application::AgentToolAuditRecord>> find(
        const std::string& auditId) const override;
    [[nodiscard]] common::Result<std::vector<application::AgentToolAuditRecord>> listForSession(
        const std::string& sessionId) const override;
    [[nodiscard]] common::Result<std::size_t> interruptRunning(
        const std::string& sessionId) override;
    [[nodiscard]] common::Result<std::size_t> interruptAllRunning() override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

} // namespace ai::infrastructure
