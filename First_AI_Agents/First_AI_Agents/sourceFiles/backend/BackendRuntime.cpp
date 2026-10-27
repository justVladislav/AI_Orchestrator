#include "ai/backend/BackendRuntime.hpp"

#include "ai/backend/StartupRecovery.hpp"

#include "ai/application/AgentToolExecutionService.hpp"
#include "ai/application/ExecutionManifestReadModelService.hpp"
#include "ai/application/ExecutionProfileWorkflowRunner.hpp"
#include "ai/application/FilesystemFollowUpFileLoader.hpp"
#include "ai/application/FollowUpComposerProcessor.hpp"
#include "ai/application/FollowUpPlanReadModel.hpp"
#include "ai/application/FollowUpPlanService.hpp"
#include "ai/application/FollowUpSourceReader.hpp"
#include "ai/application/ManifestWorkflowRunner.hpp"
#include "ai/application/FilesystemWorkspaceToolExecutor.hpp"
#include "ai/application/IAgentToolAuditRepository.hpp"
#include "ai/application/IAgentToolSessionRepository.hpp"
#include "ai/application/IChangeApprovalRepository.hpp"
#include "ai/application/IProposalReviewRepository.hpp"
#include "ai/application/ProposalReviewService.hpp"
#include "ai/application/RunWorkspaceScopeService.hpp"
#include "ai/application/VerificationEnvironmentDiscoveryService.hpp"
#include "ai/providers/IProviderAgentSession.hpp"
#include "ai/application/settings/IApplicationSettingsRepository.hpp"
#include "ai/application/IArtifactRepository.hpp"
#include "ai/application/IChatRepository.hpp"
#include "ai/application/comparison/IComparisonSessionRepository.hpp"
#include "ai/application/IModelExecutionRepository.hpp"
#include "ai/application/IArtifactStore.hpp"
#include "ai/application/IProjectMemoryRepository.hpp"
#include "ai/application/IProjectMemoryCompactionRepository.hpp"
#include "ai/application/projects/IProjectRepository.hpp"
#include "ai/application/projects/ITaskRepository.hpp"
#include "ai/application/IProviderClient.hpp"
#include "ai/application/IRunRepository.hpp"
#include "ai/application/MockWorkflowRunner.hpp"
#include "ai/application/RunManager.hpp"
#include "ai/application/RunPathClaimRegistry.hpp"
#include "ai/application/WorkflowRouter.hpp"
#include "ai/application/ChangeSetApplyService.hpp"
#include "ai/application/ManifestApplyVerificationGate.hpp"
#include "ai/application/ManifestVerificationContinuationPreparationService.hpp"
#include "ai/application/ManifestInterruptedTaskReconciler.hpp"
#include "ai/application/ManifestVerificationManualHandoffService.hpp"
#include "ai/application/ManifestVerificationManualHandoffReadModelService.hpp"
#include "ai/application/OutcomeBriefReconciler.hpp"
#include "ai/application/ProjectContextSnapshotService.hpp"
#include "ai/application/IChangeProposalStore.hpp"
#include "ai/infrastructure/DatabaseOwnerLock.hpp"
#include "ai/infrastructure/FileArtifactStore.hpp"
#include "ai/infrastructure/FileChangeProposalStore.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"

// These are optional, isolated dependencies gated by CMake options. Windows
// Credential Manager and macOS Keychain each have their own compile
// definition; real provider clients/sessions are independently selectable.
// BackendRuntime must still build when any adapter is absent and uses the
// controlled Unavailable* implementations below.
#if defined(AI_ORCHESTRATOR_HAVE_WINDOWS_CREDENTIALS)
#include "ai/infrastructure/WindowsCredentialStore.hpp"
#endif
#if defined(AI_ORCHESTRATOR_HAVE_MACOS_CREDENTIALS)
#include "ai/infrastructure/MacOSCredentialStore.hpp"
#endif
#if defined(AI_ORCHESTRATOR_HAVE_REAL_PROVIDERS)
#include "ai/backend/RealProviderClientFactory.hpp"
#endif
#if defined(AI_ORCHESTRATOR_HAVE_REAL_AGENT_SESSIONS)
#include "ai/providers/RealProviderAgentSessionFactory.hpp"
#endif

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <string_view>
#include <utility>
#include <vector>

namespace ai::backend {

namespace {

// Used only when SQLite failed to open or migrate at construction (see
// BackendRuntime::Impl::openStorage). Every operation reports a typed
// "storage.unavailable" ai::common::Result through the exact same channel a
// normal repository failure already uses, so ProjectService/
// OrchestrationService callers see a controlled, typed error instead of a
// crash or an undetectable silent fallback to a working-but-volatile store
// that would quietly lose the caller's data again on the next exit. These
// classes live in the backend implementation (not in a future UI adapter) so
// every composition root built on BackendRuntime gets the same guarantee.
class UnavailableProjectRepository final : public application::IProjectRepository {
public:
    explicit UnavailableProjectRepository(std::string reason)
        : reason_(std::move(reason))
    {
    }

    common::Result<application::Project> insert(application::Project) override
    {
        return fail<application::Project>();
    }
    common::Result<application::Project> update(application::Project) override
    {
        return fail<application::Project>();
    }
    [[nodiscard]] common::Result<std::optional<application::Project>> findById(
        const application::ProjectId&) const override
    {
        return fail<std::optional<application::Project>>();
    }
    [[nodiscard]] common::Result<std::optional<application::Project>> findByRootPath(
        const std::filesystem::path&) const override
    {
        return fail<std::optional<application::Project>>();
    }
    [[nodiscard]] common::Result<std::vector<application::Project>> listAll() const override
    {
        return fail<std::vector<application::Project>>();
    }

private:
    template <typename T>
    [[nodiscard]] common::Result<T> fail() const
    {
        return common::Result<T>::failure({"storage.unavailable", reason_});
    }

    std::string reason_;
};

class UnavailableTaskRepository final : public application::ITaskRepository {
public:
    explicit UnavailableTaskRepository(std::string reason) : reason_(std::move(reason)) {}
    common::Result<application::ProjectTask> insert(application::ProjectTask) override { return fail<application::ProjectTask>(); }
    common::Result<application::ProjectTask> update(application::ProjectTask) override { return fail<application::ProjectTask>(); }
    common::Result<std::optional<application::ProjectTask>> findById(const application::TaskId&) const override { return fail<std::optional<application::ProjectTask>>(); }
    common::Result<std::vector<application::ProjectTask>> list(const std::optional<application::ProjectId>&) const override { return fail<std::vector<application::ProjectTask>>(); }
    common::Result<void> linkRun(const application::TaskId&, const application::RunId&) override { return common::Result<void>::failure({"storage.unavailable", reason_}); }
    common::Result<std::vector<application::RunId>> linkedRuns(const application::TaskId&) const override { return fail<std::vector<application::RunId>>(); }
private:
    template <typename T> common::Result<T> fail() const { return common::Result<T>::failure({"storage.unavailable", reason_}); }
    std::string reason_;
};

class UnavailableRunRepository final : public application::IRunRepository {
public:
    explicit UnavailableRunRepository(std::string reason)
        : reason_(std::move(reason))
    {
    }

    common::Result<application::Run> insertRun(application::Run) override { return fail<application::Run>(); }
    common::Result<application::Run> insertRunWithStages(
        application::Run,
        std::vector<application::RunStage>) override
    {
        // Storage failure must reject the whole aggregate through the same
        // provider-neutral error channel as every other repository operation.
        return fail<application::Run>();
    }
    common::Result<application::Run> updateRun(application::Run) override { return fail<application::Run>(); }
    [[nodiscard]] common::Result<std::optional<application::Run>> findRun(
        const application::RunId&) const override
    {
        return fail<std::optional<application::Run>>();
    }
    [[nodiscard]] common::Result<std::vector<application::Run>> listRuns(
        const std::optional<application::ProjectId>&) const override
    {
        return fail<std::vector<application::Run>>();
    }
    [[nodiscard]] common::Result<std::vector<application::Run>> listUnfinishedRuns() const override
    {
        return fail<std::vector<application::Run>>();
    }
    common::Result<void> replaceStages(
        const application::RunId&, std::vector<application::RunStage>) override
    {
        return common::Result<void>::failure({"storage.unavailable", reason_});
    }
    common::Result<application::RunStage> updateStage(application::RunStage) override
    {
        return fail<application::RunStage>();
    }
    [[nodiscard]] common::Result<std::vector<application::RunStage>> listStages(
        const application::RunId&) const override
    {
        return fail<std::vector<application::RunStage>>();
    }

private:
    template <typename T>
    [[nodiscard]] common::Result<T> fail() const
    {
        return common::Result<T>::failure({"storage.unavailable", reason_});
    }

