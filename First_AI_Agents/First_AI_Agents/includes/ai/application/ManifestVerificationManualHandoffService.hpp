#pragma once

#include "ai/application/proposals/ChangeProposalService.hpp"
#include "ai/application/IExecutionManifestRepository.hpp"
#include "ai/application/IManifestVerificationContinuationRepository.hpp"
#include "ai/application/IManifestVerificationManualHandoffRepository.hpp"
#include "ai/application/IManifestWorkLedgerRepository.hpp"
#include "ai/application/projects/IProjectRepository.hpp"
#include "ai/application/verification/IVerificationScratchRootProvider.hpp"
#include "ai/application/IRunRepository.hpp"
#include "ai/application/verification/ManifestProposalSetVerifier.hpp"
#include "ai/application/ManifestVerificationAttributionService.hpp"
#include "ai/application/ManifestVerificationContinuationService.hpp"
#include "ai/application/ManifestVerificationLegacyBaselineNormalizationService.hpp"

#include <filesystem>
#include <mutex>
#include <stop_token>

namespace ai::application {

class IOutcomeBriefTrigger;

// One materialize() call's outcome: the immutable durable contract plus the
// candidate directory's real path, re-derived (never trusted from a stored
// absolute path) for a caller that needs to actually show/open it.
struct ManifestVerificationManualHandoffMaterializeResult {
    ManifestVerificationManualHandoff handoff;
    std::filesystem::path candidateRoot;
};

// QML-P4R.3: the typed result of probing a handoff's candidate directory at
// LOAD time (before any import), deliberately never a binary "does the live
// fingerprint match originalSnapshot" check - the user editing the candidate
// is the entire point of a manual handoff, so a changed fingerprint is
// ordinary, not corruption. Only importAndReverify() itself, which reads the
// actual diff against allowedPaths, decides whether a given edit is
// legitimate; this probe stops at "is the directory itself safe and present
// to read," which is what a caller deciding whether to expose/open/import a
// handoff actually needs to know first.
enum class ManifestVerificationManualHandoffCandidateHealth {
    // Live capture matches originalSnapshot exactly - no edits yet.
    Unchanged,
    // Live capture succeeded and differs from originalSnapshot. Whether the
    // specific edit is within allowedPaths (and therefore importable) is
    // NOT decided here - see importAndReverify's own diff logic.
    Edited,
    // The candidate directory does not exist at its expected storage path.
    Missing,
    // The directory exists but a safe capture failed (symlink/reparse point
    // introduced since materialization, permission denial, I/O error, or any
    // other WorkspaceBaselineProvenance::capture refusal).
    Unreadable,
    // resolveCandidateRoot's own canonical containment check against
    // manualHandoffRoot_ failed - the stored storageKey no longer resolves
    // to a real child of the Backend-owned root. Never trusted as any other
    // state above, regardless of what a symlink might make it look like.
    StorageMismatch,
};

struct ManifestVerificationManualHandoffCandidateProbe {
    ManifestVerificationManualHandoffCandidateHealth health {
        ManifestVerificationManualHandoffCandidateHealth::Missing};
    // Populated only for Unchanged/Edited - the live capture's own
    // fingerprint, for a caller that wants to display or further compare it
    // without capturing a second time.
    std::optional<std::string> liveFingerprint;
};

// Provider-free, read-only classification of the current candidate diff.
// editableChangedPaths are ordinary Modify edits to paths frozen into the
// handoff's allowlist. resetRequiredPaths are every other filesystem drift
// importAndReverify() must reject: a changed non-editable file, a new file,
// or any deleted frozen file (manual handoff does not support Delete). Paths
// are workspace-relative and bounded by the candidate snapshot limits; no
// absolute storage path is ever exposed through this shape.
struct ManifestVerificationManualHandoffCandidateEditInspection {
    ManifestVerificationManualHandoffCandidateProbe candidate;
    std::vector<std::string> editableChangedPaths;
    std::vector<std::string> resetRequiredPaths;
};

// Outcome of the explicit reset command. resetPaths is the exact set that
// was restored/removed. editablePreservedPaths is independently rechecked
// after the reset and proves the user's authorized edits were not rewritten.
struct ManifestVerificationManualHandoffResetResult {
    std::vector<std::string> resetPaths;
    std::vector<std::string> editablePreservedPaths;
    std::string resultingFingerprint;
};

// One importAndReverify() call's outcome: the sibling attempt Run this call
// created (or, when idempotentReplay is true, the existing attempt matching
// the exact same edited state - see importAndReverify's own doc comment) and
// its terminal status/verification. Never itself an Apply authorization -
// only ManifestApplyVerificationGate decides that, by reading this same
// attempt Run's own durable Work Ledger entry exactly like any other
// verified Run.
struct ManifestVerificationManualHandoffImportOutcome {
    RunId attemptRunId;
    RunStatus finalStatus {RunStatus::Failed};
    std::optional<ManifestProposalSetVerification> finalVerification;
    bool idempotentReplay {false};
};

// QML-P4R.3: one reconcileInterruptedAttempts() call's outcome - which
// attempt Runs it found needing cross-process crash recovery, bucketed by
// which repair branch actually applied. Audit-only; callers do not need to
// react to it (the repair is already durably applied by the time this
// returns), but a caller may want to log or assert on it.
struct ManifestVerificationManualHandoffReconciliationResult {
    // No authoritative Work Ledger entry existed: any successor proposals
    // created mid-attempt were marked missing-evidence, claims released,
    // and any still-open stage brought to a terminal status.
    std::vector<RunId> incompleteAttemptRunIds;
    // An authoritative Work Ledger entry already existed but the Run/stage
    // projection had not caught up to it (crashed after the ledger commit,
    // before the Run/stage terminal update) - repaired from the ledger,
    // exactly like importAndReverify()'s own lazy self-heal branch, just
    // applied proactively here instead of waiting for a caller to re-enter.
    std::vector<RunId> repairedFromLedgerRunIds;
    // A successor proposal reconciliation found orphaned (no authoritative
    // ledger, its attempt Run no longer active) but could NOT mark missing-
    // evidence for, because the repository call itself failed. Deliberately
    // NEVER silently discarded (see this method's own doc comment) - a
    // caller MUST treat a non-empty list here as a durable recovery gap and
    // surface it (BackendRuntime does, through its existing recoveryError()
    // channel), since an orphan successor left without this annotation
    // could otherwise be mistaken later for genuinely-verified evidence.
    std::vector<ChangeSetId> unmarkedMissingEvidenceProposalIds;
};

// QML-P4R.5: the answer to "if evaluateCreateEligibility's caller called
// materialize() for this anchor right now (assuming no existing handoff),
// would its own admission-depth checks pass?" - deliberately scoped as
// shallow as ManifestVerificationContinuationReadModelService's own
// canContinue/blockCode: the SAME origin-resolution and admission
// primitives materialize() itself calls, never the deeper manifest/
// workspace-baseline/proposal-membership checks that only matter inside a
// real materialize() attempt (those remain materialize()'s own exclusive
// responsibility - a true eligible=true here is a necessary, not
// sufficient, precondition, exactly like canContinue's own contract).
// eligible=false always carries a non-empty reasonCode/reasonMessage;
// eligible=true always carries both empty.
struct ManifestVerificationManualHandoffCreateEligibility {
    bool eligible {false};
    std::string reasonCode;
    std::string reasonMessage;
};

// QML-P4R.5: origin-resolution's own reusable result - see
// resolveOriginManifestRun's own doc comment.
struct ManifestVerificationManualHandoffOriginResolution {
    RunId originManifestRunId;
    std::optional<ManifestVerificationContinuationInput> continuationInput;
};

// QML-P4R.3.5: one enforceRetention() call's outcome. Audit-only, like
// ManifestVerificationManualHandoffReconciliationResult above - every
// effect is already durably applied by the time this returns.
struct ManifestVerificationManualHandoffRetentionResult {
    // Handoffs evicted via the SAME discardHandoff() mechanics (DB delete
    // first and authoritative, filesystem removal best-effort after),
    // oldest-evicted-first.
    std::vector<std::string> evictedHandoffIds;
    // Orphan candidate directories removed from manualHandoffRoot_ - a
    // directory whose name matches no live handoff's storageKey, left
    // behind by a crash between a past discard's DB commit and its own
    // filesystem removal. Never a symlink/reparse point, and never
    // followed into one - see enforceRetention()'s own doc comment.
    std::vector<std::string> sweptOrphanDirectoryNames;
};

// QML-P4: the free, user-authored counterpart to MW-P5.3's paid
// ManifestVerificationContinuationPreparationService::prepare(). Performs no
// provider request and never calls a Planner - identical in that respect to
// prepare() - but is a deliberately SEPARATE command boundary, never a call
// into prepare() or a reuse of its ManifestVerificationContinuationInput/
// child Run: that record is 1:1 per source Run and its child is
// single-use-then-terminal by construction, a shape that cannot support an
// unbounded number of free reverify attempts against one immutable handoff.
//
// materialize() shares its early orchestration sequence with prepare() (same
// admission check, same manifest/ledger read, same baseline capture/drift/
// legacy-normalization handling, same proposal-membership recheck, same
// ManifestProposalOverlay/ManifestProposalSetVerifier/attribution calls) by
// calling the exact same underlying reusable primitives/services - never a
// re-derivation of their logic - but the SEQUENCING itself is intentionally
// duplicated rather than factored into a shared helper, so prepare()'s own
// already-verified paid-path behavior is never put at risk by a change made
// for this free path.
//
// Where materialize() diverges from prepare():
//   - Its candidate overlay is materialized into a durable, Backend-owned
//     manualHandoffRoot (never the ephemeral trustedScratchRoot verification
//     overlays use) and is NOT cleaned up on success - the whole point is
//     that the user opens it afterward in their own editor.
//   - Its "active scope" is not the frozen exact proposal set's every
//     Create/Modify/Rename target, but only the bounded subset a REMAINING
//     owner finding (ManifestVerificationFindingAttribution, kind
//     SourceProposalOwner/IntegrationOwner) actually names, restricted
//     further to a Create/Modify target (never Rename - see the .cpp for
//     why). A handoff with no such editable path fails closed rather than
//     handing the user an unbounded, unscoped copy of the workspace.
//   - It never creates a Run, never touches
//     IManifestVerificationContinuationRepository, and is idempotent per
//     verificationAnchorRunId the same way prepare() is idempotent per
//     sourceRunId - but against its own separate table, so the paid and free
//     paths can coexist for the same anchor Run without conflict.
//
// QML-P4R.1/P4R.2: a handoff's identity is split into two Run references -
// the verificationAnchorRunId this call is keyed/idempotent on (the Run
// whose durable verification identity this handoff was materialized from)
// and the separately-tracked originManifestRunId (the Run that actually
// owns the Manifest and its frozen scope). They are equal for a Run that
// failed its own combined verification directly. When the anchor is itself
// a paid continuation child, originManifestRunId is instead resolved
// through the durable ManifestVerificationContinuationInput
// (findInputByChildRun) that froze it, the anchor's own eligibility is
// checked directly against its own Work Ledger entry (never the no-
// chaining admission_ checkpoint, which always denies a continuation-
// workflow Run), and attribution runs through
// ManifestVerificationAttributionService::attributeOrEvaluateForAnchor,
// which merges the anchor's own task dispositions with the origin's for
// whichever tasks the anchor never itself repaired. Any other anchor
// workflow still fails closed with manual_handoff.unsupported_anchor_workflow.
// Each frozen member of the handoff's proposal set separately carries its
// OWN real owningRunId, so a mixed set is both representable in storage and,
// as of QML-P4R.2, genuinely producible by materialize() itself.
//   - Two consecutive WorkspaceBaselineProvenance captures of the freshly
//     materialized candidate must agree (manual_handoff.workspace_unstable
//     otherwise) - defence against a concurrent writer touching the
//     candidate directory mid-freeze, which the paid path's own transient,
//     single-use overlay never has to guard against.
class ManifestVerificationManualHandoffService final {
public:
    ManifestVerificationManualHandoffService(
        IRunRepository& runs,
        IProjectRepository& projects,
        IExecutionManifestRepository& manifests,
        IManifestWorkLedgerRepository& workLedger,
        IManifestVerificationManualHandoffRepository& manualHandoffs,
        // QML-P4R.2: resolves originManifestRunId when the anchor Run is
        // itself a paid continuation child (findInputByChildRun) - never
        // used for anything continuation eligibility already owns.
        IManifestVerificationContinuationRepository& continuations,
        ManifestVerificationContinuationService& admission,
        ManifestVerificationAttributionService& attribution,
        ChangeProposalService& proposals,
        IManifestVerificationCommandRunner& commandRunner,
        ManifestVerificationLegacyBaselineNormalizationService& baselineNormalization,
        // QML-P4R.3: required, never defaulted to a %TEMP% fallback - a
        // materialized candidate is durable, Backend-owned application data
        // (see BackendRuntime's own config.databasePath.parent_path()
        // convention for "proposals"/"artifacts"), never an ephemeral
        // location a user's own OS temp-cleanup policy could silently
        // delete out from under an in-progress manual edit.
        std::filesystem::path manualHandoffRoot,
        std::filesystem::path ephemeralScratchRoot = {});

