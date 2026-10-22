#include "ai/orchestration/Orchestrator.hpp"

#include <future>
#include <stdexcept>
#include <utility>

namespace ai::orchestration {

void Orchestrator::addAgent(std::shared_ptr<agents::Agent> agent)
{
    if (!agent) {
        throw std::invalid_argument("Orchestrator cannot add a null agent");
    }

    const std::string agentId = agent->config().id;
    if (agentId.empty()) {
        throw std::invalid_argument("Agent id cannot be empty");
    }

    std::lock_guard lock(mutex_);
    agentsById_[agentId] = agent;
    agents_.push_back(std::move(agent));
}

std::vector<agents::AgentResult> Orchestrator::executeParallel(const std::string& task)
{
    std::vector<std::shared_ptr<agents::Agent>> agentsSnapshot;
    {
        std::lock_guard lock(mutex_);
        // Copy the registered agents while locked, then release the lock before
        // executing work. This avoids holding the mutex during slow model calls.
        agentsSnapshot = agents_;
    }

    std::vector<std::future<agents::AgentResult>> futures;
    futures.reserve(agentsSnapshot.size());

    for (const auto& agent : agentsSnapshot) {
        agents::AgentContext context;
        context.task = task;
        // Each agent receives its own context instance for thread-safe execution.
        futures.push_back(agent->executeAsync(std::move(context)));
    }

    std::vector<agents::AgentResult> results;
    results.reserve(futures.size());

    for (auto& future : futures) {
        results.push_back(future.get());
    }

    return results;
}

agents::AgentResult Orchestrator::executeOne(
    const std::string& agentId,
    const agents::AgentContext& context)
{
    std::shared_ptr<agents::Agent> agent;
    {
        std::lock_guard lock(mutex_);
        // Look up under the lock, then execute after releasing it.
        const auto it = agentsById_.find(agentId);
        if (it != agentsById_.end()) {
            agent = it->second;
        }
    }

    if (!agent) {
        return agents::AgentResult {
            agentId,
            {},
            {},
            false,
            "Agent not found: " + agentId
        };
    }

    return agent->execute(context);
}

} // namespace ai::orchestration
