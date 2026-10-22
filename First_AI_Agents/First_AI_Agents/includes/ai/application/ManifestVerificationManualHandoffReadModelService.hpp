#pragma once

#include "ai/application/ChangeProposalService.hpp"
#include "ai/application/IManifestVerificationManualHandoffRepository.hpp"
#include "ai/application/IManifestWorkLedgerRepository.hpp"
#include "ai/application/IRunRepository.hpp"
#include "ai/application/ManifestApplyVerificationGate.hpp"
#include "ai/application/ManifestVerificationManualHandoffResolution.hpp"
#include "ai/application/ManifestVerificationManualHandoffService.hpp"
#include "ai/application/ProposalReviewService.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace ai::application {

// QML-P4R.5: one attempt's own row in the handoff's audit history - never
// itself an authorization; QML renders these fields as-is, it never infers
// "is this the authoritative one" or "did this pass" from resultKind/
// attemptStatus text.
struct ManifestVerificationManualHandoffAttemptHistoryView {
    RunId attemptRunId;
    std::uint64_t attemptOrdinal {0};
    // toString(RunStatus) - the attempt Run's own current status.
    std::string attemptStatus;
    // "verified_success" | "verified_content_failure" |
    // "rejected_before_verification" | "incomplete" | "" (no typed result
    // recorded yet - e.g. the attempt is still genuinely in progress, or a
    // durable-storage integrity gap the Backend itself would already be
    // failing closed on elsewhere; QML shows this as "pending/unknown",
    // never guesses a meaning).
    std::string resultKind;
    // Populated only when resultKind == "rejected_before_verification" -
    // the SAME stable (code, message) importAndReverify() itself would
    // replay for this exact edited fingerprint. Bounded/leak-safe, like
    // every other Backend-authored error surfaced to QML.
    std::string rejectionCode;
    std::string rejectionMessage;
    // The attempt's own immutable Work Ledger verification, when one
    // exists. This is evidence/history only: a failed attempt never becomes
    // Apply authority merely because its diagnostic is projected here.
    bool hasVerification {false};
    std::string proposalSafetyStatus;
    std::string buildLintTestStatus;
    std::string acceptanceStatus;
    // Backend-authored, bounded diagnostic lines (code, optional
    // requirement id, message). QML renders them verbatim and never parses
    // them to infer authority or retry semantics.
    std::vector<std::string> verificationDiagnostics;
    // Structured finding projection split by the handoff's immutable
    // editable allowlist. These are presentation buckets only: the raw
    // verification statuses/AG-16 decision above remain authoritative and
    // are never recomputed from either list. Together with
    // generalDiagnostics below, every structured finding (or the one fallback
    // for a diagnostic without findings) is represented in exactly one
    // segmented bucket. The legacy flat verificationDiagnostics above remains
    // only for compatibility and is not rendered beside these buckets.
    std::vector<std::string> actionableDiagnostics;
    std::vector<std::string> outsideScopeDiagnostics;
    // One line per structured finding that cannot be mapped safely to a
    // candidate-relative path, or one fallback line for a diagnostic carrying
    // no structured findings at all. The Backend never guesses such evidence
    // into actionable/outsideScope, and a mixed diagnostic cannot hide an
    // unmappable finding merely because one of its siblings mapped safely.
    std::vector<std::string> generalDiagnostics;
    // Post-Stage-3 QML-lint hardening: a second, code-aware segmentation of
    // the SAME structured findings above the legacy path-based
    // actionable/outsideScope split, so the command-excerpt gap - showing a
    // raw mixed qml-lint diagnostic as one blob - is fully closed. Every
    // structured finding this service can map to a safe relative path lands
    // in exactly one of these three buckets (never duplicated across them):
    //   - unchangedBaselineDebtDiagnostics: diagnostic code
    //     verification.qml_warning_baseline_debt - proven-unchanged
    //     pre-existing debt that never fails this criterion alone. Only
    //     ever produced by the baseline-aware project-wide delta path;
    //     always empty otherwise.
    //   - newOutsideManifestOrContextDiagnostics: diagnostic code
    //     verification.qml_warning_outside_manifest (a genuinely new
    //     finding on a safe path no frozen task owns - blocking,
    //     fail-closed), OR any other qml-lint finding outside the handoff's
    //     own editable allowlist (the SAME membership test
    //     actionableDiagnostics/outsideScopeDiagnostics above already use -
    //     this never disagrees with them for a Run that never took the
    //     baseline-aware branch).
    //   - newActionableDiagnostics: every remaining qml-lint finding inside
    //     the handoff's own editable allowlist - the genuinely new,
    //     in-scope, blocking case, on either the legacy per-task-scope path
    //     or the baseline-aware project-wide path.
    // A structured finding this service cannot map to a safe relative path
    // at all is never guessed into one of these three - it stays solely in
    // generalDiagnostics above.
    std::vector<std::string> newActionableDiagnostics;
    std::vector<std::string> newOutsideManifestOrContextDiagnostics;
    std::vector<std::string> unchangedBaselineDebtDiagnostics;
    // QML-P4R.4: true for exactly the one attempt - if any - currently
    // backing Apply-authority for the whole handoff (the highest-ordinal
    // VerifiedSuccess attempt). Backend-computed; QML never re-derives
    // this by comparing ordinals or result kinds itself.
    bool isAuthoritative {false};
};