    std::string reason_;
};

class UnavailableExecutionManifestRepository final
    : public application::IExecutionManifestRepository {
public:
    explicit UnavailableExecutionManifestRepository(std::string reason)
        : reason_(std::move(reason))
    {
    }
    common::Result<application::ExecutionManifest> insert(
        application::ExecutionManifest) override
    {
        return common::Result<application::ExecutionManifest>::failure(
            {"storage.unavailable", reason_});
    }
    common::Result<std::optional<application::ExecutionManifest>> findForRun(
        const application::RunId&) const override
    {
        return common::Result<std::optional<application::ExecutionManifest>>::failure(
            {"storage.unavailable", reason_});
    }
private:
    std::string reason_;
};

class UnavailableManifestTaskExecutionRepository final
    : public application::IManifestTaskExecutionRepository {
public:
    explicit UnavailableManifestTaskExecutionRepository(std::string reason)
        : reason_(std::move(reason))
    {
    }
    common::Result<void> upsert(
        const application::RunId&, const std::string&,
        const application::ManifestTaskState&) override
    {
        return common::Result<void>::failure({"storage.unavailable", reason_});
    }
    common::Result<std::vector<application::ManifestTaskExecutionRecord>> listForRun(
        const application::RunId&) const override
    {
        return common::Result<std::vector<application::ManifestTaskExecutionRecord>>::failure(
            {"storage.unavailable", reason_});
    }
    common::Result<void> upsertNoChanges(
        const application::RunId&, const std::string&,
        const application::ManifestWorkerNoChanges&) override
    {
        return common::Result<void>::failure({"storage.unavailable", reason_});
    }
    common::Result<std::vector<application::ManifestTaskNoChangesRecord>> listNoChangesForRun(
        const application::RunId&) const override
    {
        return common::Result<std::vector<application::ManifestTaskNoChangesRecord>>::failure(
            {"storage.unavailable", reason_});
    }
private:
    std::string reason_;
};

// AG-14. Same controlled-failure guarantee as the repositories above.
class UnavailableManifestWorkLedgerRepository final
    : public application::IManifestWorkLedgerRepository {
public:
    explicit UnavailableManifestWorkLedgerRepository(std::string reason) : reason_(std::move(reason)) {}
    common::Result<void> insertEntry(application::ManifestWorkLedgerEntry) override { return failVoid(); }
    common::Result<std::optional<application::ManifestWorkLedgerEntry>> findEntry(
        const application::RunId&) const override { return fail<std::optional<application::ManifestWorkLedgerEntry>>(); }
    common::Result<std::vector<application::ManifestWorkLedgerEntry>> listRecentForProject(
        const application::ProjectId&, std::size_t) const override { return fail<std::vector<application::ManifestWorkLedgerEntry>>(); }
    common::Result<void> appendTransition(application::ManifestWorkLedgerReviewTransition) override { return failVoid(); }
    common::Result<std::vector<application::ManifestWorkLedgerReviewTransition>> listTransitionsForRun(
        const application::RunId&) const override { return fail<std::vector<application::ManifestWorkLedgerReviewTransition>>(); }
private:
    template<class T> common::Result<T> fail() const { return common::Result<T>::failure({"storage.unavailable", reason_}); }
    common::Result<void> failVoid() const { return common::Result<void>::failure({"storage.unavailable", reason_}); }
    std::string reason_;
};

class UnavailableManifestVerificationContinuationRepository final
    : public application::IManifestVerificationContinuationRepository {
public:
    explicit UnavailableManifestVerificationContinuationRepository(std::string reason)
        : reason_(std::move(reason)) {}
    common::Result<void> insert(application::ManifestVerificationContinuationCheckpoint) override { return failVoid(); }
    common::Result<std::optional<application::ManifestVerificationContinuationCheckpoint>> findBySourceRun(const application::RunId&) const override { return fail<std::optional<application::ManifestVerificationContinuationCheckpoint>>(); }
    common::Result<void> insertAttribution(application::ManifestVerificationAttributionCheckpoint) override { return failVoid(); }
    common::Result<std::optional<application::ManifestVerificationAttributionCheckpoint>> findAttributionBySourceRun(const application::RunId&) const override { return fail<std::optional<application::ManifestVerificationAttributionCheckpoint>>(); }
    common::Result<void> insertInput(application::ManifestVerificationContinuationInput) override { return failVoid(); }
    common::Result<std::optional<application::ManifestVerificationContinuationInput>> findInputBySourceRun(const application::RunId&) const override { return fail<std::optional<application::ManifestVerificationContinuationInput>>(); }
    common::Result<std::optional<application::ManifestVerificationContinuationInput>> findInputByChildRun(const application::RunId&) const override { return fail<std::optional<application::ManifestVerificationContinuationInput>>(); }
    common::Result<std::vector<application::ManifestVerificationContinuationInput>> listInputs() const override { return fail<std::vector<application::ManifestVerificationContinuationInput>>(); }
private:
    template<class T> common::Result<T> fail() const { return common::Result<T>::failure({"storage.unavailable", reason_}); }
    common::Result<void> failVoid() const { return common::Result<void>::failure({"storage.unavailable", reason_}); }
    std::string reason_;
};

// QML-P4. Same controlled-failure guarantee as the repositories above.
class UnavailableManifestVerificationManualHandoffRepository final
    : public application::IManifestVerificationManualHandoffRepository {
public:
    explicit UnavailableManifestVerificationManualHandoffRepository(std::string reason)
        : reason_(std::move(reason)) {}
    common::Result<void> insertHandoff(application::ManifestVerificationManualHandoff) override { return failVoid(); }
    common::Result<std::optional<application::ManifestVerificationManualHandoff>> findHandoffByAnchorRun(const application::RunId&) const override { return fail<std::optional<application::ManifestVerificationManualHandoff>>(); }
    common::Result<std::optional<application::ManifestVerificationManualHandoff>> findHandoffById(const std::string&) const override { return fail<std::optional<application::ManifestVerificationManualHandoff>>(); }
    common::Result<std::optional<application::ManifestVerificationManualHandoff>> findHandoffByMemberProposalId(const application::ChangeSetId&) const override { return fail<std::optional<application::ManifestVerificationManualHandoff>>(); }
    common::Result<std::vector<application::ManifestVerificationManualHandoff>> listAllHandoffs() const override { return fail<std::vector<application::ManifestVerificationManualHandoff>>(); }
    common::Result<void> discardHandoff(const std::string&) override { return failVoid(); }
    common::Result<void> insertAttempt(application::ManifestVerificationManualHandoffAttempt) override { return failVoid(); }
    common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>> findAttemptByRunId(const application::RunId&) const override { return fail<std::optional<application::ManifestVerificationManualHandoffAttempt>>(); }
    common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>> findAttemptByFingerprint(const std::string&, const std::string&) const override { return fail<std::optional<application::ManifestVerificationManualHandoffAttempt>>(); }
    common::Result<std::vector<application::ManifestVerificationManualHandoffAttempt>> listAttemptsForHandoff(const std::string&) const override { return fail<std::vector<application::ManifestVerificationManualHandoffAttempt>>(); }
    common::Result<std::vector<application::ManifestVerificationManualHandoffAttempt>> listAllAttempts() const override { return fail<std::vector<application::ManifestVerificationManualHandoffAttempt>>(); }
    common::Result<void> insertAttemptResult(application::ManifestVerificationManualHandoffAttemptResult) override { return failVoid(); }
    common::Result<std::optional<application::ManifestVerificationManualHandoffAttemptResult>> findAttemptResultByAttemptId(const std::string&) const override { return fail<std::optional<application::ManifestVerificationManualHandoffAttemptResult>>(); }
private:
    template<class T> common::Result<T> fail() const { return common::Result<T>::failure({"storage.unavailable", reason_}); }
    common::Result<void> failVoid() const { return common::Result<void>::failure({"storage.unavailable", reason_}); }
    std::string reason_;
};

// AG-15. Same controlled-failure guarantee as the repositories above.
class UnavailableRunWorkspaceScopeRepository final
    : public application::IRunWorkspaceScopeRepository {
public:
    explicit UnavailableRunWorkspaceScopeRepository(std::string reason) : reason_(std::move(reason)) {}
    common::Result<void> freeze(application::RunWorkspaceScope) override { return failVoid(); }
    common::Result<std::optional<application::RunWorkspaceScope>> findForRun(
        const application::RunId&) const override { return fail<std::optional<application::RunWorkspaceScope>>(); }
    common::Result<void> recordExpansion(application::RunWorkspaceScopeExpansion) override { return failVoid(); }
    common::Result<std::vector<application::RunWorkspaceScopeExpansion>> listExpansionsForRun(
        const application::RunId&) const override { return fail<std::vector<application::RunWorkspaceScopeExpansion>>(); }
    common::Result<void> recordDiagnosticRead(application::RunWorkspaceScopeDiagnosticRead) override { return failVoid(); }
    common::Result<std::vector<application::RunWorkspaceScopeDiagnosticRead>> listDiagnosticReadsForRun(
        const application::RunId&) const override { return fail<std::vector<application::RunWorkspaceScopeDiagnosticRead>>(); }
private:
    template<class T> common::Result<T> fail() const { return common::Result<T>::failure({"storage.unavailable", reason_}); }
    common::Result<void> failVoid() const { return common::Result<void>::failure({"storage.unavailable", reason_}); }
    std::string reason_;
};

// MW-P5.6D. Same controlled-failure guarantee as the repositories above.
class UnavailableManifestVerificationBaselineNormalizationRepository final
    : public application::IManifestVerificationBaselineNormalizationRepository {
public:
    explicit UnavailableManifestVerificationBaselineNormalizationRepository(std::string reason)
        : reason_(std::move(reason)) {}
    common::Result<void> insert(application::ManifestVerificationBaselineNormalization) override
    {
        return failVoid();
    }
    common::Result<std::optional<application::ManifestVerificationBaselineNormalization>>
    findBySourceRun(const application::RunId&) const override
    {
        return fail<std::optional<application::ManifestVerificationBaselineNormalization>>();
    }
private:
    template<class T> common::Result<T> fail() const { return common::Result<T>::failure({"storage.unavailable", reason_}); }
    common::Result<void> failVoid() const { return common::Result<void>::failure({"storage.unavailable", reason_}); }
    std::string reason_;
};

// AG-18.1. Same controlled-failure guarantee as the repositories above.
class UnavailableVerificationEnvironmentRepository final
    : public application::IVerificationEnvironmentRepository {
public:
    explicit UnavailableVerificationEnvironmentRepository(std::string reason) : reason_(std::move(reason)) {}
    common::Result<void> recordSnapshot(application::VerificationEnvironmentSnapshot) override { return failVoid(); }
    common::Result<std::optional<application::VerificationEnvironmentSnapshot>> findForRun(
        const application::RunId&) const override { return fail<std::optional<application::VerificationEnvironmentSnapshot>>(); }
private:
    template<class T> common::Result<T> fail() const { return common::Result<T>::failure({"storage.unavailable", reason_}); }
    common::Result<void> failVoid() const { return common::Result<void>::failure({"storage.unavailable", reason_}); }
    std::string reason_;
};

class UnavailableVerificationBootstrapRepository final
    : public application::IVerificationBootstrapRepository {
public:
    explicit UnavailableVerificationBootstrapRepository(std::string reason) : reason_(std::move(reason)) {}
    common::Result<void> recordDecision(application::VerificationBootstrapDecision) override { return failVoid(); }
    common::Result<std::optional<application::VerificationBootstrapDecision>> findDecisionForRun(
        const application::RunId&) const override { return fail<std::optional<application::VerificationBootstrapDecision>>(); }
    common::Result<void> appendTransition(application::VerificationBootstrapTransition) override { return failVoid(); }
    common::Result<std::vector<application::VerificationBootstrapTransition>> listTransitionsForRun(
        const application::RunId&) const override { return fail<std::vector<application::VerificationBootstrapTransition>>(); }
    common::Result<void> recordRetryLink(application::VerificationBootstrapRetryLink) override { return failVoid(); }
    common::Result<std::optional<application::VerificationBootstrapRetryLink>> findRetryLinkForPredecessor(
        const application::RunId&) const override { return fail<std::optional<application::VerificationBootstrapRetryLink>>(); }
private:
    template<class T> common::Result<T> fail() const { return common::Result<T>::failure({"storage.unavailable", reason_}); }
    common::Result<void> failVoid() const { return common::Result<void>::failure({"storage.unavailable", reason_}); }
    std::string reason_;
};

class UnavailableChatRepository final : public application::IChatRepository {
public:
    explicit UnavailableChatRepository(std::string reason) : reason_(std::move(reason)) {}
    common::Result<application::ChatConversation> insertConversation(application::ChatConversation) override { return fail<application::ChatConversation>(); }
    common::Result<application::ChatConversation> updateConversation(application::ChatConversation) override { return fail<application::ChatConversation>(); }
    common::Result<void> removeConversation(const application::ConversationId&) override { return common::Result<void>::failure({"storage.unavailable", reason_}); }
    common::Result<std::optional<application::ChatConversation>> findConversation(const application::ConversationId&) const override { return fail<std::optional<application::ChatConversation>>(); }
    common::Result<std::vector<application::ChatConversation>> listConversations() const override { return fail<std::vector<application::ChatConversation>>(); }
    common::Result<application::PreparedChatTurn> insertPreparedTurn(
        application::ChatConversation, application::PreparedChatTurn) override { return fail<application::PreparedChatTurn>(); }
    common::Result<application::ChatMessage> insertMessage(application::ChatMessage) override { return fail<application::ChatMessage>(); }
    common::Result<application::ChatMessage> updateMessage(application::ChatMessage) override { return fail<application::ChatMessage>(); }
    common::Result<std::optional<application::ChatMessage>> findMessage(const application::ChatMessageId&) const override { return fail<std::optional<application::ChatMessage>>(); }
    common::Result<std::vector<application::ChatMessage>> listMessages(const application::ConversationId&) const override { return fail<std::vector<application::ChatMessage>>(); }
private:
    template<class T> common::Result<T> fail() const { return common::Result<T>::failure({"storage.unavailable", reason_}); }
    std::string reason_;
};

// Same controlled-failure guarantee, applied to the project memory layer
// (Task B of PROJECT_MEMORY_PLAN.md): ProjectMemoryService remains fully
// constructible and callable even when the metadata database is unavailable,
// so RunManager's best-effort write simply fails typed and never crashes.
class UnavailableProjectMemoryRepository final : public application::IProjectMemoryRepository {
public:
    explicit UnavailableProjectMemoryRepository(std::string reason) : reason_(std::move(reason)) {}
    common::Result<application::ProjectMemorySegment> insertMemorySegment(application::ProjectMemorySegment) override { return fail<application::ProjectMemorySegment>(); }
    common::Result<std::vector<application::ProjectMemorySegment>> listMemorySegments(const application::ProjectId&) const override { return fail<std::vector<application::ProjectMemorySegment>>(); }
    common::Result<std::optional<application::ProjectMemorySegment>> findMemorySegment(const application::ProjectMemorySegmentId&) const override { return fail<std::optional<application::ProjectMemorySegment>>(); }
    common::Result<void> removeMemorySegments(const application::ProjectId&) override { return common::Result<void>::failure({"storage.unavailable", reason_}); }
private:
    template<class T> common::Result<T> fail() const { return common::Result<T>::failure({"storage.unavailable", reason_}); }
    std::string reason_;
};

// Same controlled-failure guarantee, applied to the AG-11 compaction
// provenance port: ProjectMemoryCompactionService remains fully
// constructible and callable even when the metadata database is unavailable.
class UnavailableProjectMemoryCompactionRepository final
    : public application::IProjectMemoryCompactionRepository {
public:
    explicit UnavailableProjectMemoryCompactionRepository(std::string reason) : reason_(std::move(reason)) {}
    common::Result<void> insertCompactionRecord(const application::ProjectMemoryCompactionRecord&) override
    {
        return common::Result<void>::failure({"storage.unavailable", reason_});
    }
    common::Result<void> commitSuccessfulCompaction(
        const application::ProjectMemoryCompactionRecord&,
        application::ProjectMemorySegment) override
    {
        return common::Result<void>::failure({"storage.unavailable", reason_});
    }
    common::Result<std::vector<application::ProjectMemoryCompactionRecord>> listCompactionRecords(
        const application::ProjectId&) const override
    {
        return fail<std::vector<application::ProjectMemoryCompactionRecord>>();
    }
private:
    template<class T> common::Result<T> fail() const { return common::Result<T>::failure({"storage.unavailable", reason_}); }
    std::string reason_;
};

class UnavailableChatMemoryRepository final : public application::IChatMemoryRepository {
public:
    explicit UnavailableChatMemoryRepository(std::string reason) : reason_(std::move(reason)) {}
    common::Result<application::ChatMemorySegment> insertMemorySegment(application::ChatMemorySegment) override { return fail<application::ChatMemorySegment>(); }
    common::Result<std::vector<application::ChatMemorySegment>> listMemorySegments(const application::ConversationId&) const override { return fail<std::vector<application::ChatMemorySegment>>(); }
    common::Result<std::optional<application::ChatMemorySegment>> findMemorySegment(const application::ChatMemorySegmentId&) const override { return fail<std::optional<application::ChatMemorySegment>>(); }
    common::Result<void> removeMemorySegments(const application::ConversationId&) override { return common::Result<void>::failure({"storage.unavailable", reason_}); }
private:
    template<class T> common::Result<T> fail() const { return common::Result<T>::failure({"storage.unavailable", reason_}); }
    std::string reason_;
};

class UnavailableModelExecutionRepository final : public application::IModelExecutionRepository {
public:
    explicit UnavailableModelExecutionRepository(std::string reason) : reason_(std::move(reason)) {}
    common::Result<application::ModelExecution> insertExecution(application::ModelExecution) override { return fail<application::ModelExecution>(); }
    common::Result<application::ModelExecution> updateExecution(application::ModelExecution) override { return fail<application::ModelExecution>(); }
    common::Result<std::optional<application::ModelExecution>> findExecution(const application::ModelExecutionId&) const override { return fail<std::optional<application::ModelExecution>>(); }
    common::Result<std::vector<application::ModelExecution>> listExecutions(application::ModelExecutionSource, const std::string&) const override { return fail<std::vector<application::ModelExecution>>(); }
    common::Result<std::vector<application::ModelExecution>> listProjectExecutions(const application::ProjectId&) const override { return fail<std::vector<application::ModelExecution>>(); }
    common::Result<std::vector<application::ModelExecution>> listUnfinishedExecutions() const override { return fail<std::vector<application::ModelExecution>>(); }
    common::Result<application::QualityEvaluation> insertEvaluation(application::QualityEvaluation) override { return fail<application::QualityEvaluation>(); }
    common::Result<std::optional<application::QualityEvaluation>> findEvaluation(const application::QualityEvaluationId&) const override { return fail<std::optional<application::QualityEvaluation>>(); }
    common::Result<std::vector<application::QualityEvaluation>> listEvaluations(const application::ModelExecutionId&) const override { return fail<std::vector<application::QualityEvaluation>>(); }
private:
    template<class T> common::Result<T> fail() const { return common::Result<T>::failure({"storage.unavailable", reason_}); }
    std::string reason_;
};

class UnavailableComparisonSessionRepository final
    : public application::IComparisonSessionRepository {
public:
    explicit UnavailableComparisonSessionRepository(std::string reason)
        : reason_(std::move(reason))
    {
    }
    common::Result<application::ComparisonSession> insertSession(
        application::ComparisonSession) override
    {
        return fail<application::ComparisonSession>();
    }
    common::Result<application::ComparisonSession> updateSession(
        application::ComparisonSession) override
    {
        return fail<application::ComparisonSession>();
    }
    common::Result<std::optional<application::ComparisonSession>> findSession(
        const application::ComparisonSessionId&) const override
    {
        return fail<std::optional<application::ComparisonSession>>();
    }
    common::Result<std::vector<application::ComparisonSession>> listSessions(
        const std::optional<application::ProjectId>&) const override
    {
        return fail<std::vector<application::ComparisonSession>>();
    }
    common::Result<std::vector<application::ComparisonSession>>
    listUnfinishedSessions() const override
    {
        return fail<std::vector<application::ComparisonSession>>();
    }

private:
    template <typename T>
    common::Result<T> fail() const
    {
        return common::Result<T>::failure({"storage.unavailable", reason_});
    }
    std::string reason_;
};

// Same controlled-failure guarantee as the two repositories above, applied
// to artifact metadata: ArtifactService remains fully constructible and
// callable even when the metadata database is unavailable.
class UnavailableArtifactRepository final : public application::IArtifactRepository {
public:
    explicit UnavailableArtifactRepository(std::string reason)
        : reason_(std::move(reason))
    {
    }

