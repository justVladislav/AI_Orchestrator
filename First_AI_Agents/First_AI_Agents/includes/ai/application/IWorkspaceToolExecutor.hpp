#pragma once

#include "ai/application/agents/AgentToolTypes.hpp"
#include "ai/application/WorkspaceReadPolicy.hpp"
#include "ai/common/Result.hpp"

namespace ai::application {

// Application boundary for bounded read-only workspace operations.
// Implementations must fail closed when path containment, audit availability,
// result-size limits, or secret scanning cannot be established.
class IWorkspaceToolExecutor {
public:
    virtual ~IWorkspaceToolExecutor() = default;

    [[nodiscard]] virtual common::Result<AgentToolResult> execute(
        const AgentToolCall& call,
        const WorkspaceReadPolicy& policy) = 0;
};

} // namespace ai::application