// Post-P4R.6 human review/Apply hardening: one file operation belonging to
// an authoritative review member, projected exactly like
// AgentSessionController::rebuildSelectedProposalDetail's own "changes"
// entries - never an absolute path, always workspace-relative.
struct ManifestVerificationManualHandoffReviewChangeView {
    std::string changeId;
    std::string operation;
    std::string relativePath;
    bool hasExpectedHash {false};
};

// One validation finding belonging to an authoritative review member -
// same shape/source (ChangeProposalService::listValidationFindings) as
// AgentSessionController's own proposal detail projection.
struct ManifestVerificationManualHandoffReviewFindingView {
    std::string code;
    std::string message;
    std::string changeId;
};

// Post-P4R.6 human review/Apply hardening: one member of the authoritative
// attempt's own exact verified proposal set (ManifestWorkLedgerEntry::
// proposalIds - the SAME set canApply above already iterates), projected
// for full human review without any AgentToolSession. Every field is a
// fresh read of already-durable state - never cached, never inferred -
// exactly mirroring AgentSessionController::rebuildSelectedProposalDetail's
// own AG-07/AG-16 projection, just keyed by the handoff's authoritative
// ledger entry instead of an agent-tool session's own proposal list.
// Already-Applied members remain listed (alreadyApplied=true) rather than
// disappearing, so a partially-applied multi-member handoff stays honestly
// visible - see requirement 7 in this feature's own closing account.
struct ManifestVerificationManualHandoffReviewMemberView {
    ChangeSetId proposalId;
    // The Run that actually owns this proposal - the origin Manifest Run
    // for an untouched member, or the manual-handoff attempt Run that
    // amended it via importAndReverify()'s own materializeRepairSuccessor
    // call. Never assumed to equal originManifestRunId/verificationAnchorRunId.
    RunId owningRunId;
    std::string summary;
    // toString(ChangeSetState) of the proposal's OWN current durable state -
    // never re-derived, never a guess. "applied" is also surfaced through
    // alreadyApplied below so a caller does not have to string-compare this
    // field to tell an Applied member apart from a blocked one.
    std::string state;
    bool alreadyApplied {false};
    bool reviewRejected {false};
    std::string reviewRejectedReason;
    // AG-16 itself, freshly re-evaluated for this exact proposal id - never
    // cached, never recomputed here. See ManifestApplyVerificationGate::evaluate().
    bool applyAllowed {false};
    std::string applyBlockCode;
    std::string applyBlockMessage;
    std::vector<ManifestVerificationManualHandoffReviewChangeView> changes;
    std::vector<ManifestVerificationManualHandoffReviewFindingView> findings;
};

// One recorded finding of the anchor Run's failed verification, shown BEFORE any repair workspace exists
// (REMAIN_TO_DO 6a (b)+(c); CODEX_CONTEXT "Manual handoff: recorded findings before the repair
// workspace"). Three separate facts, never merged: the finding itself (visibility), `authority` (may THIS
// handoff edit the file) and, on the view below, the informational verdict. Presentation only - never an
// input to Apply authority, never a gate on Create or Reverify.
struct ManifestVerificationRecordedFindingView {
    std::string diagnosticCode;
    // "blocking" | "advisory" (advisory = every non-blocking code except proven baseline debt, which is
    // only counted, never listed).
    std::string severity;
    // Candidate-relative display path; empty for a finding with no safe path and for a diagnostic that
    // carries no structured findings at all (shown as one row so a non-QML failure is never hidden).
    std::string relativePath;
    int line {0};
    int column {0};
    std::string tag;
    // One display line (code, path:line:column, text, tag).
    std::string text;
    // "editable" | "not_editable" (a handoff exists: path in / not in its frozen allowedPaths) |
    // "outside_all_create_modify_targets" (no handoff yet, PROVEN: the path is a Create or Modify
    // target of none of the anchor's frozen proposals - this handoff will certainly not be able to edit
    // it) | "unknown_until_created" (no handoff yet, some proposal writes the path) | "unknown".
    std::string authority;
};