    common::Result<application::Artifact> insertArtifact(application::Artifact) override
    {
        return fail<application::Artifact>();
    }
    [[nodiscard]] common::Result<std::optional<application::Artifact>> findArtifact(
        const application::ArtifactId&) const override
    {
        return fail<std::optional<application::Artifact>>();
    }
    [[nodiscard]] common::Result<std::vector<application::Artifact>> listArtifacts(
        const application::RunId&) const override
    {
        return fail<std::vector<application::Artifact>>();
    }

private:
    template <typename T>
    [[nodiscard]] common::Result<T> fail() const
    {
        return common::Result<T>::failure({"storage.unavailable", reason_});
    }

    std::string reason_;
};

// The same guarantee for Run warnings: a controlled failure, so a lost diagnostic artifact is still announced
// (not durable) when the metadata database is unavailable.
class UnavailableRunWarningRepository final : public application::IRunWarningRepository {
public:
    explicit UnavailableRunWarningRepository(std::string reason)
        : reason_(std::move(reason))
    {
    }

    common::Result<bool> recordWarning(application::RunWarning) override
    {
        return common::Result<bool>::failure({"storage.unavailable", reason_});
    }
    [[nodiscard]] common::Result<std::vector<application::RunWarning>> listWarnings(
        const application::RunId&) const override
    {
        return common::Result<std::vector<application::RunWarning>>::failure({"storage.unavailable", reason_});
    }

private:
    std::string reason_;
};

// The same guarantee for the verification-folder setting: a controlled failure when the database is unavailable, so the service is
// still constructible and a verification is refused with a typed error instead of silently using an unvalidated folder.
class UnavailableVerificationScratchSettingRepository final : public application::IVerificationScratchSettingRepository {
public:
    explicit UnavailableVerificationScratchSettingRepository(std::string reason)
        : reason_(std::move(reason))
    {
    }

    [[nodiscard]] common::Result<std::optional<std::filesystem::path>> load() const override
    {
        return common::Result<std::optional<std::filesystem::path>>::failure({"storage.unavailable", reason_});
    }
    common::Result<void> store(const std::optional<std::filesystem::path>&) override
    {
        return common::Result<void>::failure({"storage.unavailable", reason_});
    }

private:
    std::string reason_;
};

// The same guarantee for the per-project verification readiness: without a database every check ends with a typed error and no
// Run proceeds on an unproven setup by accident.
class UnavailableProjectReadinessRepository final : public application::IProjectReadinessRepository {
public:
    explicit UnavailableProjectReadinessRepository(std::string reason)
        : reason_(std::move(reason))
    {
    }

    [[nodiscard]] common::Result<std::optional<application::ProjectReadinessRecord>> find(
        const application::ProjectId&) const override
    {
        return common::Result<std::optional<application::ProjectReadinessRecord>>::failure({"storage.unavailable", reason_});
    }
    common::Result<void> upsert(const application::ProjectReadinessRecord&) override
    {
        return common::Result<void>::failure({"storage.unavailable", reason_});
    }

private:
    std::string reason_;
};

// Same controlled-failure guarantee for artifact contents, used when the
// filesystem artifact root itself could not be created/canonicalized (a
// failure mode independent from the SQLite metadata database).
class UnavailableArtifactStore final : public application::IArtifactStore {
public:
    explicit UnavailableArtifactStore(std::string reason)
        : reason_(std::move(reason))
    {
    }

    common::Result<application::StoredArtifactFile> writeArtifact(
        const application::RunId&, std::string_view, std::string_view) override
    {
        return common::Result<application::StoredArtifactFile>::failure({"storage.unavailable", reason_});
    }
    common::Result<void> removeArtifact(const std::filesystem::path&) override
    {
        return common::Result<void>::failure({"storage.unavailable", reason_});
    }
    [[nodiscard]] common::Result<std::filesystem::path> resolveAbsolutePath(
        const std::filesystem::path&) const override
    {
        return common::Result<std::filesystem::path>::failure({"storage.unavailable", reason_});
    }
    [[nodiscard]] common::Result<std::string> readArtifact(
        const std::filesystem::path&) const override
    {
        return common::Result<std::string>::failure({"storage.unavailable", reason_});
    }

private:
    std::string reason_;
};

// Same controlled-failure guarantee, applied to the single active-project
// workspace state value.
class UnavailableWorkspaceStateRepository final : public application::IWorkspaceStateRepository {
public:
    explicit UnavailableWorkspaceStateRepository(std::string reason)
        : reason_(std::move(reason))
    {
    }

    [[nodiscard]] common::Result<std::optional<application::ProjectId>> activeProjectId() const override
    {
        return common::Result<std::optional<application::ProjectId>>::failure(
            {"storage.unavailable", reason_});
    }
    common::Result<void> setActiveProjectId(const std::optional<application::ProjectId>&) override
    {
        return common::Result<void>::failure({"storage.unavailable", reason_});
    }

private:
    std::string reason_;
};

class UnavailableApplicationSettingsRepository final
    : public application::IApplicationSettingsRepository {
public:
    explicit UnavailableApplicationSettingsRepository(std::string reason)
        : reason_(std::move(reason))
    {
    }
    common::Result<application::ApplicationSettings> load() const override
    {
        return fail();
    }
    common::Result<application::ApplicationSettings> save(
        application::ApplicationSettings) override
    {
        return fail();
    }
private:
    common::Result<application::ApplicationSettings> fail() const
    {
        return common::Result<application::ApplicationSettings>::failure(
            {"storage.unavailable", reason_});
    }
    std::string reason_;
};

// Same controlled-failure guarantee, applied to the provider catalog.
class UnavailableProviderRepository final : public application::IProviderRepository {
public:
    explicit UnavailableProviderRepository(std::string reason)
        : reason_(std::move(reason))
    {
    }

    common::Result<application::Provider> insert(application::Provider) override
    {
        return fail<application::Provider>();
    }
    common::Result<application::Provider> update(application::Provider) override
    {
        return fail<application::Provider>();
    }
    [[nodiscard]] common::Result<std::optional<application::Provider>> findById(
        const application::ProviderId&) const override
    {
        return fail<std::optional<application::Provider>>();
    }
    [[nodiscard]] common::Result<std::vector<application::Provider>> listAll() const override
    {
        return fail<std::vector<application::Provider>>();
    }

private:
    template <typename T>
    [[nodiscard]] common::Result<T> fail() const
    {
        return common::Result<T>::failure({"storage.unavailable", reason_});
    }

    std::string reason_;
};

class UnavailableModelRepository final : public application::IModelRepository {
public:
    explicit UnavailableModelRepository(std::string reason)
        : reason_(std::move(reason))
    {
    }

    common::Result<application::Model> insert(application::Model) override { return fail<application::Model>(); }
    common::Result<application::Model> update(application::Model) override { return fail<application::Model>(); }
    [[nodiscard]] common::Result<std::optional<application::Model>> findById(
        const application::ModelId&) const override
    {
        return fail<std::optional<application::Model>>();
    }
    [[nodiscard]] common::Result<std::vector<application::Model>> listAll(
        const std::optional<application::ProviderId>&) const override
    {
        return fail<std::vector<application::Model>>();
    }
    common::Result<void> setDefaultModel(
        const application::ProviderId&, const application::ModelId&) override
    {
        return common::Result<void>::failure({"storage.unavailable", reason_});
    }
    common::Result<void> remove(const application::ModelId&) override
    {
        return common::Result<void>::failure({"storage.unavailable", reason_});
    }

private:
    template <typename T>
    [[nodiscard]] common::Result<T> fail() const
    {
        return common::Result<T>::failure({"storage.unavailable", reason_});
    }

    std::string reason_;
};

// Same controlled-failure guarantee, applied to execution profiles.
class UnavailableExecutionProfileRepository final : public application::IExecutionProfileRepository {
public:
    explicit UnavailableExecutionProfileRepository(std::string reason)
        : reason_(std::move(reason))
    {
    }

    common::Result<application::ExecutionProfile> insert(application::ExecutionProfile) override
    {
        return fail<application::ExecutionProfile>();
    }
    common::Result<application::ExecutionProfile> update(application::ExecutionProfile) override
    {
        return fail<application::ExecutionProfile>();
    }
    [[nodiscard]] common::Result<std::optional<application::ExecutionProfile>> findById(
        const application::ExecutionProfileId&) const override
    {
        return fail<std::optional<application::ExecutionProfile>>();
    }
    [[nodiscard]] common::Result<std::vector<application::ExecutionProfile>> listAll() const override
    {
        return fail<std::vector<application::ExecutionProfile>>();
    }
    common::Result<void> setDefaultProfile(const application::ExecutionProfileId&) override
    {
        return common::Result<void>::failure({"storage.unavailable", reason_});
    }

private:
    template <typename T>
    [[nodiscard]] common::Result<T> fail() const
    {
        return common::Result<T>::failure({"storage.unavailable", reason_});
    }

    std::string reason_;
};

class UnavailableRunDraftRepository final : public application::IRunDraftRepository {
public:
    explicit UnavailableRunDraftRepository(std::string reason)
        : reason_(std::move(reason))
    {
    }

    common::Result<application::RunDraft> insert(application::RunDraft) override
    {
        return fail<application::RunDraft>();
    }
    common::Result<application::RunDraft> update(application::RunDraft) override
    {
        return fail<application::RunDraft>();
    }
    [[nodiscard]] common::Result<std::optional<application::RunDraft>> findById(
        const application::RunDraftId&) const override
    {
        return fail<std::optional<application::RunDraft>>();
    }
    [[nodiscard]] common::Result<std::vector<application::RunDraft>> listAll() const override
    {
        return fail<std::vector<application::RunDraft>>();
    }
    common::Result<void> remove(const application::RunDraftId&) override
    {
        return common::Result<void>::failure({"storage.unavailable", reason_});
    }
    common::Result<std::vector<application::RunDraft>> insertGroup(
        application::RunDraftGroup, std::vector<application::RunDraft>) override
    {
        return fail<std::vector<application::RunDraft>>();
    }
    [[nodiscard]] common::Result<std::optional<application::RunDraftGroup>> findGroupById(
        const application::RunDraftGroupId&) const override
    {
        return fail<std::optional<application::RunDraftGroup>>();
    }
    [[nodiscard]] common::Result<std::optional<application::RunDraftGroup>> findGroupByIdempotencyKey(
        const std::string&) const override
    {
        return fail<std::optional<application::RunDraftGroup>>();
    }
    [[nodiscard]] common::Result<std::vector<application::RunDraft>> listGroupDrafts(
        const application::RunDraftGroupId&) const override
    {
        return fail<std::vector<application::RunDraft>>();
    }

private:
    template <typename T>
    [[nodiscard]] common::Result<T> fail() const
    {
        return common::Result<T>::failure({"storage.unavailable", reason_});
    }

