#pragma once

#include "ai/application/execution/ExecutionManifestScheduler.hpp"
#include "ai/application/proposals/IChangeApprovalRepository.hpp"
#include "ai/application/proposals/IChangeProposalRepository.hpp"
#include "ai/application/execution/IExecutionManifestRepository.hpp"
#include "ai/application/execution/IManifestTaskExecutionRepository.hpp"
#include "ai/application/execution/IManifestWorkLedgerRepository.hpp"
#include "ai/application/proposals/IProposalReviewRepository.hpp"
#include "ai/application/execution/IRunRepository.hpp"
#include "ai/application/execution/IRunWorkspaceScopeRepository.hpp"
#include "ai/application/execution/ManifestWorkLedgerTypes.hpp"
#include "ai/application/workspace/WorkspaceBaselineProvenance.hpp"
#include "ai/common/Result.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ai::application {

class ArtifactService;

// AG-14: assembles and persists the durable Project Work Ledger, and injects
// a bounded slice of it back into the next Manifest Planner. It is a no-op
// (success, nothing written) for any Run whose workflowId is not
// kExecutionManifestWorkflowId. See ARCHITECTURE_GAPS.md's AG-14.
//
// Every read this service performs against a dependent repository is
// fail-closed: a Result::failure from manifests_/taskExecutions_/proposals_
// propagates as recordTerminalRun()'s own failure rather than being treated
// as "this Run has none of that." Only a genuinely successful-but-empty
// read means absence. This is what makes the reconciliation/retry path
// below safe - a transient error can never produce a plausible-looking but
// wrong entry.
//
// AG-14.1 timing note: recordTerminalRun is only ever called AFTER the
// Run's terminal status has actually been committed by the caller (from
// RunManager::finishRun, OrchestrationService's interrupted-run recovery,
// or reconcileMissingEntries below) - never from inside
// IWorkflowRunner::run() itself, which executes strictly before that
// commit. Data that only run() holds in memory (the frozen Manifest's
// verification identity, the Planner's no-changes-needed rationale) is
// instead persisted by run() as typed JSON artifacts
// (manifest-verification-identity.v1.json,
// manifest-planning-no-changes-identity.v1.json) that recordTerminalRun
// reads back here via the injected ArtifactService - never re-derived from
// prose, never threaded through in memory across that timing boundary.
class ManifestWorkLedgerService {
public:
    // Rendered Planner-injection context plus the exact ledger entry ids it
    // was built from - the caller persists those ids into a context artifact
    // (AG-14 exit criterion: "the context artifact names the included
    // ledger entry ids").
    struct PlannerContext {
        std::string renderedText;
        std::vector<RunId> includedRunIds;
    };

    // Summary of one reconcileMissingEntries() pass.
    struct ReconciliationResult {
        std::size_t recorded {0};
        // A Run whose own findEntry()/recordTerminalRun() call itself
        // failed this pass (e.g. a transient repository error) - left for
        // the next pass rather than recorded with fabricated data.
        std::vector<RunId> stillMissing;
    };

    ManifestWorkLedgerService(
        IManifestWorkLedgerRepository& ledger,
        IRunRepository& runs,
        IExecutionManifestRepository& manifests,
        IManifestTaskExecutionRepository& taskExecutions,
        IChangeProposalRepository& proposals,
        ArtifactService* artifacts = nullptr,
        IChangeApprovalRepository* approvals = nullptr,
        IProposalReviewRepository* reviews = nullptr,
        // AG-15.3: optional; when non-null, recordTerminalRun folds this
        // Run's frozen scope identity + a bounded summary into the ledger
        // entry (ManifestWorkLedgerEntry::workspaceScopeSchemaVersion/
        // workspaceScopeSummary), and buildPlannerContext's per-entry
        // rendering includes that summary. A null value (every pre-AG-15.3
        // caller/test) keeps both fields empty - no behavior change.
        IRunWorkspaceScopeRepository* workspaceScope = nullptr);

    // Assembles and writes one ManifestWorkLedgerEntry for runId. A no-op
    // (returns success, writes nothing) when runId's Run either does not
    // exist, is not a kExecutionManifestWorkflowId Run, or is not yet in one
    // of the four terminal states (Completed/Failed/Cancelled/Interrupted).
    // Immutable once written: a second call for the same runId (a retry, a
    // reconciliation pass, or an accidental double-call) is a benign
    // idempotent success that leaves the original entry untouched - see
    // IManifestWorkLedgerRepository::insertEntry's own "insert once, never
    // replace" contract.
    // On its own final persistence failure (not the benign duplicate case
    // above), best-effort persists a bounded manifest-work-ledger-write-
    // failed.md artifact via the injected ArtifactService (when available)
    // before returning the failure, so the gap is durably discoverable
    // rather than silently lost.
    common::Result<void> recordTerminalRun(const RunId& runId);

    // Appends one ManifestWorkLedgerReviewTransition for changeSetId. A
    // no-op (returns success, writes nothing) when changeSetId is unknown or
    // its owning Run is not a kExecutionManifestWorkflowId Run - a
    // non-Manifest Run's Approve/Reject/Apply is deliberately invisible to
    // this ledger. reason is meaningful only for Rejected;
    // postActionWorkspaceFingerprint only for Applied - callers pass
    // std::nullopt for the field(s) that do not apply to kind. Same
    // best-effort failure artifact as recordTerminalRun on its own
    // persistence failure.
    // AG-14.2: the transition's own row id is deterministic (changeSetId +
    // kind - see the .cpp), so a second call for the same (changeSetId,
    // kind) pair is a benign idempotent no-op, never a duplicate row -
    // matches recordTerminalRun's own "insert once" immutability. occurredAt
    // defaults to now(); reconcileMissingTransitions passes the original
    // historical timestamp (ApprovalRecord::approvedAt/appliedAt,
    // ProposalReviewRejection::rejectedAt) instead when recovering a
    // transition after the fact.
    common::Result<void> recordReviewTransition(
        const ChangeSetId& changeSetId,
        ManifestWorkLedgerTransitionKind kind,
        std::optional<std::string> actor,
        std::optional<std::string> reason,
        std::optional<std::string> postActionWorkspaceFingerprint,
        std::optional<std::chrono::system_clock::time_point> occurredAt = std::nullopt);