struct ManifestVerificationRecordedFindingsView {
    std::vector<ManifestVerificationRecordedFindingView> findings;
    // Findings of the proven unchanged baseline debt: counted, not listed.
    std::size_t debtCount {0};
    // "blocked_outside_scope" | "no_known_obstacle" | "unknown". Informational: the full Backend
    // verification stays the only authority; never "will pass".
    std::string verdict {"unknown"};
    std::string verdictCode;
    std::string verdictMessage;
    // Distinct files of proven uneditable blocking findings, in first-seen order.
    std::vector<std::string> outsideScopePaths;
    // Set when the read needed to classify authority failed (e.g. a proposal's changes could not be
    // read): the findings stay, their authority and the verdict become unknown. Never silently empty.
    std::string classificationErrorCode;
    std::string classificationErrorMessage;
};

// Builds the recorded-findings projection of an anchor's failed verification (pure over the ledger entry
// and the proposal store; no provider, no Run, no write). `allowedPathKeys` is the frozen editable list of
// an existing handoff (detail::normalizedPathKey form), or nullptr before one exists: then authority is
// classified from the Create/Modify targets of the anchor's frozen proposals (the ordered set
// materialize() uses). "Cannot edit" is claimed only on positive proof - the changes of EVERY frozen
// proposal were read, the finding path normalised safely, and no Create/Modify target matches; every
// doubt is "unknown" and `editable` is never a default.
[[nodiscard]] ManifestVerificationRecordedFindingsView buildManifestVerificationRecordedFindings(
    bool anchorFailed,
    const std::optional<ManifestWorkLedgerEntry>& anchorLedger,
    ChangeProposalService& proposals,
    const std::unordered_set<std::string>* allowedPathKeys);

// Backend-owned P4R.5 projection - the single authoritative read model for
// one manual handoff, keyed by its verification anchor Run. QML receives
// decisions, identities, and safe normalized evidence; it never
// reconstructs lineage, selects the authoritative attempt, or decides
// Apply eligibility itself - see this service's own doc comment.
struct ManifestVerificationManualHandoffView {
    RunId verificationAnchorRunId;
    // toString(RunStatus) of the anchor Run itself - always populated,
    // independent of whether a handoff has ever been materialized.
    std::string verificationAnchorStatus;

    // Materialization state: false means "not yet created" - every field
    // below is empty/default in that case, and QML's only available action
    // is Create. Never conflated with a candidate that once existed and was
    // discarded, which also reads as hasHandoff == false (discardHandoff()
    // is a genuine physical delete - see its own doc comment).
    bool hasHandoff {false};
    std::string handoffId;
    RunId originManifestRunId;
    std::string originManifestStatus;

    // QML-P4R.3: candidateHealth is one of "unchanged" | "edited" |
    // "missing" | "unreadable" | "storage_mismatch" - see
    // ManifestVerificationManualHandoffCandidateHealth's own doc comment
    // for what each means. Only meaningful when hasHandoff is true.
    std::string candidateHealth;

    // Workspace-relative only, per ManifestVerificationManualHandoffAllowedPath's
    // own contract - never an absolute path.
    std::vector<std::string> allowedRelativePaths;

    // Current candidate diff, Backend-classified against originalSnapshot.
    // resetRequiredRelativePaths includes changed non-editable files, new
    // files, and deleted frozen files. QML may offer the explicit reset
    // command only when this list is non-empty; it never derives the diff.
    std::vector<std::string> editableChangedRelativePaths;
    std::vector<std::string> resetRequiredRelativePaths;

    // The structured findings the verification anchor Run itself failed
    // with, so the repair workspace shows what must be fixed BEFORE the first
    // Reverify attempt exists. Restricted to findings on the handoff's own
    // editable allowlist (the Run's write scope): findings elsewhere in the
    // module, unmappable findings, and proven-unchanged baseline debt
    // (verification.qml_warning_baseline_debt - never the reason a Run
    // failed) are deliberately not projected. Presentation only - never an
    // input to Apply authority. Empty when the anchor has no verification.
    std::vector<std::string> initialScopeDiagnostics;

    // The recorded findings of the failed verification, WITHOUT the editable-scope filter above and
    // available before a handoff exists. The stored findings are a bounded sample
    // (kMaximumRecordedFindingsPerDiagnostic), so this is never "all findings".
    ManifestVerificationRecordedFindingsView recordedFindings;

    // Oldest-first (attemptOrdinal ascending) - the durable, immutable
    // audit trail. Empty when no reverify has ever been attempted yet.
    std::vector<ManifestVerificationManualHandoffAttemptHistoryView> attempts;

    // The authoritative attempt's own final combined verification (the
    // SAME ManifestProposalSetVerification's three top-level statuses
    // every other verification surface in this codebase already exposes) -
    // present only when an authoritative (VerifiedSuccess) attempt exists
    // with its own Work Ledger entry. Never populated from a
    // VerifiedContentFailure/RejectedBeforeVerification/Incomplete attempt,
    // even if it is the newest one - those never carry Apply-authority.
    bool hasFinalVerification {false};
    std::string finalProposalSafetyStatus;
    std::string finalBuildLintTestStatus;
    std::string finalAcceptanceStatus;

