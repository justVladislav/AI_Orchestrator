#pragma once

#include "ai/application/AgentToolExecutionService.hpp"
#include "ai/application/AgentToolReadModelService.hpp"
#include "ai/application/IAgentToolAuditRepository.hpp"
#include "ai/application/IAgentToolSessionRepository.hpp"
#include "ai/application/artifacts/ArtifactService.hpp"
#include "ai/application/ProjectReadinessService.hpp"
#include "ai/application/VerificationScratchService.hpp"
#include "ai/providers/IProviderAgentSession.hpp"
#include "ai/application/settings/ApplicationSettingsService.hpp"
#include "ai/application/CredentialManagementService.hpp"
#include "ai/application/ChatService.hpp"
#include "ai/application/comparison/ComparisonService.hpp"
#include "ai/application/comparison/ComparisonReadModelService.hpp"
#include "ai/application/comparison/ComparisonExecutionManager.hpp"
#include "ai/application/ExecutionManifestReadModelService.hpp"
#include "ai/application/ExecutionManifestService.hpp"
#include "ai/application/catalog/ExecutionProfileService.hpp"
#include "ai/application/FollowUpPlanReadModel.hpp"
#include "ai/application/FollowUpPlanService.hpp"
#include "ai/application/RunDraftPlanService.hpp"
#include "ai/application/RunDraftService.hpp"
#include "ai/application/ExecutionAnalyticsService.hpp"
#include "ai/application/ICredentialStore.hpp"
#include "ai/application/OrchestrationService.hpp"
#include "ai/application/VerificationBootstrapRetryService.hpp"
#include "ai/application/MockWorkflowRunner.hpp"
#include "ai/application/ModelExecutionService.hpp"
#include "ai/application/ChangeProposalService.hpp"
#include "ai/application/ChangeSetApplyService.hpp"
#include "ai/application/ManifestApplyVerificationGate.hpp"
#include "ai/application/ManifestVerificationContinuationPreparationService.hpp"
#include "ai/application/ManifestVerificationContinuationReadModelService.hpp"
#include "ai/application/ManifestVerificationContinuationRepairService.hpp"
#include "ai/application/ManifestVerificationManualHandoffService.hpp"
#include "ai/application/ManifestVerificationManualHandoffReadModelService.hpp"
#include "ai/application/ProposalReviewService.hpp"
#include "ai/application/ProjectMemoryService.hpp"
#include "ai/application/ProjectMemoryCompactionService.hpp"
#include "ai/application/OutcomeBriefReadModelService.hpp"
#include "ai/application/OutcomeBriefReconciler.hpp"
#include "ai/application/ProjectContextSnapshotService.hpp"
#include "ai/application/projects/ProjectService.hpp"
#include "ai/application/projects/TaskService.hpp"
#include "ai/application/catalog/ProviderCatalogService.hpp"
#include "ai/application/WorkspaceContextService.hpp"
#include "ai/common/Error.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>

namespace ai::backend {

/**
 * Configuration for one BackendRuntime instance.
 *
 * Only standard C++ and provider-neutral types are allowed here: no Qt
 * types, no sqlite3 types, no concrete persistence types. The database
 * engine behind BackendRuntime is an implementation detail selected inside
 * BackendRuntime.cpp (currently always SQLite); adding another engine later
 * changes that .cpp, not this struct or the Application Layer.
 */
struct BackendRuntimeConfig {
    /// Path to the SQLite database file. Its parent directory is created if
    /// missing. Callers that need an isolated database (tests) pass a
    /// dedicated temporary path; callers that need the real per-user
    /// database resolve one before constructing this config.
    std::filesystem::path databasePath;

    /// Deterministic Mock workflow tuning. The defaults match the values the
    /// desktop composition has always used; tests may pass smaller/larger
    /// values to make a run finish faster or stay observably active longer.
    std::chrono::milliseconds mockStepDelay {std::chrono::milliseconds {20}};
    std::size_t mockProgressSteps {4};

    /// Root directory for stored artifact contents. When empty (the
    /// default), it is derived as databasePath.parent_path()/"artifacts" so
    /// most callers never have to think about it; tests that need an
    /// isolated artifact tree may still set an explicit path. Declared last
    /// so existing positional aggregate-init call sites
    /// (BackendRuntimeConfig{path, stepDelay, steps}) keep compiling.
    std::filesystem::path artifactRoot;