    std::string reason_;
};

// A secure OS-backed credential implementation (e.g. Windows Credential
// Manager / DPAPI) is outside this task's safe scope (see
// ICredentialStore.hpp); this is the controlled-unavailable stand-in every
// ProviderCatalogService caller sees until that adapter exists. It never
// touches its `secret`/`reference` parameters when constructing an error, so
// a caller's secret can never leak into a Result<...>::error() message.
class UnavailableCredentialStore final : public application::ICredentialStore {
public:
    common::Result<void> store(const std::string&, const std::string&) override
    {
        return fail<void>();
    }
    [[nodiscard]] common::Result<std::optional<std::string>> load(const std::string&) const override
    {
        return fail<std::optional<std::string>>();
    }
    common::Result<void> remove(const std::string&) override
    {
        return fail<void>();
    }

private:
    template <typename T>
    [[nodiscard]] common::Result<T> fail() const
    {
        return common::Result<T>::failure(
            {"credential_store.unavailable", "No secure credential store is configured in this build"});
    }
};

class UnavailableChangeProposalRepository final
    : public application::IChangeProposalRepository {
public:
    explicit UnavailableChangeProposalRepository(std::string reason)
        : reason_(std::move(reason))
    {
    }
    common::Result<void> insertChangeSet(const application::ProposedChangeSet&) override
    {
        return common::Result<void>::failure({"storage.unavailable", reason_});
    }
    common::Result<application::ProposedChangeSet> updateChangeSetState(
        const application::ChangeSetId&, application::ChangeSetState) override
    {
        return fail<application::ProposedChangeSet>();
    }
    common::Result<void> markMissingEvidence(
        const application::ChangeSetId&, std::string, std::string) override
    {
        return common::Result<void>::failure({"storage.unavailable", reason_});
    }
    common::Result<std::optional<application::ProposedChangeSet>> findChangeSet(
        const application::ChangeSetId&) const override
    {
        return fail<std::optional<application::ProposedChangeSet>>();
    }
    common::Result<std::vector<application::ProposedChangeSet>> listForRun(
        const application::RunId&) const override
    {
        return fail<std::vector<application::ProposedChangeSet>>();
    }
    common::Result<std::vector<application::ProposedChangeSet>> listForProject(
        const application::ProjectId&) const override
    {
        return fail<std::vector<application::ProposedChangeSet>>();
    }
    common::Result<std::vector<application::ProposedFileChange>> listChanges(
        const application::ChangeSetId&) const override
    {
        return fail<std::vector<application::ProposedFileChange>>();
    }
    common::Result<std::vector<application::ValidationFinding>> listValidationFindings(
        const application::ChangeSetId&) const override
    {
        return fail<std::vector<application::ValidationFinding>>();
    }
    common::Result<std::unordered_map<application::ChangeSetId,
        std::vector<application::ProposedFileChange>>>
    listChangesForSets(const std::vector<application::ChangeSetId>&) const override
    {
        return fail<std::unordered_map<application::ChangeSetId,
            std::vector<application::ProposedFileChange>>>();
    }
    common::Result<std::unordered_map<application::ChangeSetId,
        std::vector<application::ValidationFinding>>>
    listValidationFindingsForSets(const std::vector<application::ChangeSetId>&) const override
    {
        return fail<std::unordered_map<application::ChangeSetId,
            std::vector<application::ValidationFinding>>>();
    }
private:
    template <typename T>
    common::Result<T> fail() const
    {
        return common::Result<T>::failure({"storage.unavailable", reason_});
    }
    std::string reason_;
};

class UnavailableChangeProposalStore final : public application::IChangeProposalStore {
public:
    explicit UnavailableChangeProposalStore(std::string reason)
        : reason_(std::move(reason))
    {
    }
    common::Result<application::PublishedProposalLocation> publishChangeSet(
        const application::ProposedChangeSet&,
        const application::ProjectWritePolicy&) override
    {
        return common::Result<application::PublishedProposalLocation>::failure(
            {"storage.unavailable", reason_});
    }
    common::Result<std::string> readOriginalContent(
        const std::string&, const application::ChangeId&) const override
    {
        return common::Result<std::string>::failure({"storage.unavailable", reason_});
    }
    common::Result<std::string> readProposedContent(
        const std::string&, const application::ChangeId&) const override
    {
        return common::Result<std::string>::failure({"storage.unavailable", reason_});
    }
private:
    std::string reason_;
};

class UnavailableChangeApprovalRepository final
    : public application::IChangeApprovalRepository {
public:
    explicit UnavailableChangeApprovalRepository(std::string reason) : reason_(std::move(reason)) {}
    common::Result<void> recordApproval(const application::ApprovalRecord&) override { return fail<void>(); }
    common::Result<std::optional<application::ApprovalRecord>> findApproval(
        const application::ChangeSetId&) const override { return fail<std::optional<application::ApprovalRecord>>(); }
    common::Result<void> recordApplyOutcome(
        const application::ChangeSetId&, const std::string&, const std::optional<std::string>&,
        const std::vector<application::ApplyHashEntry>&,
        const std::optional<std::string>&) override { return fail<void>(); }
private:
    template <typename T> common::Result<T> fail() const { return common::Result<T>::failure({"storage.unavailable", reason_}); }
    std::string reason_;
};

class UnavailableProposalReviewRepository final
    : public application::IProposalReviewRepository {
public:
    explicit UnavailableProposalReviewRepository(std::string reason) : reason_(std::move(reason)) {}
    common::Result<void> recordRejection(const application::ProposalReviewRejection&) override { return fail<void>(); }
    common::Result<std::optional<application::ProposalReviewRejection>> findRejection(
        const application::ChangeSetId&) const override { return fail<std::optional<application::ProposalReviewRejection>>(); }
private:
    template <typename T> common::Result<T> fail() const { return common::Result<T>::failure({"storage.unavailable", reason_}); }
    std::string reason_;
};

// PC-4C. Same controlled-failure guarantee as the repositories above.
class UnavailableOutcomeBriefRepository final : public application::IOutcomeBriefRepository {
public:
    explicit UnavailableOutcomeBriefRepository(std::string reason) : reason_(std::move(reason)) {}
    common::Result<application::OutcomeBriefAppendResult> append(
        application::OutcomeBrief, application::OutcomeBriefExpectedHead) override { return fail<application::OutcomeBriefAppendResult>(); }
    common::Result<std::optional<application::OutcomeBriefRevision>> findHead(
        const application::RunId&) const override { return fail<std::optional<application::OutcomeBriefRevision>>(); }
    common::Result<std::optional<application::OutcomeBriefRevision>> findRevision(
        const application::OutcomeBriefRevisionId&) const override { return fail<std::optional<application::OutcomeBriefRevision>>(); }
    common::Result<std::vector<application::OutcomeBriefRevision>> listRevisions(
        const application::RunId&, std::size_t) const override { return fail<std::vector<application::OutcomeBriefRevision>>(); }
    common::Result<std::vector<application::OutcomeBriefRevision>> listHeads(
        const application::ProjectId&, std::size_t) const override { return fail<std::vector<application::OutcomeBriefRevision>>(); }
    common::Result<std::optional<application::OutcomeBriefHeadVersion>> findHeadVersionForAppend(
        const application::RunId&) const override { return fail<std::optional<application::OutcomeBriefHeadVersion>>(); }
private:
    template <typename T> common::Result<T> fail() const { return common::Result<T>::failure({"storage.unavailable", reason_}); }
    std::string reason_;
};

// PC-5C-2b. The request repository and Confirm port are separate interfaces
// in the composition root. The unavailable implementation covers both so
// every follow-up entry point preserves BackendRuntime's controlled-failure
// guarantee when SQLite did not open.
class UnavailableFollowUpPlanRepository final
    : public application::IFollowUpPlanRequestRepository,
      public application::IFollowUpPlanConfirmPort {
public:
    explicit UnavailableFollowUpPlanRepository(std::string reason) : reason_(std::move(reason)) {}
    common::Result<application::FollowUpInsertResult> insertQuoted(application::FollowUpPlanRequest) override
        { return fail<application::FollowUpInsertResult>(); }
    common::Result<application::FollowUpPlanRequest> transition(
        const application::FollowUpPlanRequestId&, application::FollowUpTransition,
        std::chrono::system_clock::time_point) override { return fail<application::FollowUpPlanRequest>(); }
    common::Result<std::optional<application::FollowUpPlanRequest>> findById(
        const application::FollowUpPlanRequestId&) const override
        { return fail<std::optional<application::FollowUpPlanRequest>>(); }
    common::Result<std::optional<application::FollowUpPlanRequest>> findOpenForSourceRun(
        const application::RunId&) const override
        { return fail<std::optional<application::FollowUpPlanRequest>>(); }
    common::Result<std::optional<application::FollowUpPlanRequest>> findLatestForSourceRun(
        const application::RunId&) const override
        { return fail<std::optional<application::FollowUpPlanRequest>>(); }
    common::Result<std::vector<application::FollowUpPlanRequest>> listForProject(
        const application::ProjectId&, std::size_t) const override
        { return fail<std::vector<application::FollowUpPlanRequest>>(); }
    common::Result<std::vector<application::FollowUpPlanRequest>> listByState(
        application::FollowUpPlanRequestState, std::size_t) const override
        { return fail<std::vector<application::FollowUpPlanRequest>>(); }
    common::Result<std::vector<application::FollowUpPlanRequest>> listByStateAfter(
        application::FollowUpPlanRequestState, std::size_t,
        const std::optional<application::FollowUpStateCursor>&) const override
        { return fail<std::vector<application::FollowUpPlanRequest>>(); }
    common::Result<application::FollowUpConfirmResult> confirmDrafts(
        const application::FollowUpPlanRequestId&, const std::string&,
        application::RunDraftGroup, std::vector<application::RunDraft>,
        std::chrono::system_clock::time_point) override
        { return fail<application::FollowUpConfirmResult>(); }
private:
    template <typename T> common::Result<T> fail() const
        { return common::Result<T>::failure({"storage.unavailable", reason_}); }
    std::string reason_;
};

class UnavailableAgentToolSessionRepository final
    : public application::IAgentToolSessionRepository {
public:
    explicit UnavailableAgentToolSessionRepository(std::string reason) : reason_(std::move(reason)) {}
    common::Result<application::AgentToolSession> insertSession(application::AgentToolSession) override { return fail<application::AgentToolSession>(); }
    common::Result<application::AgentToolSession> updateSession(application::AgentToolSession) override { return fail<application::AgentToolSession>(); }
    common::Result<std::optional<application::AgentToolSession>> findSession(const std::string&) const override { return fail<std::optional<application::AgentToolSession>>(); }
    common::Result<std::vector<application::AgentToolSession>> listForRun(const std::string&) const override { return fail<std::vector<application::AgentToolSession>>(); }
    common::Result<application::AgentProviderTurn> insertTurn(application::AgentProviderTurn) override { return fail<application::AgentProviderTurn>(); }
    common::Result<std::vector<application::AgentProviderTurn>> listTurns(const std::string&) const override { return fail<std::vector<application::AgentProviderTurn>>(); }
    common::Result<std::size_t> interruptActiveSessions() override { return fail<std::size_t>(); }
private:
    template <typename T> common::Result<T> fail() const { return common::Result<T>::failure({"storage.unavailable", reason_}); }
    std::string reason_;
};

class UnavailableAgentToolAuditRepository final
    : public application::IAgentToolAuditRepository {
public:
    explicit UnavailableAgentToolAuditRepository(std::string reason) : reason_(std::move(reason)) {}
    common::Result<void> insert(const application::AgentToolAuditRecord&) override { return common::Result<void>::failure({"storage.unavailable", reason_}); }
    common::Result<void> update(const application::AgentToolAuditRecord&) override { return common::Result<void>::failure({"storage.unavailable", reason_}); }
    common::Result<std::optional<application::AgentToolAuditRecord>> find(const std::string&) const override { return fail<std::optional<application::AgentToolAuditRecord>>(); }
    common::Result<std::vector<application::AgentToolAuditRecord>> listForSession(const std::string&) const override { return fail<std::vector<application::AgentToolAuditRecord>>(); }
    common::Result<std::size_t> interruptRunning(const std::string&) override { return fail<std::size_t>(); }
    common::Result<std::size_t> interruptAllRunning() override { return fail<std::size_t>(); }
private:
    template <typename T> common::Result<T> fail() const { return common::Result<T>::failure({"storage.unavailable", reason_}); }
    std::string reason_;
};

// Registered under kExecutionProfileWorkflowId exactly like a real provider
// runtime would be, so a production run always resolves to *a* runner (never
// "workflow.unsupported") and fails every stage with this one explicit,
// typed code instead of RunManager ever silently substituting the Mock
// workflow. See CMakeLists.txt's AI_ORCHESTRATOR_BUILD_REAL_PROVIDERS.
class UnavailableProviderClientFactory final : public application::IProviderClientFactory {
public:
    common::Result<std::shared_ptr<application::IProviderClient>> clientFor(
        const application::ProviderId&, const std::string&) override
    {
        return common::Result<std::shared_ptr<application::IProviderClient>>::failure(
            {"provider_runtime.unavailable", "No real provider adapter is compiled into this build"});
    }
};

// Returned by BackendRuntime::agentSessionFactory() when the feature flag is
// off or when no real provider agent session adapter is compiled in.
class UnavailableProviderAgentSessionFactory final
    : public providers::IProviderAgentSessionFactory {
public:
    explicit UnavailableProviderAgentSessionFactory(std::string reason)
        : reason_(std::move(reason)) {}

    common::Result<std::unique_ptr<providers::IProviderAgentSession>> createSession(
        const application::ProviderId&,
        const std::string&) override
    {
        return common::Result<std::unique_ptr<providers::IProviderAgentSession>>::failure(
            {"agent_session.unavailable", reason_});
    }

private:
    std::string reason_;
};

static application::ToolRegistry createDefaultRegistry()
{
    auto result = application::ToolRegistry::createDefaultReadOnly();
    if (result.success())
        return std::move(result.value());
    return application::ToolRegistry{};
}

static std::unique_ptr<providers::IProviderAgentSessionFactory> createAgentSessionFactory(
    const BackendRuntimeConfig& config,
    application::ICredentialStore& credentialStore,
    application::IProviderRepository& providerRepository)
{
    if (!config.enableAgentToolExecution)
        return std::make_unique<UnavailableProviderAgentSessionFactory>(
            "agent tool execution is disabled (enableAgentToolExecution == false)");

#if defined(AI_ORCHESTRATOR_HAVE_REAL_AGENT_SESSIONS)
    return std::make_unique<providers::RealProviderAgentSessionFactory>(
        credentialStore, providerRepository);
#else
    static_cast<void>(credentialStore);
    static_cast<void>(providerRepository);
    return std::make_unique<UnavailableProviderAgentSessionFactory>(
        "no provider agent session adapter is compiled into this build");
#endif
}

std::unique_ptr<application::ICredentialStore> createCredentialStore()
{
#if defined(AI_ORCHESTRATOR_HAVE_WINDOWS_CREDENTIALS)
    return std::make_unique<infrastructure::WindowsCredentialStore>();
#elif defined(AI_ORCHESTRATOR_HAVE_MACOS_CREDENTIALS)
    return std::make_unique<infrastructure::MacOSCredentialStore>();
#else
    // Every other platform/configuration: controlled-unavailable, never an
    // insecure plaintext fallback.
    return std::make_unique<UnavailableCredentialStore>();
#endif
}

std::unique_ptr<application::IProviderClientFactory> createProviderClientFactory(
    application::ICredentialStore& credentialStore,
    application::IProviderRepository& providerRepository)
{
#if defined(AI_ORCHESTRATOR_HAVE_REAL_PROVIDERS)
    return std::make_unique<RealProviderClientFactory>(credentialStore, providerRepository);
#else
    static_cast<void>(credentialStore);
    static_cast<void>(providerRepository);
    return std::make_unique<UnavailableProviderClientFactory>();
#endif
}

} // namespace

/**
 * Everything BackendRuntime owns, plus the two static helpers that build it
 * in dependency order. Kept in this one translation unit (PImpl) so the
 * public header never has to name SqliteDatabase, a repository class, or any
 * other persistence-layer type.
 *
 * Declaration order is the whole lifetime contract:
 *  - storage outlives workflowRunner/runManager/projectService/
 *    orchestrationService, so no service ever holds a dangling repository
 *    reference;
 *  - runManager (and the worker thread its constructor starts) is declared,
 *    and therefore destroyed, before storage - ~RunManager() joins the
 *    worker before this Impl's storage member (and the sqlite3 connection it
 *    owns) is torn down, so the worker can never touch a repository that has
 *    already been destroyed;
 *  - recoveryError is initialized last, by calling
 *    OrchestrationService::recoverInterruptedRuns() once orchestrationService
 *    is fully constructed, and before BackendRuntime returns to its caller -
 *    so the very first read any caller performs already reflects recovered
 *    state.
 */
struct BackendRuntime::Impl {
    struct StorageBundle {
        // Null when SQLite failed to open/migrate; every repository pointer
        // below still points at a valid (always-failing) Unavailable*
        // object either way, so every service constructed from this bundle
        // never binds a reference to a nonexistent object.
        std::shared_ptr<infrastructure::SqliteDatabase> database;
        std::unique_ptr<application::IProjectRepository> projectRepository;
        std::unique_ptr<application::ITaskRepository> taskRepository;
        std::unique_ptr<application::IRunRepository> runRepository;
        std::unique_ptr<application::IExecutionManifestRepository> executionManifestRepository;
        std::unique_ptr<application::IManifestTaskExecutionRepository> manifestTaskExecutionRepository;
        std::unique_ptr<application::IManifestWorkLedgerRepository> manifestWorkLedgerRepository;
        std::unique_ptr<application::IManifestVerificationContinuationRepository> manifestVerificationContinuationRepository;
        std::unique_ptr<application::IManifestVerificationBaselineNormalizationRepository> manifestVerificationBaselineNormalizationRepository;
        std::unique_ptr<application::IManifestVerificationManualHandoffRepository> manifestVerificationManualHandoffRepository;
        std::unique_ptr<application::IRunWorkspaceScopeRepository> runWorkspaceScopeRepository;
        std::unique_ptr<application::IVerificationEnvironmentRepository> verificationEnvironmentRepository;
        std::unique_ptr<application::IVerificationBootstrapRepository> verificationBootstrapRepository;
        std::unique_ptr<application::IWorkspaceStateRepository> workspaceStateRepository;
        std::unique_ptr<application::IApplicationSettingsRepository> applicationSettingsRepository;
        std::unique_ptr<application::IProviderRepository> providerRepository;
        std::unique_ptr<application::IModelRepository> modelRepository;
        std::unique_ptr<application::IExecutionProfileRepository> executionProfileRepository;
        std::unique_ptr<application::IRunDraftRepository> runDraftRepository;
        std::unique_ptr<application::IChatRepository> chatRepository;
        std::unique_ptr<application::IChatMemoryRepository> chatMemoryRepository;
        std::unique_ptr<application::IModelExecutionRepository> modelExecutionRepository;
        std::unique_ptr<application::IComparisonSessionRepository> comparisonSessionRepository;
        std::unique_ptr<application::IProjectMemoryRepository> projectMemoryRepository;
        std::unique_ptr<application::IProjectMemoryCompactionRepository> projectMemoryCompactionRepository;
        std::unique_ptr<application::IChangeProposalRepository> changeProposalRepository;
        std::unique_ptr<application::IAgentToolSessionRepository> agentToolSessionRepository;
        std::unique_ptr<application::IAgentToolAuditRepository> agentToolAuditRepository;
        std::unique_ptr<application::IChangeApprovalRepository> changeApprovalRepository;
        std::unique_ptr<application::IProposalReviewRepository> proposalReviewRepository;
        std::unique_ptr<application::IOutcomeBriefRepository> outcomeBriefRepository;
        std::unique_ptr<application::IFollowUpPlanRequestRepository> followUpPlanRepository;
        std::unique_ptr<application::IFollowUpPlanConfirmPort> followUpPlanConfirmPort;
        std::optional<common::Error> error;
    };

