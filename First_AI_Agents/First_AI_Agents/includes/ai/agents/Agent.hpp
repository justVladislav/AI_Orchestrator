#pragma once

#include "ai/agents/AgentConfig.hpp"
#include "ai/agents/AgentContext.hpp"
#include "ai/agents/AgentResult.hpp"
#include "ai/providers/IModelProvider.hpp"

#include <future>
#include <memory>

namespace ai::agents {

class Agent {

public:

    Agent(AgentConfig config, std::shared_ptr<providers::IModelProvider> provider);

    [[nodiscard]] const AgentConfig& config() const noexcept;

    AgentResult execute(const AgentContext& context);
    std::future<AgentResult> executeAsync(AgentContext context);


private:
    AgentConfig config_;
    std::shared_ptr<providers::IModelProvider> provider_;
};

} // namespace ai::agents