    /// Maximum number of provider candidates executed concurrently inside
    /// one Comparison session. Sessions themselves remain durably queued.
    std::size_t maxComparisonConcurrency {4};

    /// When false (the default) the agent tool execution subsystem is
    /// disabled: AgentToolExecutionService::storageAvailable() returns false
    /// and every session/tool-call operation returns
    /// "agent_tool.storage_unavailable". The UI does not receive direct
    /// filesystem access until this flag is explicitly set to true by the
    /// composition root that has verified the workspace is ready. Set to
    /// true only in BackendRuntime tests and future production compositions
    /// that have passed the full workspace-security audit.
    bool enableAgentToolExecution {false};

    /// PC-4C: a runtime is the ONE live owner of its database file. The default (true) takes the ownership lock before the file is
    /// opened (ai::infrastructure::DatabaseOwnerLock) - another instance of the application, or another runtime of this process, over
    /// the same file then gets unavailable storage with the reason instead of racing this one - and makes this runtime the only
    /// writer of the Outcome Briefs of that file. Every composition root uses the default.
    /// FOR TEST FIXTURES ONLY: false makes this runtime a GUEST of a database that another runtime of the same process already
    /// owns - the pattern of a fixture that seeds the database through a short-lived second runtime while the first is alive. A guest
    /// takes no lock and, not being the owner, never writes a brief: none of its producers is wired to the Outcome Brief reconciler,
    /// its history pass does not run, and the reconciler itself refuses every explicit request (reconcileRun(), reconcileHistory(),
    /// the nudges) with `outcome_brief.not_the_owner` before it reads a source. Declared last so that positional aggregate-init
    /// call sites keep compiling.
    bool exclusiveDatabaseOwnership {true};

    /// TEST-ONLY: a barrier the Mock workflow calls before every step (ai::application::IMockWorkflowGate), so that a test can hold a
    /// Mock Run at a known step instead of racing its clock. Empty (the default) changes nothing. No production composition root sets it; the
    /// only way to reach it from the Qt side is the explicit test-only BackendBridge(path, gate, parent) constructor. Declared last so that
    /// positional aggregate-init call sites keep compiling.
    std::shared_ptr<ai::application::IMockWorkflowGate> mockWorkflowGate;
};

/**
 * Backend-owned composition root over persistence, repositories, the Mock
 * workflow runner, RunManager, and the Application Layer services built on
 * top of them.
 *
 * BackendRuntime is the intended replacement for ad hoc repository/service
 * wiring in any composition root (desktop Qt adapter, CLI, tests): it is the
 * one place that decides which concrete IProjectRepository/IRunRepository
 * implementation backs the Application Layer, so a future SQL Server or
 * PostgreSQL adapter only has to change inside BackendRuntime.cpp, never the
 * Application Layer or a caller's business logic.
 *
 * The public API intentionally exposes only ai::application and
 * ai::common types. It never exposes Qt types, sqlite3 handles, or the
 * concrete Sqlite* repository classes; construction failures inside the
 * database layer become a provider-neutral ai::common::Error read through
 * storageError()/recoveryError(), never an exception and never a silent,
 * working-but-non-persistent fallback.
 */
class BackendRuntime final {
public:
    explicit BackendRuntime(BackendRuntimeConfig config);
    ~BackendRuntime();

    // Composition roots have single, well-defined ownership; copying or
    // moving one would either duplicate a live RunManager worker thread and
    // a live sqlite3 connection, or leave a moved-from instance whose
    // service references still point at soon-to-be-destroyed repositories.
    // Neither is a state this type should ever be able to represent.
    BackendRuntime(const BackendRuntime&) = delete;
    BackendRuntime& operator=(const BackendRuntime&) = delete;
    BackendRuntime(BackendRuntime&&) = delete;
    BackendRuntime& operator=(BackendRuntime&&) = delete;

    /// Project commands/queries. Always usable, even when storageAvailable()
    /// is false: every operation then returns a "storage.unavailable" Result
    /// instead of the caller needing to null-check anything.
    [[nodiscard]] ai::application::ProjectService& projectService() noexcept;
    [[nodiscard]] ai::application::TaskService& taskService() noexcept;

