#pragma once

#include "ai/application/AgentToolTypes.hpp"
#include "ai/application/proposals/ChangeProposalService.hpp"
#include "ai/application/IAgentToolAuditRepository.hpp"
#include "ai/application/IAgentToolSessionRepository.hpp"
#include "ai/common/Result.hpp"

#include <string>
#include <vector>

namespace ai::application {

// One proposal produced by an agent tool session, denormalized for direct
// UI consumption. `changes` carries only structural manifest data
// (changeId, operation, relativePath relative to the project root,
// sourcePath, expectedOriginalHash) - `proposedContent`/`capturedOriginalContent`
// are intentionally left empty here even if the repository happens to
// return them; a caller wanting file content for a diff preview must call
// ChangeProposalService::readOriginalContent()/readProposedContent()
// explicitly for one selected change, never load every file body eagerly.
struct AgentProposalReadModel {
    ProposedChangeSet header;
    std::vector<ProposedFileChange> changes;
    std::vector<ValidationFinding> findings;
};

// Denormalized, UI-safe projection of one agent tool session: the session
// aggregate, its provider turns, its audited tool calls, and every change
// proposal it produced.
//
// Safety guarantee: nothing in this type or anything it aggregates ever
// carries an absolute filesystem path, raw tool-call argument/result JSON,
// a secret, or a filesystem handle.
//   - AgentToolSession/AgentProviderTurn never stored those in the first
//     place (see AgentToolTypes.hpp).
//   - AgentToolAuditRecord stores only sha256 hashes of arguments/results,
//     never the raw payloads.
//   - ProposedFileChange::relativePath is always relative to the project
//     root (enforced by ChangeProposalValidator before a proposal is ever
//     persisted).
struct AgentToolSessionReadModel {
    AgentToolSession session;
    std::vector<AgentProviderTurn> turns;
    std::vector<AgentToolAuditRecord> toolCalls;
    std::vector<AgentProposalReadModel> proposals;
};

// Read-only joining/aggregation layer over IAgentToolSessionRepository,
// IAgentToolAuditRepository, and ChangeProposalService - the same role
// ComparisonReadModelService plays for the Comparison feature. Qt/QML never
// joins a session with its turns/tool-calls/proposals itself; it only reads
// the denormalized projection this service returns.
class AgentToolReadModelService final {
public:
    AgentToolReadModelService(
        IAgentToolSessionRepository& sessions,
        IAgentToolAuditRepository& audits,
        ChangeProposalService& proposals);

    // All sessions recorded for a run, most recent first. Empty (not an
    // error) when the run never started an agent tool session.
    [[nodiscard]] common::Result<std::vector<AgentToolSession>> listForRun(
        const RunId& runId) const;

    // Full denormalized projection for one session.
    [[nodiscard]] common::Result<AgentToolSessionReadModel> session(
        const std::string& sessionId) const;

private:
    IAgentToolSessionRepository& sessions_;
    IAgentToolAuditRepository& audits_;
    ChangeProposalService& proposals_;
};

} // namespace ai::application
