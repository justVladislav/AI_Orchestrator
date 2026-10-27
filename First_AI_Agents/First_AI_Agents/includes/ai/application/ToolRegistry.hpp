#pragma once

#include "ai/application/agents/AgentToolTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace ai::application {

class ToolRegistry {
public:
    [[nodiscard]] common::Result<void> registerTool(AgentToolDefinition definition);

    [[nodiscard]] std::optional<AgentToolDefinition> find(
        std::string_view name) const;

    [[nodiscard]] common::Result<void> validateCall(
        const AgentToolCall& call,
        const AgentToolExecutionLimits& limits) const;

    [[nodiscard]] const std::vector<AgentToolDefinition>& definitions() const noexcept;

    [[nodiscard]] static common::Result<ToolRegistry> createDefaultReadOnly();

private:
    std::vector<AgentToolDefinition> definitions_;
};

} // namespace ai::application