    /// Run commands/queries and the owning RunManager. Same
    /// always-constructible, controlled-failure guarantee as projectService().
    [[nodiscard]] ai::application::OrchestrationService& orchestrationService() noexcept;

    /// AG-18.3: creates a new Run linked to a predecessor Run's durable
    /// ActionRequired verification-bootstrap disposition (see
    /// VerificationBootstrapRetryService). The only narrow surface a future
    /// Qt/QML "retry with manual setup" action should call - never the raw
    /// bootstrap repository, which this class never exposes.
    [[nodiscard]] ai::application::VerificationBootstrapRetryService&
    verificationBootstrapRetryService() noexcept;

    [[nodiscard]] ai::application::ManifestVerificationContinuationPreparationService&
    manifestVerificationContinuationPreparationService() noexcept;

    [[nodiscard]] ai::application::ManifestVerificationContinuationReadModelService&
    manifestVerificationContinuationReadModelService() noexcept;

    /// MW-P5.4: the only command boundary allowed to promote a Prepared
    /// continuation child Run (see manifestVerificationContinuationPreparationService()
    /// above) into real executable repair work. See
    /// ManifestVerificationContinuationRepairService.hpp's own doc comment.
    [[nodiscard]] ai::application::ManifestVerificationContinuationRepairService&
    manifestVerificationContinuationRepairService() noexcept;

    /// Validates and freezes the immutable one-per-Run execution contract.
    /// Phase 2 exposes no scheduling operation through this surface.
    [[nodiscard]] ai::application::ExecutionManifestService&
    executionManifestService() noexcept;

    /// UI-safe read model joining the frozen manifest (above) with live
    /// per-task execution progress. This is the only surface a Qt/QML
    /// adapter should read manifest/task state through - never
    /// executionManifestService()/the raw repositories directly.
    [[nodiscard]] ai::application::ExecutionManifestReadModelService&
    manifestReadModelService() noexcept;

    /// Artifact read models and creation. Same always-constructible,
    /// controlled-failure guarantee: every operation returns a
    /// "storage.unavailable" Result when either the artifact metadata
    /// database or the artifact filesystem root could not be opened.
    [[nodiscard]] ai::application::ArtifactService& artifactService() noexcept;

    /// The user's choice of the folder for the TEMPORARY verification copies, validated by the Backend, and the one provider every
    /// verification asks for its root. Same controlled-failure guarantee: without a database it refuses with "storage.unavailable".
    [[nodiscard]] ai::application::VerificationScratchService& verificationScratchService() noexcept;

    /// The durable, machine-local readiness of each project's verification setup (is it configured and buildable here?), reused
    /// across Runs and restarts. Without a database every check ends with a typed "storage.unavailable" error.
    [[nodiscard]] ai::application::ProjectReadinessService& projectReadinessService() noexcept;

    /// Owns the single globally-active-project value the desktop UI's
    /// future "Continue" action will set. Same always-constructible,
    /// controlled-failure guarantee as the services above.
    [[nodiscard]] ai::application::WorkspaceContextService& workspaceContextService() noexcept;

    /// Provider/Model catalog commands and queries. Same
    /// always-constructible, controlled-failure guarantee as the services
    /// above.
    [[nodiscard]] ai::application::ProviderCatalogService& providerCatalogService() noexcept;

    /// Execution profile commands and queries. Not yet consumed by
    /// RunManager/OrchestrationService - see CODEX_CONTEXT.md for the
    /// planned IWorkflowRunner integration step. Same always-constructible,
    /// controlled-failure guarantee as the services above.
    [[nodiscard]] ai::application::ExecutionProfileService& executionProfileService() noexcept;
    [[nodiscard]] ai::application::RunDraftService& runDraftService() noexcept;
    /// PC-3C: next-run-plan preview/confirm -> Run Draft groups. Provider-free: holds only the Draft, Project and
    /// Chat repositories and a read-only "project of this Run" lookup, so it cannot start a Run, write a Run or call
    /// a model.
    [[nodiscard]] ai::application::RunDraftPlanService& runDraftPlanService() noexcept;

