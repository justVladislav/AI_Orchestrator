#include "ai/application/workspace/ToolRegistry.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace ai::application {

namespace {

common::Error registryError(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

bool validToolName(const std::string& name)
{
    return !name.empty() && name.size() <= 96
        && std::all_of(name.begin(), name.end(), [](unsigned char ch) {
            return std::islower(ch) || std::isdigit(ch) || ch == '_';
        });
}

AgentToolDefinition readTool(
    std::string name,
    std::string description,
    std::string schema,
    std::size_t maxResultBytes = 256ULL * 1024)
{
    return {
        std::move(name),
        "1",
        std::move(description),
        std::move(schema),
        true,
        maxResultBytes};
}

} // namespace

common::Result<void> ToolRegistry::registerTool(AgentToolDefinition definition)
{
    if (!validToolName(definition.name) || definition.version.empty()
        || definition.description.empty() || definition.inputSchemaJson.empty()
        || definition.maxResultBytes == 0) {
        return common::Result<void>::failure(registryError(
            "tool_registry.invalid_definition",
            "Tool definition is incomplete or invalid"));
    }
    if (!definition.readOnly) {
        return common::Result<void>::failure(registryError(
            "tool_registry.write_tool_denied",
            "Only read-only tools are accepted by this registry"));
    }
    if (find(definition.name).has_value()) {
        return common::Result<void>::failure(registryError(
            "tool_registry.duplicate_name",
            "Tool name is already registered"));
    }
    definitions_.push_back(std::move(definition));
    return common::Result<void>::ok();
}

std::optional<AgentToolDefinition> ToolRegistry::find(std::string_view name) const
{
    const auto it = std::find_if(
        definitions_.begin(), definitions_.end(),
        [&](const AgentToolDefinition& definition) {
            return definition.name == name;
        });
    if (it == definitions_.end()) return std::nullopt;
    return *it;
}

common::Result<void> ToolRegistry::validateCall(
    const AgentToolCall& call,
    const AgentToolExecutionLimits& limits) const
{
    if (call.callId.empty() || call.callId.size() > 256) {
        return common::Result<void>::failure(registryError(
            "tool_call.invalid_id",
            "Tool call identifier is missing or too long"));
    }
    const auto definition = find(call.toolName);
    if (!definition.has_value()) {
        return common::Result<void>::failure(registryError(
            "tool_call.unknown_tool",
            "Requested tool is not registered"));
    }
    if (call.argumentsJson.size() > limits.maxArgumentsBytes) {
        return common::Result<void>::failure(registryError(
            "tool_call.arguments_too_large",
            "Tool arguments exceed the configured limit"));
    }
    if (call.turnIndex >= limits.maxTurns
        || call.ordinal >= limits.maxCallsPerTurn) {
        return common::Result<void>::failure(registryError(
            "tool_call.limit_exceeded",
            "Tool call exceeds the configured turn limits"));
    }
    return common::Result<void>::ok();
}

const std::vector<AgentToolDefinition>& ToolRegistry::definitions() const noexcept
{
    return definitions_;
}

common::Result<ToolRegistry> ToolRegistry::createDefaultReadOnly()
{
    static constexpr auto pathSchema =
        R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":false})";

    ToolRegistry registry;
    const std::vector<AgentToolDefinition> tools {
        readTool(
            "workspace_list_directory",
            "List a bounded number of entries below the workspace root",
            pathSchema),
        readTool(
            "workspace_read_file",
            "Read a bounded UTF-8 text file below the workspace root",
            pathSchema),
        readTool(
            "workspace_search_text",
            "Search for literal text in bounded workspace files",
            R"({"type":"object","properties":{"path":{"type":"string"},"query":{"type":"string"}},"required":["path","query"],"additionalProperties":false})"),
        readTool(
            "workspace_find_files",
            "Recursively find files below path. glob may match a filename "
            "('*.qml') or a relative path ('**/*.qml', 'views/**/*.qml')",
            R"({"type":"object","properties":{"path":{"type":"string"},"glob":{"type":"string"}},"required":["path","glob"],"additionalProperties":false})"),
        readTool(
            "workspace_hash_file",
            "Calculate SHA-256 for a bounded workspace file",
            pathSchema,
            4096),
        readTool(
            "workspace_stat_path",
            "Return bounded metadata for a workspace path",
            pathSchema,
            4096)};

    for (auto tool : tools) {
        const auto result = registry.registerTool(std::move(tool));
        if (!result.success()) {
            return common::Result<ToolRegistry>::failure(*result.error());
        }
    }
    return common::Result<ToolRegistry>::ok(std::move(registry));
}

} // namespace ai::application
