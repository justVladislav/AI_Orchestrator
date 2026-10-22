#include "ai/orchestration/Workflow.hpp"

#include "ai/agents/Agent.hpp"
#include "ai/orchestration/ExecutionGraph.hpp"
#include "ai/orchestration/ProjectContextBuilder.hpp"
#include "ai/providers/AnthropicProvider.hpp"
#include "ai/providers/OpenAIProvider.hpp"
#include "ai/storage/FileStorage.hpp"
#include "ai/storage/Workspace.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <future>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ai::orchestration {

namespace {

namespace fs = std::filesystem;

constexpr const char* architectNodeId = "ARCHITECT";
constexpr const char* openaiNodeId = "OPENAI-REFACTOR";
constexpr const char* anthropicNodeId = "ANTHROPIC-REFACTOR";
constexpr const char* focusedImplementationNodeId = "IMPLEMENTATION";
constexpr const char* documentationNodeId = "DOCUMENTATION";

constexpr const char* architectModel = "gpt-5.6-sol";
constexpr const char* openaiRefactoringModel = "gpt-5.6-terra";
constexpr const char* anthropicRefactoringModel = "claude-opus-4-8";
constexpr const char* documentationModel = "claude-haiku-4-5-20251001";

struct BranchRunResult {
    std::string branchName;
    bool success {false};
    std::string errorMessage;
    nlohmann::json resultJson {nlohmann::json::object()};
    std::vector<std::string> generatedFiles;
    std::size_t plannedTasks {0};
    std::size_t completedTasks {0};
    std::size_t skippedTasks {0};
    std::size_t failedTasks {0};
};

struct DocumentationRunResult {
    bool success {false};
    std::string errorMessage;
    nlohmann::json resultJson {nlohmann::json::object()};
};

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

std::string envOrEmpty(const char* name)
{
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return {};
    }

    std::string result {value};
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value == nullptr ? std::string {} : std::string {value};
#endif
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

bool isSafeRelativePathText(const std::string& pathText)
{
    if (pathText.empty()) {
        return false;
    }

    const fs::path path {pathText};
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return false;
    }

    for (const auto& part : path) {
        const std::string component = part.string();
        if (component == "..") {
            return false;
        }
    }

    return true;
}

std::string normalizeRelativePathText(const std::string& pathText)
{
    if (!isSafeRelativePathText(pathText)) {
        throw std::runtime_error("Unsafe relative path rejected: " + pathText);
    }

    return fs::path(pathText).lexically_normal().generic_string();
}

void createRunDirectories(const fs::path& runRoot)
{
    // Branches write to separate directories so parallel agents cannot race on
    // the same artifact path. Source proposals stay review-only in Workspace.
    for (const auto& directory : {
             runRoot / "input",
             runRoot / "architect",
             runRoot / "openai_refactoring" / "files",
             runRoot / "anthropic_refactoring" / "files",
             runRoot / "documentation"}) {
        fs::create_directories(directory);
    }
}

agents::AgentConfig makeAgentConfig(
    std::string id,
    std::string name,
    std::string role,
    std::string systemPrompt,
    std::string model,
    std::string stageName,
    models::RequestTimeoutOptions timeouts,
    std::uint32_t maxTokens = 8192)
{
    models::GenerationOptions options;
    options.model = std::move(model);
    options.temperature = 0.1;
    options.maxOutputTokens = maxTokens;
    options.stream = false;
    options.timeouts = std::move(timeouts);
    options.metadata["workflow.stage"] = std::move(stageName);

    return {
        std::move(id),
        std::move(name),
        std::move(role),
        std::move(systemPrompt),
        std::move(options)
    };
}

models::RequestTimeoutOptions makeWorkflowTimeouts(
    std::chrono::seconds connectTimeout,
    std::chrono::seconds totalTimeout)
{
    models::RequestTimeoutOptions timeouts;
    timeouts.connectTimeout = std::chrono::duration_cast<std::chrono::milliseconds>(connectTimeout);
    timeouts.totalTimeout = std::chrono::duration_cast<std::chrono::milliseconds>(totalTimeout);
    return timeouts;
}

ExecutionGraph createInitialGraph()
{
    ExecutionGraph graph;

    graph.addNode({
        architectNodeId,
        {},
        "architect",
        "Architect",
        "Analyze the project and produce the bounded execution plan.",
        "openai",
        architectModel
    });

    graph.addNode({
        openaiNodeId,
        {architectNodeId},
        "coding",
        "OpenAI Refactoring",
        "Generate reviewable OpenAI-related source proposals.",
        "openai",
        openaiRefactoringModel
    });

    graph.addNode({
        anthropicNodeId,
        {architectNodeId},
        "coding",
        "Anthropic Refactoring",
        "Generate reviewable Anthropic-related source proposals.",
        "anthropic",
        anthropicRefactoringModel
    });

    graph.addNode({
        documentationNodeId,
        {openaiNodeId, anthropicNodeId},
        "documentation",
        "Documentation",
        "Document the run and the generated Workspace proposals.",
        "anthropic",
        documentationModel
    });

    return graph;
}

void persistGraphOrThrow(const ExecutionGraph& graph, const fs::path& runRoot)
{
    if (!graph.writeJson(runRoot / "execution_graph.json")) {
        throw std::runtime_error("Failed to write execution_graph.json");
    }
}

std::string architectSystemPrompt()
{
    return R"(You are the lead software architect for a C++ multi-provider AI orchestration project.

Analyze the supplied project as a complete system.

Identify:
1. architectural weaknesses;
2. duplicated responsibilities;
3. unsafe ownership or lifetime behavior;
4. poor interfaces;
5. error-handling gaps;
6. concurrency risks;
7. provider-specific duplication;
8. Workflow and ExecutionGraph weaknesses;
9. Workspace and artifact-management weaknesses;
10. documentation gaps;
11. small improvements that can be implemented safely now;
12. larger ideas that should be deferred.

Do not rewrite the project.

Create a bounded execution plan for exactly two parallel coding agents:
- one agent works only on OpenAI-related files;
- one agent works only on Anthropic-related files.

The two branches must not modify the same file. Shared infrastructure changes must not be assigned to both branches.
For this first run, shared or cross-cutting improvements should be recorded as deferred backlog items unless they can be assigned safely to one branch.

Prefer small, reviewable improvements. Assign no more than 3 tasks to each branch.

Do not include generated files, build files, secrets, or Workspace outputs.

Return strict JSON only, with these top-level fields:
The first character of your response must be `{` and the last character must be `}`.
Do not include Markdown fences, explanations, greetings, notes, or any text before or after the JSON object.
project_summary, architecture_findings, openai_tasks, anthropic_tasks, deferred_tasks.)";
}

std::string buildArchitectTask(const ProjectContext& context)
{
    std::ostringstream task;
    task << "Project manifest:\n";
    task << context.manifest << "\n";
    task << "Goal:\n";
    task << "Create the first controlled orchestration pipeline: Architect -> "
         << "OpenAI refactoring || Anthropic refactoring -> Documentation.\n\n";
    task << "First-run constraints:\n";
    task << "- coding branches must create reviewable full-file proposals in Workspace only;\n";
    task << "- original source files must not be edited automatically by agents;\n";
    task << "- OpenAI branch and Anthropic branch must not modify the same file;\n";
    task << "- shared or cross-cutting ideas should usually be deferred;\n";
    task << "- return strict JSON only; first character must be '{', last character must be '}'; no Markdown fences or text before/after JSON.\n\n";
    task << "Expected JSON schema:\n";
    task << R"({
  "project_summary": "Short factual project summary",
  "architecture_findings": [
    {
      "id": "ARCH-001",
      "severity": "low|medium|high",
      "title": "Finding title",
      "description": "Finding description",
      "related_files": ["relative/path/File.cpp"]
    }
  ],
  "openai_tasks": [
    {
      "id": "OPENAI-001",
      "title": "Task title",
      "description": "Exact requested change",
      "files": ["relative/path/OpenAIFile.cpp"],
      "acceptance_criteria": ["Criterion 1"],
      "priority": 1
    }
  ],
  "anthropic_tasks": [
    {
      "id": "ANTHROPIC-001",
      "title": "Task title",
      "description": "Exact requested change",
      "files": ["relative/path/AnthropicFile.cpp"],
      "acceptance_criteria": ["Criterion 1"],
      "priority": 1
    }
  ],
  "deferred_tasks": [
    {
      "id": "DEFERRED-001",
      "title": "Cross-cutting improvement",
      "reason": "Why it should not be executed in parallel now",
      "related_files": []
    }
  ]
})";
    task << "\n\nProject context:\n";
    task << context.context;
    return task.str();
}

nlohmann::json parseStrictJson(const std::string& text, const std::string& stageName)
{
    try {
        return nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception& ex) {
        throw std::runtime_error(stageName + " did not return valid strict JSON: " + ex.what());
    }
}

void ensureArrayField(nlohmann::json& value, const char* fieldName)
{
    if (!value.contains(fieldName)) {
        value[fieldName] = nlohmann::json::array();
        return;
    }

    if (!value[fieldName].is_array()) {
        throw std::runtime_error(std::string {"Architect field is not an array: "} + fieldName);
    }
}

