#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace ai::orchestration {

enum class ExecutionStatus {
    Pending,
    Running,
    Completed,
    Failed,
    Skipped
};

struct ExecutionNode {
    std::string id;
    std::vector<std::string> dependsOn;
    std::string type;
    std::string title;
    std::string description;
    std::string provider;
    std::string model;
    std::vector<std::string> relatedFiles;
    ExecutionStatus status {ExecutionStatus::Pending};
    std::string input;
    std::string outputPath;
    std::string errorMessage;
    std::string startedAt;
    std::string finishedAt;
};

/**
 * @brief Stores the DAG and persisted state for one orchestration run.
 *
 * Workflow decides when stages run; ExecutionGraph records what each node is,
 * what it depends on, which provider/model owns it, and where its artifacts
 * were written. The graph is serializable so a failed run still leaves an
 * inspectable execution_graph.json in Workspace.
 */
class ExecutionGraph {
public:
    void addNode(ExecutionNode node);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::vector<std::string> executionOrder() const;
    [[nodiscard]] const std::unordered_map<std::string, ExecutionNode>& nodes() const noexcept;
    [[nodiscard]] ExecutionNode* findNode(const std::string& id);
    [[nodiscard]] const ExecutionNode* findNode(const std::string& id) const;
    [[nodiscard]] std::string toJsonString() const;

    void setStatus(
        const std::string& id,
        ExecutionStatus status,
        std::string timestamp = {},
        std::string errorMessage = {});

    bool writeJson(const std::filesystem::path& path) const;

private:
    std::unordered_map<std::string, ExecutionNode> nodes_;
};

[[nodiscard]] const char* toString(ExecutionStatus status) noexcept;

} // namespace ai::orchestration
