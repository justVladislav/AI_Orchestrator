#include "ai/application/AgentToolReadModelService.hpp"

#include <algorithm>
#include <vector>

namespace ai::application {

AgentToolReadModelService::AgentToolReadModelService(
    IAgentToolSessionRepository& sessions,
    IAgentToolAuditRepository& audits,
    ChangeProposalService& proposals)
    : sessions_(sessions)
    , audits_(audits)
    , proposals_(proposals)
{
}

common::Result<std::vector<AgentToolSession>> AgentToolReadModelService::listForRun(
    const RunId& runId) const
{
    auto listed = sessions_.listForRun(runId);
    if (!listed.success())
        return common::Result<std::vector<AgentToolSession>>::failure(*listed.error());

    auto rows = std::move(listed.value());
    std::sort(rows.begin(), rows.end(), [](const AgentToolSession& a, const AgentToolSession& b) {
        return a.createdAt > b.createdAt;
    });
    return common::Result<std::vector<AgentToolSession>>::ok(std::move(rows));
}

common::Result<AgentToolSessionReadModel> AgentToolReadModelService::session(
    const std::string& sessionId) const
{
    using Result = common::Result<AgentToolSessionReadModel>;

    const auto found = sessions_.findSession(sessionId);
    if (!found.success())
        return Result::failure(*found.error());
    if (!found.value().has_value())
        return Result::failure({"agent_session.not_found", "Agent tool session not found"});

    AgentToolSessionReadModel model;
    model.session = *found.value();

    const auto turns = sessions_.listTurns(sessionId);
    if (!turns.success())
        return Result::failure(*turns.error());
    model.turns = turns.value();
    std::sort(model.turns.begin(), model.turns.end(),
        [](const AgentProviderTurn& a, const AgentProviderTurn& b) {
            return a.turnIndex < b.turnIndex;
        });

    const auto toolCalls = audits_.listForSession(sessionId);
    if (!toolCalls.success())
        return Result::failure(*toolCalls.error());
    model.toolCalls = toolCalls.value();
    std::sort(model.toolCalls.begin(), model.toolCalls.end(),
        [](const AgentToolAuditRecord& a, const AgentToolAuditRecord& b) {
            return a.createdAt < b.createdAt;
        });

    const auto proposalHeaders = proposals_.listForRun(model.session.runId);
    if (!proposalHeaders.success())
        return Result::failure(*proposalHeaders.error());

    // Batch changes/findings across every proposal in one query each instead
    // of one query pair per proposal: this loop runs synchronously on the Qt
    // GUI thread every time AgentSessionController::refresh() is called, so
    // an N+1 here was a direct, measured contributor to the UI freezing
    // while a session had several proposals.
    std::vector<ChangeSetId> changeSetIds;
    changeSetIds.reserve(proposalHeaders.value().size());
    for (const auto& header : proposalHeaders.value())
        changeSetIds.push_back(header.changeSetId);

    const auto changesBySet = proposals_.listChangesForSets(changeSetIds);
    if (!changesBySet.success())
        return Result::failure(*changesBySet.error());
    const auto findingsBySet = proposals_.listValidationFindingsForSets(changeSetIds);
    if (!findingsBySet.success())
        return Result::failure(*findingsBySet.error());

    model.proposals.reserve(proposalHeaders.value().size());
    for (const auto& header : proposalHeaders.value()) {
        AgentProposalReadModel proposal;
        proposal.header = header;
        proposal.header.changes.clear();
        proposal.header.validationFindings.clear();

        const auto changesIt = changesBySet.value().find(header.changeSetId);
        if (changesIt != changesBySet.value().end())
            proposal.changes = changesIt->second;
        // Never surface file bodies eagerly: only structural manifest data
        // is meant to travel through this projection. A caller that needs
        // content calls ChangeProposalService::readOriginalContent()/
        // readProposedContent() explicitly for one selected change.
        for (auto& change : proposal.changes) {
            change.proposedContent.reset();
            change.capturedOriginalContent.reset();
        }

        const auto findingsIt = findingsBySet.value().find(header.changeSetId);
        if (findingsIt != findingsBySet.value().end())
            proposal.findings = findingsIt->second;

        model.proposals.push_back(std::move(proposal));
    }
    std::sort(model.proposals.begin(), model.proposals.end(),
        [](const AgentProposalReadModel& a, const AgentProposalReadModel& b) {
            return a.header.createdAt > b.header.createdAt;
        });

    return Result::ok(std::move(model));
}

} // namespace ai::application
