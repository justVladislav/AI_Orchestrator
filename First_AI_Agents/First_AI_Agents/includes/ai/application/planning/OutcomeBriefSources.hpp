#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/proposals/ChangeProposalTypes.hpp"
#include "ai/application/verification/ManifestVerificationManualHandoffResolution.hpp"
#include "ai/application/execution/ManifestWorkLedgerTypes.hpp"
#include "ai/application/IRunRepository.hpp"
#include "ai/application/proposals/ProposalReviewTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ai::application {

class IChangeApprovalRepository;
class IChangeProposalRepository;
class IManifestVerificationContinuationRepository;
class IManifestVerificationManualHandoffRepository;
class IManifestWorkLedgerRepository;
class IProjectRepository;
class IProposalReviewRepository;
class IRunRepository;

// PC-4C: everything the Outcome Brief reconciler may READ, and nothing it may write. The interface has no mutating method, so a
// reconciler built on it cannot alter a Run, a proposal, an Apply, a review decision or the Work Ledger - the compiler says so, and
// the reconciler tests hand it a fake that records every call. Every read is fail-closed: a failure is a failure, never "absent".
class IOutcomeBriefSources {
public:
    virtual ~IOutcomeBriefSources() = default;

    [[nodiscard]] virtual common::Result<std::optional<Run>> findRun(const RunId& runId) const = 0;
    // The history pass reads project by project, page by page: the ids of the projects, then ONE project's terminal outcome Runs (an ordinary
    // Manifest Run or a paid continuation child) newest first, strictly after the cursor, at most `limit` of them.
    [[nodiscard]] virtual common::Result<std::vector<ProjectId>> listProjectIds() const = 0;
    [[nodiscard]] virtual common::Result<std::vector<Run>> listOutcomeRunsPage(
        const ProjectId& projectId, const std::optional<RunPageCursor>& after, std::size_t limit) const = 0;
    [[nodiscard]] virtual common::Result<std::optional<Project>> findProject(const ProjectId& projectId) const = 0;

    // The immutable Work Ledger entry of the Run and its append-only review transitions (audit evidence only).
    [[nodiscard]] virtual common::Result<std::optional<ManifestWorkLedgerEntry>> findLedgerEntry(const RunId& runId) const = 0;
    [[nodiscard]] virtual common::Result<std::vector<ManifestWorkLedgerReviewTransition>> listLedgerTransitions(
        const RunId& runId) const = 0;

    // The CURRENT decision facts of one proposal: its state, the human approval record, the human rejection record.
    [[nodiscard]] virtual common::Result<std::optional<ProposedChangeSet>> findChangeSet(const ChangeSetId& changeSetId) const = 0;
    [[nodiscard]] virtual common::Result<std::optional<ApprovalRecord>> findApproval(const ChangeSetId& changeSetId) const = 0;
    [[nodiscard]] virtual common::Result<std::optional<ProposalReviewRejection>> findRejection(
        const ChangeSetId& changeSetId) const = 0;

    // Typed relations (never Run::parentRunId): the paid continuation child that took over a Failed source, and the source of a child.
    [[nodiscard]] virtual common::Result<std::optional<RunId>> continuationChildOf(const RunId& sourceRunId) const = 0;
    [[nodiscard]] virtual common::Result<std::optional<RunId>> continuationSourceOf(const RunId& childRunId) const = 0;

    // Manual handoff: the handoff of a Failed anchor Run, the anchor a frozen member proposal belongs to, the anchor a handoff
    // attempt Run verifies, and the resolution the existing Backend reader reports for the anchor.
    [[nodiscard]] virtual common::Result<std::optional<std::string>> handoffIdOfAnchor(const RunId& anchorRunId) const = 0;
    [[nodiscard]] virtual common::Result<std::optional<RunId>> handoffAnchorOfMemberProposal(const ChangeSetId& changeSetId) const = 0;
    [[nodiscard]] virtual common::Result<std::optional<RunId>> handoffAnchorOfAttemptRun(const RunId& attemptRunId) const = 0;
    [[nodiscard]] virtual common::Result<ManifestVerificationManualHandoffResolution> handoffResolutionOf(const Run& anchor) const = 0;
};

// The production sources: thin read-only views over the existing repositories and the resolution reader. It only ever calls their
// find / list methods.
class RepositoryOutcomeBriefSources final : public IOutcomeBriefSources {
public:
    RepositoryOutcomeBriefSources(
        const IRunRepository& runs,
        const IProjectRepository& projects,
        const IManifestWorkLedgerRepository& workLedger,
        const IChangeProposalRepository& proposals,
        const IChangeApprovalRepository& approvals,
        const IProposalReviewRepository& reviews,
        const IManifestVerificationContinuationRepository& continuations,
        const IManifestVerificationManualHandoffRepository& manualHandoffs,
        const IManifestVerificationManualHandoffResolutionReader& resolutions);

    [[nodiscard]] common::Result<std::optional<Run>> findRun(const RunId& runId) const override;
    [[nodiscard]] common::Result<std::vector<ProjectId>> listProjectIds() const override;
    [[nodiscard]] common::Result<std::vector<Run>> listOutcomeRunsPage(
        const ProjectId& projectId, const std::optional<RunPageCursor>& after, std::size_t limit) const override;
    [[nodiscard]] common::Result<std::optional<Project>> findProject(const ProjectId& projectId) const override;
    [[nodiscard]] common::Result<std::optional<ManifestWorkLedgerEntry>> findLedgerEntry(const RunId& runId) const override;
    [[nodiscard]] common::Result<std::vector<ManifestWorkLedgerReviewTransition>> listLedgerTransitions(
        const RunId& runId) const override;
    [[nodiscard]] common::Result<std::optional<ProposedChangeSet>> findChangeSet(const ChangeSetId& changeSetId) const override;
    [[nodiscard]] common::Result<std::optional<ApprovalRecord>> findApproval(const ChangeSetId& changeSetId) const override;
    [[nodiscard]] common::Result<std::optional<ProposalReviewRejection>> findRejection(
        const ChangeSetId& changeSetId) const override;
    [[nodiscard]] common::Result<std::optional<RunId>> continuationChildOf(const RunId& sourceRunId) const override;
    [[nodiscard]] common::Result<std::optional<RunId>> continuationSourceOf(const RunId& childRunId) const override;
    [[nodiscard]] common::Result<std::optional<std::string>> handoffIdOfAnchor(const RunId& anchorRunId) const override;
    [[nodiscard]] common::Result<std::optional<RunId>> handoffAnchorOfMemberProposal(const ChangeSetId& changeSetId) const override;
    [[nodiscard]] common::Result<std::optional<RunId>> handoffAnchorOfAttemptRun(const RunId& attemptRunId) const override;
    [[nodiscard]] common::Result<ManifestVerificationManualHandoffResolution> handoffResolutionOf(const Run& anchor) const override;

private:
    const IRunRepository& runs_;
    const IProjectRepository& projects_;
    const IManifestWorkLedgerRepository& workLedger_;
    const IChangeProposalRepository& proposals_;
    const IChangeApprovalRepository& approvals_;
    const IProposalReviewRepository& reviews_;
    const IManifestVerificationContinuationRepository& continuations_;
    const IManifestVerificationManualHandoffRepository& manualHandoffs_;
    const IManifestVerificationManualHandoffResolutionReader& resolutions_;
};

} // namespace ai::application
