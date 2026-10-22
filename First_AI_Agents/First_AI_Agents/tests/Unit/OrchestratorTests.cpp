#include "ai/agents/Agent.hpp"
#include "ai/orchestration/ExecutionGraph.hpp"
#include "ai/orchestration/Orchestrator.hpp"
#include "ai/orchestration/Workflow.hpp"
#include "ai/providers/MockProvider.hpp"

#include <cassert>
#include <memory>

int runOrchestratorTests()
{
    auto provider = std::make_shared<ai::providers::MockProvider>();

    ai::agents::AgentConfig architectConfig;
    architectConfig.id = "architect";
    architectConfig.name = "Architect";
    architectConfig.generationOptions.model = "mock-architect";

    ai::agents::AgentConfig developerConfig;
    developerConfig.id = "developer";
    developerConfig.name = "Developer";
    developerConfig.generationOptions.model = "mock-developer";

    ai::orchestration::Orchestrator orchestrator;
    orchestrator.addAgent(std::make_shared<ai::agents::Agent>(architectConfig, provider));
    orchestrator.addAgent(std::make_shared<ai::agents::Agent>(developerConfig, provider));

    const auto parallelResults = orchestrator.executeParallel("Design a module");
    assert(parallelResults.size() == 2);
    assert(parallelResults[0].success);
    assert(parallelResults[1].success);

    ai::agents::AgentContext context;
    context.task = "Implement a module";
    const auto singleResult = orchestrator.executeOne("developer", context);
    assert(singleResult.success);
    assert(singleResult.agentId == "developer");

    const auto missingResult = orchestrator.executeOne("missing", context);
    assert(!missingResult.success);

    ai::orchestration::Workflow workflow;
    workflow.addStep({"plan", {"architect"}, {}});
    workflow.addStep({"build", {"developer"}, {"plan"}});
    assert(!workflow.empty());
    assert(workflow.findStep("build").has_value());

    ai::orchestration::ExecutionGraph graph;
    graph.addNode({"plan", {}});
    graph.addNode({"build", {"plan"}});
    const auto order = graph.executionOrder();
    assert(order.size() == 2);

    return 0;
}