void moveExtraTasksToDeferred(nlohmann::json& plan, const char* branchKey, const std::string& branchName)
{
    auto& tasks = plan[branchKey];
    if (tasks.size() <= 3) {
        return;
    }

    auto& deferred = plan["deferred_tasks"];
    nlohmann::json kept = nlohmann::json::array();

    for (std::size_t index = 0; index < tasks.size(); ++index) {
        if (index < 3) {
            kept.push_back(tasks[index]);
            continue;
        }

        deferred.push_back({
            {"id", tasks[index].value("id", branchName + "-DEFERRED-" + std::to_string(index + 1))},
            {"title", tasks[index].value("title", "Extra " + branchName + " task")},
            {"reason", "Only the first three branch tasks are executable in the initial pipeline."},
            {"related_files", tasks[index].value("files", nlohmann::json::array())}
        });
    }

    tasks = std::move(kept);
}

nlohmann::json normalizeArchitectPlan(nlohmann::json plan)
{
    if (!plan.is_object()) {
        throw std::runtime_error("Architect plan root is not a JSON object.");
    }

    if (!plan.contains("project_summary") || !plan["project_summary"].is_string()) {
        plan["project_summary"] = "";
    }

    ensureArrayField(plan, "architecture_findings");
    ensureArrayField(plan, "openai_tasks");
    ensureArrayField(plan, "anthropic_tasks");
    ensureArrayField(plan, "deferred_tasks");

    moveExtraTasksToDeferred(plan, "openai_tasks", "OPENAI");
    moveExtraTasksToDeferred(plan, "anthropic_tasks", "ANTHROPIC");

    return plan;
}

std::set<std::string> collectTaskFiles(const nlohmann::json& tasks)
{
    std::set<std::string> files;
    if (!tasks.is_array()) {
        return files;
    }

    for (const auto& task : tasks) {
        const auto fileIt = task.find("files");
        if (fileIt == task.end() || !fileIt->is_array()) {
            continue;
        }

        for (const auto& file : *fileIt) {
            if (!file.is_string()) {
                throw std::runtime_error("Architect task file path is not a string.");
            }
            files.insert(normalizeRelativePathText(file.get<std::string>()));
        }
    }

    return files;
}

void validateNoBranchFileOverlap(const nlohmann::json& plan)
{
    const std::set<std::string> openaiFiles = collectTaskFiles(plan.at("openai_tasks"));
    const std::set<std::string> anthropicFiles = collectTaskFiles(plan.at("anthropic_tasks"));

    std::vector<std::string> overlap;
    std::set_intersection(
        openaiFiles.begin(),
        openaiFiles.end(),
        anthropicFiles.begin(),
        anthropicFiles.end(),
        std::back_inserter(overlap));

    if (!overlap.empty()) {
        std::ostringstream error;
        error << "Architect assigned the same file to both branches:";
        for (const auto& file : overlap) {
            error << ' ' << file;
        }
        throw std::runtime_error(error.str());
    }
}

std::string architectureReviewMarkdown(const nlohmann::json& plan)
{
    std::ostringstream review;
    review << "# Architecture Review\n\n";
    review << "## Project Summary\n\n";
    review << plan.value("project_summary", "") << "\n\n";

    review << "## Findings\n\n";
    for (const auto& finding : plan.at("architecture_findings")) {
        review << "- " << finding.value("id", "ARCH")
               << " [" << finding.value("severity", "unknown") << "] "
               << finding.value("title", "Untitled") << ": "
               << finding.value("description", "") << "\n";
    }

    review << "\n## OpenAI Tasks\n\n";
    for (const auto& task : plan.at("openai_tasks")) {
        review << "- " << task.value("id", "OPENAI")
               << ": " << task.value("title", "Untitled") << "\n";
    }

    review << "\n## Anthropic Tasks\n\n";
    for (const auto& task : plan.at("anthropic_tasks")) {
        review << "- " << task.value("id", "ANTHROPIC")
               << ": " << task.value("title", "Untitled") << "\n";
    }

    review << "\n## Deferred Tasks\n\n";
    for (const auto& task : plan.at("deferred_tasks")) {
        review << "- " << task.value("id", "DEFERRED")
               << ": " << task.value("title", "Untitled") << "\n";
    }

    return review.str();
}

std::string readFileBlock(
    const fs::path& projectRoot,
    const std::string& relativePath,
    const std::string& mode)
{
    ai::storage::FileStorage storage;
    std::ostringstream block;

    const std::string normalized = normalizeRelativePathText(relativePath);
    const fs::path absolutePath = projectRoot / fs::path(normalized);
    block << "===== " << mode << " FILE: " << normalized << " =====\n\n";

    const auto content = storage.readText(absolutePath);
    if (content.has_value()) {
        block << *content;
    } else {
        block << "[File not found in the current source tree.]";
    }

    block << "\n\n===== END FILE: " << normalized << " =====\n\n";
    return block.str();
}

std::vector<std::string> commonReadOnlyFiles()
{
    return {
        "includes/ai/providers/IModelProvider.hpp",
        "includes/ai/providers/ModelType.hpp",
        "includes/ai/models/GenerationOptions.hpp",
        "includes/ai/models/ModelRequest.hpp",
        "includes/ai/models/ModelResponse.hpp",
        "includes/ai/models/Message.hpp"
    };
}

std::string branchSystemPrompt(const std::string& branchName)
{
    std::ostringstream prompt;
    prompt << "You are a focused C++20 coding agent for the " << branchName << " branch.\n\n";
    prompt << "Return strict JSON only. The first character of your response must be '{' and the last character must be '}'.\n";
    prompt << "Do not include Markdown fences, explanations, greetings, notes, or any text before or after the JSON object.\n";
    prompt << "You must only propose full-file contents for files assigned to your branch.\n";
    prompt << "Do not modify Main.cpp, Workflow, ExecutionGraph, Workspace, API keys, endpoints, model selection, CMake dependencies, or the other provider branch.\n";
    prompt << "Do not return unified diffs. Each returned file content must be the complete proposed file.\n";
    prompt << "Generated files are proposals for Workspace review only; they are not applied to the source tree automatically.";
    return prompt.str();
}

std::string buildBranchTask(
    const fs::path& projectRoot,
    const std::string& branchName,
    const nlohmann::json& tasks,
    const std::set<std::string>& editableFiles)
{
    std::ostringstream task;
    task << "Branch: " << branchName << "\n\n";
    task << "Assigned tasks JSON:\n" << tasks.dump(2) << "\n\n";
    task << "Required response schema:\n";
    task << R"({
  "completed_tasks": ["TASK-ID"],
  "skipped_tasks": [
    {
      "id": "TASK-ID",
      "reason": "Reason"
    }
  ],
  "summary": "Short description of completed work",
  "files": [
    {
      "path": "relative/path/File.cpp",
      "action": "modify",
      "content": "Complete updated file content"
    }
  ],
  "notes": ["Important implementation note"]
})";
    task << "\n\nEditable files:\n";
    for (const auto& file : editableFiles) {
        task << "- " << file << "\n";
    }

    task << "\nEditable file contents:\n";
    for (const auto& file : editableFiles) {
        task << readFileBlock(projectRoot, file, "EDITABLE");
    }

    task << "\nRead-only shared interfaces:\n";
    for (const auto& file : commonReadOnlyFiles()) {
        if (!editableFiles.contains(file)) {
            task << readFileBlock(projectRoot, file, "READ-ONLY");
        }
    }

    return task.str();
}

void validateRequiredArray(const nlohmann::json& value, const char* fieldName)
{
    if (!value.contains(fieldName) || !value.at(fieldName).is_array()) {
        throw std::runtime_error(std::string {"Coding response missing array field: "} + fieldName);
    }
}

void validateBranchResponse(
    const nlohmann::json& response,
    const std::set<std::string>& allowedFiles)
{
    if (!response.is_object()) {
        throw std::runtime_error("Coding response root is not an object.");
    }

    validateRequiredArray(response, "completed_tasks");
    validateRequiredArray(response, "skipped_tasks");
    validateRequiredArray(response, "files");

    if (!response.contains("summary") || !response.at("summary").is_string()) {
        throw std::runtime_error("Coding response missing string field: summary");
    }

    std::unordered_set<std::string> returnedPaths;
    for (const auto& file : response.at("files")) {
        if (!file.is_object()
            || !file.contains("path")
            || !file.at("path").is_string()
            || !file.contains("action")
            || !file.at("action").is_string()
            || !file.contains("content")
            || !file.at("content").is_string()) {
            throw std::runtime_error("Coding response file entry is incomplete.");
        }

        const std::string path = normalizeRelativePathText(file.at("path").get<std::string>());
        const std::string action = file.at("action").get<std::string>();
        const std::string content = file.at("content").get<std::string>();

        if (action != "modify" && action != "create") {
            throw std::runtime_error("Coding response contains unsupported file action: " + action);
        }

        if (content.empty()) {
            throw std::runtime_error("Coding response file content is empty: " + path);
        }

        if (!allowedFiles.contains(path)) {
            throw std::runtime_error("Coding response tried to write an unassigned file: " + path);
        }

        if (!returnedPaths.insert(path).second) {
            throw std::runtime_error("Coding response returned the same file more than once: " + path);
        }
    }
}

std::vector<std::string> saveBranchFiles(
    const nlohmann::json& response,
    const fs::path& branchRoot)
{
    ai::storage::FileStorage storage;
    std::vector<std::string> generatedFiles;

    for (const auto& file : response.at("files")) {
        const std::string relativePath = normalizeRelativePathText(file.at("path").get<std::string>());
        const fs::path outputPath = branchRoot / "files" / fs::path(relativePath);

        // Full file content is saved with the original extension so the user can
        // review or diff proposed .cpp/.hpp changes before applying anything.
        writeTextOrThrow(storage, outputPath, file.at("content").get<std::string>());
        generatedFiles.push_back((fs::path("files") / fs::path(relativePath)).generic_string());
    }

    return generatedFiles;
}