    // PC-4C (review 2): the runtime is the ONE live owner of its database file. Everything the application keeps to guard its data - the
    // path claims, the manual-handoff import lock, the single Outcome Brief reconciler - lives in the memory of one process and assumes
    // nobody else writes the file, so the ownership lock is taken BEFORE SQLite opens it and released after the connection is gone (it is
    // declared before `storage`). A second instance, or a second runtime of this process, gets unavailable storage with the reason
    // instead of racing the first.
    struct DatabaseOwnership {
        std::unique_ptr<infrastructure::DatabaseOwnerLock> lock;
        std::optional<common::Error> error;
        // A guest (BackendRuntimeConfig::exclusiveDatabaseOwnership == false, test fixtures only) owns nothing and writes no brief.
        bool guest {false};
    };

    static DatabaseOwnership takeOwnership(const BackendRuntimeConfig& config)
    {
        if (!config.exclusiveDatabaseOwnership) return DatabaseOwnership {nullptr, std::nullopt, true};
        auto taken = infrastructure::DatabaseOwnerLock::acquire(config.databasePath);
        if (!taken.success()) return DatabaseOwnership {nullptr, taken.error(), false};
        return DatabaseOwnership {std::move(taken.value()), std::nullopt, false};
    }

    // A guest is not the owner of the file, so its reconciler refuses EVERY request - the pull, the nudges, the history pass - before it
    // reads a source: there is exactly one writer of the briefs of a database, and the public accessor of the reconciler cannot be used to
    // become a second one. The owner's reconciler has no refusal.
    static std::optional<common::Error> briefRefusalFor(const DatabaseOwnership& ownership)
    {
        if (!ownership.guest) return std::nullopt;
        return common::Error {"outcome_brief.not_the_owner",
            "This runtime is a guest of a database that another runtime owns, so it does not write Outcome Briefs"};
    }

    static std::optional<common::Error> followUpRefusalFor(const DatabaseOwnership& ownership)
    {
        if (!ownership.guest) return std::nullopt;
        return common::Error {"follow_up.not_the_owner",
            "This runtime is a guest of a database that another runtime owns, so it does not compose follow-up plans"};
    }

    static StorageBundle openStorage(const std::filesystem::path& databasePath, const DatabaseOwnership& ownership)
    {
        auto opened = ownership.error.has_value()
            ? common::Result<std::shared_ptr<infrastructure::SqliteDatabase>>::failure(*ownership.error)
            : infrastructure::SqliteDatabase::open(databasePath);
        if (!opened.success()) {
            const std::string reason = opened.error()->message;
            return StorageBundle {
                nullptr,
                std::make_unique<UnavailableProjectRepository>(reason),
                std::make_unique<UnavailableTaskRepository>(reason),
                std::make_unique<UnavailableRunRepository>(reason),
                std::make_unique<UnavailableExecutionManifestRepository>(reason),
                std::make_unique<UnavailableManifestTaskExecutionRepository>(reason),
                std::make_unique<UnavailableManifestWorkLedgerRepository>(reason),
                std::make_unique<UnavailableManifestVerificationContinuationRepository>(reason),
                std::make_unique<UnavailableManifestVerificationBaselineNormalizationRepository>(reason),
                std::make_unique<UnavailableManifestVerificationManualHandoffRepository>(reason),
                std::make_unique<UnavailableRunWorkspaceScopeRepository>(reason),
                std::make_unique<UnavailableVerificationEnvironmentRepository>(reason),
                std::make_unique<UnavailableVerificationBootstrapRepository>(reason),
                std::make_unique<UnavailableWorkspaceStateRepository>(reason),
                std::make_unique<UnavailableApplicationSettingsRepository>(reason),
                std::make_unique<UnavailableProviderRepository>(reason),
                std::make_unique<UnavailableModelRepository>(reason),
                std::make_unique<UnavailableExecutionProfileRepository>(reason),
                std::make_unique<UnavailableRunDraftRepository>(reason),
                std::make_unique<UnavailableChatRepository>(reason),
                std::make_unique<UnavailableChatMemoryRepository>(reason),
                std::make_unique<UnavailableModelExecutionRepository>(reason),
                std::make_unique<UnavailableComparisonSessionRepository>(reason),
                std::make_unique<UnavailableProjectMemoryRepository>(reason),
                std::make_unique<UnavailableProjectMemoryCompactionRepository>(reason),
                std::make_unique<UnavailableChangeProposalRepository>(reason),
                std::make_unique<UnavailableAgentToolSessionRepository>(reason),
                std::make_unique<UnavailableAgentToolAuditRepository>(reason),
                std::make_unique<UnavailableChangeApprovalRepository>(reason),
                std::make_unique<UnavailableProposalReviewRepository>(reason),
                std::make_unique<UnavailableOutcomeBriefRepository>(reason),
                std::make_unique<UnavailableFollowUpPlanRepository>(reason),
                std::make_unique<UnavailableFollowUpPlanRepository>(reason),
                opened.error()};
        }

        auto database = opened.value();
        return StorageBundle {
            database,
            std::make_unique<infrastructure::SqliteProjectRepository>(database),
            std::make_unique<infrastructure::SqliteTaskRepository>(database),
            std::make_unique<infrastructure::SqliteRunRepository>(database),
            std::make_unique<infrastructure::SqliteExecutionManifestRepository>(database),
            std::make_unique<infrastructure::SqliteManifestTaskExecutionRepository>(database),
            std::make_unique<infrastructure::SqliteManifestWorkLedgerRepository>(database),
            std::make_unique<infrastructure::SqliteManifestVerificationContinuationRepository>(database),
            std::make_unique<infrastructure::SqliteManifestVerificationBaselineNormalizationRepository>(database),
            std::make_unique<infrastructure::SqliteManifestVerificationManualHandoffRepository>(database),
            std::make_unique<infrastructure::SqliteRunWorkspaceScopeRepository>(database),
            std::make_unique<infrastructure::SqliteVerificationEnvironmentRepository>(database),
            std::make_unique<infrastructure::SqliteVerificationBootstrapRepository>(database),
            std::make_unique<infrastructure::SqliteWorkspaceStateRepository>(database),
            std::make_unique<infrastructure::SqliteApplicationSettingsRepository>(database),
            std::make_unique<infrastructure::SqliteProviderRepository>(database),
            std::make_unique<infrastructure::SqliteModelRepository>(database),
            std::make_unique<infrastructure::SqliteExecutionProfileRepository>(database),
            std::make_unique<infrastructure::SqliteRunDraftRepository>(database),
            std::make_unique<infrastructure::SqliteChatRepository>(database),
            std::make_unique<infrastructure::SqliteChatMemoryRepository>(database),
            std::make_unique<infrastructure::SqliteModelExecutionRepository>(database),
            std::make_unique<infrastructure::SqliteComparisonSessionRepository>(database),
            std::make_unique<infrastructure::SqliteProjectMemoryRepository>(database),
            std::make_unique<infrastructure::SqliteProjectMemoryCompactionRepository>(database),
            std::make_unique<infrastructure::SqliteChangeProposalRepository>(database),
            std::make_unique<infrastructure::SqliteAgentToolSessionRepository>(database),
            std::make_unique<infrastructure::SqliteAgentToolAuditRepository>(database),
            std::make_unique<infrastructure::SqliteChangeApprovalRepository>(database),
            std::make_unique<infrastructure::SqliteProposalReviewRepository>(database),
            std::make_unique<infrastructure::SqliteOutcomeBriefRepository>(database),
            std::make_unique<infrastructure::SqliteFollowUpPlanRepository>(database),
            std::make_unique<infrastructure::SqliteFollowUpPlanRepository>(database),
            std::nullopt};
    }

    struct ArtifactBundle {
        std::unique_ptr<application::IArtifactStore> store;
        std::unique_ptr<application::IArtifactRepository> repository;
        std::unique_ptr<application::IRunWarningRepository> warnings;
    };

    static ArtifactBundle openArtifactStorage(
        const StorageBundle& storage,
        const BackendRuntimeConfig& config)
    {
        // The artifact filesystem root is a failure mode independent from
        // the SQLite metadata database: either can fail without the other.
        std::filesystem::path artifactRoot = config.artifactRoot;
        if (artifactRoot.empty()) {
            artifactRoot = config.databasePath.parent_path() / "artifacts";
        }

        auto storeResult = infrastructure::FileArtifactStore::open(artifactRoot);
        if (!storeResult.success()) {
            const std::string reason = storeResult.error()->message;
            return ArtifactBundle {
                std::make_unique<UnavailableArtifactStore>(reason),
                std::make_unique<UnavailableArtifactRepository>(reason),
                // The database may be fine while the artifact root is not - exactly the case in which a warning
                // about the lost artifacts can still be kept.
                storage.database == nullptr
                    ? std::unique_ptr<application::IRunWarningRepository>(
                          std::make_unique<UnavailableRunWarningRepository>(storage.error->message))
                    : std::unique_ptr<application::IRunWarningRepository>(
                          std::make_unique<infrastructure::SqliteRunWarningRepository>(storage.database))};
        }

        if (storage.database == nullptr) {
            // Metadata persistence is unavailable even though the
            // filesystem root opened fine; keep the working store (a write
            // through it is harmless - it simply never becomes visible
            // because ArtifactService's insertArtifact step will fail) but
            // route every metadata operation through the same controlled
            // failure every other repository already uses.
            return ArtifactBundle {
                std::move(storeResult.value()),
                std::make_unique<UnavailableArtifactRepository>(storage.error->message),
                std::make_unique<UnavailableRunWarningRepository>(storage.error->message)};
        }

        return ArtifactBundle {
            std::move(storeResult.value()),
            std::make_unique<infrastructure::SqliteArtifactRepository>(storage.database),
            std::make_unique<infrastructure::SqliteRunWarningRepository>(storage.database)};
    }

    static std::unique_ptr<application::IVerificationScratchSettingRepository> openScratchSettingRepository(
        const StorageBundle& storage)
    {
        if (storage.database == nullptr) {
            return std::make_unique<UnavailableVerificationScratchSettingRepository>(storage.error->message);
        }
        return std::make_unique<infrastructure::SqliteVerificationScratchSettingRepository>(storage.database);
    }

    static std::unique_ptr<application::IProjectReadinessRepository> openProjectReadinessRepository(
        const StorageBundle& storage)
    {
        if (storage.database == nullptr) {
            return std::make_unique<UnavailableProjectReadinessRepository>(storage.error->message);
        }
        return std::make_unique<infrastructure::SqliteProjectReadinessRepository>(storage.database);
    }

    struct ProposalBundle {
        std::unique_ptr<application::IChangeProposalStore> store;
    };

    static ProposalBundle openProposalStorage(
        const StorageBundle& storage,
        const BackendRuntimeConfig& config)
    {
        // Proposal content lives beside the artifact tree.  Derive the root
        // from the database path parent so tests that only pass databasePath
        // still get an isolated directory without extra configuration.
        std::filesystem::path proposalRoot =
            config.databasePath.parent_path() / "proposals";

        auto storeResult = infrastructure::FileChangeProposalStore::open(proposalRoot);
        if (!storeResult.success()) {
            const std::string reason = storeResult.error()->message;
            return ProposalBundle{
                std::make_unique<UnavailableChangeProposalStore>(reason)};
        }

        if (storage.database == nullptr) {
            // Metadata persistence is unavailable; store opened fine but the
            // repository side will fail every call, so use Unavailable store
            // too for a consistent failure surface.
            return ProposalBundle{
                std::make_unique<UnavailableChangeProposalStore>(storage.error->message)};
        }

        return ProposalBundle{std::move(storeResult.value())};
    }

    // The startup recovery sequence itself lives in ai::backend::runStartupRecovery
    // (StartupRecovery.hpp): one function, called once from the constructor body below and by
    // the tests that prove what a restart does, so they exercise the production order.

