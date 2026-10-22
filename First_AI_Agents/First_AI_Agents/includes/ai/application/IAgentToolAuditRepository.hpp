#pragma once

#include "ai/application/AgentToolTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ai::application {

// Persistence boundary for fail-closed tool execution. A coordinator must
// record Requested before invoking a tool, advance to Running, and persist a
// terminal state. If any required audit write fails, execution must stop.
class IAgentToolAuditRepository {
public:
    virtual ~IAgentToolAuditRepository() = default;

    [[nodiscard]] virtual common::Result<void> insert(
        const AgentToolAuditRecord& record) = 0;

    [[nodiscard]] virtual common::Result<void> update(
        const AgentToolAuditRecord& record) = 0;

    [[nodiscard]] virtual common::Result<std::optional<AgentToolAuditRecord>> find(
        const std::string& auditId) const = 0;

    [[nodiscard]] virtual common::Result<std::vector<AgentToolAuditRecord>> listForSession(
        const std::string& sessionId) const = 0;

    // Per-session recovery: change Requested/Running rows in one session to
    // Interrupted. Used when a coordinator detects a session is being aborted.
    [[nodiscard]] virtual common::Result<std::size_t> interruptRunning(
        const std::string& sessionId) = 0;

    // Startup recovery: change ALL Requested/Running rows across every session
    // to Interrupted before BackendRuntime exposes anything to callers.
    // Returns the total count of rows updated.
    [[nodiscard]] virtual common::Result<std::size_t> interruptAllRunning() = 0;
};

} // namespace ai::application