    /// PC-5C-2b: the owner-only follow-up composer command/read surface. It
    /// quotes, authorises, previews and atomically confirms Drafts; it never
    /// starts a Run. A guest runtime refuses every entry with
    /// `follow_up.not_the_owner` before reading or writing.
    [[nodiscard]] ai::application::FollowUpPlanService& followUpPlanService() noexcept;

    /// PC-5D: the read-only side of the follow-up dialog and of the Outcome block: the gate per Run, the model picker, the
    /// roster templates and the source-Run choices. It cannot write or call a provider; a guest runtime refuses every entry with
    /// `follow_up.not_the_owner`.
    [[nodiscard]] ai::application::FollowUpPlanReadModel& followUpPlanReadModel() noexcept;

    /// Typed, durable application preferences. Credentials and destructive
    /// commands are deliberately outside this aggregate.
    [[nodiscard]] ai::application::ApplicationSettingsService& applicationSettingsService() noexcept;

    /// Provider-neutral secret storage boundary (see ICredentialStore.hpp).
    /// Always non-null. Windows uses Credential Manager and macOS uses
    /// Keychain when their platform adapters are enabled (the default on
    /// each platform); unsupported/configured-out builds use a controlled-
    /// unavailable stand-in whose store()/load()/remove() calls return
    /// "credential_store.unavailable".
    [[nodiscard]] ai::application::ICredentialStore& credentialStore() noexcept;

    /// The only command surface a future Qt credential adapter should use
    /// to configure/remove/query a provider's secret - never reads a secret
    /// back out. Same always-constructible guarantee as the services above.
    [[nodiscard]] ai::application::CredentialManagementService& credentialManagementService() noexcept;

    [[nodiscard]] ai::application::ChatService& chatService() noexcept;

    /// Shared per-model execution history used by Runs, Comparison and
    /// Chats. Lifecycle changes go through this service so immutable request
    /// snapshots cannot be rewritten by UI adapters.
    [[nodiscard]] ai::application::ModelExecutionService& modelExecutionService() noexcept;

    /// Read-only aggregate projection over the same model-execution ledger.
    /// UI consumers use it for project/source totals and provider breakdowns.
    [[nodiscard]] ai::application::ExecutionAnalyticsService& executionAnalyticsService() noexcept;

    /// Deterministic, LLM-free project memory writer (Task B of
    /// PROJECT_MEMORY_PLAN.md). RunManager already calls this best-effort on
    /// every run that reaches RunStatus::Completed; exposed here mainly for
    /// tests and any future read-only UI surface (Task E), same
    /// always-constructible, controlled-failure guarantee as the services
    /// above.
    [[nodiscard]] ai::application::ProjectMemoryService& projectMemoryService() noexcept;

    /// AG-11: provider-driven compaction of a project's accumulated memory
    /// segments into one consolidated segment. Unlike projectMemoryService()
    /// above, this makes a real provider call and is invoked only by explicit
    /// operator action, never automatically. Same always-constructible,
    /// controlled-failure guarantee as the services above.
    [[nodiscard]] ai::application::ProjectMemoryCompactionService& projectMemoryCompactionService() noexcept;

    /// Stage 1 safe change-proposal pipeline: parse, validate, secret-scan,
    /// snapshot originals, and atomically publish proposals without ever
    /// applying them to the working tree. Same always-constructible,
    /// controlled-failure guarantee as the services above.
    [[nodiscard]] ai::application::ChangeProposalService& changeProposalService() noexcept;

    /// Stage 8 safe apply pipeline: applies a ReadyForReview proposal to
    /// the real workspace with hash verification and rollback on failure.
    [[nodiscard]] ai::application::ChangeSetApplyService& changeSetApplyService() noexcept;

    /// AG-07: durable human reject/discard decision for a ReadyForReview
    /// proposal, kept separate from validator truth and from the positive
    /// approve/apply audit trail above.
    [[nodiscard]] ai::application::ProposalReviewService& proposalReviewService() noexcept;

    /// AG-16.1/AG-16.2: the single, read-only, authoritative decision for
    /// whether an ordinary Approve/Apply may proceed for a change set -
    /// exactly the same evaluator changeSetApplyService() itself re-checks
    /// immediately before every workspace mutation. Exposed here so a Qt/QML
    /// read model can project the same eligibility the Backend will actually
    /// enforce, without duplicating any proposal-set/hash/lineage/verification
    /// logic in a Controller or QML - see ManifestApplyVerificationGate.hpp.
    [[nodiscard]] ai::application::ManifestApplyVerificationGate&
    manifestApplyVerificationGate() noexcept;

