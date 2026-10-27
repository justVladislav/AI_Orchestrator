#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/proposals/ChangeProposalTypes.hpp"
#include "ai/application/ManifestVerificationManualHandoffResolution.hpp"
#include "ai/application/ManifestWorkLedgerTypes.hpp"
#include "ai/common/Result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

// PC-4A: the pure core of the provider-free Outcome Brief. One brief describes ONE terminal outcome Run (an ordinary
// Manifest Run or a paid verification-continuation child) and says whether that outcome is RESOLVED. This header and its
// .cpp read no repository, no file and no clock, call no provider and write nothing: the reconciler that gathers the typed
// facts and the storage of the revisions are later slices (PC-4B/4C). See CODEX_CONTEXT "PC-4 design".
inline constexpr std::string_view kOutcomeBriefSchemaVersion = "outcome-brief.v1";
// Version of the resolver + renderer rules. It is part of the canonical source state, so a rule change yields a new
// source-state fingerprint (and therefore a new revision) even when the typed facts are unchanged.
inline constexpr std::string_view kOutcomeBriefPolicyId = "outcome_brief_policy_v1";
inline constexpr std::size_t kOutcomeBriefMaxRenderedBytes = 64 * 1024;

// The ONE list of workflows whose Runs bear a ledger entry and so an outcome brief: an ordinary Manifest Run and a paid
// verification-continuation child. A manual-handoff attempt Run, a Mock Run and every legacy or execution-profile Run are not
// outcome Runs. Every consumer - the resolver, the storage gate, the reconciler and the follow-up planner's source gate -
// classifies a persisted Run::workflowId through this table and keeps no id list of its own.
enum class OutcomeWorkflowKind {
    OrdinaryManifest,
    VerificationContinuation,
};

struct OutcomeWorkflow {
    std::string_view id;
    OutcomeWorkflowKind kind {OutcomeWorkflowKind::OrdinaryManifest};
};

[[nodiscard]] std::span<const OutcomeWorkflow> outcomeWorkflows() noexcept;
// Exact, case-sensitive match against that table; nullopt = not an outcome workflow.
[[nodiscard]] std::optional<OutcomeWorkflowKind> outcomeWorkflowKind(std::string_view workflowId) noexcept;

// The outcome of a Run, as far as the Backend can tell from typed facts.
enum class OutcomeBriefState {
    ResolvedNoChange,           // Completed and nothing reviewable was proposed
    AwaitingReview,             // some reviewable proposal has no human decision yet
    AwaitingApply,              // some reviewable proposal is Approved but not Applied
    ResolvedApplied,            // every reviewable proposal is Applied
    ResolvedRejected,           // every reviewable proposal is rejected by a human
    ResolvedMixed,              // every reviewable proposal is final: some Applied, some human-Rejected
    AttentionRequired,          // ApplyFailed, a missing ledger entry or facts that cannot be true together
    AwaitingManualResolution,   // a Failed anchor with a manual handoff that is not yet fully applied
    ResolvedManuallyApplied,    // a Failed anchor whose manual handoff was fully applied
    SupersededByContinuation,   // a Failed source that a paid continuation child took over (the child has its own brief)
    RecoveryOnly,               // a Failed / Cancelled / Interrupted Run nothing else owns: recovery context only
};

[[nodiscard]] const char* toString(OutcomeBriefState state) noexcept;
[[nodiscard]] std::optional<OutcomeBriefState> outcomeBriefStateFromString(std::string_view value) noexcept;
// resolved_* only. `recovery_only` and `superseded_by_continuation` are neither resolved nor pending.
[[nodiscard]] bool isResolvedOutcomeBriefState(OutcomeBriefState state) noexcept;
// awaiting_review, awaiting_apply, awaiting_manual_resolution and attention_required: something can still change.
[[nodiscard]] bool isPendingOutcomeBriefState(OutcomeBriefState state) noexcept;

// Typed reasons of `attention_required`. Always listed in this order, without repeats.
enum class OutcomeBriefReason {
    SourceLedgerMissing,        // a terminal outcome Run without its Work Ledger entry (never rebuilt or invented here)
    InconsistentSourceFacts,    // the sources disagree about the Run, its proposals or its relations
    InconsistentResolution,     // a decision that cannot exist together with the state (for example human-Rejected AFTER Applied)
    ApplyFailed,                // a proposal whose Apply failed: the workspace may be dirty
};

[[nodiscard]] const char* toString(OutcomeBriefReason reason) noexcept;
[[nodiscard]] std::optional<OutcomeBriefReason> outcomeBriefReasonFromString(std::string_view value) noexcept;