    // QML-P4R.3.5: immediately before inserting a genuinely NEW handoff row
    // (never on the idempotent existing-handoff fast path above), this
    // call is serialized against materialize()/importAndReverify()/
    // discardHandoff() through the same importMutex_ those already use -
    // but ONLY around that final count-check-then-insert segment, not this
    // whole function (which never otherwise mutates shared state and has
    // its own established "raced" self-re-entry recovery for a
    // concurrent duplicate insert - taking the lock for the entire body
    // would make that recursive call deadlock on itself). See
    // ensureRetentionCapacityForNewHandoffLocked's own doc comment for the
    // capacity check/eviction/fail-closed contract.
    [[nodiscard]] common::Result<ManifestVerificationManualHandoffMaterializeResult>
    materialize(const RunId& verificationAnchorRunId, std::stop_token stopToken = {});

    // QML-P4's free, repeatable reverify: diffs the live (possibly
    // user-edited) candidate directory against the handoff's immutable
    // originalSnapshot, rejects anything outside the bounded contract
    // (new path, deleted path, a change to a path not in allowedPaths, a
    // symlink/reparse point - the last via WorkspaceBaselineProvenance::
    // capture's own existing safety check, never re-derived here), and -
    // only once the diff is entirely legitimate - creates ONE new
    // user-authored successor per amended proposal (via
    // ChangeProposalService::materializeRepairSuccessor, always amending the
    // handoff's own original proposal id, never a previous attempt's
    // successor), assembles the complete final proposal set, and re-runs
    // the same ManifestProposalSetVerifier combined verification prepare()/
    // repair() use. Never runs verification directly against the editable
    // candidate directory itself - only the diff is read from it; a fresh,
    // clean overlay is built from the trusted proposal store for the actual
    // verification, exactly like every other verification in this codebase.
    //
    // Idempotent per (handoffId, exact edited state): a repeated call
    // against unchanged files returns the SAME existing attempt
    // (idempotentReplay = true) rather than creating a duplicate Run - see
    // IManifestVerificationManualHandoffRepository::insertAttempt's own
    // unique constraint, which this call's own pre-check mirrors.
    //
    // QML-P4R.3.4: which of the two happens is decided by a prior
    // attempt's own durable ManifestVerificationManualHandoffAttemptResult
    // - VerifiedSuccess/VerifiedContentFailure always replay (self-healing
    // the Run/stage/typed-result projections from the Work Ledger's own
    // entry, never re-verifying), Incomplete always permits this new
    // ordinal retry. Never decided from an error message's text. An
    // attempt whose Run reached a terminal status with neither a ledger
    // entry nor a typed result fails closed
    // (manual_handoff.attempt_result_missing) rather than guessing, and an
    // existing typed result that disagrees with the ledger's own
    // terminalStatus fails closed too (manual_handoff.attempt_result_conflict)
    // rather than silently trusting either one.
    //
    // QML-P4R.3.4 hardening: a fourth outcome, RejectedBeforeVerification,
    // covers a deterministic pre-verification content/policy rejection of
    // the edited bytes themselves (materializeRepairSuccessor's own
    // validation, or a bounded per-file content check) - a stable verdict
    // that a later call against the SAME edited fingerprint always
    // replays verbatim (its own recorded rejectionCode/rejectionMessage),
    // exactly like a VerifiedContentFailure, never allocating an unbounded
    // number of new ordinals for an outcome that can never change without
    // the user actually editing something. Distinct from Incomplete, which
    // remains reserved for genuinely retryable transient/tool/
    // infrastructure failures and cancellation.
    //
    // Creates a NEW sibling attempt Run every time it does real work -
    // never mutates or reopens a previous attempt. A failed attempt leaves
    // the handoff itself untouched, so the user may edit the candidate
    // again and call this again for free.
    [[nodiscard]] common::Result<ManifestVerificationManualHandoffImportOutcome>
    importAndReverify(const std::string& handoffId, std::stop_token stopToken = {});