    // Startup-recovery pass (called once from BackendRuntime's existing
    // recovery sequence, beside OrchestrationService::recoverInterruptedRuns()):
    // finds every terminal kExecutionManifestWorkflowId Run missing a
    // ledger entry - the gap left by a process crash between a Run's
    // terminal status update and its ledger write, or a best-effort write
    // that failed and was discarded at its call site - and records it via
    // the repository-backed fallback path. Never touches a Run that already
    // has an entry (insertEntry's own immutability already guarantees that,
    // but this checks first to avoid the wasted read/failure-artifact
    // churn of a doomed duplicate insert).
    // AG-17.7D-1: `deferredRunIds` are Runs whose own account is not complete yet (startup
    // recovery's interrupted-task pass could not finish them): the entry is immutable, so such a
    // Run is NOT recorded now but reported in `stillMissing`, and the next startup finds it again.
    common::Result<ReconciliationResult> reconcileMissingEntries(
        const std::set<RunId>& deferredRunIds = {});

    // AG-14.2: companion startup-recovery pass for review/apply transitions
    // (called once from BackendRuntime, beside reconcileMissingEntries
    // above): for every Manifest-workflow Run's own change proposals,
    // recovers any Approved/Rejected/Applied transition that a durable
    // Approval/Rejection record (IChangeApprovalRepository/
    // IProposalReviewRepository) already proves happened but this ledger's
    // own transitions table is missing - the gap left by a best-effort
    // recordReviewTransition call that failed and was discarded at its call
    // site. No-op (returns an empty result) when approvals_/reviews_ were
    // not supplied to the constructor. `recorded` counts transitions
    // written (not Runs). AG-14.3 persists the post-Apply workspace
    // fingerprint in ApprovalRecord, so a recovered Applied transition uses
    // the same authoritative fingerprint as the original live write.
    common::Result<ReconciliationResult> reconcileMissingTransitions();

    // Bounded, deterministic, most-recent-first rendering of up to
    // maxEntries prior ledger entries for projectId, capped at maxBytes of
    // rendered text (the returned text, including its header, never exceeds
    // maxBytes - see buildPlannerContext's own implementation note).
    // currentBaseline is this Run's own already-captured
    // WorkspaceBaselineSnapshot (ManifestWorkflowRunner::run() passes the
    // exact snapshot it holds locally, no new capture) - AG-14.2: always
    // rendered as a "Current workspace baseline" section, even for a
    // project's first Manifest Run with no ledger history at all, and, when
    // the most recent ledger entry has its baseline still recoverable, also
    // renders a real bounded added/modified/deleted diff against it
    // (WorkspaceBaselineProvenance::compare) rather than leaving the Planner
    // two bare fingerprints to compare by eye. Ordinary Manifest Runs own
    // their workspace-baseline.v1.json artifact directly; continuation and
    // manual-handoff verification Runs deliberately reuse the immutable
    // parent Manifest lineage's artifact and must resolve to it through a
    // same-project, cycle-free Run parent chain. Repository/artifact/lineage/
    // parse failures are fail-closed:
    // callers must stop before opening the provider session. A small byte
    // budget may omit/truncate history and diff paths, but preserves the
    // current baseline anchor whenever that anchor itself fits.
    [[nodiscard]] common::Result<PlannerContext> buildPlannerContext(
        const ProjectId& projectId,
        const WorkspaceBaselineSnapshot& currentBaseline,
        std::size_t maxEntries = 5,
        std::size_t maxBytes = 8192) const;

    // MW-P3A paid-retry guard. An unchanged planning-only failure is blocked
    // before a provider session is created. A linked Retry/Edit-and-rerun is
    // the explicit user confirmation boundary and may intentionally proceed.
    [[nodiscard]] common::Result<void> authorizePlanningAttempt(
        const ProjectId& projectId,
        std::string_view planningInputFingerprint,
        bool explicitlyConfirmedPaidRetry) const;

    // The durable baseline identity a Run's ledger row carries: who it belongs to and which baseline fingerprint it was derived
    // from (the verification record's, else the row's own; both present and different is a conflict, not a choice). An empty
    // fingerprint means the row records no baseline; a non-empty one makes the Run's baseline artifact mandatory. std::nullopt: no
    // row. Read-only; exposes neither the row nor the repository.
    struct BaselineIdentity {
        RunId runId;
        ProjectId projectId;
        std::string durableBaselineFingerprint;
    };
    [[nodiscard]] common::Result<std::optional<BaselineIdentity>> findBaselineIdentity(const RunId& runId) const;

private:
    IManifestWorkLedgerRepository& ledger_;
    IRunRepository& runs_;
    IExecutionManifestRepository& manifests_;
    IManifestTaskExecutionRepository& taskExecutions_;
    IChangeProposalRepository& proposals_;
    ArtifactService* artifacts_;
    IChangeApprovalRepository* approvals_;
    IProposalReviewRepository* reviews_;
    IRunWorkspaceScopeRepository* workspaceScope_;
};

} // namespace ai::application
