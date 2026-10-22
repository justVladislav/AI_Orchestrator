#include "ai/orchestration/Workflow.hpp"

#include "ai/agents/Agent.hpp"
#include "ai/orchestration/ExecutionGraph.hpp"
#include "ai/orchestration/Orchestrator.hpp"
#include "ai/orchestration/ProjectContextBuilder.hpp"
#include "ai/providers/AnthropicProvider.hpp"
#include "ai/providers/OpenAIProvider.hpp"
#include "ai/storage/FileStorage.hpp"
#include "ai/storage/Workspace.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ai::orchestration {

namespace {

namespace fs = std::filesystem;

constexpr const char* architectNodeId = "ARCHITECT";
constexpr const char* implementationNodeId = "IMPLEMENTATION";
constexpr const char* documentationNodeId = "DOCUMENTATION";

constexpr const char* architectAgentId = "architect";
constexpr const char* implementationAgentId = "implementation";
constexpr const char* documentationAgentId = "documentation";

std::tm localTime(std::time_t value)
{
    std::tm result {};
#if defined(_WIN32)
    localtime_s(&result, &value);
#else
    localtime_r(&value, &result);
#endif
    return result;
}

std::string nowTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    const std::tm local = localTime(time);

    std::ostringstream output;
    output << std::put_time(&local, "%Y-%m-%dT%H:%M:%S");
    return output.str();
}

std::string makeRunId()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    const std::tm local = localTime(time);

    std::ostringstream output;
    output << "run_" << std::put_time(&local, "%Y-%m-%d_%H%M%S");
    return output.str();
}

void writeTextOrThrow(
    ai::storage::FileStorage& storage,
    const fs::path& path,
    const std::string& content)
{
    if (!storage.writeText(path, content)) {
        throw std::runtime_error("Failed to write file: " + path.string());
    }
}

void createRunDirectories(const fs::path& runRoot)
{
    for (const auto& directory : {
             runRoot / "input",
             runRoot / "architect",
             runRoot / "implementation",
             runRoot / "documentation"}) {
        fs::create_directories(directory);
    }
}

models::RequestTimeoutOptions makeTimeouts(
    std::chrono::seconds connectTimeout,
    std::chrono::seconds totalTimeout)
{
    models::RequestTimeoutOptions timeouts;
    timeouts.connectTimeout = std::chrono::duration_cast<std::chrono::milliseconds>(connectTimeout);
    timeouts.totalTimeout = std::chrono::duration_cast<std::chrono::milliseconds>(totalTimeout);
    return timeouts;
}

std::string readPromptOrFallback(
    const fs::path& projectRoot,
    const std::string& filename,
    const std::string& fallback)
{
    ai::storage::FileStorage storage;
    return storage.readText(projectRoot / "prompts" / filename).value_or(fallback);
}

agents::AgentConfig makeAgentConfig(
    std::string id,
    std::string name,
    std::string role,
    std::string systemPrompt,
    models::RequestTimeoutOptions timeouts,
    std::uint32_t maxTokens)
{
    models::GenerationOptions options;
    options.model = "provider-resolved";
    options.temperature = 0.1;
    options.maxOutputTokens = maxTokens;
    options.stream = false;
    options.timeouts = std::move(timeouts);
    options.metadata["workflow.stage"] = id;

    return {
        std::move(id),
        std::move(name),
        std::move(role),
        std::move(systemPrompt),
        std::move(options)
    };
}