    // QML-P4R.3: the safe, explicit "Discard handoff" operation the P4R.1-
    // acceptance review required. A genuine, physical delete of the
    // immutable handoff row (never a soft flag - the handoffs table's own
    // reject-update trigger makes that structurally impossible), which
    // transactionally releases every one of its frozen members' GLOBAL
    // UNIQUE(proposal_id) claim so a future anchor may refreeze the same
    // proposal into a new handoff.
    //
    // Serialized against importAndReverify() through the same importMutex_
    // - discarding a handoff a concurrent reverify is actively writing to
    // would be its own race. Refuses with
    // manual_handoff.discard_attempt_active while any attempt Run is still
    // Queued/Running/Cancelling.
    //
    // Ordering is deliberate and load-bearing: the database delete (which
    // cascades attempts/members) commits FIRST; the filesystem candidate
    // directory is removed only afterward, best-effort. The database and
    // filesystem are never one transaction, so a crash between the two steps
    // must always leave a directory with no referencing row (a safe,
    // harmless orphan a future retention pass can sweep) - never a
    // still-referenced row pointing at a directory that silently vanished.
    // Never reverse this order. QML-P4R.5: cooperative cancellation is
    // observed before taking the serialization mutex, again after acquiring
    // it, and once more immediately before the irreversible database delete.
    // Once that delete commits, filesystem cleanup must run best-effort and
    // cancellation can no longer change the successful durable outcome.
    [[nodiscard]] common::Result<void> discardHandoff(
        const std::string& handoffId, std::stop_token stopToken = {});

