#pragma once

#include "ai/providers/ModelType.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ai::orchestration {

struct WorkflowStep {
    std::string id;
    std::vector<std::string> agentIds;
    std::vector<std::string> dependsOn;
};

enum class WorkflowMode {
    ParallelMultiAgent,
    FocusedSequential
};

enum class FocusedScope {
    ProjectContextCollection
};

struct OrchestrationConfig {
    struct TimeoutConfig {
        std::chrono::seconds connect {30};
        std::chrono::seconds architectTotal {600};
        std::chrono::seconds codingTotal {300};
        std::chrono::seconds documentationTotal {300};
        std::chrono::seconds focusedArchitectTotal {300};
        std::chrono::seconds focusedImplementationTotal {300};
        std::chrono::seconds focusedDocumentationTotal {180};
    };

    std::filesystem::path projectRoot;
    std::filesystem::path workspaceRoot;
    TimeoutConfig timeouts;
    WorkflowMode mode {WorkflowMode::ParallelMultiAgent};
    FocusedScope focusedScope {FocusedScope::ProjectContextCollection};
    providers::ModelType focusedArchitectModel {providers::ModelTypeOpenAi56Sol};
    providers::ModelType focusedImplementationModel {providers::ModelTypeClaudeSonnet};
    providers::ModelType focusedDocumentationModel {providers::ModelTypeClaudeHaiku};
};

struct OrchestrationResult {
    bool success {false};
    std::string status;
    std::string message;
    std::string runId;
    std::filesystem::path runRoot;
};

/**
 * @brief Owns the lifecycle of one controlled orchestration run.
 *
 * ExecutionGraph stores the task DAG and status; Workflow selects the configured
 * scenario, calls agents in that order, and persists run artifacts. Generated
 * source proposals are written to Workspace only and are never applied to the
 * project tree.
 */
class Workflow {
public:
    Workflow() = default;
    explicit Workflow(OrchestrationConfig config);

    void addStep(WorkflowStep step);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] const std::vector<WorkflowStep>& steps() const noexcept;
    [[nodiscard]] std::optional<WorkflowStep> findStep(const std::string& id) const;
    [[nodiscard]] OrchestrationResult run() const;

private:
    std::vector<WorkflowStep> steps_;
    std::optional<OrchestrationConfig> config_;
};

} // namespace ai::orchestration
