#pragma once

#include "ai/application/agents/AgentToolExecutionService.hpp"
#include "ai/application/ExecutionManifestService.hpp"
#include "ai/application/ManifestRouting.hpp"
#include "ai/application/ProviderExecutionBudget.hpp"
#include "ai/application/VerificationEnvironmentSnapshotTypes.hpp"
#include "ai/application/workspace/WorkspaceBaselineProvenance.hpp"
#include "ai/common/Result.hpp"
#include "ai/models/TokenUsage.hpp"
#include "ai/providers/IProviderAgentSession.hpp"

#include <optional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

inline constexpr const char* kSubmitExecutionManifestToolName =
    "submit_execution_manifest";

// Terminal alternative to kSubmitExecutionManifestToolName: declares that the
// current Run task's acceptance criteria are already satisfied, so no
// Execution Manifest should be produced or frozen. Requires bounded,
// inspected evidence; see PlannerNoChangesNeeded.
inline constexpr const char* kSubmitNoChangesNeededToolName =
    "submit_no_changes_needed";

// AG-18.3 hardening (blocker 2): a third terminal alternative, explicit and
// typed - declares that the immutable AG-18.1 snapshot has no usable
// verification profile and this Run cannot safely form a same-Run bootstrap
// either. The Backend accepts this disposition only when the Run's own
// verificationEnvironment snapshot genuinely reports SetupRequired (see
// ExecutionManifestPlanner::plan()'s evaluateSubmission) - never inferred
// from an ordinary Planner failure, and never a silent side effect of
// submitting an inspect-only Manifest or a no-op.
inline constexpr const char* kSubmitVerificationActionRequiredToolName =
    "submit_verification_action_required";

// AG-17.4: versions the roster Planner contract - the trusted roster prompt
// section, the assigneeMemberId tool-schema variant and the repair addendum. It
// participates in a roster Run's planning-input fingerprint (never a
// single-assignment one), so a changed roster contract is never an unchanged
// replay of an earlier roster planning failure. Bump it whenever any of those
// three changes.
inline constexpr std::string_view kPlannerRosterContractId = "manifest-planner-roster.v1";

inline constexpr std::string_view kPlannerNoChangesNeededSchemaVersion =
    "execution-manifest-no-changes.v1";
inline constexpr std::string_view kPlannerVerificationActionRequiredSchemaVersion =
    "verification-action-required.v1";

// AG-17.0: the one Backend classification of a submission failure that a
// second provider turn can never fix. Every verification_policy.* code is a
// deterministic Backend decision over the frozen verification environment and
// the Manifest's own scope (ManifestMandatoryVerificationPolicy); spending the
// single bounded validation repair on it would only buy a second identical
// answer. Such a failure ends planning immediately: no repair session, no
// Manifest freeze, no worker dispatch, and the typed code is the Run's error.
[[nodiscard]] bool isNonRepairableSubmissionFailure(std::string_view errorCode) noexcept;

[[nodiscard]] AgentToolDefinition submitExecutionManifestToolDefinition();
[[nodiscard]] AgentToolDefinition submitNoChangesNeededToolDefinition();
[[nodiscard]] AgentToolDefinition submitVerificationActionRequiredToolDefinition();

// A Backend-produced VCS projection. The planner can reason about repository
// state without receiving a raw git command/tool or access to .git contents.
struct RepositoryInspectionProjection {
    std::string revision;
    std::vector<std::string> changedPaths;
    std::string diffSummary;
};