    // QML-P4R.3: the cross-process counterpart to importAndReverify()'s own
    // in-process crash cleanup (its ScopeExit incompleteAttempt handler,
    // which only ever runs if THIS process is still alive to unwind).
    // Must be called once at Backend startup, AFTER
    // OrchestrationService::recoverInterruptedRuns() has already turned any
    // Queued/Running/Cancelling Run into Interrupted (this call trusts that
    // ordering and never re-derives it) - see its own doc comment for why a
    // Prepared Run is excluded there and therefore here too.
    // recoverInterruptedRuns() only ever touches the Run row itself; it
    // never touches stages, proposal claims, or successor proposals, which
    // is exactly the gap this call closes for every manual-handoff attempt
    // Run specifically. Scans EVERY attempt (listAllAttempts(), not just
    // one handoff's) because a crash can leave an attempt whose own handoff
    // is undiscoverable except by working backward from the attempt row
    // itself. For each:
    //   - no Run row at all (crashed between insertAttempt() and
    //     insertRunWithStages()): left untouched - nothing downstream could
    //     have been created yet, and findAttemptByFingerprint's own
    //     "no Run" branch already lets a later importAndReverify() call
    //     allocate a fresh ordinal for the same edited state instead of
    //     ever blocking on this permanently-dead row.
    //   - Run still Queued/Running (should not happen after
    //     recoverInterruptedRuns() has run first, but never assumed):
    //     left untouched - it may genuinely still be in progress.
    //   - an authoritative Work Ledger entry exists: Run/stage repaired
    //     from it and claims released, exactly like importAndReverify()'s
    //     own lazy self-heal branch - proposals/successors are NEVER
    //     touched here, since the ledger is already the authoritative
    //     record of what became of them. QML-P4R.3.4: this attempt's own
    //     typed ManifestVerificationManualHandoffAttemptResult is also
    //     self-healed here (VerifiedSuccess/VerifiedContentFailure,
    //     derived purely from the ledger's own terminalStatus) if a crash
    //     landed before that row's own commit.
    //   - no ledger entry, Run non-active: every successor proposal
    //     created under this attempt Run (ChangeProposalService::listForRun)
    //     is marked missing-evidence, claims are released, and any stage
    //     still short of a terminal status is marked Failed - the exact
    //     same three effects importAndReverify()'s in-process ScopeExit
    //     produces for an in-process crash, now produced here for a
    //     cross-process one. QML-P4R.3.4: this attempt's own typed result
    //     is durably recorded as Incomplete here too (with a real Result
    //     channel, unlike the in-process ScopeExit's best-effort write),
    //     so the SAME retry-permitted classification survives a restart.
    // Idempotent: a repeated call performs no further mutation for an
    // attempt this call (or a prior call) already reconciled - every branch
    // above re-checks the CURRENT state (ledger presence, Run status, stage
    // status, typed-result presence) before writing, never blindly
    // reapplies. Never touches an
    // immutable audit row's own content (Run/proposal ids, ledger entries,
    // attempt records themselves) - only ever advances Run/stage status,
    // proposal missing-evidence annotations, and claim release, the same
    // three effects ScopeExit already produces today. Zero provider/Planner
    // calls, exactly like every other operation in this class.
    [[nodiscard]] common::Result<ManifestVerificationManualHandoffReconciliationResult>
    reconcileInterruptedAttempts();