ProjectContextSelection focusedContextSelection(FocusedScope scope)
{
    switch (scope) {
    case FocusedScope::ProjectContextCollection:
        return {
            "Project context collection and preparation",
            {
                "includes/ai/orchestration/ProjectContextBuilder.hpp",
                "sourceFiles/orchestration/ProjectContextBuilder.cpp",
                "includes/ai/storage/IStorage.hpp",
                "includes/ai/storage/FileStorage.hpp",
                "sourceFiles/storage/FileStorage.cpp",
                "includes/ai/storage/Workspace.hpp",
                "sourceFiles/storage/Workspace.cpp"
            },
            {
                "includes/ai/orchestration/Workflow.hpp",
                "includes/ai/orchestration/ExecutionGraph.hpp",
                "includes/ai/orchestration/Orchestrator.hpp",
                "includes/ai/agents/Agent.hpp",
                "includes/ai/providers/IModelProvider.hpp",
                "includes/ai/models/ModelRequest.hpp",
                "includes/ai/models/GenerationOptions.hpp"
            },
            {
                "Architect chooses one focused task.",
                "Implementation proposes work for the focused scope only.",
                "Workflow saves model output as reviewable artifacts and does not apply source changes."
            }
        };
    }

    return {};
}

ExecutionGraph createGraph()
{
    ExecutionGraph graph;

    graph.addNode({
        architectNodeId,
        {},
        "architect",
        "Architect",
        "Analyze focused project context and choose the next task.",
        "openai",
        "provider-resolved"
    });

    graph.addNode({
        implementationNodeId,
        {architectNodeId},
        "implementation",
        "Implementation",
        "Produce a reviewable implementation proposal for the architect task.",
        "anthropic",
        "provider-resolved"
    });

    graph.addNode({
        documentationNodeId,
        {implementationNodeId},
        "documentation",
        "Documentation",
        "Summarize the run and artifact locations.",
        "anthropic/openai",
        "provider-resolved"
    });

    return graph;
}

void persistGraphOrThrow(const ExecutionGraph& graph, const fs::path& runRoot)
{
    if (!graph.writeJson(runRoot / "execution_graph.json")) {
        throw std::runtime_error("Failed to write execution_graph.json");
    }
}

void registerAgents(
    Orchestrator& orchestrator,
    const OrchestrationConfig& config)
{
    orchestrator.addAgent(std::make_shared<agents::Agent>(
        makeAgentConfig(
            architectAgentId,
            "Architect",
            "Focused planning",
            readPromptOrFallback(
                config.projectRoot,
                "architect.md",
                "You are a software architect. Analyze the context and choose one focused task."),
            makeTimeouts(config.timeouts.connect, config.timeouts.focusedArchitectTotal),
            8000),
        std::make_shared<providers::OpenAIProvider>(config.focusedArchitectModel)));

    orchestrator.addAgent(std::make_shared<agents::Agent>(
        makeAgentConfig(
            implementationAgentId,
            "Implementation",
            "Focused implementation",
            readPromptOrFallback(
                config.projectRoot,
                "developer.md",
                "You are a senior C++20 developer. Produce a reviewable implementation proposal."),
            makeTimeouts(config.timeouts.connect, config.timeouts.focusedImplementationTotal),
            12000),
        std::make_shared<providers::AnthropicProvider>(config.focusedImplementationModel)));

    std::shared_ptr<providers::IModelProvider> documentationProvider;
    if (config.focusedDocumentationModel == providers::ModelTypeOpenAi54) {
        documentationProvider = std::make_shared<providers::OpenAIProvider>(config.focusedDocumentationModel);
    } else {
        documentationProvider = std::make_shared<providers::AnthropicProvider>(config.focusedDocumentationModel);
    }

    orchestrator.addAgent(std::make_shared<agents::Agent>(
        makeAgentConfig(
            documentationAgentId,
            "Documentation",
            "Run documentation",
            readPromptOrFallback(
                config.projectRoot,
                "documentation.md",
                "You are a technical writer. Produce concise and accurate run documentation."),
            makeTimeouts(config.timeouts.connect, config.timeouts.focusedDocumentationTotal),
            8000),
        std::move(documentationProvider)));
}

std::string buildArchitectTask(const ProjectContext& context)
{
    std::ostringstream task;
    task << "Goal: choose exactly one focused implementation task for the supplied project context.\n\n";
    task << "Project manifest:\n" << context.manifest << "\n";
    task << "Project context:\n" << context.context;
    return task.str();
}