struct ExecutionManifestPlannerRequest {
    providers::ProviderAgentSessionRequest sessionRequest;
    RunId runId;
    std::string providerKind;
    WorkspaceReadPolicy inspectPolicy;
    ExecutionManifestPolicy trustedManifestPolicy;
    // AG-17.1: the Backend-owned routing record (ManifestRouting::fromAssignments
    // over the Run's frozen assignment snapshot). Like trustedManifestPolicy it is
    // never provider-authored: the Planner stamps it onto the compiled Manifest
    // before validation and freeze. An invalid record fails planning before any
    // provider session exists (planner.routing_invalid) - a Backend defect is never
    // sent to the paid repair. nullopt keeps the pre-AG-17.1 behaviour.
    std::optional<ManifestRoutingRecord> trustedRouting;
    // AG-17.4: what the Planner is told about each roster member (id, model
    // label, exact instructions), built by the Backend from the same frozen
    // assignment snapshot as trustedRouting and checked against it before any
    // provider session (planner.roster_catalog_invalid). Required exactly when the
    // routing record is a Roster; must be empty for SingleAssignment, whose
    // planning prompt, tool schema and behaviour stay unchanged. In a Roster Run
    // the Planner is shown only the assignable members, its tool schema requires
    // an assigneeMemberId drawn from them, and the validator judges every
    // assignee against the record.
    std::vector<ManifestRosterMemberInfo> rosterCatalog;
    // Captured before planning. When present, existing/new/read-role and
    // file-dependency findings participate in the Planner's bounded repair
    // before the accepted Manifest is frozen.
    std::optional<WorkspaceBaselineSnapshot> baselineSnapshot;
    std::optional<RepositoryInspectionProjection> repositoryProjection;
    // AG-18.2: the Run's own already-fetched, already-identity-validated
    // VerificationEnvironmentSnapshot (see
    // ManifestVerificationProfileBinder::validateSnapshotIdentity, called
    // by ManifestWorkflowRunner before the Planner session is created).
    // std::nullopt means no verification environment is available for this
    // Run (discovery not wired) - every environment-dependent verification
    // kind (cmake_build/qml_lint/cmake_target_consistency) then fails
    // binding; inspect_paths is unaffected either way. The exact same
    // snapshot is reused unchanged across the primary attempt and the one
    // bounded repair attempt - it is never rediscovered or silently
    // switched.
    std::optional<VerificationEnvironmentSnapshot> verificationEnvironment;
    bool allowEvidenceBackedNoChanges {false};
    // Shared with every worker session for this Run. Required by the
    // production Manifest workflow; null keeps direct legacy unit callers
    // source-compatible.
    std::shared_ptr<ProviderSessionBudgetGuard> providerBudgetGuard;
    // AG-17 gpt-6-sol checkpoint M2b: the long-context threshold of the model that PRICES this
    // planning session (the primary attempt and its bounded repair alike). Every provider response
    // is classified against it by accumulateResponseUsage(); it must come from the SAME immutable
    // Model copy that later supplies the prices. 0 (the default) is flat pricing. Never read by
    // the budget guard.
    std::uint64_t longContextThresholdInputTokens {0};
};

// What planning concluded with. Meaningful only when the outcome succeeded:
// a failed/cancelled outcome carries neither a manifest nor a no-op result,
// regardless of this field's default value.
enum class PlanningDisposition {
    ManifestReady,   // outcome.manifest is populated and frozen/persisted.
    NoChangesNeeded, // outcome.noChangesNeeded is populated; no manifest exists.
    // AG-18.3 hardening: outcome.verificationActionRequired is populated; no
    // manifest exists and no worker was ever considered. Reachable only via
    // an explicit submit_verification_action_required call the Backend
    // accepted (verificationEnvironment genuinely SetupRequired) - never a
    // fallback for an ordinary Planner/provider failure.
    VerificationActionRequired
};

// One inspected fact backing a NoChangesNeeded disposition: what acceptance
// criterion it addresses, the workspace-relative path that was inspected,
// and what was actually observed there. Never fabricated - see
// ExecutionManifestPlanner::plan()'s evidence-before-evaluation requirement.
struct PlannerNoChangesEvidenceItem {
    std::string criterion;
    std::string path;
    std::string observation;
};

// Typed terminal result for "the current Run task is already done; do not
// produce an Execution Manifest". Distinct from the worker-side
// ChangeSetState::NoChangesNeeded (ChangeProposalTypes.hpp), which describes
// an individual manifest task's proposal outcome, not a planning-level
// disposition - the two are never interchangeable.
struct PlannerNoChangesNeeded {
    std::string schemaVersion {std::string(kPlannerNoChangesNeededSchemaVersion)};
    std::string summary;
    std::vector<PlannerNoChangesEvidenceItem> evidence;
    // A suggestion for a human, never an instruction the Backend acts on:
    // the planner must not use it to silently execute the next catalog
    // Stage or otherwise chain further work.
    std::optional<std::string> recommendedNextStep;
};

// AG-14.1: typed round-trip for the manifest-planning-no-changes-identity.v1
// artifact ManifestWorkflowRunner::run() persists at the Planner no-op
// terminal point, so ManifestWorkLedgerService::recordTerminalRun (called
// later, correctly-timed, from RunManager::finishRun) can recover
// recommendedNextStep without needing this threaded through in memory.
[[nodiscard]] std::string renderPlannerNoChangesNeededJson(
    const PlannerNoChangesNeeded& noChangesNeeded);