    explicit Impl(const BackendRuntimeConfig& config)
        // pathClaimRegistry has zero dependencies (a plain mutex + map), so
        // it is declared and constructed first, before anything that might
        // reference it - currently only changeProposalService.
        : pathClaimRegistry()
        , ownership(takeOwnership(config))
        , storage(openStorage(config.databasePath, ownership))
        , executionManifestService(
              *storage.executionManifestRepository, *storage.runRepository)
        , manifestReadModelService(
              executionManifestService, *storage.manifestTaskExecutionRepository,
              storage.runWorkspaceScopeRepository.get(), storage.changeProposalRepository.get(),
              // AG-18.4A: verification-environment/bootstrap projection -
              // same optional-pointer pattern as the two pointers above.
              storage.verificationEnvironmentRepository.get(),
              storage.verificationBootstrapRepository.get(),
              // QML-lint advisory-severity policy (2026-09-28): reuses the
              // one already-existing production IManifestWorkLedgerRepository
              // (constructed above as part of `storage`, before this member)
              // - never a second persistence/repository model.
              storage.manifestWorkLedgerRepository.get(),
              // AG-17.6: display names for the roster/task routing projection.
              storage.providerRepository.get(), storage.modelRepository.get(),
              // AG-17.7B: the Run's ModelExecution rows, read only to reconstruct
              // member-level usage/cost (no schema change).
              storage.modelExecutionRepository.get())
        , artifacts(openArtifactStorage(storage, config))
        , artifactService(*artifacts.repository, *artifacts.store, artifacts.warnings.get())
        , scratchSettingRepository(openScratchSettingRepository(storage))
        , verificationScratchService(*scratchSettingRepository)
        , projectReadinessRepository(openProjectReadinessRepository(storage))
        , projectReadinessService(*projectReadinessRepository)
        // AG-14: declared here (after storage and artifactService, both
        // already constructed above) and before every service that takes a
        // pointer to it (changeSetApplyService, proposalReviewService below,
        // plus runManager/manifestWorkflowRunner much later) - matches this
        // file's own "declared after storage" lifetime rule, and being
        // constructed this early is required since changeSetApplyService/
        // proposalReviewService are themselves constructed well before
        // projectMemoryService/runManager.
        , manifestWorkLedgerService(
              *storage.manifestWorkLedgerRepository, *storage.runRepository,
              *storage.executionManifestRepository, *storage.manifestTaskExecutionRepository,
              *storage.changeProposalRepository, &artifactService,
              storage.changeApprovalRepository.get(), storage.proposalReviewRepository.get(),
              storage.runWorkspaceScopeRepository.get())
        // AG-15: declared here (same reasoning as manifestWorkLedgerService
        // immediately above - after storage, before manifestWorkflowRunner
        // much later) so its address can be passed into manifestWorkflowRunner's
        // constructor below.
        , runWorkspaceScopeService(*storage.runWorkspaceScopeRepository)
        // AG-18.1: same reasoning as runWorkspaceScopeService immediately
        // above - constructed here so its address can be passed into
        // manifestWorkflowRunner's constructor below.
        , verificationEnvironmentDiscoveryService(*storage.verificationEnvironmentRepository)
        , proposals(openProposalStorage(storage, config))
        , changeProposalService(
              *storage.changeProposalRepository, *proposals.store, &pathClaimRegistry)
        , manifestVerificationLegacyBaselineNormalizationService(
              *storage.manifestVerificationBaselineNormalizationRepository, artifactService,
              changeProposalService)
        , continuationVerificationCommandRunner()
        , manifestVerificationContinuationService(
              *storage.runRepository, *storage.manifestWorkLedgerRepository,
              *storage.changeProposalRepository,
              *storage.manifestVerificationContinuationRepository)
        , manifestVerificationAttributionService(
              *storage.executionManifestRepository, *storage.manifestWorkLedgerRepository,
              *storage.manifestVerificationContinuationRepository,
              *storage.runWorkspaceScopeRepository,
              *storage.manifestVerificationBaselineNormalizationRepository)
        , manifestVerificationContinuationPreparationService(
              *storage.runRepository, *storage.projectRepository,
              *storage.executionManifestRepository, *storage.manifestWorkLedgerRepository,
              *storage.manifestVerificationContinuationRepository,
              manifestVerificationContinuationService,
              manifestVerificationAttributionService,
              changeProposalService, continuationVerificationCommandRunner,
              manifestVerificationLegacyBaselineNormalizationService)
        , manifestVerificationContinuationReadModelService(
              *storage.runRepository, *storage.manifestWorkLedgerRepository,
              *storage.manifestVerificationContinuationRepository,
              *storage.manifestVerificationBaselineNormalizationRepository,
              manifestVerificationContinuationService)
        // QML-P4R.3: constructed here - every dependency it needs
        // (changeProposalService, manifestVerificationContinuationService/
        // AttributionService/LegacyBaselineNormalizationService,
        // continuationVerificationCommandRunner) already exists above, the
        // same set manifestVerificationContinuationPreparationService itself
        // required. Durable candidate storage root matches
        // openArtifactStorage/openProposalStorage's own
        // config.databasePath.parent_path() convention ("artifacts",
        // "proposals") - never %TEMP%, per this service's own constructor
        // contract.
        , manifestVerificationManualHandoffService(
              *storage.runRepository, *storage.projectRepository,
              *storage.executionManifestRepository, *storage.manifestWorkLedgerRepository,
              *storage.manifestVerificationManualHandoffRepository,
              *storage.manifestVerificationContinuationRepository,
              manifestVerificationContinuationService,
              manifestVerificationAttributionService,
              changeProposalService, continuationVerificationCommandRunner,
              manifestVerificationLegacyBaselineNormalizationService,
              config.databasePath.parent_path() / "manual-handoff")
        // AG-16.1: constructed here (after changeProposalService/storage,
        // before changeSetApplyService) so its address can be passed into
        // changeSetApplyService's constructor immediately below.
        , manifestApplyVerificationGate(
              changeProposalService, *storage.manifestWorkLedgerRepository, *storage.runRepository,
              storage.verificationBootstrapRepository.get(),
              storage.executionManifestRepository.get(),
              storage.manifestVerificationContinuationRepository.get(),
              storage.manifestVerificationManualHandoffRepository.get())
        , changeSetApplyService(
              *storage.changeProposalRepository, *proposals.store,
              *storage.runRepository, *storage.projectRepository,
              *storage.changeApprovalRepository, &artifactService, &manifestWorkLedgerService,
              &manifestApplyVerificationGate)
        , proposalReviewService(
              *storage.changeProposalRepository, *storage.proposalReviewRepository,
              &manifestWorkLedgerService)
        // QML-P4R.5: constructed here - after manifestVerificationManualHandoffService
        // (whose probeCandidateHealth()/resolveCandidateRoot() it reuses,
        // never re-deriving candidate-health logic) and manifestApplyVerificationGate
        // (the sole Apply-eligibility authority it defers to, never
        // recomputing AG-16 itself). Read-only: never calls materialize(),
        // importAndReverify(), discardHandoff(), or enforceRetention() -
        // those remain the Qt adapter's own explicit command surface.
        // Post-P4R.6 human review/Apply hardening: also declared after
        // changeProposalService/proposalReviewService, whose already-proven
        // application services its reviewMembers projection reuses read-only
        // (findChangeSet/listChanges/listValidationFindings/findRejection) -
        // never a second, independently-maintained copy of that logic.
        , manifestVerificationManualHandoffReadModelService(
              *storage.runRepository, *storage.manifestWorkLedgerRepository,
              *storage.manifestVerificationManualHandoffRepository,
              manifestVerificationManualHandoffService, manifestApplyVerificationGate,
              changeProposalService, proposalReviewService)
        // PC-1: read-only; holds references only to storage repositories and the manual-handoff read model
        // (its Run resolution projection), all already constructed above. Nothing else holds a pointer to it
        // and it owns no state, so its position only has to satisfy "after those, destroyed before storage".
        , projectContextSnapshotService(
              *storage.projectRepository, *storage.runRepository, *storage.manifestWorkLedgerRepository,
              manifestVerificationManualHandoffReadModelService, *storage.projectMemoryRepository)
        // PC-4C: read-only views over the repositories and the manual-handoff resolution reader (all constructed above),
        // and the reconciler that writes ONLY the Outcome Brief repository. Both are destroyed before the services and the
        // storage they reference; the services that hold a pointer to the reconciler are nudged only while the runtime lives.
        , outcomeBriefSources(
              *storage.runRepository, *storage.projectRepository, *storage.manifestWorkLedgerRepository,
              *storage.changeProposalRepository, *storage.changeApprovalRepository, *storage.proposalReviewRepository,
              *storage.manifestVerificationContinuationRepository, *storage.manifestVerificationManualHandoffRepository,
              manifestVerificationManualHandoffReadModelService)
        , outcomeBriefReconciler(outcomeBriefSources, *storage.outcomeBriefRepository, briefRefusalFor(ownership))
        // PC-4D: const views of the Run and brief repositories, constructed above.
        , outcomeBriefReadModelService(*storage.runRepository, *storage.outcomeBriefRepository)
        // PC-5C-2b: the production follow-up source is read-only. It uses the
        // already-constructed PC-1 snapshot service and current Outcome head;
        // file bytes enter only through the safe single-handle loader.
        , followUpFileLoader()
        , followUpSourceReader(
              *storage.projectRepository, *storage.runRepository, *storage.outcomeBriefRepository,
              projectContextSnapshotService, *storage.changeProposalRepository, followUpFileLoader)
        , agentToolReadModelService(
              *storage.agentToolSessionRepository, *storage.agentToolAuditRepository,
              changeProposalService)
        // credentialStore/credentialManagementService/providerClientFactory/
        // workspaceContextService/providerCatalogService/
        // executionProfileService/workflowRouter reference only storage's
        // repositories (or each other, never workflowRunner/
        // productionWorkflowRunner/runManager), so their exact position
        // between artifactService and workflowRunner only has to satisfy one
        // rule: declared after storage (so its repositories already exist)
        // and destroyed before storage (see the member list below).
        , credentialStore(createCredentialStore())
        , providerClientFactory(createProviderClientFactory(*credentialStore, *storage.providerRepository))
        , workspaceContextService(*storage.workspaceStateRepository, *storage.projectRepository)
        , providerCatalogService(*storage.providerRepository, *storage.modelRepository)
        , credentialManagementService(*credentialStore, providerCatalogService)
        , executionProfileService(
              *storage.executionProfileRepository, *storage.providerRepository, *storage.modelRepository)
        , runDraftService(*storage.runDraftRepository)
        // PC-3C: Draft + Project + Chat repositories and a read-only "project of this Run" lookup (no Run writes, no
        // RunManager, no provider): the service can write Draft groups and nothing else.
        , runSourceLookup(*storage.runRepository)
        , runDraftPlanService(
              *storage.runDraftRepository, *storage.projectRepository, *storage.chatRepository, runSourceLookup)
        , followUpPlanService(
              *storage.followUpPlanRepository, *storage.followUpPlanConfirmPort, followUpSourceReader,
              *storage.providerRepository, *storage.modelRepository, *storage.runDraftRepository,
              {}, followUpRefusalFor(ownership))
        // PC-5D: the read-only side of the follow-up dialog (gate per Run, model picker, templates, source Runs).
        , followUpPlanReadModel(
              *storage.outcomeBriefRepository, *storage.followUpPlanRepository, *storage.providerRepository,
              *storage.modelRepository, *storage.runDraftRepository, runSourceLookup, followUpRefusalFor(ownership))
        , applicationSettingsService(
              *storage.applicationSettingsRepository, *storage.executionProfileRepository,
              *storage.providerRepository, *storage.modelRepository)
        , modelExecutionService(*storage.modelExecutionRepository)
        , followUpComposerProcessor(
              *storage.followUpPlanRepository, followUpSourceReader, *storage.providerRepository,
              *storage.modelRepository, *providerClientFactory, modelExecutionService,
              {}, followUpRefusalFor(ownership))
        , followUpComposerRunner(followUpComposerProcessor)
        // AG-17.7D-1: startup recovery's interrupted-task pass; reads the same repositories the
        // Work Ledger pass after it does, and runs before it.
        , manifestInterruptedTaskReconciler(
              *storage.runRepository, *storage.executionManifestRepository,
              *storage.manifestTaskExecutionRepository, *storage.modelExecutionRepository,
              *storage.manifestWorkLedgerRepository)
        , executionAnalyticsService(*storage.modelExecutionRepository, *storage.modelRepository)
        , comparisonService(*storage.comparisonSessionRepository, *storage.projectRepository,
              providerCatalogService, modelExecutionService)
        , comparisonReadModelService(
              comparisonService, modelExecutionService, providerCatalogService)
        , chatService(*storage.chatRepository, *storage.projectRepository, *storage.workspaceStateRepository,
              *storage.providerRepository, *storage.modelRepository, *providerClientFactory,
              &modelExecutionService, storage.chatMemoryRepository.get(),
              &applicationSettingsService)
        , comparisonExecutionManager(
              comparisonService, modelExecutionService, providerCatalogService,
              *storage.projectRepository, *providerClientFactory,
              config.maxComparisonConcurrency)
        , workflowRouter(*storage.executionProfileRepository, *storage.providerRepository, *storage.modelRepository)
        // filesystemExecutor/agentToolExecutionService/agentSessionFactory are
        // declared (and therefore constructed) here, before
        // productionWorkflowRunner, specifically so their addresses can be
        // passed into its constructor below for workspaceAccess roster
        // agents - productionWorkflowRunner only holds pointers to them
        // (never owns them), so this is otherwise the same "never outlive
        // what you reference" rule as everywhere else in this file.
        , filesystemExecutor()
        , agentToolExecutionService(
              createDefaultRegistry(),
              filesystemExecutor,
              *storage.agentToolSessionRepository,
              *storage.agentToolAuditRepository,
              config.enableAgentToolExecution && storage.database != nullptr)
        , agentSessionFactory(createAgentSessionFactory(config,
              *credentialStore, *storage.providerRepository))
        // MockWorkflowRunner/ExecutionProfileWorkflowRunner only hold raw
        // pointers/references (never own what they point at), so they may
        // sit before artifactService/providerClientFactory in declaration
        // order as long as they are still destroyed - see below - before
        // those objects; the actual requirement is "a runner never outlives
        // what it references", which the reversed destruction order below
        // guarantees regardless of exactly where between storage and
        // runManager the runners are declared.
        , workflowRunner(config.mockStepDelay, config.mockProgressSteps, &artifactService, config.mockWorkflowGate)
        , productionWorkflowRunner(
              *providerClientFactory, *storage.providerRepository, *storage.modelRepository,
              &artifactService, &modelExecutionService,
              storage.projectMemoryRepository.get(),
              agentSessionFactory.get(), &agentToolExecutionService, &changeProposalService,
              &executionManifestService)
        // Phase 5: registered beside workflowRunner/productionWorkflowRunner
        // under kExecutionManifestWorkflowId, never replacing either. Same
        // "holds only refs/pointers, never owns" rule as productionWorkflowRunner.
        , manifestWorkflowRunner(
              *agentSessionFactory, agentToolExecutionService, changeProposalService,
              executionManifestService, *providerClientFactory,
              *storage.providerRepository, *storage.modelRepository,
              &artifactService, &modelExecutionService,
              storage.manifestTaskExecutionRepository.get(), nullptr,
              &manifestWorkLedgerService, &runWorkspaceScopeService,
              &verificationEnvironmentDiscoveryService,
              storage.verificationBootstrapRepository.get())
        // MW-P5.4: constructed here - the same point manifestWorkflowRunner
        // above becomes available, since it needs the identical
        // agentSessionFactory/agentToolExecutionService dependency pair plus
        // everything manifestVerificationContinuationPreparationService
        // (constructed much earlier) already required.
        , manifestVerificationContinuationRepairService(
              *storage.runRepository, *storage.projectRepository, executionManifestService,
              *storage.manifestWorkLedgerRepository, *storage.manifestVerificationContinuationRepository,
              *storage.manifestTaskExecutionRepository, *storage.providerRepository,
              *storage.modelRepository, changeProposalService, manifestWorkLedgerService,
              artifactService, *agentSessionFactory, agentToolExecutionService,
              continuationVerificationCommandRunner, &modelExecutionService)
        , projectMemoryService(*storage.projectMemoryRepository)
        , projectMemoryCompactionService(
              *storage.projectMemoryRepository, *storage.projectMemoryCompactionRepository,
              *storage.projectRepository, *storage.providerRepository, *storage.modelRepository,
              *providerClientFactory)
        , runManager(*storage.projectRepository, *storage.runRepository, workflowRouter,
              &projectMemoryService, &pathClaimRegistry, &modelExecutionService,
              &manifestWorkLedgerService)
        , projectService(*storage.projectRepository)
        , taskService(*storage.taskRepository, *storage.projectRepository)
        , orchestrationService(*storage.projectRepository, *storage.runRepository, runManager)
        , verificationBootstrapRetryService(
              *storage.verificationBootstrapRepository, *storage.runRepository,
              *storage.projectRepository, orchestrationService)
    {
        // Registration happens here (not in the initializer list) because it
        // needs workflowRunner/productionWorkflowRunner to already be fully
        // constructed objects with a stable address; both are already
        // guaranteed to outlive workflowRouter and runManager by the
        // declaration order below.
        workflowRouter.registerRunner(application::kMockWorkflowId, workflowRunner);
        workflowRouter.registerRunner(application::kExecutionProfileWorkflowId, productionWorkflowRunner);
        workflowRouter.registerRunner(application::kExecutionManifestWorkflowId, manifestWorkflowRunner);

        // Every service that builds a TEMPORARY copy of a workspace for verification asks the one validated provider for its root.
        // Set once here, before startup recovery or any caller can reach a service.
        manifestWorkflowRunner.setVerificationScratchProvider(&verificationScratchService);
        // The durable per-project readiness (AG-18.4B-R1): a Run proves or reuses the project's configured setup before it spends
        // provider money.
        manifestWorkflowRunner.setProjectReadinessService(&projectReadinessService);
        manifestVerificationContinuationPreparationService.setVerificationScratchProvider(&verificationScratchService);
        manifestVerificationContinuationRepairService.setVerificationScratchProvider(&verificationScratchService);
        manifestVerificationManualHandoffService.setVerificationScratchProvider(&verificationScratchService);

        // A lost diagnostic artifact is recorded durably by ArtifactService first; only then is the UI told. runManager
        // is declared after artifactService and joins its worker before artifactService goes away, so the capture
        // cannot dangle while a worker can still lose an artifact.
        artifactService.setWarningObserver(
            [this](const application::RunWarning& warning, const std::optional<common::Error>& recordError) {
                application::RunEvent event;
                event.type = application::RunEventType::RunWarning;
                event.runId = warning.runId;
                event.stageId = warning.stageId;
                event.message = recordError.has_value()
                    ? warning.message + " [not saved: " + recordError->message + "]"
                    : warning.message;
                runManager.announceWarning(event);
            });

        // The whole startup recovery sequence (Runs, model executions, comparison sessions,
        // agent audit, then the Work Ledger / continuation / manual-handoff reconciliation and
        // retention) is ONE function shared with the tests that prove what a restart does. It
        // runs here, before BackendRuntime returns to its caller, so the very first read already
        // reflects recovered state. Each recovery error channel keeps its exact meaning.
        application::ComparisonService* const comparison = &comparisonService;
        auto outcome = backend::runStartupRecovery(backend::StartupRecoveryServices {
            orchestrationService, modelExecutionService, manifestInterruptedTaskReconciler,
            *storage.agentToolSessionRepository, *storage.agentToolAuditRepository,
            manifestWorkLedgerService, comparison,
            &manifestVerificationContinuationPreparationService,
            &manifestVerificationManualHandoffService,
            ownership.guest ? nullptr : &followUpComposerProcessor});
        recoveryError = std::move(outcome.recoveryError);
        modelExecutionRecoveryError = std::move(outcome.modelExecutionRecoveryError);
        comparisonRecoveryError = std::move(outcome.comparisonRecoveryError);
        agentAuditRecoveryError = std::move(outcome.agentAuditRecoveryError);

        // The shared startup pass above does all recovery synchronously and
        // never calls a provider. Only after it returns do we connect the
        // owner-only worker and queue the explicitly authorised live requests
        // it found. A guest receives a fail-closed service/processor and no
        // callbacks or jobs.
        if (!ownership.guest) {
            {
                std::lock_guard lock(followUpComposerErrorMutex);
                followUpComposerError = std::move(outcome.followUpRecoveryError);
            }
            followUpComposerRunner.setErrorSink([this](const common::Error& error) {
                std::lock_guard lock(followUpComposerErrorMutex);
                followUpComposerError = error;
            });
            followUpPlanService.setAuthorizedNudge(
                [this](const application::FollowUpPlanRequestId& id) { followUpComposerRunner.nudge(id); });
            followUpPlanService.setComposingCancel(
                [this](const application::FollowUpPlanRequestId& id) { return followUpComposerRunner.requestStop(id); });
            for (const auto& id : outcome.followUpToProcess) followUpComposerRunner.nudge(id);
        }

        // PC-4C: only now, with the startup recovery finished exactly as before, the producers get their best-effort nudge to the
        // Outcome Brief reconciler. It is set once here, before any caller can reach a service, and never changes afterwards.
        // Only the OWNER of the database writes briefs: a guest (test fixtures) wires nothing, so there is never a second reconciler
        // writing to the same file.
        if (!ownership.guest) {
            runManager.setOutcomeBriefTrigger(&outcomeBriefReconciler);
            proposalReviewService.setOutcomeBriefTrigger(&outcomeBriefReconciler);
            changeSetApplyService.setOutcomeBriefTrigger(&outcomeBriefReconciler);
            manifestVerificationContinuationPreparationService.setOutcomeBriefTrigger(&outcomeBriefReconciler);
            manifestVerificationContinuationRepairService.setOutcomeBriefTrigger(&outcomeBriefReconciler);
            manifestVerificationManualHandoffService.setOutcomeBriefTrigger(&outcomeBriefReconciler);
        }
    }

