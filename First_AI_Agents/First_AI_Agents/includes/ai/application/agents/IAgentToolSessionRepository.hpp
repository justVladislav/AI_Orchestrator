#pragma once

#include "ai/application/agents/AgentToolTypes.hpp"
#include "ai/common/Result.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace ai::application {

// Persistence boundary for agent tool sessions and provider turns.
// Sessions are the outermost audit scope; individual tool calls are tracked
// via IAgentToolAuditRepository, linked by sessionId.
//
// Rules:
//   - callers must insert a session row before recording any audit records;
//   - session state transitions: Active → Completed or Active → Interrupted;
//   - interruptActiveSessions() is called once at startup recovery, before
//     any session is exposed to the UI, so no session row ever stays Active
//     after a crash;
//   - if this repository is unavailable, the tool executor must refuse to
//     start any tool call (fail-closed audit requirement).
class IAgentToolSessionRepository {
public:
    virtual ~IAgentToolSessionRepository() = default;

    [[nodiscard]] virtual common::Result<AgentToolSession> insertSession(
        AgentToolSession session) = 0;

    [[nodiscard]] virtual common::Result<AgentToolSession> updateSession(
        AgentToolSession session) = 0;

    [[nodiscard]] virtual common::Result<std::optional<AgentToolSession>> findSession(
        const std::string& sessionId) const = 0;

    [[nodiscard]] virtual common::Result<std::vector<AgentToolSession>> listForRun(
        const std::string& runId) const = 0;

    [[nodiscard]] virtual common::Result<AgentProviderTurn> insertTurn(
        AgentProviderTurn turn) = 0;

    [[nodiscard]] virtual common::Result<std::vector<AgentProviderTurn>> listTurns(
        const std::string& sessionId) const = 0;

    // Startup recovery: set state=Interrupted and finishedAt=now for every
    // row still in state=Active. Returns the count of rows updated.
    [[nodiscard]] virtual common::Result<std::size_t> interruptActiveSessions() = 0;
};

} // namespace ai::application
