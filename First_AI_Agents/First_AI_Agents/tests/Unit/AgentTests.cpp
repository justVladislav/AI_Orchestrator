#include "ai/agents/Agent.hpp"
#include "ai/providers/MockProvider.hpp"

#include <cassert>
#include <memory>
#include <string>

int runAgentTests()
{
    ai::agents::AgentConfig config;
    config.id = "agent-test";
    config.name = "Agent Test";
    config.role = "Test role";
    config.systemPrompt = "System prompt";
    config.generationOptions.model = "mock-agent-test";

    auto provider = std::make_shared<ai::providers::MockProvider>();
    ai::agents::Agent agent(config, provider);

    ai::agents::AgentContext context;
    context.task = "Check agent execution";

    const ai::agents::AgentResult result = agent.execute(context);
    assert(result.agentId == "agent-test");
    assert(result.success);
    assert(result.output.find("MockProvider") != std::string::npos);
    assert(result.output.find("Check agent execution") != std::string::npos);

    auto future = agent.executeAsync(context);
    const ai::agents::AgentResult asyncResult = future.get();
    assert(asyncResult.success);
    assert(asyncResult.agentId == "agent-test");

    return 0;
}