void writeBranchFailure(
    const fs::path& branchRoot,
    const std::string& branchName,
    const std::string& provider,
    const std::string& model,
    const std::string& errorMessage)
{
    ai::storage::FileStorage storage;
    const nlohmann::json failure = {
        {"success", false},
        {"branch", branchName},
        {"provider", provider},
        {"model", model},
        {"error", errorMessage}
    };

    storage.writeText(branchRoot / "result.json", failure.dump(2));
    storage.writeText(
        branchRoot / "summary.md",
        "# " + branchName + " Branch\n\nStatus: failed\n\nError: " + errorMessage + "\n");
}

BranchRunResult runCodingBranch(
    const fs::path& projectRoot,
    const fs::path& branchRoot,
    const std::string& branchName,
    const std::string& providerName,
    const std::string& modelName,
    providers::ModelType modelType,
    models::RequestTimeoutOptions timeouts,
    const nlohmann::json& tasks)
{
    BranchRunResult result;
    result.branchName = branchName;
    result.plannedTasks = tasks.is_array() ? tasks.size() : 0;

    try {
        const std::set<std::string> editableFiles = collectTaskFiles(tasks);

        std::shared_ptr<providers::IModelProvider> provider;
        if (providerName == "openai") {
            provider = std::make_shared<providers::OpenAIProvider>(modelType);
        } else {
            provider = std::make_shared<providers::AnthropicProvider>(modelType);
        }

        agents::Agent agent(
            makeAgentConfig(
                branchName + "-agent",
                branchName + " Agent",
                "Focused C++ refactoring",
                branchSystemPrompt(branchName),
                modelName,
                branchName + " refactoring",
                std::move(timeouts),
                12000),
            std::move(provider));

        agents::AgentContext context;
        context.task = buildBranchTask(projectRoot, branchName, tasks, editableFiles);

        const agents::AgentResult agentResult = agent.execute(context);
        if (!agentResult.success) {
            throw std::runtime_error(agentResult.errorMessage);
        }

        ai::storage::FileStorage storage;
        writeTextOrThrow(storage, branchRoot / "raw_response.txt", agentResult.output);
       

        nlohmann::json response = parseStrictJson(agentResult.output, branchName);
        validateBranchResponse(response, editableFiles);
      
        result.generatedFiles = saveBranchFiles(response, branchRoot);
        writeTextOrThrow(storage, branchRoot / "result.json", response.dump(2));
        writeTextOrThrow(
            storage,
            branchRoot / "summary.md",
            "# " + branchName + " Branch\n\n" + response.value("summary", "") + "\n");

        result.completedTasks = response.at("completed_tasks").size();
        result.skippedTasks = response.at("skipped_tasks").size();
        result.failedTasks = 0;
        result.resultJson = std::move(response);
        result.success = true;
    } catch (const std::exception& ex) {
        result.errorMessage = ex.what();
        result.failedTasks = result.plannedTasks;
        result.success = false;
        result.resultJson = {
            {"success", false},
            {"branch", branchName},
            {"provider", providerName},
            {"model", modelName},
            {"error", result.errorMessage}
        };
        writeBranchFailure(branchRoot, branchName, providerName, modelName, result.errorMessage);
    }

    return result;
}

std::string documentationSystemPrompt()
{
    return R"(You are the documentation agent for a C++ multi-provider AI orchestration project.

Using the original project information, architect plan, and both coding-agent results, create accurate project documentation.

Do not claim that proposed Workspace changes have already been applied to the source repository.

Clearly distinguish:
- current source state;
- proposed changes generated in Workspace;
- successfully completed agent tasks;
- failed or skipped tasks;
- deferred architectural recommendations.

Produce exactly two files:
1. README.md
2. EXECUTION_REPORT.md

Return strict JSON only using the requested file-output schema.
The first character of your response must be `{` and the last character must be `}`.
Do not include Markdown fences, explanations, greetings, notes, or any text before or after the JSON object.)";
}

std::string buildDocumentationTask(
    const nlohmann::json& architectPlan,
    const BranchRunResult& openaiResult,
    const BranchRunResult& anthropicResult,
    const std::string& readme,
    const std::string& codexContext)
{
    std::ostringstream task;
    task << "Expected JSON schema:\n";
    task << R"({
  "summary": "Documentation generation summary",
  "files": [
    {
      "path": "README.md",
      "action": "create",
      "content": "Complete README content"
    },
    {
      "path": "EXECUTION_REPORT.md",
      "action": "create",
      "content": "Complete execution report"
    }
  ]
})";

    task << "\n\nArchitect plan:\n" << architectPlan.dump(2) << "\n\n";
    task << "OpenAI branch result:\n" << openaiResult.resultJson.dump(2) << "\n\n";
    task << "OpenAI generated files:\n" << nlohmann::json(openaiResult.generatedFiles).dump(2) << "\n\n";
    task << "Anthropic branch result:\n" << anthropicResult.resultJson.dump(2) << "\n\n";
    task << "Anthropic generated files:\n" << nlohmann::json(anthropicResult.generatedFiles).dump(2) << "\n\n";
    task << "Existing README.md:\n" << readme << "\n\n";
    task << "Relevant CODEX_CONTEXT.md:\n" << codexContext << "\n";
    return task.str();
}

void validateDocumentationResponse(const nlohmann::json& response)
{
    if (!response.is_object()
        || !response.contains("summary")
        || !response.at("summary").is_string()
        || !response.contains("files")
        || !response.at("files").is_array()) {
        throw std::runtime_error("Documentation response does not match the expected object schema.");
    }

    std::set<std::string> expected {"README.md", "EXECUTION_REPORT.md"};
    std::set<std::string> seen;

    for (const auto& file : response.at("files")) {
        if (!file.is_object()
            || !file.contains("path")
            || !file.at("path").is_string()
            || !file.contains("action")
            || !file.at("action").is_string()
            || !file.contains("content")
            || !file.at("content").is_string()) {
            throw std::runtime_error("Documentation response file entry is incomplete.");
        }

        const std::string path = normalizeRelativePathText(file.at("path").get<std::string>());
        if (!expected.contains(path)) {
            throw std::runtime_error("Documentation tried to write an unexpected file: " + path);
        }

        if (file.at("action").get<std::string>() != "create") {
            throw std::runtime_error("Documentation file action must be create: " + path);
        }

        if (file.at("content").get<std::string>().empty()) {
            throw std::runtime_error("Documentation file content is empty: " + path);
        }

        seen.insert(path);
    }

    if (seen != expected) {
        throw std::runtime_error("Documentation response must contain README.md and EXECUTION_REPORT.md.");
    }
}

DocumentationRunResult runDocumentationAgent(
    const fs::path& projectRoot,
    const fs::path& documentationRoot,
    const nlohmann::json& architectPlan,
    const BranchRunResult& openaiResult,
    const BranchRunResult& anthropicResult,
    models::RequestTimeoutOptions timeouts)
{
    DocumentationRunResult result;

    try {
        ai::storage::FileStorage storage;
        const std::string readme = storage.readText(projectRoot / "README.md").value_or({});
        const std::string codexContext = storage.readText(projectRoot / "CODEX_CONTEXT.md").value_or({});

        auto provider = std::make_shared<providers::AnthropicProvider>(providers::ModelTypeClaudeHaiku);
        agents::Agent agent(
            makeAgentConfig(
                "documentation-agent",
                "Documentation Agent",
                "Run documentation",
                documentationSystemPrompt(),
                documentationModel,
                "Documentation",
                std::move(timeouts),
                12000),
            std::move(provider));

        agents::AgentContext context;
        context.task = buildDocumentationTask(
            architectPlan,
            openaiResult,
            anthropicResult,
            readme,
            codexContext);

        const agents::AgentResult agentResult = agent.execute(context);
        if (!agentResult.success) {
            throw std::runtime_error(agentResult.errorMessage);
        }

        writeTextOrThrow(storage, documentationRoot / "raw_response.txt", agentResult.output);

        nlohmann::json response = parseStrictJson(agentResult.output, "Documentation");
        validateDocumentationResponse(response);

        writeTextOrThrow(storage, documentationRoot / "result.json", response.dump(2));
        for (const auto& file : response.at("files")) {
            const std::string path = normalizeRelativePathText(file.at("path").get<std::string>());
            writeTextOrThrow(storage, documentationRoot / path, file.at("content").get<std::string>());
        }

        result.success = true;
        result.resultJson = std::move(response);
    } catch (const std::exception& ex) {
        result.success = false;
        result.errorMessage = ex.what();
        result.resultJson = {
            {"success", false},
            {"error", result.errorMessage}
        };

        ai::storage::FileStorage storage;
        storage.writeText(documentationRoot / "result.json", result.resultJson.dump(2));
    }

    return result;
}

std::string statusOf(const ExecutionGraph& graph, const std::string& nodeId)
{
    const ExecutionNode* node = graph.findNode(nodeId);
    return node == nullptr ? "missing" : toString(node->status);
}

