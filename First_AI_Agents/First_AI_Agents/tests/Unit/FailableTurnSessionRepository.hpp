#pragma once

// Test double: the in-memory tool-session repository, with a switch that makes the provider-turn insert fail
// the way a refusing store would. Everything else (sessions, listing, recovery) is the real in-memory behaviour.

#include "ai/application/agents/IAgentToolSessionRepository.hpp"
#include "ai/application/inmemory/InMemoryRepositories.hpp"

#include <string>
#include <utility>
#include <vector>

namespace ai::application::testing {

class FailableTurnSessionRepository final : public IAgentToolSessionRepository {
public:
    bool failTurnInserts {false};
    std::size_t refusedTurnInserts {0};

    common::Result<AgentToolSession> insertSession(AgentToolSession session) override
    {
        return inner_.insertSession(std::move(session));
    }
    common::Result<AgentToolSession> updateSession(AgentToolSession session) override
    {
        return inner_.updateSession(std::move(session));
    }
    common::Result<std::optional<AgentToolSession>> findSession(const std::string& sessionId) const override
    {
        return inner_.findSession(sessionId);
    }
    common::Result<std::vector<AgentToolSession>> listForRun(const std::string& runId) const override
    {
        return inner_.listForRun(runId);
    }
    common::Result<AgentProviderTurn> insertTurn(AgentProviderTurn turn) override
    {
        if (failTurnInserts) {
            ++refusedTurnInserts;
            return common::Result<AgentProviderTurn>::failure(
                {"persistence.insert_failed", "simulated refusal of the turn store"});
        }
        return inner_.insertTurn(std::move(turn));
    }
    common::Result<std::vector<AgentProviderTurn>> listTurns(const std::string& sessionId) const override
    {
        return inner_.listTurns(sessionId);
    }
    common::Result<std::size_t> interruptActiveSessions() override
    {
        return inner_.interruptActiveSessions();
    }

private:
    InMemoryAgentToolSessionRepository inner_;
};

} // namespace ai::application::testing