std::string buildImplementationTask(
    const ProjectContext& context,
    const std::string& architectOutput)
{
    std::ostringstream task;
    task << "Architect result:\n" << architectOutput << "\n\n";
    task << "Use the focused context below to produce a reviewable implementation proposal.\n";
    task << "Do not apply files directly; describe the proposed source changes clearly.\n\n";
    task << "Focused context:\n" << context.context;
    return task.str();
}

std::string buildDocumentationTask(
    const fs::path& runRoot,
    const std::string& architectOutput,
    const std::string& implementationOutput)
{
    std::ostringstream task;
    task << "Create WORKFLOW_REPORT.md for this sequential run.\n\n";
    task << "Artifact root: " << runRoot.generic_string() << "\n";
    task << "Architect artifact: architect/architecture_plan.md\n";
    task << "Implementation artifact: implementation/implementation_proposal.md\n\n";
    task << "Architect output:\n" << architectOutput << "\n\n";
    task << "Implementation output:\n" << implementationOutput << "\n";
    return task.str();
}

agents::AgentResult runStage(
    Orchestrator& orchestrator,
    ExecutionGraph& graph,
    ai::storage::FileStorage& storage,
    const fs::path& runRoot,
    const std::string& nodeId,
    const std::string& agentId,
    const std::string& task,
    const fs::path& relativeOutputPath)
{
    graph.setStatus(nodeId, ExecutionStatus::Running, nowTimestamp());
    persistGraphOrThrow(graph, runRoot);

    agents::AgentContext context;
    context.task = task;

    agents::AgentResult result = orchestrator.executeOne(agentId, context);
    ExecutionNode* node = graph.findNode(nodeId);
    if (node != nullptr) {
        node->outputPath = relativeOutputPath.generic_string();
    }

    if (result.success) {
        writeTextOrThrow(storage, runRoot / relativeOutputPath, result.output);
        graph.setStatus(nodeId, ExecutionStatus::Completed, nowTimestamp());
    } else {
        const fs::path errorPath = relativeOutputPath.parent_path() / "error.txt";
        writeTextOrThrow(storage, runRoot / errorPath, result.errorMessage);
        if (node != nullptr) {
            node->outputPath = errorPath.generic_string();
        }
        graph.setStatus(nodeId, ExecutionStatus::Failed, nowTimestamp(), result.errorMessage);
    }

    persistGraphOrThrow(graph, runRoot);
    return result;
}

nlohmann::json makeRunSummary(
    const std::string& runId,
    const std::string& startedAt,
    const std::string& finishedAt,
    const std::string& status,
    const fs::path& runRoot,
    const ExecutionGraph& graph)
{
    return {
        {"run_id", runId},
        {"mode", "focused_sequential"},
        {"started_at", startedAt},
        {"finished_at", finishedAt},
        {"status", status},
        {"stage_status", {
            {"architect", toString(graph.findNode(architectNodeId)->status)},
            {"implementation", toString(graph.findNode(implementationNodeId)->status)},
            {"documentation", toString(graph.findNode(documentationNodeId)->status)}
        }},
        {"artifacts", {
            {"project_manifest", (runRoot / "input" / "project_manifest.txt").generic_string()},
            {"project_context", (runRoot / "input" / "project_context.txt").generic_string()},
            {"architect_output", (runRoot / "architect" / "architecture_plan.md").generic_string()},
            {"implementation_output", (runRoot / "implementation" / "implementation_proposal.md").generic_string()},
            {"documentation_report", (runRoot / "documentation" / "WORKFLOW_REPORT.md").generic_string()},
            {"execution_graph", (runRoot / "execution_graph.json").generic_string()},
            {"run_summary", (runRoot / "run_summary.json").generic_string()}
        }}
    };
}