nlohmann::json makeRunSummary(
    const std::string& runId,
    const std::string& startedAt,
    const std::string& finishedAt,
    const std::string& status,
    const fs::path& runRoot,
    const ExecutionGraph& graph,
    const BranchRunResult& openaiResult,
    const BranchRunResult& anthropicResult)
{
    const std::size_t plannedTasks = openaiResult.plannedTasks + anthropicResult.plannedTasks;
    const std::size_t completedTasks = openaiResult.completedTasks + anthropicResult.completedTasks;
    const std::size_t skippedTasks = openaiResult.skippedTasks + anthropicResult.skippedTasks;
    const std::size_t failedTasks = openaiResult.failedTasks + anthropicResult.failedTasks;

    return {
        {"run_id", runId},
        {"started_at", startedAt},
        {"finished_at", finishedAt},
        {"status", status},
        {"providers_and_models", {
            {"architect", {{"provider", "openai"}, {"model", architectModel}}},
            {"openai_refactoring", {{"provider", "openai"}, {"model", openaiRefactoringModel}}},
            {"anthropic_refactoring", {{"provider", "anthropic"}, {"model", anthropicRefactoringModel}}},
            {"documentation", {{"provider", "anthropic"}, {"model", documentationModel}}}
        }},
        {"stage_status", {
            {"architect", statusOf(graph, architectNodeId)},
            {"openai_branch", statusOf(graph, openaiNodeId)},
            {"anthropic_branch", statusOf(graph, anthropicNodeId)},
            {"documentation", statusOf(graph, documentationNodeId)}
        }},
        {"artifacts", {
            {"project_manifest", (runRoot / "input" / "project_manifest.txt").generic_string()},
            {"project_context", (runRoot / "input" / "project_context.txt").generic_string()},
            {"execution_plan", (runRoot / "architect" / "execution_plan.json").generic_string()},
            {"architecture_review", (runRoot / "architect" / "architecture_review.md").generic_string()},
            {"openai_result", (runRoot / "openai_refactoring" / "result.json").generic_string()},
            {"anthropic_result", (runRoot / "anthropic_refactoring" / "result.json").generic_string()},
            {"documentation_readme", (runRoot / "documentation" / "README.md").generic_string()},
            {"documentation_report", (runRoot / "documentation" / "EXECUTION_REPORT.md").generic_string()},
            {"execution_graph", (runRoot / "execution_graph.json").generic_string()},
            {"run_summary", (runRoot / "run_summary.json").generic_string()}
        }},
        {"planned_task_count", plannedTasks},
        {"completed_task_count", completedTasks},
        {"failed_task_count", failedTasks},
        {"skipped_task_count", skippedTasks}
    };
}

void updateBranchNode(
    ExecutionGraph& graph,
    const std::string& nodeId,
    const fs::path& outputPath,
    const BranchRunResult& result)
{
    ExecutionNode* node = graph.findNode(nodeId);
    if (node == nullptr) {
        throw std::runtime_error("Missing branch node: " + nodeId);
    }

    node->outputPath = outputPath.generic_string();
    node->errorMessage = result.errorMessage;
    node->finishedAt = nowTimestamp();
    node->status = result.success ? ExecutionStatus::Completed : ExecutionStatus::Failed;
}

struct FocusedImplementationRunResult {
    bool success {false};
    std::string status {"failed"};
    std::string errorMessage;
    nlohmann::json resultJson {nlohmann::json::object()};
    std::vector<std::string> generatedFiles;
};

std::string focusedScopeName(FocusedScope scope)
{
    switch (scope) {
    case FocusedScope::ProjectContextCollection:
        return "Project context collection and preparation";
    }

    return "Unknown focused scope";
}

constexpr const char* providerResolvedModelLabel = "provider-resolved ModelType";

std::string focusedDocumentationProviderName(providers::ModelType modelType)
{
    return modelType == providers::ModelTypeOpenAi54 ? "openai" : "anthropic";
}

std::shared_ptr<providers::IModelProvider> makeFocusedDocumentationProvider(providers::ModelType modelType)
{
    if (modelType == providers::ModelTypeOpenAi54) {
        return std::make_shared<providers::OpenAIProvider>(modelType);
    }

    return std::make_shared<providers::AnthropicProvider>(modelType);
}

std::vector<std::string> focusedEditableFiles(FocusedScope scope)
{
    switch (scope) {
    case FocusedScope::ProjectContextCollection:
        return {
            "includes/ai/orchestration/ProjectContextBuilder.hpp",
            "sourceFiles/orchestration/ProjectContextBuilder.cpp",
            "includes/ai/storage/IStorage.hpp",
            "includes/ai/storage/FileStorage.hpp",
            "sourceFiles/storage/FileStorage.cpp",
            "includes/ai/storage/Workspace.hpp",
            "sourceFiles/storage/Workspace.cpp"
        };
    }

    return {};
}

std::vector<std::string> focusedReadOnlyFiles(FocusedScope scope)
{
    switch (scope) {
    case FocusedScope::ProjectContextCollection:
        return {
            "CMakeLists.txt",
            "includes/ai/orchestration/Workflow.hpp",
            "includes/ai/orchestration/ExecutionGraph.hpp",
            "includes/ai/models/ModelRequest.hpp",
            "includes/ai/models/GenerationOptions.hpp",
            "includes/ai/providers/IModelProvider.hpp"
        };
    }

    return {};
}

ProjectContextSelection focusedContextSelection(FocusedScope scope)
{
    return {
        focusedScopeName(scope),
        focusedEditableFiles(scope),
        focusedReadOnlyFiles(scope),
        {
            "Only files marked EDITABLE may be selected for implementation.",
            "Provider adapters, response parsers, Main.cpp, Workflow.cpp, CMake dependency changes, and generated Workspace artifacts are out of scope."
        }
    };
}

std::set<std::string> toNormalizedPathSet(const std::vector<std::string>& files)
{
    std::set<std::string> normalized;
    for (const auto& file : files) {
        normalized.insert(normalizeRelativePathText(file));
    }
    return normalized;
}

void createFocusedRunDirectories(const fs::path& runRoot)
{
    for (const auto& directory : {
             runRoot / "input",
             runRoot / "architect",
             runRoot / "implementation" / "files",
             runRoot / "documentation"}) {
        fs::create_directories(directory);
    }
}

nlohmann::json focusedScopeJson(
    FocusedScope scope,
    const std::set<std::string>& editableFiles,
    const std::set<std::string>& readOnlyFiles)
{
    return {
        {"scope", "project_context_collection"},
        {"title", focusedScopeName(scope)},
        {"editable_files", editableFiles},
        {"read_only_files", readOnlyFiles},
        {"rules", {
            "Architect must select exactly one task.",
            "Selected task files must be inside editable_files.",
            "Implementation output is saved to Workspace only.",
            "No provider, parser, Main.cpp, Workflow.cpp, CMake dependency, or generated Workspace changes are allowed."
        }}
    };
}

ExecutionGraph createFocusedGraph(const OrchestrationConfig& config)
{
    ExecutionGraph graph;

    graph.addNode({
        architectNodeId,
        {},
        "architect",
        "Focused Architect",
        "Inspect the focused project-context scope and select exactly one safe task.",
        "openai",
        providerResolvedModelLabel
    });

    graph.addNode({
        focusedImplementationNodeId,
        {architectNodeId},
        "implementation",
        "Focused Implementation",
        "Generate reviewable full-file proposals for the selected task only.",
        "anthropic",
        providerResolvedModelLabel
    });

    graph.addNode({
        documentationNodeId,
        {focusedImplementationNodeId},
        "documentation",
        "Focused Documentation",
        "Write one Workflow report from stage results and artifact paths.",
        focusedDocumentationProviderName(config.focusedDocumentationModel),
        providerResolvedModelLabel
    });

    return graph;
}

std::string focusedArchitectSystemPrompt()
{
    return R"(You are the focused Architect for a C++20 AI orchestration project.

You only analyze the supplied focused scope. Do not ask for or assume the whole project.
Select exactly one small implementation task for the project-context collection/preparation subsystem.

The selected task must:
- touch only files listed as editable;
- be useful without changing provider adapters, response parsers, Main.cpp, Workflow.cpp, or CMake dependencies;
- be small enough for one implementation agent;
- leave larger or cross-cutting ideas in deferred_findings.

Return strict JSON only. The first character must be `{` and the last character must be `}`.
Do not include Markdown fences, explanations, greetings, or text before or after the JSON object.)";
}

std::string buildFocusedArchitectTask(const ProjectContext& context, const nlohmann::json& scope)
{
    std::ostringstream task;
    task << "Focused scope JSON:\n" << scope.dump(2) << "\n\n";
    task << "Required response schema:\n";
    task << R"({
  "scope_summary": "Short factual summary of the focused scope",
  "findings": [
    {
      "id": "FINDING-001",
      "severity": "low|medium|high",
      "title": "Finding title",
      "description": "Finding description",
      "files": ["relative/path/File.cpp"]
    }
  ],
  "selected_task": {
    "id": "FOCUSED-001",
    "title": "Task title",
    "description": "Exact implementation request",
    "reason": "Why this is the safest useful next task",
    "files": ["relative/path/File.cpp"],
    "acceptance_criteria": ["Criterion 1"],
    "out_of_scope": ["What must not be changed"]
  },
  "deferred_findings": [
    {
      "id": "DEFERRED-001",
      "title": "Deferred item",
      "reason": "Why it is not selected now",
      "files": []
    }
  ]
})";
    task << "\n\nProject manifest:\n" << context.manifest << "\n";
    task << "Focused project context:\n" << context.context;
    return task.str();
}