    // QML-P4R.5: Apply eligibility, decided ENTIRELY by
    // ManifestApplyVerificationGate::evaluate() (AG-16) against the
    // authoritative attempt's own final verified proposal set - never
    // recomputed here or in QML. False (with a stable reasonCode/Message)
    // whenever no authoritative attempt exists yet, exactly as AG-16 itself
    // would deny any of that set's proposals in that case.
    bool canApply {false};
    std::string applyReasonCode;
    std::string applyReasonMessage;

    // Post-P4R.6 human review/Apply hardening: the exact authoritative
    // verified proposal set (ManifestWorkLedgerEntry::proposalIds from the
    // SAME authoritative attempt canApply above is derived from), each
    // projected for full human review/Apply without an AgentToolSession.
    // Empty whenever hasFinalVerification is false - there is nothing
    // authoritative to review yet. Never independently selected: this is
    // the identical ordered set canApply iterates.
    std::vector<ManifestVerificationManualHandoffReviewMemberView> reviewMembers;

    // QML-P4R.5: whether Create is currently offered - decided ENTIRELY by
    // ManifestVerificationManualHandoffService::evaluateCreateEligibility()
    // (the read-only counterpart of materialize()'s own admission-depth
    // checks), never recomputed here or in QML. Only meaningful when
    // hasHandoff is false; left at its default (false/empty) when hasHandoff
    // is true, since Create is no longer the available action then.
    bool canCreate {false};
    std::string createReasonCode;
    std::string createReasonMessage;
};

class ManifestVerificationManualHandoffReadModelService final
    : public IManifestVerificationManualHandoffResolutionReader {
public:
    ManifestVerificationManualHandoffReadModelService(
        IRunRepository& runs,
        IManifestWorkLedgerRepository& workLedger,
        IManifestVerificationManualHandoffRepository& manualHandoffs,
        // Read-only reuse of probeCandidateHealth()/resolveCandidateRoot() -
        // this read model never calls materialize(), importAndReverify(),
        // discardHandoff(), or enforceRetention() itself; those remain the
        // Qt adapter's own explicit, user-triggered command surface
        // (QML-P4R.5's second package).
        ManifestVerificationManualHandoffService& manualHandoffService,
        // AG-16 itself - the sole Apply-eligibility authority. Never a
        // second, independently-maintained copy of its logic.
        ManifestApplyVerificationGate& applyGate,
        // Post-P4R.6 human review/Apply hardening: read-only reuse of
        // ChangeProposalService::findChangeSet/listChanges/
        // listValidationFindings (exactly what AgentSessionController's own
        // rebuildSelectedProposalDetail already calls) and
        // ProposalReviewService::findRejection to project reviewMembers.
        // This read model NEVER calls approve()/apply()/reject() itself -
        // those remain the Qt adapter's own explicit command surface,
        // mirroring this service's own established Create/Reverify/Discard
        // separation.
        ChangeProposalService& proposals,
        ProposalReviewService& reviewService);

    // Returns std::nullopt (never a failure) when verificationAnchorRunId
    // does not exist, or exists but is not one of the two anchor workflows
    // materialize() itself ever accepts (kExecutionManifestWorkflowId or
    // kManifestVerificationContinuationWorkflowId) - there is genuinely
    // nothing a manual handoff could ever mean for any other Run, exactly
    // like ManifestVerificationContinuationReadModelService's own
    // precedent. A real repository read failure, or a durable row that
    // fails its own typed validation, always fails closed instead.
    [[nodiscard]] common::Result<std::optional<ManifestVerificationManualHandoffView>>
    viewForRun(const RunId& verificationAnchorRunId) const;

    // Lightweight batch projection for RunsPage. It deliberately does not
    // call viewForRun(): no candidate-directory probe, no attempt diagnostic
    // expansion, and no per-row Create eligibility. listAllHandoffs() is read
    // once, then only matching Failed anchors use the shared AG-16 authority
    // resolver and exact authoritative proposal states. This keeps the Runs
    // list free of QML-side inference and of an N+1 read-model fan-out.
    [[nodiscard]] common::Result<std::vector<ManifestVerificationManualHandoffRunResolution>>
    resolutionsForRuns(const std::vector<Run>& runs) const override;

private:
    IRunRepository& runs_;
    IManifestWorkLedgerRepository& workLedger_;
    IManifestVerificationManualHandoffRepository& manualHandoffs_;
    ManifestVerificationManualHandoffService& manualHandoffService_;
    ManifestApplyVerificationGate& applyGate_;
    ChangeProposalService& proposals_;
    ProposalReviewService& reviewService_;
};

} // namespace ai::application
