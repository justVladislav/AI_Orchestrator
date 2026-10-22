#pragma once

#include "ai/application/ChangeProposalTypes.hpp"
#include "ai/common/Result.hpp"

#include <string>

namespace ai::application {

class ChangeProposalService;
class IExecutionManifestRepository;
class IManifestVerificationContinuationRepository;
class IManifestVerificationManualHandoffRepository;
class IManifestWorkLedgerRepository;
class IRunRepository;
class IVerificationBootstrapRepository;

// AG-16.1: the single Backend decision for whether an ordinary (safe,
// non-override) Approve/Apply may proceed for a given change set.
// Centralizes "does an exact, authoritative, successfully-passed
// ManifestProposalSetVerification exist for this exact proposal set" so
// ChangeSetApplyService (approve() and apply()) and a future read-only
// AG-16.2 UI eligibility indicator share one implementation instead of
// re-deriving the same logic independently - see ARCHITECTURE_GAPS.md's
// AG-16 exit criteria.
//
// Read-only: evaluate() never mutates any state (proposal, approval,
// ledger, or workspace) and is safe to call speculatively or repeatedly.
// There is deliberately no TOCTOU window between an informational caller
// (a future UI) and the real decision: ChangeSetApplyService re-runs this
// exact same evaluate() call itself, immediately before mutating anything,
// rather than trusting any caller-supplied prior result.
//
// Scope decision (explicitly confirmed, not silently expanded): this gate
// only enforces AG-16's verification requirement when the change set's
// owning Run is a kExecutionManifestWorkflowId Run - exactly the scoping
// ManifestWorkLedgerService already uses for every other Manifest-specific
// behavior (recordReviewTransition, reconcileMissingEntries, etc., all
// already no-op for a non-Manifest-workflow Run).
// ManifestProposalSetVerification is a Manifest-workflow-only concept; a
// proposal from any other workflow (the Mock workflow, or the legacy
// ExecutionProfileWorkflowRunner roster) has never had one, and AG-16's own
// incident (a Manifest Run proposal applied after its Run's combined
// verification failed) was itself a Manifest Run. For a real, successfully
// resolved non-Manifest-workflow Run, evaluate() returns allowed=true with
// the explicit typed reasonCode kGateNotApplicableCode - this is NEVER to
// be read as "Manifest verification passed," only as "this gate does not
// apply to this Run's workflow." A missing owning Run, or any repository
// read failure while resolving it, is NOT this case - both fail closed
// (see evaluate()'s own doc comment).
struct ManifestApplyEligibility {
    bool allowed {false};
    // Always set, even when allowed is true - a caller never has to infer
    // "why" from an empty string.
    std::string reasonCode;
    // Bounded, leak-safe: never an absolute path, file content, provider
    // payload, or secret. Safe to show directly in a UI or log verbatim.
    std::string reasonMessage;
};

class ManifestApplyVerificationGate {
public:
    // Stable reasonCode for the one allowed=true outcome that is NOT a
    // passed Manifest verification - see the class doc comment's scope
    // decision. Exported so callers/tests can assert on it by name instead
    // of a string literal.
    static constexpr const char* kGateNotApplicableCode = "manifest_apply.gate_not_applicable";
    // Stable reasonCode for the one allowed=true outcome that IS a passed,
    // exact-match Manifest verification.
    static constexpr const char* kEligibleCode = "manifest_apply.eligible";

