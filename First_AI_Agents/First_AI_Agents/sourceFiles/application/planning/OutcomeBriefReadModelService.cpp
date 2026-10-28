#include "ai/application/planning/OutcomeBriefReadModelService.hpp"

#include "ai/application/planning/OutcomeBriefReconciler.hpp"

#include <utility>

namespace ai::application {

namespace {

OutcomeBriefView unreadable(const RunId& runId, const std::optional<common::Error>& error)
{
    OutcomeBriefView view;
    view.availability = OutcomeBriefView::Availability::Unreadable;
    view.runId = runId;
    if (error.has_value()) {
        view.errorCode = error->code;
        view.errorMessage = error->message;
    }
    return view;
}

OutcomeBriefView present(const OutcomeBriefRevision& revision)
{
    const OutcomeBrief& brief = revision.brief;
    OutcomeBriefView view;
    view.availability = OutcomeBriefView::Availability::Present;
    view.runId = brief.runId;
    view.state = toString(brief.state);
    view.resolved = isResolvedOutcomeBriefState(brief.state);
    view.pending = isPendingOutcomeBriefState(brief.state);
    view.terminalStatus = toString(brief.terminalStatus);
    for (const OutcomeBriefReason reason : brief.reasons) view.reasons.emplace_back(toString(reason));
    view.counts = brief.counts;
    for (const OutcomeBriefMemberDecision& member : brief.memberDecisions) {
        view.members.push_back({member.changeSetId, toString(member.decision)});
    }
    view.recoveryEligible = brief.recoveryEligible;
    view.requiresInspection = brief.requiresInspection;
    view.continuationSourceRunId = brief.continuationSourceRunId;
    view.continuationChildRunId = brief.continuationChildRunId;
    view.manualHandoffId = brief.manualHandoffId;
    view.revision = revision.ordinal;
    view.recordedAt = revision.createdAt;
    view.sourceStateFingerprint = brief.sourceStateFingerprint;
    view.contentFingerprint = brief.contentFingerprint;
    view.renderedText = brief.renderedText;
    return view;
}

} // namespace

OutcomeBriefReadModelService::OutcomeBriefReadModelService(const IRunRepository& runs, const IOutcomeBriefRepository& briefs)
    : runs_(runs)
    , briefs_(briefs)
{
}

OutcomeBriefView OutcomeBriefReadModelService::viewForRun(const RunId& runId) const
{
    OutcomeBriefView none;
    none.runId = runId;
    if (runId.empty()) return none;

    // The Run first: an id that is not a Run (a selection that outlived its Run) is "nothing to show", not a fault of the brief.
    const auto run = runs_.findRun(runId);
    if (!run.success()) return unreadable(runId, run.error());
    if (!run.value().has_value()) return none;

    // The head applies the READ authority of the repository: a brief whose Run moved on, or that names another project, is refused
    // with persistence.invalid_data - shown as a fault, never as the stale brief.
    const auto head = briefs_.findHead(runId);
    if (!head.success()) return unreadable(runId, head.error());
    if (head.value().has_value()) return present(*head.value());

    // No brief: only a terminal outcome Run is expected to have one.
    if (isTerminal(run.value()->status) && isOutcomeBriefWorkflow(run.value()->workflowId)) {
        none.availability = OutcomeBriefView::Availability::NotRecorded;
    }
    return none;
}

} // namespace ai::application
