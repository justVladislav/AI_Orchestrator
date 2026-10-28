#pragma once

#include "ai/application/agents/AgentToolExecutionService.hpp"
#include "ai/application/ProviderExecutionBudget.hpp"
#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/proposals/ChangeProposalService.hpp"
#include "ai/application/execution/ExecutionAcceptanceContract.hpp"
#include "ai/application/execution/ManifestWorkerNoChanges.hpp"
#include "ai/application/execution/RunEvent.hpp"
#include "ai/application/execution/RunWorkspaceScopeTypes.hpp"
#include "ai/application/workspace/WorkspaceBaselineProvenance.hpp"
#include "ai/models/TokenUsage.hpp"
#include "ai/providers/IProviderAgentSession.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <vector>

namespace ai::application {

class RunWorkspaceScopeService;

// Shared multi-turn, read-only-workspace-tools provider session driver.
//
// Owns exactly: session creation (IProviderAgentSession via
// IProviderAgentSessionFactory), the tool-use turn loop (budget enforcement,
// mode-specific terminal interception, audited tool dispatch via
// AgentToolExecutionService::executeToolCall/recordTurn), the terminal
// AgentToolSession state update, one ChangeProposalService::createFromModelOutput
// proposal persistence when applicable, and — when the failure is a shape mistake the model itself can fix
// (see isModelFormatError below) — exactly one JSON-repair retry session.
//
// Does NOT own: prompt/project-memory assembly, Report-artifact creation, or
// Run/Stage lifecycle events beyond an optional mid-loop StageProgress event
// (eventSink may be null). Those stay caller-specific because different
// callers (a single-stage read-only-analysis run vs. one stage among many in
// a roster) have genuinely different stage semantics and must decide their
// own StageStarted/Completed/Failed/Cancelled events from the returned
// AgentToolTurnLoopOutcome.
//
// Extracted from ReadOnlyAnalysisWorkflowRunner (2026-07-26) so
// ExecutionProfileWorkflowRunner can give individual roster agents the same
// workspace-tool capability without duplicating this machinery.

// Default budgets for a single-agent tool-use turn loop: the model is
// expected to conclude with one final proposal.v1 submission, not spend
// unbounded turns exploring. Callers set these directly on
// ProviderAgentSessionRequest (the adapter enforces maxTurns/
// maxToolCallsPerTurn; the turn loop itself additionally re-checks
// maxTotalToolCalls as a defensive measure independent of the adapter).
// Exported so every caller wiring a workspace-access agent starts from the
// same defaults instead of redeclaring these numbers.
inline constexpr std::size_t kAgentToolTurnLoopMaxTurns = 12;
inline constexpr std::size_t kAgentToolTurnLoopMaxTotalToolCalls = 48;
// A model re-verifying several files at once (e.g. after a repair prompt
// asking it to re-check current filesystem state) can easily emit more than
// 8 read-only calls in one turn. Real-Run incident (Run
// a739e790-db80-4efd-a98f-5d91af2e5e86, 2026-09-14): an integration task
// that genuinely needs to read many newly-created plus several pre-existing
// files before editing legitimately batched more than 16 parallel reads into
// its very first turn; validateTurnResponse hard-failed the whole worker
// session before a single successful inspection. 32 gives real multi-file
// integration work headroom while staying comfortably under
// kAgentToolTurnLoopMaxTotalToolCalls (48) - a single turn can never consume
// more than the whole session's budget. This raises the common-case ceiling;
// AgentToolTurnLoopRequest::sessionRequest.disallowParallelToolCalls is the
// separate, structural guarantee a bounded retry uses so the same wall can
// never be hit again regardless of how large a future task's real read
// requirement is.
inline constexpr std::size_t kAgentToolTurnLoopMaxToolCallsPerTurn = 32;
inline constexpr std::uint32_t kAgentToolTurnLoopMaxOutputTokens = 4096;
inline constexpr std::chrono::milliseconds kAgentToolTurnLoopDefaultTurnTimeout {60'000};
// Incident (Run 38f03ba5-2f50-4ef1-82ea-469c213d352e, 2026-09-30): a worker Propose
// turn writes whole files as escaped JSON in one response; a 4096-token ceiling
// (the model record's max_output_tokens) cut a data file off before the propose
// tool call, twice. Worker turns get at least this ceiling (still capped by the
// Run budget) and a turn timeout long enough to actually generate it.
inline constexpr std::uint32_t kWorkerProposeMinOutputTokens = 16384;
inline constexpr std::chrono::milliseconds kWorkerProposeTurnTimeout {240'000};
// Upper bound for a retained rejected-attempt terminal text (audit only).
inline constexpr std::size_t kMaximumRetainedAttemptTextBytes = 64 * 1024;
// Stable code for a non-terminal tool call returned on a Backend-forced
// terminal-only turn. The call is never executed.
inline constexpr const char* kWorkerForcedTerminalViolationCode =
    "worker.forced_terminal_turn_violated";
// Stable code for a typed no-change submission (or an empty change set standing in for one) on a
// task whose Manifest allocation declares plannedCreatePaths: a file that does not exist yet can
// never be "already satisfied", so no evidence can make a no-op true. Rejected by the loop itself,
// whatever the adapter or a scripted session let through.
inline constexpr const char* kWorkerNoChangesPlannedCreateRequiredCode =
    "worker_no_changes.planned_create_required";

// Name of the local (not registered in AgentToolExecutionService's shared
// ToolRegistry) tool the model calls to submit its final proposal.v1 payload
// as structured tool-call arguments instead of a text-only JSON fence. It is
// scoped here rather than the shared registry because it is not a
// workspace/filesystem operation and does not belong in the audited
// read-only tool-dispatch path — the turn loop intercepts a call to it
// directly rather than routing it through AgentToolExecutionService::executeToolCall.
inline constexpr const char* kProposeChangeSetToolName = "propose_change_set";
inline constexpr const char* kSubmitTaskVerificationToolName =
    "submit_task_verification";
inline constexpr const char* kSubmitTaskNoChangesToolName =
    "submit_task_no_changes_needed";
// AG-15: a non-terminal tool - unlike the three above, a call to this one
// does not end the turn loop. Only registered on the request when the
// caller wires runWorkspaceScopeService (see AgentToolTurnLoopRequest
// below); intercepted the same way as a workspace tool call, but handled
// locally through RunWorkspaceScopeService::requestExpansion instead of
// AgentToolExecutionService::executeToolCall.
inline constexpr const char* kRequestScopeExpansionToolName =
    "request_scope_expansion";

// Verification-repair reliability hardening: a second non-terminal tool,
// handled the same local way as kRequestScopeExpansionToolName (never
// routed through AgentToolExecutionService::executeToolCall) but backed by
// RunWorkspaceScopeService::requestDiagnosticRead instead of
// requestExpansion. Only ever registered when the caller sets
// AgentToolTurnLoopRequest::diagnosticReadEligible - i.e. only for the ONE
// bounded combined-verification repair attempt (see
// ExecutionManifestExecutor::runOneTask's repairContext), never for an
// ordinary primary dispatch or the turn loop's own internal same-session
// JSON-repair retry. The handler independently re-checks
// diagnosticReadEligible regardless of what was advertised, so a session
// that somehow still calls this tool name outside a repair attempt is
// rejected rather than silently honoured.
inline constexpr const char* kRequestDiagnosticReadToolName =
    "request_diagnostic_read";

[[nodiscard]] AgentToolDefinition proposeChangeSetToolDefinition();
// Manifest workers have an immutable exact-file write allocation. Advertising
// those paths in the strict terminal schema prevents a provider from spending
// the bounded repair budget on changes the Backend must reject anyway. Empty
// input deliberately falls back to the shared unscoped contract for legacy
// callers whose policy is not an exact-file allow-list.
[[nodiscard]] AgentToolDefinition proposeChangeSetToolDefinition(
    const std::vector<std::filesystem::path>& allowedRelativePaths);
[[nodiscard]] AgentToolDefinition submitTaskVerificationToolDefinition();
[[nodiscard]] AgentToolDefinition submitTaskNoChangesToolDefinition();
[[nodiscard]] AgentToolDefinition requestScopeExpansionToolDefinition();
[[nodiscard]] AgentToolDefinition requestDiagnosticReadToolDefinition();

// The read-only-tools-plus-propose_change_set framing both callers build
// their ProviderAgentSessionRequest::systemPrompt from. A caller with
// additional role/instructions (e.g. a roster agent's assigned role) appends
// its own suffix after this base text rather than duplicating the prose.
extern const char* const kWorkspaceToolsSystemPrompt;

// Error codes ProposedChangeSetParser::parse (via ChangeProposalService's
// Step 1) can return - i.e. the model's raw text was not a well-formed
// proposal.v1 payload at all (stray prose around/instead of the fence, wrong
// shape, an oversized/malformed field). Deliberately excludes every other
// validation-stage code (secret findings, hash mismatches, policy
// rejections, proposal.cross_agent_conflict) - those mean the JSON parsed
// fine but its *content* was rejected against real repository/policy state
// or another agent's concurrent work, which re-prompting the model can
// never fix.
//
// proposal.missing_proposed_content and the file-existence precondition
// codes (proposal.create_target_exists/file_not_found/rename_target_exists)
// are the ChangeProposalValidator/ChangeProposalService invariants included
// here even though they are technically validation-stage codes: each is
// still just a shape/belief mistake the model can correct given its
// read-only tools (re-read the file listing, resubmit the right operation
// and full content), not an unfixable fact about policy or another agent.
[[nodiscard]] bool isModelFormatError(const std::string& code);

// One already-built provider session request (systemPrompt/userContent/tools/
// turnTimeout/maxOutputTokens/maxTurns/maxTotalToolCalls/maxToolCallsPerTurn/
// providerId/modelId/apiModelId all assembled by the caller - the caller
// decides the tools() list but MUST include proposeChangeSetToolDefinition()
// in it) plus everything needed to run it end to end.
enum class AgentToolTurnLoopMode { Propose, Inspect };

struct ManifestTaskVerificationEvidence {
    std::string requirementId;
    std::string inspectedPath;
    std::string observation;
    ExecutionVerificationStatus status {ExecutionVerificationStatus::NotRun};
};

struct ManifestTaskVerificationSubmission {
    std::string schemaVersion {"manifest-task-verification.v1"};
    std::string summary;
    std::vector<ManifestTaskVerificationEvidence> criterionEvidence;
    std::vector<std::string> inspectedRelativePaths;
};

struct AgentToolTurnLoopRequest {
    providers::ProviderAgentSessionRequest sessionRequest;
    RunId runId;
    // Needed only to create the repair session via
    // IProviderAgentSessionFactory::createSession(providerId, providerKind) -
    // not otherwise present on ProviderAgentSessionRequest.
    std::string providerKind;
    // Trusted policy for dispatching tool calls (AgentToolExecutionService::
    // buildPolicy(workspaceRoot), derived by the caller from Project::rootPath
    // - never from model input).
    WorkspaceReadPolicy policy;
    // Read tools may intentionally point at a materialized candidate overlay
    // (most importantly during the one combined-verification repair wave),
    // but Modify/Delete/Rename precondition hashes must always describe the
    // live baseline that a later human Apply will mutate.  Callers that set
    // this policy keep those two trust domains separate.  Direct/legacy
    // callers may leave it empty and retain the historical `policy`
    // behaviour.
    std::optional<WorkspaceReadPolicy> trustedHashPolicy;
    // Broad trusted project policy used only to validate an explicit
    // request_scope_expansion. `policy` above is the task-exact effective
    // allow-list. Keeping the two separate prevents an expansion from
    // being rejected merely because the requested path is not active yet,
    // without ever discarding project-level deny rules.
    WorkspaceReadPolicy scopeExpansionPolicy;
    AgentToolTurnLoopMode mode {AgentToolTurnLoopMode::Propose};
    // Manifest workers set this true so an evidence-backed no-op uses its
    // own typed terminal. Legacy standalone analysis callers may retain the
    // proposal.v1 empty-array behavior until migrated.
    bool requireTypedNoChangesTerminal {false};
    // True for a Manifest task that declares plannedCreatePaths (files it MUST create). Such a
    // task has no valid no-op: the caller does not advertise submit_task_no_changes_needed, and the
    // loop itself rejects it (and an empty propose_change_set) with
    // kWorkerNoChangesPlannedCreateRequiredCode even if an adapter or a scripted session returns
    // one. Never set for a legacy caller.
    bool noChangesForbidden {false};
    std::vector<std::string> requiredRequirementIds;
    std::unordered_map<std::string, std::vector<std::string>>
        requiredOutputsByRequirement;
    std::unordered_map<std::string, ExecutionVerificationKind>
        verificationKindsByRequirement;
    bool dependencyProposalContextPresent {false};
    // runId/projectId/providerId/modelId/agentRole/policy pre-filled by the
    // caller; rawModelOutput is overwritten internally on every attempt.
    CreateChangeProposalRequest proposalTemplate;

    // Manifest verification reliability hardening (lineage-aware repair
    // materialization): explicit trusted repair mode, populated ONLY by the
    // bounded combined-verification repair path in
    // ExecutionManifestExecutor - never inferred from provider/model data,
    // and never set for an ordinary primary dispatch or for a task's own
    // internal same-session JSON-repair retry outside that path. When set,
    // EVERY propose_change_set submission in this run() call (the primary
    // attempt and, if it fires, the internal one-shot repair) is routed
    // through ChangeProposalService::materializeRepairSuccessor instead of
    // createFromModelOutput: the model's raw output is treated strictly as
    // an amendment to the predecessor proposal named here, never trusted as
    // a complete replacement on its own. The value is the exact predecessor
    // changeSetId to amend - normally the same id as
    // proposalTemplate.supersedesChangeSetId, which this does not replace
    // or duplicate the meaning of; it exists separately because
    // supersedesChangeSetId alone is also used by the unrelated ordinary
    // same-session repair-retry lineage link, which must never trigger
    // materialization semantics.
    std::optional<ChangeSetId> repairMaterializationPredecessorId;

    // MW-P5.4: when repairMaterializationPredecessorId is set, this
    // additionally names the Run that predecessor is expected to belong to
    // when it differs from proposalTemplate.runId - the one cross-Run
    // continuation-repair case (see
    // ChangeProposalService::MaterializeRepairSuccessorRequest::
    // expectedPredecessorRunId's own doc comment for the trust boundary).
    // Left unset (the default), materialization keeps requiring the
    // predecessor to belong to proposalTemplate.runId exactly as before -
    // this field only ever widens that check when a caller explicitly opts
    // in, never narrows it.
    std::optional<RunId> repairMaterializationExpectedPredecessorRunId;

    // AG-15: this task's frozen required-read paths (frozen RequiredRead
    // scope entries only - never ownedPaths/referencePaths), pre-normalized
    // by the caller through the exact same key format
    // recordInspectedPathIfPresent uses (forward-slash, lowercased, "."
    // for the workspace root) so the same-attempt read gate can do a plain
    // set-membership check against attemptInspectedPaths with no repeated
    // normalization logic. Empty means the task has no required reads (the
    // gate is then a no-op). Checked before ReadyForReview and before an
    // evidence-backed NoChangesNeeded is accepted - never before an
    // Inspect-mode submit_task_verification, which has its own independent
    // per-requirement evidence contract.
    std::vector<std::string> requiredReadPathKeys;

    // AG-15: optional - when runWorkspaceScopeService is non-null, the
    // request_scope_expansion tool is added to this session's toolset and a
    // call to it is validated/persisted through
    // RunWorkspaceScopeService::requestExpansion(runId, taskId, ...,
    // policy, scopeBaseline, maxScopeExpansions). An approved RequiredRead
    // expansion is added to the effective required-read set for the REST OF
    // THIS ATTEMPT ONLY (a repair session starts with none of the previous
    // attempt's expansions still counted as read - it must call the tool
    // again if it still needs that path, exactly like every other
    // attempt-scoped evidence in this runner). The frozen RunWorkspaceScope
    // itself is never mutated from here.
    RunWorkspaceScopeService* runWorkspaceScopeService {nullptr};
    std::string taskId;
    std::optional<WorkspaceBaselineSnapshot> scopeBaseline;
    std::size_t maxScopeExpansions {5};

    // Verification-repair reliability hardening (audited diagnostic read
    // scope): true ONLY for the one bounded combined-verification repair
    // attempt - set exclusively by ExecutionManifestExecutor::runOneTask
    // when repairContext.has_value(), never for an ordinary primary
    // dispatch or the turn loop's own internal same-session JSON-repair
    // retry. Gates both advertising request_diagnostic_read in this
    // session's toolset and the handler's own acceptance of a call to it -
    // see kRequestDiagnosticReadToolName's own doc comment. A granted
    // diagnostic read widens attemptReadPolicy for the REST OF THIS ATTEMPT
    // ONLY, exactly like an approved request_scope_expansion, and (see
    // missingDiagnosticReadKeys in the .cpp) must actually be read in this
    // same attempt before a ReadyForReview/NoChangesNeeded proposal is
    // accepted - never satisfied by a primary attempt's own inspection, and
    // never by a request that was made but never successfully read.
    bool diagnosticReadEligible {false};
    std::size_t maxDiagnosticReads {5};
    // One Run-wide authority shared by Planner and every parallel worker.
    // Production Manifest callers always set it; null preserves legacy
    // standalone callers until they are migrated.
    std::shared_ptr<ProviderSessionBudgetGuard> providerBudgetGuard;

    // Worker capacity fairness (policy worker_capacity_reservation_v3).
    //
    // budgetReservationOwner names the reservation the PRIMARY fresh provider session's turns are
    // charged to in providerBudgetGuard (the task's primary reservation owner, or its retry
    // reservation owner for a later outer attempt - see manifestPrimaryReservationOwner /
    // manifestRetryReservationOwner): they spend that reservation first and afterwards only the unreserved
    // Run-wide surplus, never a turn reserved for another owner. Empty keeps the historical
    // unowned behavior (surplus-only while reservations exist, everything otherwise).
    std::string budgetReservationOwner;

    // The reservation every LATER fresh session of this run() call (the one bounded JSON repair and
    // any typed terminal repair) is charged to. When such a session starts the loop returns the
    // primary owner's unused reservation to the shared surplus and switches to this owner, so a
    // primary session that used every turn it could never leaves the repair session without the
    // floor the policy guarantees it. Empty (the default) keeps every session on
    // budgetReservationOwner. Equal to budgetReservationOwner for an outer retry attempt (it has
    // no further pool).
    std::string budgetRepairReservationOwner;

    // When true (Manifest workers only), the last provider turn the task may
    // spend is Backend-forced to a terminal-only tool policy - the same
    // provider-neutral NextTurnToolPolicy mechanism the Planner already uses.
    // "Last" is the earlier of (a) the session's own maxTurns and (b) the
    // owner's last available turn according to the guard, decided atomically
    // at admission. Propose mode allows ONLY propose_change_set - the typed
    // no-change terminal stays available on every earlier turn, so a worker
    // that is genuinely done submits it as soon as it has its evidence, never
    // by running out its budget; Inspect mode allows only
    // submit_task_verification. Exactly one allowed tool is therefore forced by
    // name on both provider adapters (a multi-tool "any" policy let Claude keep
    // calling inspection tools on its last turn); parallel calls are
    // disallowed. A non-terminal tool call on a forced turn is rejected before
    // execution as worker.forced_terminal_turn_violated. Forcing never waives
    // an evidence gate: a terminal call that lacks its same-attempt evidence
    // still fails closed.
    bool forceTerminalOnFinalTaskTurn {false};

    // AG-17 gpt-6-sol checkpoint M2b: the long-context threshold of the model that PRICES this
    // loop's usage. Every provider response of the loop is classified against it (strictly
    // greater, by that response's own input) by accumulateResponseUsage(), so the outcome's usage
    // carries the exact short/long split. It must come from the SAME immutable Model copy that
    // later supplies the prices (the catalog is never re-read in between). 0 (the default) is flat
    // pricing: nothing is classified and legacy callers stay flat. Never read by the budget guard.
    std::uint64_t longContextThresholdInputTokens {0};
};

struct AgentToolTurnLoopOutcome {
    // True only for a genuinely completed task: a ReadyForReview proposal,
    // a typed same-attempt worker no-op, or typed Inspect verification whose
    // required criteria passed. False for every other terminal shape,
    // including a proposal the parser/service durably persisted as
    // Rejected (see `proposal` below) - a caller must never treat "we
    // persisted a change-set record" as "the task succeeded" on its own.
    bool success {false};
    bool cancelled {false};
    std::string errorCode;
    std::string errorMessage;
    // Terminal turn's raw assistant text (the Stop-reason prose, or the raw
    // propose_change_set arguments JSON when that path was used). Feeds a
    // Report artifact and, for a caller assembling downstream-agent context,
    // should generally be preferred over raw JSON when `proposal` is set.
    std::string finalText;
    // Audit-only: the terminal text of the PRIMARY attempt, set only when the
    // one bounded repair session ran and the primary text is non-empty and
    // differs from finalText (which then holds the repair's terminal text).
    // Bounded to kMaximumRetainedAttemptTextBytes. Persisted verbatim into a
    // failed attempt's ModelExecution::outputContent so a parse failure of
    // either attempt stays auditable; never projected to UI read models.
    std::string primaryAttemptText;
    // Set whenever createFromModelOutput was actually invoked, even when the
    // resulting proposal is Rejected (success is false in that case - see
    // above; this field still lets a caller inspect what was rejected and
    // why via proposal->validationFindings).
    std::optional<ProposedChangeSet> proposal;
    std::optional<ManifestWorkerNoChanges> workerNoChanges;
    std::optional<ManifestTaskVerificationSubmission> taskVerification;
    models::TokenUsage usage;

    // Audited tool-session evidence, populated on every return path
    // (success, failure, cancellation) so a caller building downstream-agent
    // context can report what was actually inspected even for a task that
    // did not succeed - previously this evidence never left the runner, so
    // conditional integration context could only ever see proposal state/
    // findings, never the tool activity behind them.
    std::size_t totalTurns {0};
    std::size_t totalToolCalls {0};
    std::size_t successfulInspectionCalls {0};
    std::size_t totalReturnedBytes {0};
    // AG-15: the FINAL attempt's own attemptInspectedPaths (primary, or the
    // repair's own fresh set when repair ran - never a union of both, since
    // a repair session's evidence never inherits the primary attempt's, see
    // requiredReadPathKeys' own doc comment). Already normalized
    // (forward-slash, lowercased). Durable actual-read evidence for a
    // caller to persist alongside this task's terminal state - never
    // inferred from outputSummary or any other free-text field.
    std::vector<std::string> inspectedRelativePaths;

    // True whenever the one-shot internal repair session actually ran (a
    // fresh session was created and driven through runTurnLoop), regardless
    // of whether the repair itself then succeeded, failed, or was cancelled.
    // False for every failure that never reached repair, e.g. a failed
    // initial provider/session call or a Rejected finding repair cannot fix
    // (see isModelFormatError). A caller dispatching bounded outer retries
    // (ExecutionManifestExecutor) must not retry once this is true: the
    // "at most one bounded repair" budget for this task is already spent,
    // and re-dispatching a fresh outer attempt would let a rejected worker
    // launder its already-repaired failure through another full attempt.
    bool repairAttempted {false};

    // True only when the FINAL persisted proposal (whichever attempt
    // produced it - primary or repaired) is a durably-stored Rejected
    // change set that still contains real, non-empty changes[]. Independent
    // of repairAttempted: a finding outside isModelFormatError() never makes
    // a proposal eligible for internal repair, so repairAttempted can be
    // false here even though this is exactly the same "the worker found
    // real rejected work" fact repairAttempted exists to guard. A caller
    // dispatching bounded outer retries (ExecutionManifestExecutor) must
    // treat this exactly like an already-consumed repair: never retry, since
    // a fresh unrelated attempt's own answer could otherwise paper over a
    // real, durably-recorded rejection.
    bool nonEmptyRejected {false};
};

class AgentToolTurnLoopRunner {
public:
    AgentToolTurnLoopRunner(
        providers::IProviderAgentSessionFactory& sessionFactory,
        AgentToolExecutionService& toolExecution,
        ChangeProposalService& changeProposals);

    // eventSink/stageId: when eventSink is non-null, stageId must be non-null
    // too - the loop publishes a StageProgress event after each executed
    // tool-call batch. Callers that want no mid-loop progress events (or
    // that publish their own) pass eventSink = nullptr.
    [[nodiscard]] AgentToolTurnLoopOutcome run(
        AgentToolTurnLoopRequest request,
        std::stop_token stopToken,
        IRunEventSink* eventSink,
        const StageId* stageId) const;

private:
    providers::IProviderAgentSessionFactory& sessionFactory_;
    AgentToolExecutionService& toolExecution_;
    ChangeProposalService& changeProposals_;
};

} // namespace ai::application