    // QML-P4R.3.5: bounded retention. Evicts the OLDEST eligible handoffs
    // (via the exact same discardHandoff() mechanics - DB delete first and
    // authoritative, filesystem removal best-effort after) until at most
    // kManifestVerificationManualHandoffMaxRetainedHandoffs remain, then
    // sweeps orphan candidate directories left behind by a past discard
    // whose filesystem removal step never completed. A handoff is eligible
    // for eviction only if it is NOT: younger than
    // kManifestVerificationManualHandoffMinRetentionAge, currently the
    // subject of an active (Queued/Running/Cancelling) attempt, or already
    // carrying Apply-authority (an authoritative Completed Work Ledger entry;
    // the typed VerifiedSuccess projection must agree when present) - see
    // isHandoffRetentionProtectedLocked's own doc
    // comment. Best-effort at making room: if every remaining handoff is
    // protected, this simply stops evicting and returns what it managed -
    // it is materialize()'s OWN capacity check (called before every new
    // insertHandoff(), under the same importMutex_) that fails closed
    // with manual_handoff.retention_capacity_exhausted when a genuinely
    // new candidate has nowhere to go; enforceRetention() itself never
    // refuses a caller for "couldn't fully get under capacity".
    //
    // Called once at Backend startup (after reconcileInterruptedAttempts()
    // - retention's own eligibility checks must see already-reconciled
    // state, never mid-crash projections) and directly callable for a
    // standalone maintenance pass. Serialized against
    // materialize()/importAndReverify()/discardHandoff() through the same
    // importMutex_ those already use. Idempotent: a repeated call once
    // already at/under capacity with no orphan directories left performs
    // no mutation. Never updates an immutable row in place - only ever a
    // permitted cascading delete, exactly like discardHandoff() itself.
    // Zero provider/Planner calls.
    [[nodiscard]] common::Result<ManifestVerificationManualHandoffRetentionResult>
    enforceRetention();