// What the typed repositories say about ONE proposal now.
enum class OutcomeProposalDecision {
    Applied,
    HumanRejected,      // a ProposalReviewRejection exists; a validator rejection is NEVER this
    Approved,           // approved by a human, not yet Applied
    Undecided,          // ReadyForReview, neither approved nor rejected
    ApplyFailed,
    NoChanges,          // ChangeSetState::NoChangesNeeded: nothing to review or apply
    ValidatorRejected,  // ChangeSetState::Rejected: blocked by the Backend validator, not a human decision
    Superseded,         // replaced by a successor inside the same Run
    Unsettled,          // Received / Validated: a proposal that never finished
    Inconsistent,       // the typed facts about it contradict each other
};

[[nodiscard]] const char* toString(OutcomeProposalDecision decision) noexcept;
[[nodiscard]] std::optional<OutcomeProposalDecision> outcomeProposalDecisionFromString(std::string_view value) noexcept;

// Audit evidence taken from the Work Ledger review transitions. It only ENRICHES the typed decision (time, reason,
// post-apply fingerprint) and can never override it. The transition's actor is deliberately not carried: it is a local OS
// username, not an authenticated identity, and a brief may later be read by a provider.
struct OutcomeBriefDecisionEvidence {
    std::optional<std::chrono::system_clock::time_point> occurredAt;
    std::optional<std::string> reason;                           // a human reject reason: untrusted prose
    std::optional<std::string> postActionWorkspaceFingerprint;   // Applied only
};

// The current typed facts about one member of the Run's own `ledger.proposalIds`.
struct OutcomeBriefProposalFacts {
    ChangeSetId changeSetId;
    ChangeSetState state {ChangeSetState::Received};   // the proposal repository
    bool humanRejected {false};                        // the proposal-review repository holds a rejection
    bool approved {false};                             // an approval record exists
    std::optional<OutcomeBriefDecisionEvidence> approvedEvidence;
    std::optional<OutcomeBriefDecisionEvidence> rejectedEvidence;
    std::optional<OutcomeBriefDecisionEvidence> appliedEvidence;
};

// A manual handoff exists for this Failed anchor Run; `resolution` is what the existing Backend reader reports for it.
struct OutcomeBriefHandoffFacts {
    std::string handoffId;
    ManifestVerificationManualHandoffResolution resolution {ManifestVerificationManualHandoffResolution::NotApplicable};
};

// Everything the resolver may look at. The reconciler (PC-4C) fills it from the repositories.
//
// Authority: the Work Ledger entry is authoritative for WHICH proposals belong to the Run, its terminal status, tasks,
// verification and baseline. The CURRENT post-Run decision of each proposal comes from `proposals` (proposal / review /
// apply repositories). Proposals whose id is not in `ledgerEntry->proposalIds` are ignored completely. A ledger holds at most
// 64 proposal ids: more than that (or a repeated id, or a lineage row of a non-member, or an unknown ledger schema version) is a
// corrupt source - the first 64 members are judged, the rest is kept as a typed finding, and the verdict is attention_required.
struct OutcomeBriefFacts {
    RunId runId;
    ProjectId projectId;
    std::string workflowId;                    // the ordinary Manifest workflow or the continuation workflow only
    RunStatus terminalStatus {RunStatus::Failed};
    std::optional<ManifestWorkLedgerEntry> ledgerEntry;
    std::vector<OutcomeBriefProposalFacts> proposals;
    // Typed relations (continuation tables, handoff table), never derived from Run::parentRunId.
    std::optional<RunId> continuationSourceRunId;   // set exactly for a continuation child
    std::optional<RunId> continuationChildRunId;    // the paid child that took over this Failed source
    std::optional<OutcomeBriefHandoffFacts> manualHandoff;
    // Absolute path spellings that must never appear in the brief (see projectContextPathFragments).
    std::vector<std::string> forbiddenPathFragments;
};

struct OutcomeBriefCounts {
    std::uint32_t members {0};            // the Run's own ledger proposal ids that are listed (never more than 64)
    std::uint32_t reviewable {0};         // applied + humanRejected + approved + undecided
    std::uint32_t applied {0};
    std::uint32_t humanRejected {0};
    std::uint32_t approved {0};
    std::uint32_t undecided {0};
    std::uint32_t applyFailed {0};
    std::uint32_t noChanges {0};
    std::uint32_t validatorRejected {0};
    std::uint32_t superseded {0};
    std::uint32_t unsettled {0};
    std::uint32_t inconsistent {0};
    [[nodiscard]] bool operator==(const OutcomeBriefCounts&) const noexcept = default;
};

