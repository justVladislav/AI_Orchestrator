#include "ai/orchestration/ExecutionGraph.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace ai::orchestration {

namespace {

void visitNode(
    const std::string& id,
    const std::unordered_map<std::string, ExecutionNode>& nodes,
    std::unordered_set<std::string>& visiting,
    std::unordered_set<std::string>& visited,
    std::vector<std::string>& ordered)
{
    if (visited.contains(id)) {
        return;
    }

    if (visiting.contains(id)) {
        throw std::logic_error("ExecutionGraph contains a cycle");
    }

    const auto it = nodes.find(id);
    if (it == nodes.end()) {
        throw std::out_of_range("ExecutionGraph dependency is missing: " + id);
    }

    visiting.insert(id);
    for (const auto& dependency : it->second.dependsOn) {
        // Depth-first traversal guarantees dependencies are emitted first.
        visitNode(dependency, nodes, visiting, visited, ordered);
    }
    visiting.erase(id);

    visited.insert(id);
    ordered.push_back(id);
}

nlohmann::json nodeToJson(const ExecutionNode& node)
{
    return {
        {"id", node.id},
        {"type", node.type},
        {"title", node.title},
        {"description", node.description},
        {"provider", node.provider},
        {"model", node.model},
        {"related_files", node.relatedFiles},
        {"dependencies", node.dependsOn},
        {"status", toString(node.status)},
        {"input", node.input},
        {"output_path", node.outputPath.empty() ? nlohmann::json(nullptr) : nlohmann::json(node.outputPath)},
        {"error", node.errorMessage.empty() ? nlohmann::json(nullptr) : nlohmann::json(node.errorMessage)},
        {"started_at", node.startedAt.empty() ? nlohmann::json(nullptr) : nlohmann::json(node.startedAt)},
        {"finished_at", node.finishedAt.empty() ? nlohmann::json(nullptr) : nlohmann::json(node.finishedAt)}
    };
}

} // namespace

void ExecutionGraph::addNode(ExecutionNode node)
{
    const std::string id = node.id;
    nodes_[id] = std::move(node);
}

bool ExecutionGraph::empty() const noexcept
{
    return nodes_.empty();
}

std::vector<std::string> ExecutionGraph::executionOrder() const
{
    std::vector<std::string> ordered;
    std::unordered_set<std::string> visiting;
    std::unordered_set<std::string> visited;

    for (const auto& [id, node] : nodes_) {                 // Это structured binding — структурное связывание. id - key, node - value
        static_cast<void>(node);                            // Я намеренно не использую переменную node.
        visitNode(id, nodes_, visiting, visited, ordered);
    }

    return ordered;
}

const std::unordered_map<std::string, ExecutionNode>& ExecutionGraph::nodes() const noexcept
{
    return nodes_;
}

ExecutionNode* ExecutionGraph::findNode(const std::string& id)
{
    const auto it = nodes_.find(id);        
    if (it == nodes_.end()) {
        return nullptr;
    }

    return &it->second;
}

const ExecutionNode* ExecutionGraph::findNode(const std::string& id) const
{
    const auto it = nodes_.find(id);
    if (it == nodes_.end()) {
        return nullptr;
    }

    return &it->second;
}

std::string ExecutionGraph::toJsonString() const
{
    nlohmann::json root;
    root["nodes"] = nlohmann::json::array();

    const std::vector<std::string> ordered = executionOrder();
    for (const auto& id : ordered) {
        root["nodes"].push_back(nodeToJson(nodes_.at(id)));
    }

    return root.dump(2);
}

void ExecutionGraph::setStatus(
    const std::string& id,
    ExecutionStatus status,
    std::string timestamp,
    std::string errorMessage)
{
    ExecutionNode* node = findNode(id);
    if (node == nullptr) {
        throw std::out_of_range("ExecutionGraph node is missing: " + id);
    }

    node->status = status;
    if (status == ExecutionStatus::Running) {
        node->startedAt = std::move(timestamp);
        node->finishedAt.clear();
    } else if (status == ExecutionStatus::Completed
        || status == ExecutionStatus::Failed
        || status == ExecutionStatus::Skipped) {
        node->finishedAt = std::move(timestamp);
    }

    node->errorMessage = std::move(errorMessage);
}

bool ExecutionGraph::writeJson(const std::filesystem::path& path) const
{
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    std::ofstream output(path, std::ios::binary);
    if (!output) {
        return false;
    }

    output << toJsonString();
    return static_cast<bool>(output);
}

const char* toString(ExecutionStatus status) noexcept
{
    switch (status) {
    case ExecutionStatus::Pending:
        return "pending";
    case ExecutionStatus::Running:
        return "running";
    case ExecutionStatus::Completed:
        return "completed";
    case ExecutionStatus::Failed:
        return "failed";
    case ExecutionStatus::Skipped:
        return "skipped";
    }

    return "unknown";
}

} // namespace ai::orchestration