OrchestrationResult runFocusedSequential(const OrchestrationConfig& config)
{
    const std::string startedAt = nowTimestamp();
    const std::string runId = makeRunId();

    ai::storage::Workspace workspace(config.workspaceRoot);
    if (!workspace.ensure()) {
        throw std::runtime_error("Failed to prepare workspace root.");
    }

    const fs::path runRoot = workspace.runsDir() / runId;
    createRunDirectories(runRoot);

    ai::storage::FileStorage storage;
    ExecutionGraph graph = createGraph();
    persistGraphOrThrow(graph, runRoot);

    const ProjectContext context =
        ProjectContextBuilder(config.projectRoot).buildSelected(focusedContextSelection(config.focusedScope));
    writeTextOrThrow(storage, runRoot / "input" / "project_manifest.txt", context.manifest);
    writeTextOrThrow(storage, runRoot / "input" / "project_context.txt", context.context);

    Orchestrator orchestrator;
    registerAgents(orchestrator, config);

    const agents::AgentResult architect = runStage(
        orchestrator,
        graph,
        storage,
        runRoot,
        architectNodeId,
        architectAgentId,
        buildArchitectTask(context),
        fs::path("architect") / "architecture_plan.md");
    if (!architect.success) {
        const nlohmann::json summary = makeRunSummary(
            runId,
            startedAt,
            nowTimestamp(),
            "failed",
            runRoot,
            graph);
        writeTextOrThrow(storage, runRoot / "run_summary.json", summary.dump(2));
        return {false, "failed", architect.errorMessage, runId, runRoot};
    }

    const agents::AgentResult implementation = runStage(
        orchestrator,
        graph,
        storage,
        runRoot,
        implementationNodeId,
        implementationAgentId,
        buildImplementationTask(context, architect.output),
        fs::path("implementation") / "implementation_proposal.md");
    if (!implementation.success) {
        const nlohmann::json summary = makeRunSummary(
            runId,
            startedAt,
            nowTimestamp(),
            "failed",
            runRoot,
            graph);
        writeTextOrThrow(storage, runRoot / "run_summary.json", summary.dump(2));
        return {false, "failed", implementation.errorMessage, runId, runRoot};
    }

    const agents::AgentResult documentation = runStage(
        orchestrator,
        graph,
        storage,
        runRoot,
        documentationNodeId,
        documentationAgentId,
        buildDocumentationTask(runRoot, architect.output, implementation.output),
        fs::path("documentation") / "WORKFLOW_REPORT.md");

    const bool success = documentation.success;
    const std::string status = success ? "completed" : "failed";
    const nlohmann::json summary = makeRunSummary(
        runId,
        startedAt,
        nowTimestamp(),
        status,
        runRoot,
        graph);
    writeTextOrThrow(storage, runRoot / "run_summary.json", summary.dump(2));

    return {
        success,
        status,
        success ? "Focused sequential workflow completed." : documentation.errorMessage,
        runId,
        runRoot
    };
}

} // namespace

Workflow::Workflow(OrchestrationConfig config)
    : config_(std::move(config))
{
}

void Workflow::addStep(WorkflowStep step)
{
    steps_.push_back(std::move(step));
}

bool Workflow::empty() const noexcept
{
    return steps_.empty();
}

const std::vector<WorkflowStep>& Workflow::steps() const noexcept
{
    return steps_;
}

std::optional<WorkflowStep> Workflow::findStep(const std::string& id) const
{
    const auto it = std::find_if(steps_.begin(), steps_.end(), [&id](const WorkflowStep& step) {
        return step.id == id;
    });

    if (it == steps_.end()) {
        return std::nullopt;
    }

    return *it;
}

OrchestrationResult Workflow::run() const
{
    if (!config_.has_value()) {
        throw std::logic_error("Workflow::run requires OrchestrationConfig.");
    }

    return runFocusedSequential(*config_);
}

} // namespace ai::orchestration