void requireStringField(const nlohmann::json& value, const char* fieldName, const std::string& owner)
{
    if (!value.contains(fieldName) || !value.at(fieldName).is_string()) {
        throw std::runtime_error(owner + " missing string field: " + fieldName);
    }
}

void requireArrayField(const nlohmann::json& value, const char* fieldName, const std::string& owner)
{
    if (!value.contains(fieldName) || !value.at(fieldName).is_array()) {
        throw std::runtime_error(owner + " missing array field: " + fieldName);
    }
}

void requireStringArray(const nlohmann::json& value, const char* fieldName, const std::string& owner)
{
    requireArrayField(value, fieldName, owner);
    for (const auto& entry : value.at(fieldName)) {
        if (!entry.is_string()) {
            throw std::runtime_error(owner + " array contains a non-string entry: " + fieldName);
        }
    }
}

nlohmann::json validateFocusedArchitectResponse(
    nlohmann::json response,
    const std::set<std::string>& editableFiles)
{
    if (!response.is_object()) {
        throw std::runtime_error("Focused Architect response root is not an object.");
    }

    requireStringField(response, "scope_summary", "Focused Architect response");
    requireArrayField(response, "findings", "Focused Architect response");
    requireArrayField(response, "deferred_findings", "Focused Architect response");

    if (!response.contains("selected_task") || !response.at("selected_task").is_object()) {
        throw std::runtime_error("Focused Architect response must contain one selected_task object.");
    }

    nlohmann::json& task = response["selected_task"];
    for (const char* field : {"id", "title", "description", "reason"}) {
        requireStringField(task, field, "Focused selected_task");
    }
    requireStringArray(task, "files", "Focused selected_task");
    requireStringArray(task, "acceptance_criteria", "Focused selected_task");
    requireStringArray(task, "out_of_scope", "Focused selected_task");

    if (task.at("files").empty() || task.at("files").size() > 3) {
        throw std::runtime_error("Focused selected_task must include from 1 to 3 files.");
    }
    if (task.at("acceptance_criteria").empty()) {
        throw std::runtime_error("Focused selected_task must include acceptance criteria.");
    }

    nlohmann::json normalizedFiles = nlohmann::json::array();
    std::set<std::string> seenFiles;
    for (const auto& file : task.at("files")) {
        const std::string path = normalizeRelativePathText(file.get<std::string>());
        if (!editableFiles.contains(path)) {
            throw std::runtime_error("Focused Architect selected an out-of-scope file: " + path);
        }
        if (!seenFiles.insert(path).second) {
            throw std::runtime_error("Focused Architect selected a file more than once: " + path);
        }
        normalizedFiles.push_back(path);
    }
    task["files"] = std::move(normalizedFiles);

    return response;
}

std::string focusedImplementationSystemPrompt()
{
    return R"(You are the focused C++20 implementation agent.

Implement only the selected task. Use only the supplied editable file contents and read-only interfaces.
Return full proposed file contents, not diffs. The project source tree is not modified by you; your output is saved for manual review.

Do not change provider adapters, response parsers, Main.cpp, Workflow.cpp, CMake dependencies, credentials, endpoints, or generated Workspace artifacts.
Do not introduce new dependencies.

Return strict JSON only. The first character must be `{` and the last character must be `}`.
Do not include Markdown fences, explanations, greetings, or text before or after the JSON object.)";
}

std::vector<std::string> focusedImplementationReadOnlyFiles(
    const std::set<std::string>& selectedFiles,
    FocusedScope scope)
{
    std::vector<std::string> result;
    for (const auto& file : focusedReadOnlyFiles(scope)) {
        const std::string normalized = normalizeRelativePathText(file);
        if (!selectedFiles.contains(normalized)) {
            result.push_back(normalized);
        }
    }

    for (const auto& file : focusedEditableFiles(scope)) {
        const std::string normalized = normalizeRelativePathText(file);
        if (!selectedFiles.contains(normalized)) {
            result.push_back(normalized);
        }
    }

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::set<std::string> collectSelectedTaskFiles(const nlohmann::json& selectedTask)
{
    std::set<std::string> files;
    for (const auto& file : selectedTask.at("files")) {
        files.insert(normalizeRelativePathText(file.get<std::string>()));
    }
    return files;
}

std::vector<std::string> toPathVector(const std::set<std::string>& files)
{
    return {files.begin(), files.end()};
}

std::string buildFocusedImplementationTask(
    const fs::path& projectRoot,
    const nlohmann::json& selectedTask,
    FocusedScope scope)
{
    const std::set<std::string> selectedFiles = collectSelectedTaskFiles(selectedTask);

    std::ostringstream task;
    task << "Selected task JSON:\n" << selectedTask.dump(2) << "\n\n";
    task << "Required response schema:\n";
    task << R"({
  "task_id": "FOCUSED-001",
  "status": "completed|skipped",
  "summary": "Short implementation summary",
  "changes": [
    {
      "path": "relative/path/File.cpp",
      "action": "modify",
      "reason": "Why this file changed",
      "content": "Complete proposed file content"
    }
  ],
  "acceptance_criteria": [
    {
      "criterion": "Criterion text",
      "status": "met|partial|not_met|skipped",
      "evidence": "Concrete evidence from the proposed changes"
    }
  ],
  "risks": ["Known risk or limitation"],
  "manual_review_notes": ["Note for the human reviewer"]
})";

    const ProjectContext selectedContext = ProjectContextBuilder(projectRoot).buildSelected({
        "Focused implementation context",
        toPathVector(selectedFiles),
        focusedImplementationReadOnlyFiles(selectedFiles, scope),
        {"Implement only the selected task. Read-only files are context, not editable targets."}
    });
    task << "\n\nSelected implementation context:\n" << selectedContext.context;

    return task.str();
}

std::string trimWhitespace(std::string value)
{
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }

    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool hasMarkdownFenceWrapper(const std::string& content)
{
    const std::string trimmed = trimWhitespace(content);
    return trimmed.starts_with("```") || trimmed.ends_with("```");
}

bool containsSecretLikeText(const std::string& content)
{
    const std::string lower = [&content]() {
        std::string copy = content;
        std::transform(copy.begin(), copy.end(), copy.begin(), [](unsigned char ch) {
            if (ch >= 'A' && ch <= 'Z') {
                return static_cast<char>(ch - 'A' + 'a');
            }
            return static_cast<char>(ch);
        });
        return copy;
    }();

    return content.find("sk-") != std::string::npos
        || content.find("AIza") != std::string::npos
        || lower.find("authorization: bearer ") != std::string::npos
        || lower.find("x-api-key:") != std::string::npos
        || lower.find("begin private key") != std::string::npos;
}

void validateFocusedImplementationResponse(
    const nlohmann::json& response,
    const std::string& expectedTaskId,
    const std::set<std::string>& selectedFiles)
{
    if (!response.is_object()) {
        throw std::runtime_error("Focused Implementation response root is not an object.");
    }

    requireStringField(response, "task_id", "Focused Implementation response");
    requireStringField(response, "status", "Focused Implementation response");
    requireStringField(response, "summary", "Focused Implementation response");
    requireArrayField(response, "changes", "Focused Implementation response");
    requireArrayField(response, "acceptance_criteria", "Focused Implementation response");
    requireStringArray(response, "risks", "Focused Implementation response");
    requireStringArray(response, "manual_review_notes", "Focused Implementation response");

    if (response.at("task_id").get<std::string>() != expectedTaskId) {
        throw std::runtime_error("Focused Implementation returned the wrong task_id.");
    }

    const std::string status = response.at("status").get<std::string>();
    if (status != "completed" && status != "skipped") {
        throw std::runtime_error("Focused Implementation status must be completed or skipped.");
    }
    if (status == "completed" && response.at("changes").empty()) {
        throw std::runtime_error("Focused Implementation completed without returning changes.");
    }

    std::set<std::string> returnedPaths;
    for (const auto& change : response.at("changes")) {
        if (!change.is_object()) {
            throw std::runtime_error("Focused Implementation change entry is not an object.");
        }
        for (const char* field : {"path", "action", "reason", "content"}) {
            requireStringField(change, field, "Focused Implementation change");
        }

        const std::string path = normalizeRelativePathText(change.at("path").get<std::string>());
        const std::string action = change.at("action").get<std::string>();
        const std::string content = change.at("content").get<std::string>();

        if (action != "modify" && action != "create") {
            throw std::runtime_error("Focused Implementation returned unsupported action: " + action);
        }
        if (!selectedFiles.contains(path)) {
            throw std::runtime_error("Focused Implementation tried to change an unselected file: " + path);
        }
        if (!returnedPaths.insert(path).second) {
            throw std::runtime_error("Focused Implementation returned the same file more than once: " + path);
        }
        if (content.empty()) {
            throw std::runtime_error("Focused Implementation returned empty content for: " + path);
        }
        if (hasMarkdownFenceWrapper(content)) {
            throw std::runtime_error("Focused Implementation wrapped file content in Markdown fences: " + path);
        }
        if (containsSecretLikeText(content)) {
            throw std::runtime_error("Focused Implementation returned content that looks like a secret: " + path);
        }
    }

    for (const auto& criterion : response.at("acceptance_criteria")) {
        if (!criterion.is_object()) {
            throw std::runtime_error("Focused Implementation acceptance_criteria entry is not an object.");
        }
        for (const char* field : {"criterion", "status", "evidence"}) {
            requireStringField(criterion, field, "Focused Implementation acceptance_criteria");
        }
        const std::string criterionStatus = criterion.at("status").get<std::string>();
        if (criterionStatus != "met"
            && criterionStatus != "partial"
            && criterionStatus != "not_met"
            && criterionStatus != "skipped") {
            throw std::runtime_error("Focused Implementation acceptance criterion has unsupported status.");
        }
    }
}