    // Re-derives (manualHandoffRoot / handoff.storageKey) with a canonical
    // containment check - the real directory is never trusted from a stored
    // or caller-supplied absolute path. Fails closed
    // (manual_handoff.storage_containment_failed) on any mismatch.
    [[nodiscard]] common::Result<std::filesystem::path> resolveCandidateRoot(
        const ManifestVerificationManualHandoff& handoff) const;

    // QML-P4R.5: the only anchor-keyed convenience helper that hands a real
    // absolute filesystem path to the Qt adapter - deliberately NOT exposed
    // through ManifestVerificationManualHandoffReadModelService, whose own
    // contract is QML-safe (never an absolute path). Used exclusively by the
    // adapter's native "Open"/"Copy path" actions, which act on the real
    // path entirely in C++ (QDesktopServices/QClipboard) and never let it
    // reach QML as a string. Thin convenience wrapper around
    // findHandoffByAnchorRun + resolveCandidateRoot; fails with
    // manual_handoff.not_found when no handoff exists yet for this anchor.
    [[nodiscard]] common::Result<std::filesystem::path> resolveCandidateRootForAnchor(
        const RunId& verificationAnchorRunId) const;

    // QML-P4R.3: classifies the candidate directory's current on-disk state
    // without ever treating "the user changed it" as an error - see
    // ManifestVerificationManualHandoffCandidateHealth's own doc comment.
    // Used by materialize()'s idempotent existing-handoff paths (which must
    // fail closed on Missing/Unreadable/StorageMismatch rather than handing
    // back a path to a directory that no longer safely exists) and by a
    // future read-model consumer that needs to show candidate health without
    // itself reimplementing capture/compare logic.
    [[nodiscard]] common::Result<ManifestVerificationManualHandoffCandidateProbe>
    probeCandidateHealth(const ManifestVerificationManualHandoff& handoff) const;

    // Richer read-only counterpart of probeCandidateHealth(): captures the
    // candidate once and classifies its exact diff against originalSnapshot.
    // Missing/unreadable/storage-mismatched candidates retain their typed
    // health and return empty path lists; a healthy candidate never guesses
    // from a directory timestamp or QML-side state.
    [[nodiscard]] common::Result<ManifestVerificationManualHandoffCandidateEditInspection>
    inspectCandidateEdits(const ManifestVerificationManualHandoff& handoff) const;

    // Explicit, user-triggered recovery for an accidentally dirtied candidate.
    // Reconstructs the immutable frozen proposal set from the authoritative
    // workspace baseline/proposal store, then restores only resetRequiredPaths.
    // Authorized Modify edits are preserved byte-for-byte and verified after
    // the mutation against two matching captures; path containment/reparse
    // safety is rechecked immediately before each filesystem action.
    // Serialized with import/discard; never changes the handoff,
    // creates an attempt, calls a provider/Planner, edits the live project, or
    // widens allowedPaths/AG-16 authority.
    [[nodiscard]] common::Result<ManifestVerificationManualHandoffResetResult>
    resetNonEditableChanges(
        const std::string& handoffId, std::stop_token stopToken = {});

    // QML-P4R.5: read-only counterpart of materialize()'s own admission-depth
    // checks, for a read model that must show "can I create a handoff here"
    // without ever performing (or risking) the mutating parts of a real
    // materialize() call. Reuses resolveOriginManifestRun() and the exact
    // same two admission primitives materialize() itself calls: the anchor-
    // is-its-own-origin case defers to ManifestVerificationContinuationService
    // ::evaluate() (the already-established read-only counterpart of
    // admitOrEvaluate() - never admitOrEvaluate() itself, which would write a
    // durable checkpoint as a side effect of a mere read), and the paid-
    // continuation-child case defers to the same
    // determineContentRepairableFailureDenial() free function. Never creates
    // a Run, a checkpoint, or any other durable row - a pure read. Takes the
    // already-resolved anchor Run (never re-reads it) so a caller who has
    // already looked it up (as every read-model caller has) pays no
    // redundant repository round trip.
    [[nodiscard]] common::Result<ManifestVerificationManualHandoffCreateEligibility>
    evaluateCreateEligibility(const Run& anchor) const;