    /// QML-P4R.5: the only command boundary for the manual-handoff feature -
    /// materialize() (Create), importAndReverify() (Reverify), discardHandoff()
    /// (Discard), enforceRetention(), and the read-only probeCandidateHealth()/
    /// resolveCandidateRoot() helpers a Qt adapter needs for "Open candidate"/
    /// "Copy path". A Qt/QML caller reads state through
    /// manifestVerificationManualHandoffReadModelService() below instead of
    /// this service's own accessors wherever both exist.
    [[nodiscard]] ai::application::ManifestVerificationManualHandoffService&
    manifestVerificationManualHandoffService() noexcept;

    /// QML-P4R.5: the single authoritative read model for the manual-handoff
    /// feature - origin Manifest Run, verification anchor, materialization
    /// state, candidate health, editable paths, attempt history, final
    /// verification, and Apply eligibility (via manifestApplyVerificationGate()
    /// above, never recomputed here). This is the only surface a Qt/QML
    /// adapter should read manual-handoff state through - never
    /// manifestVerificationManualHandoffService()'s own repository-facing
    /// internals, and never re-derived in Controller/QML code.
    [[nodiscard]] ai::application::ManifestVerificationManualHandoffReadModelService&
    manifestVerificationManualHandoffReadModelService() noexcept;

    /// PC-1: the read-only, provider-free builder of the Project Context Snapshot (authoritative loaders and
    /// the opt-in derived Project Memory). Synchronous and potentially slow (the workspace capture hashes the
    /// tree): a UI adapter must call build() off the UI thread and only on an explicit user action. It never
    /// writes and never calls a provider.
    [[nodiscard]] ai::application::ProjectContextSnapshotService& projectContextSnapshotService() noexcept;

    /// PC-4C: the provider-free Outcome Brief reconciler and the repository it writes (the only thing it writes). The
    /// producers (Run finish, reject / approve / apply, continuation, manual handoff) already nudge it, best effort;
    /// reconcileRun() is the on-demand pull and the source of truth. It never changes a Run, a proposal, an Apply or the
    /// Work Ledger and never calls a provider.
    ///
    /// The reconciler is the ONLY writer of the briefs, and that is held by types and by the lock, not by promise: the repository
    /// is handed out as a const view (reads only), and the runtime is the one live owner of its database file - a second runtime
    /// or application instance over the same file gets unavailable storage (storageAvailable() false, storageError() code
    /// `persistence.owned_by_another_instance`) instead of a second reconciler racing this one.
    [[nodiscard]] ai::application::OutcomeBriefReconciler& outcomeBriefReconciler() noexcept;
    [[nodiscard]] const ai::application::IOutcomeBriefRepository& outcomeBriefRepository() const noexcept;

    /// PC-4D: the read-only projection of the Outcome Brief of one Run, for the `Outcome` block of Run Details. It reads through const
    /// views of the Run and brief repositories, so it can neither write nor reconcile; a guest of a database (see
    /// BackendRuntimeConfig::exclusiveDatabaseOwnership) reads like the owner.
    [[nodiscard]] const ai::application::OutcomeBriefReadModelService& outcomeBriefReadModelService() const noexcept;

    /// PC-4C: starts the bounded history pass (the newest 200 outcome Runs of every project, plus every older one whose outcome
    /// is not settled) on a worker thread, once; further calls do nothing. It runs after the startup recovery that the
    /// constructor already completed, never delays the caller, and is stopped and joined when the runtime is destroyed.
    void startOutcomeBriefBackfill();
    /// Blocks until the pass started above has finished and returns its counts; empty if it was never started or could not read the
    /// Runs. (A pass that the destructor stops has nobody left to ask.)
    [[nodiscard]] std::optional<ai::application::OutcomeBriefHistoryResult> waitForOutcomeBriefBackfill();

    /// Durable Comparison sessions whose candidate attempts and evaluations
    /// live in the shared ModelExecution ledger.
    [[nodiscard]] ai::application::ComparisonService& comparisonService() noexcept;

