#pragma once

#include "ai/agents/Agent.hpp"
#include "ai/agents/AgentContext.hpp"
#include "ai/agents/AgentResult.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ai::orchestration {

// Coordinates registered agents. It does not know provider internals; each Agent
// owns its provider dependency and returns a normalized AgentResult.
class Orchestrator {
public:
    void addAgent(std::shared_ptr<agents::Agent> agent);

    // First version of orchestration: send the same task to every agent concurrently.
    [[nodiscard]] std::vector<agents::AgentResult> executeParallel(const std::string& task);

    // Direct agent lookup for focused execution or future workflow steps.
    [[nodiscard]] agents::AgentResult executeOne(
        const std::string& agentId,
        const agents::AgentContext& context);

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<agents::Agent>> agents_;
    std::unordered_map<std::string, std::shared_ptr<agents::Agent>> agentsById_;
};

} // namespace ai::orchestration