    // PC-4C: the best-effort nudge to the Outcome Brief reconciler (see IOutcomeBriefTrigger). Set once while the runtime is
    // composed, before any concurrent use; null (the default) means no nudge.
    void setOutcomeBriefTrigger(IOutcomeBriefTrigger* trigger) noexcept { outcomeBriefTrigger_ = trigger; }

    // The validated root of the TEMPORARY verification copies (never the durable candidate under manualHandoffRoot). Set once while
    // the runtime is composed; null (the default) keeps ephemeralScratchRoot unchanged. With a provider every operation that
    // builds a temporary copy asks it first and is refused - before anything is copied - when the root is not usable.
    void setVerificationScratchProvider(const IVerificationScratchRootProvider* provider) noexcept
    {
        scratchProvider_ = provider;
    }

private:
    IOutcomeBriefTrigger* outcomeBriefTrigger_ {nullptr};
    const IVerificationScratchRootProvider* scratchProvider_ {nullptr};
    // PC-4C: the anchor Run of every handoff whose row discardHandoffLocked has actually deleted, collected while importMutex_ is held
    // (guarded by it) and drained by the command that holds the lock, which nudges them once the lock is released.
    std::vector<RunId> discardedAnchorsLocked_;
    // QML-P4R.5: extracted from materialize()'s own new-handoff preamble
    // (behavior unchanged) so evaluateCreateEligibility() can reuse the
    // IDENTICAL origin-resolution rule without re-deriving it - resolves
    // which Run actually owns the frozen Manifest/tasks a handoff for this
    // anchor must verify against: the anchor itself for the simple case, or
    // its real origin via the durable continuation input when the anchor is
    // itself a paid continuation child. Fails (never guesses) with
    // manual_handoff.unsupported_anchor_workflow for any other anchor
    // workflow, manual_handoff.origin_unresolvable when no continuation
    // input names this anchor as its child, or
    // manual_handoff.origin_identity_conflict when the continuation input's
    // own recorded identity does not agree with the anchor/origin Run rows -
    // exactly materialize()'s own three failure codes for this segment,
    // unchanged.
    [[nodiscard]] common::Result<ManifestVerificationManualHandoffOriginResolution>
    resolveOriginManifestRun(const Run& anchor) const;


    IRunRepository& runs_;
    IProjectRepository& projects_;
    IExecutionManifestRepository& manifests_;
    IManifestWorkLedgerRepository& workLedger_;
    IManifestVerificationManualHandoffRepository& manualHandoffs_;
    IManifestVerificationContinuationRepository& continuations_;
    ManifestVerificationContinuationService& admission_;
    ManifestVerificationAttributionService& attribution_;
    ChangeProposalService& proposals_;
    IManifestVerificationCommandRunner& commandRunner_;
    ManifestVerificationLegacyBaselineNormalizationService& baselineNormalization_;
    std::filesystem::path manualHandoffRoot_;
    std::filesystem::path ephemeralScratchRoot_;

    // QML-P4R.3.4 hardening: validates a PRE-EXISTING typed result row
    // against what THIS call's own authoritative signal expects - never
    // silently trusted just because a row is present. See its own .cpp
    // doc comment.
    [[nodiscard]] common::Result<void> checkAttemptResultAgrees(
        const ManifestVerificationManualHandoffAttemptResult& existing,
        const std::string& attemptId, const RunId& attemptRunId,
        ManifestVerificationManualHandoffAttemptResultKind expectedKind);
    // QML-P4R.3.4: derives an attempt's typed classification purely from
    // the Work Ledger's own terminalStatus for that attempt Run
    // (Completed -> VerifiedSuccess, anything else -> VerifiedContentFailure)
    // - never from error text - and writes it if a crash landed between
    // the ledger commit and this row's own commit. Idempotent: does
    // nothing if the row already exists and agrees (self-heal, not
    // overwrite - this row is never updated in place); fails closed with
    // manual_handoff.attempt_result_conflict if an existing row disagrees.
    // Both call sites invoke this BEFORE repairing the Run/stage
    // projection from the ledger, never after - a conflict must be caught
    // before any mutation, not discovered only once the projection has
    // already been rewritten.
    [[nodiscard]] common::Result<void> ensureVerifiedAttemptResult(
        const std::string& attemptId, const RunId& attemptRunId, RunStatus ledgerTerminalStatus);
    // Records that an attempt ended WITHOUT ever producing a Work Ledger
    // entry for a genuinely transient/tool/infrastructure reason (or a
    // cancellation) - never a deterministic content verdict. Idempotent:
    // does nothing if the row already exists and agrees. ScopeExit-only -
    // see its own call site's doc comment for why a conflict here is not
    // a realistically reachable path.
    [[nodiscard]] common::Result<void> ensureIncompleteAttemptResult(
        const std::string& attemptId, const RunId& attemptRunId);
    // QML-P4R.3.4 hardening: the RejectedBeforeVerification counterpart of
    // ensureIncompleteAttemptResult above - records a deterministic,
    // pre-verification content/policy rejection of the edited bytes
    // themselves (never a transient/tool/infrastructure condition) with
    // its own stable (code, message), which a later call against the SAME
    // edited fingerprint replays verbatim instead of allocating a new
    // ordinal. ScopeExit-only, same reasoning as
    // ensureIncompleteAttemptResult.
    [[nodiscard]] common::Result<void> ensureRejectedBeforeVerificationAttemptResult(
        const std::string& attemptId, const RunId& attemptRunId,
        std::string rejectionCode, std::string rejectionMessage);