    /// Authoritative Comparison projection consumed by desktop adapters.
    /// It joins sessions, shared-ledger executions, provider/model labels and
    /// quality evaluations without moving aggregation logic into Qt/QML.
    [[nodiscard]] ai::application::ComparisonReadModelService& comparisonReadModelService() noexcept;

    /// Asynchronous, bounded Comparison provider executor. This is the only
    /// runtime surface allowed to advance pending sessions and candidates.
    [[nodiscard]] ai::application::ComparisonExecutionManager& comparisonExecutionManager() noexcept;

    /// False if the database could not be opened or migrated at construction
    /// time. See storageError() for why.
    [[nodiscard]] bool storageAvailable() const noexcept;

    /// Set only when storageAvailable() is false; the reason the database
    /// backing this runtime is unavailable.
    [[nodiscard]] const std::optional<ai::common::Error>& storageError() const noexcept;

    /// Set when startup recovery of interrupted Runs or durable Manifest Work
    /// Ledger entries/transitions failed or remained incomplete. Multiple
    /// recovery failures are combined in the message rather than hidden.
    /// storageAvailable() can still be true here: recovery is a follow-up
    /// step against an already-open database, not a precondition for it.
    [[nodiscard]] const std::optional<ai::common::Error>& recoveryError() const noexcept;

    /// Set only if unfinished per-model work could not be recovered during
    /// startup. Independent from run aggregate recoveryError().
    [[nodiscard]] const std::optional<ai::common::Error>& modelExecutionRecoveryError() const noexcept;

    /// Most recent visible failure of the follow-up startup pass or its
    /// owner worker. Returned by value because the worker updates it under a
    /// mutex while the runtime is alive.
    [[nodiscard]] std::optional<ai::common::Error> followUpComposerError() const;

    [[nodiscard]] const std::optional<ai::common::Error>& comparisonRecoveryError() const noexcept;

    /// Agent tool session/turn persistence — one repository per agent run.
    /// Returns an unavailable adapter when storageAvailable() is false.
    [[nodiscard]] ai::application::IAgentToolSessionRepository& agentToolSessionRepository() noexcept;

    /// Per-call audit records within an agent tool session.
    /// Returns an unavailable adapter when storageAvailable() is false.
    [[nodiscard]] ai::application::IAgentToolAuditRepository& agentToolAuditRepository() noexcept;

    /// True only if the database is open and the audit tables were migrated.
    /// Tool executors must check this and refuse to start if it returns false.
    [[nodiscard]] bool agentAuditStorageAvailable() const noexcept;

    /// Set only if the startup recovery pass over active sessions and
    /// running/requested tool calls failed. storageAvailable() can still be
    /// true: recovery is a follow-up step on an already-open database.
    [[nodiscard]] const std::optional<ai::common::Error>& agentAuditRecoveryError() const noexcept;

    /// Audited, fail-closed agent tool execution service. Always non-null.
    /// storageAvailable() returns false when enableAgentToolExecution is
    /// false or when the database could not be opened.
    [[nodiscard]] ai::application::AgentToolExecutionService& agentToolExecutionService() noexcept;

    /// True iff enableAgentToolExecution was set in the config AND the
    /// database opened successfully. Equivalent to
    /// agentToolExecutionService().storageAvailable().
    [[nodiscard]] bool agentToolExecutionAvailable() const noexcept;

    /// Factory for provider-specific multi-turn agent sessions.
    /// Returns an always-failing unavailable adapter when
    /// enableAgentToolExecution is false or when no real provider adapter
    /// is linked in this build (AI_ORCHESTRATOR_HAVE_REAL_PROVIDERS not set).
    [[nodiscard]] ai::providers::IProviderAgentSessionFactory& agentSessionFactory() noexcept;

    /// UI-safe, denormalized read model over agent tool sessions, their
    /// provider turns, audited tool calls, and change proposals. This is the
    /// only surface a Qt/QML adapter should read agent-session state
    /// through - never the raw repositories/AgentToolExecutionService
    /// directly. See AgentToolReadModelService.hpp for the safety guarantee
    /// (no absolute paths, raw tool arguments, secrets, or filesystem
    /// handles ever appear in anything it returns).
    [[nodiscard]] ai::application::AgentToolReadModelService& agentToolReadModelService() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ai::backend