    // Declaration order is the same kind of lifetime contract as storage
    // above: artifacts/artifactService are declared (and therefore
    // destroyed) around the runners so the raw pointers/references they hold
    // stay valid for as long as runManager's worker thread might still call
    // into them - runManager itself is destroyed first (joining that
    // thread) before workflowRouter, the runners, providerClientFactory,
    // credentialStore, artifactService, or artifacts. credentialStore/
    // credentialManagementService/providerClientFactory/
    // workspaceContextService/providerCatalogService/executionProfileService/
    // workflowRouter are all declared after storage/artifacts (so they are
    // destroyed before storage/artifacts) for the same "never outlive what
    // you reference" rule, even though most of them are never reached from a
    // background thread.
    // Zero dependencies; must be declared before changeProposalService,
    // which stores a pointer to it.
    application::RunPathClaimRegistry pathClaimRegistry;
    // Declared right before `storage`: the ownership lock is taken before the database is opened and, destroyed after it, released only
    // once the connection is closed.
    DatabaseOwnership ownership;
    StorageBundle storage;
    application::ExecutionManifestService executionManifestService;
    // References executionManifestService (declared immediately above) and
    // storage.manifestTaskExecutionRepository; holds no state of its own.
    application::ExecutionManifestReadModelService manifestReadModelService;
    ArtifactBundle artifacts;
    application::ArtifactService artifactService;
    // The user's choice of the folder for the temporary verification copies, and the one validated provider of that root; declared
    // before every service that is handed the provider in the constructor body.
    std::unique_ptr<application::IVerificationScratchSettingRepository> scratchSettingRepository;
    application::VerificationScratchService verificationScratchService;
    // The durable, machine-local readiness of each project's verification setup (schema step 70) and its one owner; declared before
    // every service that is handed it in the constructor body.
    std::unique_ptr<application::IProjectReadinessRepository> projectReadinessRepository;
    application::ProjectReadinessService projectReadinessService;
    // AG-14: declared here (see the matching constructor init-list comment)
    // so it already exists for changeSetApplyService/proposalReviewService
    // just below, and for runManager/manifestWorkflowRunner much later.
    application::ManifestWorkLedgerService manifestWorkLedgerService;
    // AG-15: declared here (same reasoning as manifestWorkLedgerService
    // immediately above); manifestWorkflowRunner only holds a pointer to it.
    application::RunWorkspaceScopeService runWorkspaceScopeService;
    // AG-18.1: same reasoning as runWorkspaceScopeService immediately
    // above - manifestWorkflowRunner only holds a pointer to it.
    application::VerificationEnvironmentDiscoveryService verificationEnvironmentDiscoveryService;
    ProposalBundle proposals;
    application::ChangeProposalService changeProposalService;
    // MW-P5.6D: declared after artifactService/changeProposalService (which
    // it references) and before manifestVerificationContinuationPreparationService
    // (which holds a reference to it) - see the matching constructor
    // init-list comment.
    application::ManifestVerificationLegacyBaselineNormalizationService
        manifestVerificationLegacyBaselineNormalizationService;
    application::SystemManifestVerificationCommandRunner continuationVerificationCommandRunner;
    application::ManifestVerificationContinuationService manifestVerificationContinuationService;
    application::ManifestVerificationAttributionService manifestVerificationAttributionService;
    application::ManifestVerificationContinuationPreparationService
        manifestVerificationContinuationPreparationService;
    application::ManifestVerificationContinuationReadModelService
        manifestVerificationContinuationReadModelService;
    // QML-P4R.3: declared after every dependency it needs (changeProposalService,
    // manifestVerificationContinuationService/AttributionService/
    // LegacyBaselineNormalizationService, continuationVerificationCommandRunner,
    // all already constructed above). reconcileInterruptedAttempts() is
    // called exactly once, from the constructor body below - right after
    // recoverInterruptedRuns() and manifestVerificationContinuationPreparationService's
    // own reconcileMissingChildren() pass, before BackendRuntime ever returns
    // to its caller - so the very first read any caller performs already
    // reflects a fully reconciled manual-handoff attempt matrix, the same
    // guarantee recoveryError's own doc comment already states for Run
    // recovery. Minimal composition only: this package's own scope is
    // startup crash reconciliation, never a public accessor or UI wiring -
    // see QML-P4R.5 for exposing the service itself.
    application::ManifestVerificationManualHandoffService manifestVerificationManualHandoffService;
    // AG-16.1: declared after changeProposalService (which it references)
    // and before changeSetApplyService (which holds a pointer to it) - see
    // the matching constructor init-list comment.
    application::ManifestApplyVerificationGate manifestApplyVerificationGate;
    // Depends only on the same two members as changeProposalService;
    // declared immediately after so it is destroyed before them.
    application::ChangeSetApplyService changeSetApplyService;
    // AG-07: independent of changeSetApplyService (different repository,
    // storage.proposalReviewRepository) but reads changeProposalService's
    // repository to check a change set's current state before recording a
    // rejection - declared after both are constructed.
    application::ProposalReviewService proposalReviewService;
    // QML-P4R.5: the single authoritative read model for the manual-handoff
    // feature - see this class's own header doc comment. Declared after
    // manifestVerificationManualHandoffService/manifestApplyVerificationGate
    // (which it holds references to) and, post-P4R.6 human review/Apply
    // hardening, after changeProposalService/proposalReviewService too
    // (its reviewMembers projection - see the header's own doc comment).
    application::ManifestVerificationManualHandoffReadModelService
        manifestVerificationManualHandoffReadModelService;
    // PC-1: see the matching constructor init-list comment.
    application::ProjectContextSnapshotService projectContextSnapshotService;
    // PC-4C: see the matching constructor init-list comment.
    application::RepositoryOutcomeBriefSources outcomeBriefSources;
    application::OutcomeBriefReconciler outcomeBriefReconciler;
    // PC-4D: see the matching constructor init-list comment.
    application::OutcomeBriefReadModelService outcomeBriefReadModelService;
    application::FilesystemFollowUpFileLoader followUpFileLoader;
    application::FollowUpSourceReader followUpSourceReader;
    // References storage's audit repos and changeProposalService, all
    // already constructed above; holds no state of its own.
    application::AgentToolReadModelService agentToolReadModelService;
    std::unique_ptr<application::ICredentialStore> credentialStore;
    std::unique_ptr<application::IProviderClientFactory> providerClientFactory;
    application::WorkspaceContextService workspaceContextService;
    application::ProviderCatalogService providerCatalogService;
    application::CredentialManagementService credentialManagementService;
    application::ExecutionProfileService executionProfileService;
    application::RunDraftService runDraftService;
    application::RunRepositorySourceLookup runSourceLookup;
    application::RunDraftPlanService runDraftPlanService;
    application::FollowUpPlanService followUpPlanService;
    application::FollowUpPlanReadModel followUpPlanReadModel;
    application::ApplicationSettingsService applicationSettingsService;
    application::ModelExecutionService modelExecutionService;
    // Declared before the worker so reverse destruction stops and joins the
    // worker before either its processor or error sink storage disappears.
    mutable std::mutex followUpComposerErrorMutex;
    std::optional<common::Error> followUpComposerError;
    application::FollowUpComposerProcessor followUpComposerProcessor;
    application::FollowUpComposerRunner followUpComposerRunner;
    application::ManifestInterruptedTaskReconciler manifestInterruptedTaskReconciler;
    application::ExecutionAnalyticsService executionAnalyticsService;
    application::ComparisonService comparisonService;
    application::ComparisonReadModelService comparisonReadModelService;
    application::ChatService chatService;
    application::ComparisonExecutionManager comparisonExecutionManager;
    application::WorkflowRouter workflowRouter;
    // filesystemExecutor is stateless (no dependencies other than the
    // filesystem itself); declared before agentToolExecutionService so it
    // is destroyed after agentToolExecutionService_ (which holds a ref to
    // it), per the reversed-destruction-order rule. All three are declared
    // here, before productionWorkflowRunner, so their addresses can be
    // passed into its constructor for workspaceAccess roster agents.
    application::FilesystemWorkspaceToolExecutor filesystemExecutor;
    // agentToolExecutionService holds refs to storage audit repos and
    // filesystemExecutor; it is declared (and therefore destroyed) before
    // both of them so the refs remain valid for its entire lifetime.
    application::AgentToolExecutionService agentToolExecutionService;
    // agentSessionFactory holds no refs into other Impl members; its
    // unique_ptr is destroyed in reverse declaration order (before
    // filesystemExecutor and agentToolExecutionService, which is fine since
    // it has no dependency on either).
    std::unique_ptr<providers::IProviderAgentSessionFactory> agentSessionFactory;
    application::MockWorkflowRunner workflowRunner;
    // Holds pointers to agentSessionFactory/agentToolExecutionService/
    // changeProposalService (all declared above) for workspaceAccess roster
    // agents; never owns them.
    application::ExecutionProfileWorkflowRunner productionWorkflowRunner;
    // Same "holds only refs/pointers, never owns" rule as
    // productionWorkflowRunner above; also references executionManifestService,
    // constructed near the top of this list, well before this point.
    application::ManifestWorkflowRunner manifestWorkflowRunner;
    // MW-P5.4: declared immediately after manifestWorkflowRunner above - see
    // the matching constructor init-list comment.
    application::ManifestVerificationContinuationRepairService
        manifestVerificationContinuationRepairService;
    // Declared after storage.projectMemoryRepository exists and before
    // runManager (which holds a raw ProjectMemoryService* best-effort
    // dependency - see RunManager.hpp): this guarantees projectMemoryService
    // is fully constructed before runManager's worker thread can ever call
    // into it, and is destroyed only after ~RunManager() has already joined
    // that worker. modelExecutionService (declared much earlier, at the top
    // of this list) is runManager's other raw-pointer best-effort dependency
    // (AG-10's reconcileRunExecutions() safety net) and is safe by the same
    // "declared before, destroyed after ~RunManager() joins" reasoning.
    application::ProjectMemoryService projectMemoryService;
    // AG-11: declared after storage and providerClientFactory (both already
    // constructed above) and after projectMemoryService, which it logically
    // extends. Unlike projectMemoryService/runManager, nothing else in this
    // class holds a pointer to it - it is only ever called synchronously
    // from an explicit operator-triggered UI action, never from runManager's
    // background worker - so its declaration position only has to satisfy
    // the ordinary "storage/providerClientFactory outlive it" rule.
    application::ProjectMemoryCompactionService projectMemoryCompactionService;
    application::RunManager runManager;
    application::ProjectService projectService;
    application::TaskService taskService;
    application::OrchestrationService orchestrationService;
    // AG-18.3 hardening (blocker 4): declared after orchestrationService -
    // its own constructor takes a reference to it, and real member
    // construction always follows declaration order regardless of the
    // constructor init-list order.
    application::VerificationBootstrapRetryService verificationBootstrapRetryService;
    std::optional<common::Error> recoveryError;
    std::optional<common::Error> modelExecutionRecoveryError;
    std::optional<common::Error> comparisonRecoveryError;
    std::optional<common::Error> agentAuditRecoveryError;