    // QML-P4R.3.5: the discardHandoff() body, assuming importMutex_ is
    // ALREADY held by the caller - discardHandoff() itself is now a thin
    // { lock_guard; return discardHandoffLocked(...); } wrapper. Exists so
    // retention eviction (already holding the lock for its own
    // count-then-evict loop) can reuse the exact same DB-first-then-
    // filesystem-best-effort discard mechanics without a self-deadlock on
    // the non-recursive importMutex_.
    [[nodiscard]] common::Result<void> discardHandoffLocked(
        const std::string& handoffId, std::stop_token stopToken = {});
    // True if this handoff must NEVER be auto-evicted by retention:
    // younger than kManifestVerificationManualHandoffMinRetentionAge, the
    // subject of a currently active (Queued/Running/Cancelling) attempt, or
    // already carrying Apply-authority. Work Ledger is terminal authority:
    // Completed protects even when the typed projection is not committed yet;
    // a present typed result must agree with ledger identity/status, and a
    // verified typed result without a ledger fails closed rather than being
    // trusted or deleted. RejectedBeforeVerification/Incomplete do not protect
    // on their own. Assumes importMutex_ is already held.
    [[nodiscard]] common::Result<bool> isHandoffRetentionProtectedLocked(
        const ManifestVerificationManualHandoff& handoff);
    // Finds and discards (via discardHandoffLocked) the OLDEST handoff
    // that is not retention-protected. Assumes importMutex_ is already
    // held. Returns the evicted handoff's id, or std::nullopt if every
    // remaining handoff is protected - never itself an error in that
    // case, only a genuine repository failure is.
    [[nodiscard]] common::Result<std::optional<std::string>>
    evictOldestEligibleHandoffLocked();
    // QML-P4R.3.5: sweeps manualHandoffRoot_'s own DIRECT children only -
    // never recursive, never following into a symlink/reparse point (a
    // child entry that is one is left completely untouched, not removed
    // either - see enforceRetention()'s own doc comment). A child
    // directory whose name matches no live handoff's storageKey is a
    // safe, unreferenced orphan (left behind by a past discard whose
    // filesystem removal step failed or never ran) and is removed.
    // Assumes importMutex_ is already held.
    [[nodiscard]] common::Result<std::vector<std::string>>
    sweepOrphanCandidateDirectoriesLocked();
    // materialize()'s own capacity gate, called immediately before its
    // insertHandoff() (under the same importMutex_ lock materialize()
    // takes ONLY around that final segment - see materialize()'s own doc
    // comment for why the whole function is not lock-guarded). If the
    // current handoff count is already at or above
    // kManifestVerificationManualHandoffMaxRetainedHandoffs, preflights the
    // complete oldest-eligible batch required to land below capacity before
    // the new insert. If too few are eligible it fails mutation-free with
    // manual_handoff.retention_capacity_exhausted; a genuinely new candidate
    // never silently exceeds the bound and a failed capacity check never
    // performs a hidden partial eviction.
    [[nodiscard]] common::Result<void> ensureRetentionCapacityForNewHandoffLocked();

    // Serializes reservation/import within the product process. SQLite's
    // unique (handoff, attemptOrdinal) constraint is the cross-process final
    // arbiter; this mutex prevents two UI double-clicks from doing duplicate
    // filesystem/proposal work before that constraint is reached.
    std::mutex importMutex_;
};

} // namespace ai::application