[[nodiscard]] common::Result<PlannerNoChangesNeeded>
parsePlannerNoChangesNeededJson(std::string_view json);

// AG-18.3 hardening (blocker 2): the explicit, typed, bounded terminal
// payload for kSubmitVerificationActionRequiredToolName. Contains no raw
// path, command, provider payload, preset JSON, or secret - summary/reason
// are the Planner's own bounded prose explaining why no safe bootstrap
// could be formed, never Backend-authoritative diagnostics themselves.
struct PlannerVerificationActionRequired {
    std::string schemaVersion {std::string(kPlannerVerificationActionRequiredSchemaVersion)};
    std::string summary;
    std::string reason;
    std::optional<std::string> recommendedNextStep;
};

[[nodiscard]] std::string renderPlannerVerificationActionRequiredJson(
    const PlannerVerificationActionRequired& value);
[[nodiscard]] common::Result<PlannerVerificationActionRequired>
parsePlannerVerificationActionRequiredJson(std::string_view json);

struct ExecutionManifestPlannerOutcome {
    bool success {false};
    bool cancelled {false};
    std::string errorCode;
    std::string errorMessage;
    // The rejection code from the very first (primary) submission attempt,
    // set once and never overwritten by the repair attempt. Empty when the
    // primary attempt was not rejected (including outright success).
    std::string primaryRejectionCode;
    // The rejection code from the most recent evaluated submission: equal to
    // primaryRejectionCode when no repair ran or the repair itself failed
    // before producing a new typed rejection; otherwise the repair's own
    // rejection code.
    std::string lastRejectionCode;
    // Safe Backend validation findings from the first and most recent
    // rejected attempts. These remain available after a successful repair;
    // raw model submission JSON is deliberately never retained here.
    std::vector<ExecutionManifestFinding> primaryFindings;
    std::vector<ExecutionManifestFinding> lastFindings;
    // Audit lines for the Backend normalizations applied to the MOST RECENT evaluated
    // submission (replaced on every evaluation, so after a success they describe the accepted
    // Manifest). Today: a requestedMaxParallelism above the Run's trusted ceiling is lowered to it
    // instead of being rejected (Stage 9B Run cbe83247: the Planner is not told the ceiling and
    // guessed wrong in 4 of 6 roster Runs; each rejection cost a paid repair session and one
    // repair regressed), and a requirement id that repeats the id of an earlier task is renamed
    // (the verifier keys its criteria by id across the whole Manifest; Stage 10A-Repair Run
    // 8e22e38a). Never a way to raise a limit and never carries provider text.
    std::vector<std::string> normalizations;
    // Compatibility view of the most recent manifest-validation findings.
    // New diagnostics consumers should prefer primaryFindings/lastFindings.
    std::vector<ExecutionManifestFinding> findings;
    PlanningDisposition disposition {PlanningDisposition::ManifestReady};
    std::optional<ExecutionManifest> manifest;
    std::optional<PlannerNoChangesNeeded> noChangesNeeded;
    std::optional<PlannerVerificationActionRequired> verificationActionRequired;
    models::TokenUsage usage;
    // Number of additional fresh sessions actually requested. Bounded to at
    // most two: one terminalization recovery for missing terminal submission
    // and one validation repair for a rejected typed submission.
    std::size_t repairAttempts {0};
    std::size_t totalTurns {0};
    std::size_t totalToolCalls {0};
    std::size_t successfulInspectionCalls {0};
    std::size_t totalReturnedBytes {0};
};

// Runs the Lead Architect as an inspect-only agent. It advertises only the
// audited read-only registry plus submit_execution_manifest and
// submit_no_changes_needed, intercepts either terminal call locally,
// validates with independently bounded terminalization/validation recovery
// (at most two additional fresh sessions), and either freezes a valid manifest or
// returns a typed NoChangesNeeded result - without creating a
// ProposedChangeSet, persisting a manifest for a no-op, or starting workers.
class ExecutionManifestPlanner final {
public:
    ExecutionManifestPlanner(
        providers::IProviderAgentSessionFactory& sessionFactory,
        AgentToolExecutionService& toolExecution,
        ExecutionManifestService& manifests);

    [[nodiscard]] ExecutionManifestPlannerOutcome plan(
        ExecutionManifestPlannerRequest request,
        std::stop_token stopToken) const;

private:
    providers::IProviderAgentSessionFactory& sessionFactory_;
    AgentToolExecutionService& toolExecution_;
    ExecutionManifestService& manifests_;
};

} // namespace ai::application