std::vector<std::string> saveFocusedImplementationFiles(
    const nlohmann::json& response,
    const fs::path& implementationRoot)
{
    ai::storage::FileStorage storage;
    std::vector<std::string> generatedFiles;

    for (const auto& change : response.at("changes")) {
        const std::string relativePath = normalizeRelativePathText(change.at("path").get<std::string>());
        const fs::path outputPath = implementationRoot / "files" / fs::path(relativePath);

        writeTextOrThrow(storage, outputPath, change.at("content").get<std::string>());
        generatedFiles.push_back((fs::path("files") / fs::path(relativePath)).generic_string());
    }

    return generatedFiles;
}

FocusedImplementationRunResult runFocusedImplementationAgent(
    const fs::path& projectRoot,
    const fs::path& implementationRoot,
    const nlohmann::json& selectedTask,
    FocusedScope scope,
    providers::ModelType modelType,
    models::RequestTimeoutOptions timeouts)
{
    FocusedImplementationRunResult result;
    ai::storage::FileStorage storage;

    try {
        const std::set<std::string> selectedFiles = collectSelectedTaskFiles(selectedTask);
        auto provider = std::make_shared<providers::AnthropicProvider>(modelType);

        agents::Agent agent(
            makeAgentConfig(
                "focused-implementation-agent",
                "Focused Implementation Agent",
                "Focused C++ implementation",
                focusedImplementationSystemPrompt(),
                providerResolvedModelLabel,
                "Focused Implementation",
                std::move(timeouts),
                12000),
            std::move(provider));

        agents::AgentContext context;
        context.task = buildFocusedImplementationTask(projectRoot, selectedTask, scope);

        const agents::AgentResult agentResult = agent.execute(context);
        if (!agentResult.success) {
            throw std::runtime_error(agentResult.errorMessage);
        }

        writeTextOrThrow(storage, implementationRoot / "raw_response.txt", agentResult.output);

        nlohmann::json response = parseStrictJson(agentResult.output, "Focused Implementation");
        validateFocusedImplementationResponse(
            response,
            selectedTask.at("id").get<std::string>(),
            selectedFiles);

        result.generatedFiles = saveFocusedImplementationFiles(response, implementationRoot);
        writeTextOrThrow(storage, implementationRoot / "result.json", response.dump(2));

        result.status = response.at("status").get<std::string>();
        result.success = result.status == "completed";
        result.resultJson = std::move(response);
        if (!result.success) {
            result.errorMessage = "Focused Implementation skipped the selected task.";
        }
    } catch (const std::exception& ex) {
        result.success = false;
        result.status = "failed";
        result.errorMessage = ex.what();
        result.resultJson = {
            {"success", false},
            {"error", result.errorMessage},
            {"model", providerResolvedModelLabel}
        };
        storage.writeText(implementationRoot / "result.json", result.resultJson.dump(2));
    }

    return result;
}

std::string focusedDocumentationSystemPrompt()
{
    return R"(You are the focused Documentation agent for a C++20 AI orchestration run.

Use only the supplied stage results, statuses, and artifact paths.
Do not infer that generated Workspace proposals were applied to the source tree.
Produce one concise Markdown report for manual review.

Return strict JSON only. The first character must be `{` and the last character must be `}`.
Do not include Markdown fences, explanations, greetings, or text before or after the JSON object.)";
}

std::string buildFocusedDocumentationTask(
    const std::string& runId,
    const ExecutionGraph& graph,
    const nlohmann::json& scope,
    const nlohmann::json& architectResult,
    const FocusedImplementationRunResult& implementationResult,
    const std::vector<std::string>& implementationFiles)
{
    std::ostringstream task;
    task << "Required response schema:\n";
    task << R"({
  "summary": "Short documentation summary",
  "files": [
    {
      "path": "WORKFLOW_REPORT.md",
      "content": "Complete Markdown report content"
    }
  ]
})";

    task << "\n\nRun ID: " << runId << "\n";
    task << "Focused scope:\n" << scope.dump(2) << "\n\n";
    task << "Stage statuses:\n";
    task << "- Architect: " << statusOf(graph, architectNodeId) << "\n";
    task << "- Implementation: " << statusOf(graph, focusedImplementationNodeId) << "\n";
    task << "- Documentation: " << statusOf(graph, documentationNodeId) << "\n\n";
    task << "Architect result JSON:\n" << architectResult.dump(2) << "\n\n";
    task << "Implementation result JSON:\n" << implementationResult.resultJson.dump(2) << "\n\n";
    task << "Implementation generated artifact paths:\n"
         << nlohmann::json(implementationFiles).dump(2) << "\n";
    return task.str();
}

void validateFocusedDocumentationResponse(const nlohmann::json& response)
{
    if (!response.is_object()) {
        throw std::runtime_error("Focused Documentation response root is not an object.");
    }

    requireStringField(response, "summary", "Focused Documentation response");
    requireArrayField(response, "files", "Focused Documentation response");

    if (response.at("files").size() != 1) {
        throw std::runtime_error("Focused Documentation must produce exactly one file.");
    }

    const nlohmann::json& file = response.at("files").front();
    requireStringField(file, "path", "Focused Documentation file");
    requireStringField(file, "content", "Focused Documentation file");

    const std::string path = normalizeRelativePathText(file.at("path").get<std::string>());
    if (path != "WORKFLOW_REPORT.md") {
        throw std::runtime_error("Focused Documentation tried to write an unexpected file: " + path);
    }
    if (file.at("content").get<std::string>().empty()) {
        throw std::runtime_error("Focused Documentation report content is empty.");
    }
}

DocumentationRunResult runFocusedDocumentationAgent(
    const fs::path& documentationRoot,
    const std::string& runId,
    const ExecutionGraph& graph,
    const nlohmann::json& scope,
    const nlohmann::json& architectResult,
    const FocusedImplementationRunResult& implementationResult,
    providers::ModelType modelType,
    models::RequestTimeoutOptions timeouts)
{
    DocumentationRunResult result;
    ai::storage::FileStorage storage;

    try {
        auto provider = makeFocusedDocumentationProvider(modelType);
        agents::Agent agent(
            makeAgentConfig(
                "focused-documentation-agent",
                "Focused Documentation Agent",
                "Focused run documentation",
                focusedDocumentationSystemPrompt(),
                providerResolvedModelLabel,
                "Focused Documentation",
                std::move(timeouts),
                8000),
            std::move(provider));

        agents::AgentContext context;
        context.task = buildFocusedDocumentationTask(
            runId,
            graph,
            scope,
            architectResult,
            implementationResult,
            implementationResult.generatedFiles);

        const agents::AgentResult agentResult = agent.execute(context);
        if (!agentResult.success) {
            throw std::runtime_error(agentResult.errorMessage);
        }

        writeTextOrThrow(storage, documentationRoot / "raw_response.txt", agentResult.output);

        nlohmann::json response = parseStrictJson(agentResult.output, "Focused Documentation");
        validateFocusedDocumentationResponse(response);

        writeTextOrThrow(storage, documentationRoot / "result.json", response.dump(2));
        const nlohmann::json& file = response.at("files").front();
        writeTextOrThrow(
            storage,
            documentationRoot / "WORKFLOW_REPORT.md",
            file.at("content").get<std::string>());

        result.success = true;
        result.resultJson = std::move(response);
    } catch (const std::exception& ex) {
        result.success = false;
        result.errorMessage = ex.what();
        result.resultJson = {
            {"success", false},
            {"error", result.errorMessage},
            {"model", providerResolvedModelLabel}
        };
        storage.writeText(documentationRoot / "result.json", result.resultJson.dump(2));
    }

    return result;
}

nlohmann::json makeFocusedRunSummary(
    const std::string& runId,
    const std::string& startedAt,
    const std::string& finishedAt,
    const std::string& status,
    const fs::path& runRoot,
    const OrchestrationConfig& config,
    const ExecutionGraph& graph,
    const FocusedImplementationRunResult& implementationResult,
    const DocumentationRunResult& documentationResult)
{
    return {
        {"run_id", runId},
        {"mode", "focused_sequential"},
        {"scope", focusedScopeName(config.focusedScope)},
        {"started_at", startedAt},
        {"finished_at", finishedAt},
        {"status", status},
        {"providers_and_models", {
            {"architect", {
                {"provider", "openai"},
                {"model", providerResolvedModelLabel}
            }},
            {"implementation", {
                {"provider", "anthropic"},
                {"model", providerResolvedModelLabel}
            }},
            {"documentation", {
                {"provider", focusedDocumentationProviderName(config.focusedDocumentationModel)},
                {"model", providerResolvedModelLabel}
            }}
        }},
        {"stage_status", {
            {"architect", statusOf(graph, architectNodeId)},
            {"implementation", statusOf(graph, focusedImplementationNodeId)},
            {"documentation", statusOf(graph, documentationNodeId)}
        }},
        {"artifacts", {
            {"scope", (runRoot / "input" / "scope.json").generic_string()},
            {"project_manifest", (runRoot / "input" / "project_manifest.txt").generic_string()},
            {"project_context", (runRoot / "input" / "project_context.txt").generic_string()},
            {"architect_result", (runRoot / "architect" / "result.json").generic_string()},
            {"selected_task", (runRoot / "architect" / "task.json").generic_string()},
            {"implementation_result", (runRoot / "implementation" / "result.json").generic_string()},
            {"implementation_files", implementationResult.generatedFiles},
            {"documentation_result", (runRoot / "documentation" / "result.json").generic_string()},
            {"documentation_report", (runRoot / "documentation" / "WORKFLOW_REPORT.md").generic_string()},
            {"execution_graph", (runRoot / "execution_graph.json").generic_string()},
            {"run_summary", (runRoot / "run_summary.json").generic_string()}
        }},
        {"errors", {
            {"implementation", implementationResult.errorMessage},
            {"documentation", documentationResult.errorMessage}
        }}
    };
}