    // PC-4C: the bounded history pass of the Outcome Brief reconciler runs on a worker thread so that it never delays the caller
    // (the UI thread). It is started explicitly, once, and stopped and joined first when the runtime goes away.
    void startOutcomeBriefBackfill()
    {
        if (ownership.guest) return;   // only the owner of the database writes briefs
        std::lock_guard lock(outcomeBriefBackfillMutex);
        if (outcomeBriefBackfillStarted) return;
        outcomeBriefBackfillStarted = true;
        outcomeBriefBackfillThread = std::jthread([this](std::stop_token stop) {
            application::OutcomeBriefHistoryOptions options;
            options.shouldStop = [&stop] { return stop.stop_requested(); };
            std::optional<application::OutcomeBriefHistoryResult> result;
            try {
                auto pass = outcomeBriefReconciler.reconcileHistory(options);
                if (pass.success()) result = std::move(pass.value());
            } catch (...) {
                // A background pass never takes the process down; it simply has no result.
            }
            {
                std::lock_guard lock(outcomeBriefBackfillMutex);
                outcomeBriefBackfillResult = std::move(result);
                outcomeBriefBackfillFinished = true;
            }
            outcomeBriefBackfillDone.notify_all();
        });
    }

    std::optional<application::OutcomeBriefHistoryResult> waitForOutcomeBriefBackfill()
    {
        std::unique_lock lock(outcomeBriefBackfillMutex);
        if (!outcomeBriefBackfillStarted) return std::nullopt;
        outcomeBriefBackfillDone.wait(lock, [this] { return outcomeBriefBackfillFinished; });
        return outcomeBriefBackfillResult;
    }

    std::mutex outcomeBriefBackfillMutex;
    std::condition_variable outcomeBriefBackfillDone;
    bool outcomeBriefBackfillStarted {false};
    bool outcomeBriefBackfillFinished {false};
    std::optional<application::OutcomeBriefHistoryResult> outcomeBriefBackfillResult;
    // LAST member: destroyed first, so its stop is requested and it is joined before anything it reads goes away.
    std::jthread outcomeBriefBackfillThread;
};

BackendRuntime::BackendRuntime(BackendRuntimeConfig config)
    : impl_(std::make_unique<Impl>(config))
{
}

// Defined where Impl is a complete type: unique_ptr<Impl>'s destructor needs
// Impl's full definition, so this cannot be defaulted in the header.
BackendRuntime::~BackendRuntime() = default;

application::ProjectService& BackendRuntime::projectService() noexcept
{
    return impl_->projectService;
}

application::TaskService& BackendRuntime::taskService() noexcept
{
    return impl_->taskService;
}

application::OrchestrationService& BackendRuntime::orchestrationService() noexcept
{
    return impl_->orchestrationService;
}

application::VerificationBootstrapRetryService&
BackendRuntime::verificationBootstrapRetryService() noexcept
{
    return impl_->verificationBootstrapRetryService;
}

application::ManifestVerificationContinuationPreparationService&
BackendRuntime::manifestVerificationContinuationPreparationService() noexcept
{
    return impl_->manifestVerificationContinuationPreparationService;
}

application::ManifestVerificationContinuationReadModelService&
BackendRuntime::manifestVerificationContinuationReadModelService() noexcept
{
    return impl_->manifestVerificationContinuationReadModelService;
}

application::ManifestVerificationContinuationRepairService&
BackendRuntime::manifestVerificationContinuationRepairService() noexcept
{
    return impl_->manifestVerificationContinuationRepairService;
}

application::ExecutionManifestService&
BackendRuntime::executionManifestService() noexcept
{
    return impl_->executionManifestService;
}

application::ExecutionManifestReadModelService&
BackendRuntime::manifestReadModelService() noexcept
{
    return impl_->manifestReadModelService;
}

application::ArtifactService& BackendRuntime::artifactService() noexcept
{
    return impl_->artifactService;
}

application::VerificationScratchService& BackendRuntime::verificationScratchService() noexcept
{
    return impl_->verificationScratchService;
}

application::ProjectReadinessService& BackendRuntime::projectReadinessService() noexcept
{
    return impl_->projectReadinessService;
}

application::WorkspaceContextService& BackendRuntime::workspaceContextService() noexcept
{
    return impl_->workspaceContextService;
}

application::ProviderCatalogService& BackendRuntime::providerCatalogService() noexcept
{
    return impl_->providerCatalogService;
}

application::ExecutionProfileService& BackendRuntime::executionProfileService() noexcept
{
    return impl_->executionProfileService;
}

application::RunDraftService& BackendRuntime::runDraftService() noexcept
{
    return impl_->runDraftService;
}

application::RunDraftPlanService& BackendRuntime::runDraftPlanService() noexcept
{
    return impl_->runDraftPlanService;
}

application::FollowUpPlanService& BackendRuntime::followUpPlanService() noexcept
{
    return impl_->followUpPlanService;
}

application::FollowUpPlanReadModel& BackendRuntime::followUpPlanReadModel() noexcept
{
    return impl_->followUpPlanReadModel;
}

application::ApplicationSettingsService& BackendRuntime::applicationSettingsService() noexcept
{
    return impl_->applicationSettingsService;
}

application::ICredentialStore& BackendRuntime::credentialStore() noexcept
{
    return *impl_->credentialStore;
}

application::CredentialManagementService& BackendRuntime::credentialManagementService() noexcept
{
    return impl_->credentialManagementService;
}

application::ChatService& BackendRuntime::chatService() noexcept
{
    return impl_->chatService;
}

application::ModelExecutionService& BackendRuntime::modelExecutionService() noexcept
{
    return impl_->modelExecutionService;
}

application::ExecutionAnalyticsService& BackendRuntime::executionAnalyticsService() noexcept
{
    return impl_->executionAnalyticsService;
}

application::ProjectMemoryService& BackendRuntime::projectMemoryService() noexcept
{
    return impl_->projectMemoryService;
}

application::ProjectMemoryCompactionService& BackendRuntime::projectMemoryCompactionService() noexcept
{
    return impl_->projectMemoryCompactionService;
}

application::ComparisonService& BackendRuntime::comparisonService() noexcept
{
    return impl_->comparisonService;
}

application::ComparisonReadModelService& BackendRuntime::comparisonReadModelService() noexcept
{
    return impl_->comparisonReadModelService;
}

application::ComparisonExecutionManager& BackendRuntime::comparisonExecutionManager() noexcept
{
    return impl_->comparisonExecutionManager;
}

bool BackendRuntime::storageAvailable() const noexcept
{
    return impl_->storage.database != nullptr;
}

const std::optional<common::Error>& BackendRuntime::storageError() const noexcept
{
    return impl_->storage.error;
}

const std::optional<common::Error>& BackendRuntime::recoveryError() const noexcept
{
    return impl_->recoveryError;
}

const std::optional<common::Error>& BackendRuntime::modelExecutionRecoveryError() const noexcept
{
    return impl_->modelExecutionRecoveryError;
}

std::optional<common::Error> BackendRuntime::followUpComposerError() const
{
    std::lock_guard lock(impl_->followUpComposerErrorMutex);
    return impl_->followUpComposerError;
}

const std::optional<common::Error>& BackendRuntime::comparisonRecoveryError() const noexcept
{
    return impl_->comparisonRecoveryError;
}

application::ChangeProposalService& BackendRuntime::changeProposalService() noexcept
{
    return impl_->changeProposalService;
}

application::ChangeSetApplyService& BackendRuntime::changeSetApplyService() noexcept
{
    return impl_->changeSetApplyService;
}

application::ProposalReviewService& BackendRuntime::proposalReviewService() noexcept
{
    return impl_->proposalReviewService;
}

application::ManifestApplyVerificationGate& BackendRuntime::manifestApplyVerificationGate() noexcept
{
    return impl_->manifestApplyVerificationGate;
}

application::ManifestVerificationManualHandoffService&
BackendRuntime::manifestVerificationManualHandoffService() noexcept
{
    return impl_->manifestVerificationManualHandoffService;
}

application::ManifestVerificationManualHandoffReadModelService&
BackendRuntime::manifestVerificationManualHandoffReadModelService() noexcept
{
    return impl_->manifestVerificationManualHandoffReadModelService;
}

application::ProjectContextSnapshotService& BackendRuntime::projectContextSnapshotService() noexcept
{
    return impl_->projectContextSnapshotService;
}

application::OutcomeBriefReconciler& BackendRuntime::outcomeBriefReconciler() noexcept
{
    return impl_->outcomeBriefReconciler;
}

const application::IOutcomeBriefRepository& BackendRuntime::outcomeBriefRepository() const noexcept
{
    return *impl_->storage.outcomeBriefRepository;
}

const application::OutcomeBriefReadModelService& BackendRuntime::outcomeBriefReadModelService() const noexcept
{
    return impl_->outcomeBriefReadModelService;
}

void BackendRuntime::startOutcomeBriefBackfill()
{
    impl_->startOutcomeBriefBackfill();
}

std::optional<application::OutcomeBriefHistoryResult> BackendRuntime::waitForOutcomeBriefBackfill()
{
    return impl_->waitForOutcomeBriefBackfill();
}

application::IAgentToolSessionRepository& BackendRuntime::agentToolSessionRepository() noexcept
{
    return *impl_->storage.agentToolSessionRepository;
}

application::IAgentToolAuditRepository& BackendRuntime::agentToolAuditRepository() noexcept
{
    return *impl_->storage.agentToolAuditRepository;
}

bool BackendRuntime::agentAuditStorageAvailable() const noexcept
{
    // Audit storage is available only when the main SQLite database opened
    // successfully. If storage is unavailable, every audit repository call
    // returns "storage.unavailable", which is the fail-closed signal a tool
    // executor must treat as a hard refusal to start.
    return impl_->storage.database != nullptr;
}

const std::optional<common::Error>& BackendRuntime::agentAuditRecoveryError() const noexcept
{
    return impl_->agentAuditRecoveryError;
}

application::AgentToolExecutionService& BackendRuntime::agentToolExecutionService() noexcept
{
    return impl_->agentToolExecutionService;
}

bool BackendRuntime::agentToolExecutionAvailable() const noexcept
{
    return impl_->agentToolExecutionService.storageAvailable();
}

providers::IProviderAgentSessionFactory& BackendRuntime::agentSessionFactory() noexcept
{
    return *impl_->agentSessionFactory;
}

application::AgentToolReadModelService& BackendRuntime::agentToolReadModelService() noexcept
{
    return impl_->agentToolReadModelService;
}

} // namespace ai::backend