    // verificationBootstrapRepository and manifestRepository are optional
    // compatibility dependencies (AG-18.3). When both are non-null,
    // evaluate() additionally requires - for a Run whose immutable Manifest declared a
    // bootstrapDeclaration - a durable BootstrapPending decision AND a
    // durable BootstrapVerified transition whose candidate identity matches
    // this same Run's authoritative verification/proposal-overlay identity
    // (verification.overlayFingerprint) before allowing Apply. This is a
    // narrow prerequisite layered in front of the existing AG-16 checks
    // above, not a re-derivation of them - see evaluate()'s own doc comment
    // for the exact check. A null value (the default) keeps pre-AG-18.3
    // behavior unchanged for pre-AG-18.3 embedders; BackendRuntime always
    // supplies both repositories.
    // MW-P5.4: continuationRepository is a third optional compatibility
    // dependency, same "null keeps pre-existing behavior unchanged for
    // pre-existing embedders" contract as the two above. When non-null,
    // evaluate() additionally recognizes a kManifestVerificationContinuationWorkflowId
    // Run - a P5.3-prepared continuation child - as a SEPARATE eligibility
    // path (never a relaxation of the ordinary Manifest-workflow path above):
    // every member of its verified set that belongs to the child Run itself
    // is checked exactly like the ordinary path; every member that instead
    // belongs to the immutable source Run is accepted only by proving, via
    // the durable P5.3 ManifestVerificationContinuationInput this exact child
    // names, that it was part of the originally admitted exact source
    // candidate - never trusted merely for pointing at some other Run. A
    // null value (the default) makes a continuation-workflow Run's proposal
    // fail closed with a stable "gate cannot evaluate" code rather than
    // silently falling through to kGateNotApplicableCode - see evaluate()'s
    // own doc comment.
    // QML-P4: manualHandoffRepository is a fourth optional compatibility
    // dependency, same "null keeps pre-existing behavior unchanged" contract
    // as the three above. When non-null, evaluate() additionally recognizes
    // a kManifestVerificationManualHandoffWorkflowId Run - a free reverify
    // attempt - as a SEPARATE eligibility path, structurally the same shape
    // as the continuation path immediately above but anchored to the
    // immutable ManifestVerificationManualHandoff a durable
    // ManifestVerificationManualHandoffAttempt row names, never to
    // ManifestVerificationContinuationInput (a manual handoff never touches
    // that table - see ManifestVerificationManualHandoffTypes.hpp's own doc
    // comment for why the two are deliberately separate). QML-P4R.4 makes
    // the newest successful attempt the ONE authority for the whole handoff
    // (never independently selected per proposal; older green attempts remain
    // audit-only). Its verified sequence must represent every frozen member
    // in the same order exactly once, either unchanged or as one genuine
    // direct attempt-owned successor. Each representation is independently
    // rebound to the frozen member's recorded proposalId, owningRunId,
    // contentHash, and lineageRootProposalId before Apply is allowed. A null
    // value (the default) makes a
    // manual-handoff-workflow Run's proposal fail closed with a stable
    // "gate cannot evaluate" code rather than silently falling through to
    // kGateNotApplicableCode - see evaluate()'s own doc comment.
    ManifestApplyVerificationGate(
        ChangeProposalService& proposals,
        IManifestWorkLedgerRepository& ledger,
        IRunRepository& runs,
        IVerificationBootstrapRepository* verificationBootstrapRepository = nullptr,
        IExecutionManifestRepository* manifestRepository = nullptr,
        IManifestVerificationContinuationRepository* continuationRepository = nullptr,
        IManifestVerificationManualHandoffRepository* manualHandoffRepository = nullptr);

    // Resolves changeSetId's owning Run and, only for a
    // kExecutionManifestWorkflowId Run, proves an exact-match, fully-passed
    // ManifestProposalSetVerification exists for it. Order of checks (each
    // documented on its own deny site in the .cpp):
    //   1. The proposal itself must be readable and exist.
    //   2. Its owning Run must be readable and exist - a read failure or a
    //      missing Run always fails closed, never kGateNotApplicableCode.
    //   3. Only once the Run is confirmed present: a workflowId other than
    //      kExecutionManifestWorkflowId returns kGateNotApplicableCode.
    //      Otherwise, the proposal's own projectId must match the Run's.
    //   4. For a Manifest-workflow Run, every AG-16 requirement below is
    //      strict, and every identity below is cross-checked, never trusted
    //      in isolation: the Work Ledger entry's own runId/projectId must
    //      match the Run; its terminalStatus must be Completed and its
    //      disposition ManifestReady; its stored verification must be
    //      structurally valid, and its workspaceIdentity/baselineFingerprint
    //      must match this Run's project/recorded baseline; every required
    //      verification status must have passed; the verified
    //      orderedProposalIds must be duplicate-free and must exactly match
    //      (no extra, none missing) the changeSetIds this Run's own task
    //      dispositions recorded; the target proposal must be a member of
    //      that exact set and currently ReadyForReview (not, e.g., already
    //      Applied); and every OTHER member of that verified set must still
    //      resolve to the same Run/project, remain ReadyForReview or
    //      Applied, and have byte-identical durable content to what was
    //      actually verified.
    [[nodiscard]] ManifestApplyEligibility evaluate(const ChangeSetId& changeSetId) const;

private:
    ChangeProposalService& proposals_;
    IManifestWorkLedgerRepository& ledger_;
    IRunRepository& runs_;
    IVerificationBootstrapRepository* verificationBootstrapRepository_;
    IExecutionManifestRepository* manifestRepository_;
    IManifestVerificationContinuationRepository* continuationRepository_;
    IManifestVerificationManualHandoffRepository* manualHandoffRepository_;
};

} // namespace ai::application