struct OutcomeBriefMemberDecision {
    ChangeSetId changeSetId;
    OutcomeProposalDecision decision {OutcomeProposalDecision::Undecided};
    [[nodiscard]] bool operator==(const OutcomeBriefMemberDecision&) const noexcept = default;
};

// One derived brief. Everything in it is a pure function of the facts and the policy: the same facts always give the
// same bytes and the same fingerprints.
struct OutcomeBrief {
    std::string schemaVersion {std::string(kOutcomeBriefSchemaVersion)};
    std::string policyId {std::string(kOutcomeBriefPolicyId)};
    RunId runId;
    ProjectId projectId;
    std::string workflowId;
    RunStatus terminalStatus {RunStatus::Failed};
    OutcomeBriefState state {OutcomeBriefState::RecoveryOnly};
    std::vector<OutcomeBriefReason> reasons;     // non-empty exactly when state is AttentionRequired
    OutcomeBriefCounts counts;
    std::vector<OutcomeBriefMemberDecision> memberDecisions;   // in ledger order
    // A function of the terminal status alone: Failed -> recovery eligible; Interrupted -> eligible AND must be inspected
    // (recovery is allowed but never safe or automatic); Cancelled by the user and Completed -> neither. The STATE says which
    // path owns the outcome; these flags only describe the status.
    bool recoveryEligible {false};
    bool requiresInspection {false};
    std::optional<RunId> continuationSourceRunId;
    std::optional<RunId> continuationChildRunId;
    std::optional<std::string> manualHandoffId;

    // The canonical JSON of the sanitized, bounded typed facts plus the policy / schema versions: exactly what the
    // brief is derived from. `sourceStateFingerprint` = sha256 of it. The reconciler compares this one with the current
    // head to decide whether a new revision is due.
    std::string sourceStateJson;
    std::string sourceStateFingerprint;
    // The bounded rendering and `contentFingerprint` = sha256 of its exact bytes: what a later consumer cites.
    std::string renderedText;
    std::string contentFingerprint;
};

// Derives the brief. Fails (code `outcome_brief.invalid_request`) only when the REQUEST is unusable: an empty Run id or
// project id, a Run that is not terminal (Completed / Failed / Cancelled / Interrupted) or a workflow that is not
// ledger-bearing. Facts that are merely wrong or missing are NOT an error: they produce `attention_required` with typed
// reasons. A manual-handoff attempt Run is never a brief; it is resolution evidence for its anchor.
[[nodiscard]] common::Result<OutcomeBrief> deriveOutcomeBrief(const OutcomeBriefFacts& facts);

// The storage gate (PC-4B) and the consumers' fail-closed check. A brief is valid only when it is exactly what
// deriveOutcomeBrief could have produced from its own source state:
//   - both fingerprints are the sha256 of the bytes they name, the rendering is bounded and valid UTF-8;
//   - the source state is a canonical document that reads back strictly (known enums, no unknown keys, canonical spelling)
//     and has a possible shape (closed vocabularies, formats, sizes, a well-formed narrative);
//   - EVERY typed field - identity, workflow, terminal status, state, reasons, counts, relations - is the one the source
//     state says;
//   - every proposal row carries the decision its own facts lead to (the resolver's rule, applied again to each row), the
//     counts are the arithmetic of those decisions and the member decisions ARE those rows;
//   - the reasons and the state are EXACTLY the verdict of the content (the very function the resolver uses): every reason has
//     its proof in the source state and none is extra;
//   - the rendering is byte for byte the canonical rendering of the source state (nothing added, removed or reworded).
// Failure code: `outcome_brief.invalid_brief`.
[[nodiscard]] common::Result<void> validateOutcomeBrief(const OutcomeBrief& brief);

// The canonical rendering of a stored source state (the inverse of what deriveOutcomeBrief writes), or
// `outcome_brief.invalid_brief` when the text is not a canonical source state. The rendering of a brief is a pure function
// of its source state, so a consumer can rebuild it instead of trusting stored bytes.
[[nodiscard]] common::Result<std::string> renderOutcomeBriefFromSourceState(std::string_view sourceStateJson);

// The boundary token of one brief: a domain-separated digest of its source-state fingerprint. Untrusted prose is part of
// the source state, so it cannot contain the token of its own brief.
[[nodiscard]] std::string outcomeBriefBoundaryToken(std::string_view sourceStateFingerprint);

// "YYYY-MM-DDTHH:MM:SSZ" in UTC, locale independent (seconds, rounded towards the past).
[[nodiscard]] std::string formatOutcomeBriefTimestamp(std::chrono::system_clock::time_point time);

} // namespace ai::application