OrchestrationResult runFocusedWorkflow(const OrchestrationConfig& config)
{
    std::vector<std::string> missingKeys;
    if (envOrEmpty("OPENAI_API_KEY").empty()) {
        missingKeys.push_back("OPENAI_API_KEY");
    }
    if (envOrEmpty("ANTHROPIC_API_KEY").empty()) {
        missingKeys.push_back("ANTHROPIC_API_KEY");
    }
    if (!missingKeys.empty()) {
        std::ostringstream message;
        message << "Missing required API key environment variable(s):";
        for (const auto& key : missingKeys) {
            message << ' ' << key;
        }
        return {false, "failed", message.str(), {}, {}};
    }

    const std::string startedAt = nowTimestamp();
    const std::string runId = makeRunId();

    ai::storage::Workspace workspace(config.workspaceRoot);
    if (!workspace.ensure()) {
        throw std::runtime_error("Failed to prepare workspace root.");
    }

    const fs::path runRoot = workspace.runsDir() / runId;
    createFocusedRunDirectories(runRoot);

    ai::storage::FileStorage storage;
    ExecutionGraph graph = createFocusedGraph(config);
    persistGraphOrThrow(graph, runRoot);

    const std::set<std::string> editableFiles = toNormalizedPathSet(focusedEditableFiles(config.focusedScope));
    const std::set<std::string> readOnlyFiles = toNormalizedPathSet(focusedReadOnlyFiles(config.focusedScope));
    const nlohmann::json scope = focusedScopeJson(config.focusedScope, editableFiles, readOnlyFiles);

    nlohmann::json architectResultJson = nlohmann::json::object();
    FocusedImplementationRunResult implementationResult;
    DocumentationRunResult documentationResult;

    try {
        std::cout << "Creating focused project context...\n";
        const ProjectContext projectContext =
            ProjectContextBuilder(config.projectRoot).buildSelected(focusedContextSelection(config.focusedScope));

        writeTextOrThrow(storage, runRoot / "input" / "scope.json", scope.dump(2));
        writeTextOrThrow(storage, runRoot / "input" / "project_manifest.txt", projectContext.manifest);
        writeTextOrThrow(storage, runRoot / "input" / "project_context.txt", projectContext.context);
        std::cout << "Focused project files collected: " << projectContext.files.size() << "\n\n";

        graph.setStatus(architectNodeId, ExecutionStatus::Running, nowTimestamp());
        persistGraphOrThrow(graph, runRoot);

        std::cout << "[Focused Architect] Starting OpenAIProvider...\n";
        agents::Agent architectAgent(
            makeAgentConfig(
                "focused-architect-agent",
                "Focused Architect",
                "Focused project-context planning",
                focusedArchitectSystemPrompt(),
                providerResolvedModelLabel,
                "Focused Architect",
                makeWorkflowTimeouts(config.timeouts.connect, config.timeouts.focusedArchitectTotal),
                8000),
            std::make_shared<providers::OpenAIProvider>(config.focusedArchitectModel));

        agents::AgentContext architectContext;
        architectContext.task = buildFocusedArchitectTask(projectContext, scope);

        const agents::AgentResult architectResult = architectAgent.execute(architectContext);
        if (!architectResult.success) {
            throw std::runtime_error(architectResult.errorMessage);
        }

        writeTextOrThrow(storage, runRoot / "architect" / "raw_response.txt", architectResult.output);
        architectResultJson = validateFocusedArchitectResponse(
            parseStrictJson(architectResult.output, "Focused Architect"),
            editableFiles);

        const nlohmann::json selectedTask = architectResultJson.at("selected_task");
        writeTextOrThrow(storage, runRoot / "architect" / "result.json", architectResultJson.dump(2));
        writeTextOrThrow(storage, runRoot / "architect" / "task.json", selectedTask.dump(2));

        {
            ExecutionNode* architectNode = graph.findNode(architectNodeId);
            architectNode->outputPath = "architect/result.json";
            architectNode->input = scope.dump(2);
            graph.setStatus(architectNodeId, ExecutionStatus::Completed, nowTimestamp());

            const std::set<std::string> selectedFiles = collectSelectedTaskFiles(selectedTask);
            ExecutionNode* implementationNode = graph.findNode(focusedImplementationNodeId);
            implementationNode->relatedFiles.assign(selectedFiles.begin(), selectedFiles.end());
            implementationNode->input = selectedTask.dump(2);
        }

        persistGraphOrThrow(graph, runRoot);
        std::cout << "[Focused Architect] Selected task "
                  << selectedTask.value("id", "unknown")
                  << ".\n\n";

        graph.setStatus(focusedImplementationNodeId, ExecutionStatus::Running, nowTimestamp());
        persistGraphOrThrow(graph, runRoot);

        std::cout << "[Focused Implementation] Starting AnthropicProvider...\n";
        implementationResult = runFocusedImplementationAgent(
            config.projectRoot,
            runRoot / "implementation",
            selectedTask,
            config.focusedScope,
            config.focusedImplementationModel,
            makeWorkflowTimeouts(config.timeouts.connect, config.timeouts.focusedImplementationTotal));

        ExecutionNode* implementationNode = graph.findNode(focusedImplementationNodeId);
        implementationNode->outputPath = "implementation/result.json";
        const ExecutionStatus implementationStatus = implementationResult.success
            ? ExecutionStatus::Completed
            : (implementationResult.status == "skipped" ? ExecutionStatus::Skipped : ExecutionStatus::Failed);
        graph.setStatus(
            focusedImplementationNodeId,
            implementationStatus,
            nowTimestamp(),
            implementationResult.errorMessage);
        persistGraphOrThrow(graph, runRoot);
        std::cout << "[Focused Implementation] "
                  << (implementationResult.success ? "Completed." : "Failed.")
                  << "\n\n";

        graph.setStatus(documentationNodeId, ExecutionStatus::Running, nowTimestamp());
        persistGraphOrThrow(graph, runRoot);

        std::cout << "[Focused Documentation] Starting "
                  << focusedDocumentationProviderName(config.focusedDocumentationModel)
                  << " provider...\n";
        documentationResult = runFocusedDocumentationAgent(
            runRoot / "documentation",
            runId,
            graph,
            scope,
            architectResultJson,
            implementationResult,
            config.focusedDocumentationModel,
            makeWorkflowTimeouts(config.timeouts.connect, config.timeouts.focusedDocumentationTotal));

        ExecutionNode* documentationNode = graph.findNode(documentationNodeId);
        documentationNode->outputPath = "documentation/WORKFLOW_REPORT.md";
        graph.setStatus(
            documentationNodeId,
            documentationResult.success ? ExecutionStatus::Completed : ExecutionStatus::Failed,
            nowTimestamp(),
            documentationResult.errorMessage);
        persistGraphOrThrow(graph, runRoot);
        std::cout << "[Focused Documentation] "
                  << (documentationResult.success ? "Completed." : "Failed.")
                  << "\n\n";

        const bool allCompleted = implementationResult.success && documentationResult.success;
        const std::string status = allCompleted ? "completed" : "partial";
        const nlohmann::json summary = makeFocusedRunSummary(
            runId,
            startedAt,
            nowTimestamp(),
            status,
            runRoot,
            config,
            graph,
            implementationResult,
            documentationResult);
        writeTextOrThrow(storage, runRoot / "run_summary.json", summary.dump(2));

        return {
            allCompleted,
            status,
            allCompleted ? "Focused run completed." : "Focused run finished with partial errors.",
            runId,
            runRoot
        };
    } catch (const std::exception& ex) {
        const std::string finishedAt = nowTimestamp();
        architectResultJson = {
            {"success", false},
            {"error", ex.what()}
        };
        storage.writeText(runRoot / "architect" / "result.json", architectResultJson.dump(2));

        if (ExecutionNode* architectNode = graph.findNode(architectNodeId);
            architectNode != nullptr && architectNode->status == ExecutionStatus::Running) {
            graph.setStatus(architectNodeId, ExecutionStatus::Failed, finishedAt, ex.what());
        }

        persistGraphOrThrow(graph, runRoot);
        const nlohmann::json summary = makeFocusedRunSummary(
            runId,
            startedAt,
            finishedAt,
            "failed",
            runRoot,
            config,
            graph,
            implementationResult,
            documentationResult);
        writeTextOrThrow(storage, runRoot / "run_summary.json", summary.dump(2));

        return {
            false,
            "failed",
            ex.what(),
            runId,
            runRoot
        };
    }
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

    const OrchestrationConfig& config = *config_;
    if (config.mode == WorkflowMode::FocusedSequential) {
        return runFocusedWorkflow(config);
    }

    const std::string openaiKey = envOrEmpty("OPENAI_API_KEY");
    const std::string anthropicKey = envOrEmpty("ANTHROPIC_API_KEY");
    if (openaiKey.empty() || anthropicKey.empty()) {
        std::ostringstream message;
        message << "Missing required API key environment variable(s):";
        if (openaiKey.empty()) {
            message << " OPENAI_API_KEY";
        }
        if (anthropicKey.empty()) {
            message << " ANTHROPIC_API_KEY";
        }

        return {
            false,
            "failed",
            message.str(),
            {},
            {}
        };
    }

    const std::string startedAt = nowTimestamp();
    const std::string runId = makeRunId();

    ai::storage::Workspace workspace(config.workspaceRoot);
    if (!workspace.ensure()) {
        throw std::runtime_error("Failed to prepare workspace root.");
    }

    const fs::path runRoot = workspace.runsDir() / runId;
    createRunDirectories(runRoot);

    ai::storage::FileStorage storage;
    ExecutionGraph graph = createInitialGraph();
    persistGraphOrThrow(graph, runRoot);

    BranchRunResult openaiResult;
    openaiResult.branchName = "OpenAI";
    BranchRunResult anthropicResult;
    anthropicResult.branchName = "Anthropic";

    try {
        std::cout << "Creating project context...\n";
        const ProjectContext projectContext = ProjectContextBuilder(config.projectRoot).build();
        writeTextOrThrow(storage, runRoot / "input" / "project_manifest.txt", projectContext.manifest);
        writeTextOrThrow(storage, runRoot / "input" / "project_context.txt", projectContext.context);
        std::cout << "Project files collected: " << projectContext.files.size() << "\n\n";

        graph.setStatus(architectNodeId, ExecutionStatus::Running, nowTimestamp());
        persistGraphOrThrow(graph, runRoot);

        std::cout << "[Architect] Starting " << architectModel << "...\n";
        auto architectProvider = std::make_shared<providers::OpenAIProvider>(providers::ModelTypeOpenAi56Sol);
        agents::Agent architectAgent(
            makeAgentConfig(
                "architect-agent",
                "Architect",
                "Project architecture planning",
                architectSystemPrompt(),
                architectModel,
                "Architect",
                makeWorkflowTimeouts(config.timeouts.connect, config.timeouts.architectTotal),
                12000),
            std::move(architectProvider));

        agents::AgentContext architectContext;
        architectContext.task = buildArchitectTask(projectContext);

        

        const agents::AgentResult architectResult = architectAgent.execute(architectContext);
        if (!architectResult.success) {
            throw std::runtime_error(architectResult.errorMessage);
        }

        writeTextOrThrow(storage, runRoot / "architect" / "raw_response.txt", architectResult.output);

        nlohmann::json architectPlan = normalizeArchitectPlan(
            parseStrictJson(architectResult.output, "Architect"));
        validateNoBranchFileOverlap(architectPlan);

        writeTextOrThrow(
            storage,
            runRoot / "architect" / "execution_plan.json",
            architectPlan.dump(2));
        writeTextOrThrow(
            storage,
            runRoot / "architect" / "architecture_review.md",
            architectureReviewMarkdown(architectPlan));

        {
            ExecutionNode* architectNode = graph.findNode(architectNodeId);
            architectNode->outputPath = "architect/execution_plan.json";
            graph.setStatus(architectNodeId, ExecutionStatus::Completed, nowTimestamp());

            const std::set<std::string> openaiFiles = collectTaskFiles(architectPlan.at("openai_tasks"));
            ExecutionNode* openaiNode = graph.findNode(openaiNodeId);
            openaiNode->relatedFiles.assign(openaiFiles.begin(), openaiFiles.end());
            openaiNode->input = architectPlan.at("openai_tasks").dump(2);

            const std::set<std::string> anthropicFiles = collectTaskFiles(architectPlan.at("anthropic_tasks"));
            ExecutionNode* anthropicNode = graph.findNode(anthropicNodeId);
            anthropicNode->relatedFiles.assign(anthropicFiles.begin(), anthropicFiles.end());
            anthropicNode->input = architectPlan.at("anthropic_tasks").dump(2);
        }

        persistGraphOrThrow(graph, runRoot);
        std::cout << "[Architect] Execution plan created.\n\n";

        graph.setStatus(openaiNodeId, ExecutionStatus::Running, nowTimestamp());
        graph.setStatus(anthropicNodeId, ExecutionStatus::Running, nowTimestamp());
        persistGraphOrThrow(graph, runRoot);

        std::cout << "[OpenAI branch] Starting " << openaiRefactoringModel << "...\n";
        std::cout << "[Anthropic branch] Starting " << anthropicRefactoringModel << "...\n";

        const nlohmann::json openaiTasks = architectPlan.at("openai_tasks");
        const nlohmann::json anthropicTasks = architectPlan.at("anthropic_tasks");
        const models::RequestTimeoutOptions codingTimeouts =
            makeWorkflowTimeouts(config.timeouts.connect, config.timeouts.codingTotal);

        auto openaiFuture = std::async(std::launch::async, [&config, &runRoot, openaiTasks, codingTimeouts]() {
            return runCodingBranch(
                config.projectRoot,
                runRoot / "openai_refactoring",
                "OpenAI",
                "openai",
                openaiRefactoringModel,
                providers::ModelTypeOpenAi56Terra,
                codingTimeouts,
                openaiTasks);
        });

        auto anthropicFuture = std::async(std::launch::async, [&config, &runRoot, anthropicTasks, codingTimeouts]() {
            return runCodingBranch(
                config.projectRoot,
                runRoot / "anthropic_refactoring",
                "Anthropic",
                "anthropic",
                anthropicRefactoringModel,
                providers::ModelTypeClaudeOpus,
                codingTimeouts,
                anthropicTasks);
        });

        openaiResult = openaiFuture.get();
        anthropicResult = anthropicFuture.get();

        updateBranchNode(
            graph,
            openaiNodeId,
            fs::path("openai_refactoring") / "result.json",
            openaiResult);
        persistGraphOrThrow(graph, runRoot);
        std::cout << "[OpenAI branch] " << (openaiResult.success ? "Completed." : "Failed.") << "\n";

        updateBranchNode(
            graph,
            anthropicNodeId,
            fs::path("anthropic_refactoring") / "result.json",
            anthropicResult);
        persistGraphOrThrow(graph, runRoot);
        std::cout << "[Anthropic branch] " << (anthropicResult.success ? "Completed." : "Failed.") << "\n\n";

        graph.setStatus(documentationNodeId, ExecutionStatus::Running, nowTimestamp());
        persistGraphOrThrow(graph, runRoot);

        std::cout << "[Documentation] Starting " << documentationModel << "...\n";
        const DocumentationRunResult documentationResult = runDocumentationAgent(
            config.projectRoot,
            runRoot / "documentation",
            architectPlan,
            openaiResult,
            anthropicResult,
            makeWorkflowTimeouts(config.timeouts.connect, config.timeouts.documentationTotal));

        ExecutionNode* documentationNode = graph.findNode(documentationNodeId);
        documentationNode->outputPath = "documentation/EXECUTION_REPORT.md";
        graph.setStatus(
            documentationNodeId,
            documentationResult.success ? ExecutionStatus::Completed : ExecutionStatus::Failed,
            nowTimestamp(),
            documentationResult.errorMessage);
        persistGraphOrThrow(graph, runRoot);
        std::cout << "[Documentation] "
                  << (documentationResult.success ? "Completed." : "Failed.")
                  << "\n\n";

        const bool allCompleted = openaiResult.success
            && anthropicResult.success
            && documentationResult.success;
        const std::string status = allCompleted ? "completed" : "partial";
        const nlohmann::json summary = makeRunSummary(
            runId,
            startedAt,
            nowTimestamp(),
            status,
            runRoot,
            graph,
            openaiResult,
            anthropicResult);

        writeTextOrThrow(storage, runRoot / "run_summary.json", summary.dump(2));

        std::cout << "Run " << (allCompleted ? "completed." : "finished with partial errors.") << "\n";
        std::cout << "Workspace: " << runRoot.string() << "\n";

        return {
            allCompleted,
            status,
            allCompleted ? "Run completed." : "Run finished with partial errors.",
            runId,
            runRoot
        };
    } catch (const std::exception& ex) {
        const std::string finishedAt = nowTimestamp();
        storage.writeText(runRoot / "architect" / "error.txt", ex.what());

        if (ExecutionNode* architectNode = graph.findNode(architectNodeId);
            architectNode != nullptr && architectNode->status == ExecutionStatus::Running) {
            graph.setStatus(architectNodeId, ExecutionStatus::Failed, finishedAt, ex.what());
        }

        persistGraphOrThrow(graph, runRoot);
        const nlohmann::json summary = makeRunSummary(
            runId,
            startedAt,
            finishedAt,
            "failed",
            runRoot,
            graph,
            openaiResult,
            anthropicResult);
        writeTextOrThrow(storage, runRoot / "run_summary.json", summary.dump(2));

        return {
            false,
            "failed",
            ex.what(),
            runId,
            runRoot
        };
    }
}

} // namespace ai::orchestration
