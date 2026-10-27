#include "ai/application/planning/OutcomeBriefSources.hpp"

#include "ai/application/IChangeApprovalRepository.hpp"
#include "ai/application/IChangeProposalRepository.hpp"
#include "ai/application/IManifestVerificationContinuationRepository.hpp"
#include "ai/application/IManifestVerificationManualHandoffRepository.hpp"
#include "ai/application/IManifestWorkLedgerRepository.hpp"
#include "ai/application/projects/IProjectRepository.hpp"
#include "ai/application/IProposalReviewRepository.hpp"
#include "ai/application/IRunRepository.hpp"
#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/ManifestVerificationContinuationPreparationService.hpp"

#include <algorithm>
#include <limits>

namespace ai::application {

namespace {

// Maps one repository result onto another type without ever turning a failure into "absent".
template <typename To, typename From, typename Convert>
common::Result<To> mapResult(const common::Result<From>& from, Convert convert)
{
    if (!from.success()) return common::Result<To>::failure(*from.error());
    return common::Result<To>::ok(convert(from.value()));
}

} // namespace

RepositoryOutcomeBriefSources::RepositoryOutcomeBriefSources(
    const IRunRepository& runs,
    const IProjectRepository& projects,
    const IManifestWorkLedgerRepository& workLedger,
    const IChangeProposalRepository& proposals,
    const IChangeApprovalRepository& approvals,
    const IProposalReviewRepository& reviews,
    const IManifestVerificationContinuationRepository& continuations,
    const IManifestVerificationManualHandoffRepository& manualHandoffs,
    const IManifestVerificationManualHandoffResolutionReader& resolutions)
    : runs_(runs)
    , projects_(projects)
    , workLedger_(workLedger)
    , proposals_(proposals)
    , approvals_(approvals)
    , reviews_(reviews)
    , continuations_(continuations)
    , manualHandoffs_(manualHandoffs)
    , resolutions_(resolutions)
{
}

common::Result<std::optional<Run>> RepositoryOutcomeBriefSources::findRun(const RunId& runId) const
{
    return runs_.findRun(runId);
}

common::Result<std::vector<ProjectId>> RepositoryOutcomeBriefSources::listProjectIds() const
{
    return mapResult<std::vector<ProjectId>>(projects_.listAll(), [](const std::vector<Project>& projects) {
        std::vector<ProjectId> ids;
        ids.reserve(projects.size());
        for (const auto& project : projects) ids.push_back(project.id);
        std::sort(ids.begin(), ids.end());
        return ids;
    });
}

common::Result<std::vector<Run>> RepositoryOutcomeBriefSources::listOutcomeRunsPage(
    const ProjectId& projectId, const std::optional<RunPageCursor>& after, std::size_t limit) const
{
    // The two workflows that bear a ledger entry and so a brief; a manual-handoff attempt Run is never one.
    const std::vector<std::string> workflowIds = {std::string(kExecutionManifestWorkflowId),
        std::string(kManifestVerificationContinuationWorkflowId)};
    return runs_.listTerminalRunsPage(projectId, workflowIds, after, limit);
}

common::Result<std::optional<Project>> RepositoryOutcomeBriefSources::findProject(const ProjectId& projectId) const
{
    return projects_.findById(projectId);
}

common::Result<std::optional<ManifestWorkLedgerEntry>> RepositoryOutcomeBriefSources::findLedgerEntry(const RunId& runId) const
{
    return workLedger_.findEntry(runId);
}

common::Result<std::vector<ManifestWorkLedgerReviewTransition>> RepositoryOutcomeBriefSources::listLedgerTransitions(
    const RunId& runId) const
{
    return workLedger_.listTransitionsForRun(runId);
}

common::Result<std::optional<ProposedChangeSet>> RepositoryOutcomeBriefSources::findChangeSet(const ChangeSetId& changeSetId) const
{
    return proposals_.findChangeSet(changeSetId);
}

common::Result<std::optional<ApprovalRecord>> RepositoryOutcomeBriefSources::findApproval(const ChangeSetId& changeSetId) const
{
    return approvals_.findApproval(changeSetId);
}

common::Result<std::optional<ProposalReviewRejection>> RepositoryOutcomeBriefSources::findRejection(
    const ChangeSetId& changeSetId) const
{
    return reviews_.findRejection(changeSetId);
}

common::Result<std::optional<RunId>> RepositoryOutcomeBriefSources::continuationChildOf(const RunId& sourceRunId) const
{
    return mapResult<std::optional<RunId>>(continuations_.findInputBySourceRun(sourceRunId),
        [](const std::optional<ManifestVerificationContinuationInput>& input) -> std::optional<RunId> {
            if (!input.has_value()) return std::nullopt;
            return input->childRunId;
        });
}

common::Result<std::optional<RunId>> RepositoryOutcomeBriefSources::continuationSourceOf(const RunId& childRunId) const
{
    return mapResult<std::optional<RunId>>(continuations_.findInputByChildRun(childRunId),
        [](const std::optional<ManifestVerificationContinuationInput>& input) -> std::optional<RunId> {
            if (!input.has_value()) return std::nullopt;
            return input->sourceRunId;
        });
}

common::Result<std::optional<std::string>> RepositoryOutcomeBriefSources::handoffIdOfAnchor(const RunId& anchorRunId) const
{
    return mapResult<std::optional<std::string>>(manualHandoffs_.findHandoffByAnchorRun(anchorRunId),
        [](const std::optional<ManifestVerificationManualHandoff>& handoff) -> std::optional<std::string> {
            if (!handoff.has_value()) return std::nullopt;
            return handoff->id;
        });
}

common::Result<std::optional<RunId>> RepositoryOutcomeBriefSources::handoffAnchorOfMemberProposal(
    const ChangeSetId& changeSetId) const
{
    return mapResult<std::optional<RunId>>(manualHandoffs_.findHandoffByMemberProposalId(changeSetId),
        [](const std::optional<ManifestVerificationManualHandoff>& handoff) -> std::optional<RunId> {
            if (!handoff.has_value()) return std::nullopt;
            return handoff->verificationAnchorRunId;
        });
}

common::Result<std::optional<RunId>> RepositoryOutcomeBriefSources::handoffAnchorOfAttemptRun(const RunId& attemptRunId) const
{
    return mapResult<std::optional<RunId>>(manualHandoffs_.findAttemptByRunId(attemptRunId),
        [](const std::optional<ManifestVerificationManualHandoffAttempt>& attempt) -> std::optional<RunId> {
            if (!attempt.has_value()) return std::nullopt;
            return attempt->verificationAnchorRunId;
        });
}

common::Result<ManifestVerificationManualHandoffResolution> RepositoryOutcomeBriefSources::handoffResolutionOf(const Run& anchor) const
{
    using Result = common::Result<ManifestVerificationManualHandoffResolution>;
    const auto resolved = resolutions_.resolutionsForRuns({anchor});
    if (!resolved.success()) return Result::failure(*resolved.error());
    for (const auto& item : resolved.value()) {
        if (item.runId == anchor.id) return Result::ok(item.resolution);
    }
    // The reader said nothing about this Run: it has no handoff to resolve.
    return Result::ok(ManifestVerificationManualHandoffResolution::NotApplicable);
}

} // namespace ai::application
