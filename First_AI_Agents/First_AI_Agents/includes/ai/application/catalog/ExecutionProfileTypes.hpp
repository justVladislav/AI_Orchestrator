#pragma once

#include "ai/application/catalog/ProviderCatalogTypes.hpp"
#include "ai/application/ProviderExecutionBudget.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

using ExecutionProfileId = std::string;

// Minimal stable stage-type vocabulary this profile format supports.
// MockWorkflowRunner's own stage types ("context", "architect",
// "implementation", "documentation") are a superset used only by the
// deterministic Mock pipeline; a profile only assigns providers/models to
// the three stages a real provider workflow will actually need model
// selection for.
[[nodiscard]] bool isKnownExecutionStageType(std::string_view stageType) noexcept;

// One stage's provider/model/timeout selection inside an ExecutionProfile.
struct StageModelAssignment {
    std::string stageType;
    ProviderId providerId;
    ModelId modelId;
    std::chrono::milliseconds timeout {0};
    std::string agentName;
    std::string role;
    std::string instructions;
    std::uint32_t position {0};
    bool leadArchitect {false};

    // When true, this agent's task runs through the shared
    // AgentToolTurnLoopRunner (read-only workspace tools + propose_change_set)
    // instead of a single-shot completion. Independent of leadArchitect: a
    // lead Architect typically stays workspaceAccess == false (it plans/
    // delegates, it does not itself touch files), while any regular roster
    // agent may opt in. Ignored by MockWorkflowRunner. A workspaceAccess
    // agent does not currently support maxAttempts > 1 or a fallback
    // provider/model - see ExecutionAssignmentValidation.
    bool workspaceAccess {false};

    // Stable task ids this agent must wait for. Empty dependencies are
    // normalized by the production runner: the lead architect is the root,
    // regular agents depend on the architect, and the synthetic final
    // synthesis depends on every regular agent.
    std::vector<std::string> dependencies;

    // The first call counts as attempt 1. Transient failures may be retried
    // up to this bound; every attempt is recorded as its own ModelExecution.
    std::uint32_t maxAttempts {1};

    // Optional provider/model pair used only after the primary attempts are
    // exhausted. Both values must be present together and are validated at
    // plan resolution time.
    std::optional<ProviderId> fallbackProviderId;
    std::optional<ModelId> fallbackModelId;
};

/**
 * A saved, named configuration of which provider/model/timeout each stage of
 * a workflow should use. WorkflowRouter validates and resolves this durable
 * configuration into an immutable run plan before RunManager starts work.
 */
struct ExecutionProfile {
    ExecutionProfileId id;
    std::string name;
    std::string workflowId;
    bool enabled {true};
    bool isDefault {false};
    std::vector<StageModelAssignment> assignments;
    std::chrono::system_clock::time_point createdAt;
    std::chrono::system_clock::time_point updatedAt;
    // Optional user-selected reductions below Backend hard ceilings. The
    // resolver freezes a complete ProviderExecutionBudgetPolicy onto each Run.
    ProviderExecutionBudgetOverrides providerBudget;
};

// isDefault is absent here for the same reason as CreateModelRequest: only
// the dedicated atomic setDefaultProfile operation may set it.
struct CreateExecutionProfileRequest {
    std::string name;
    std::string workflowId;
    bool enabled {true};
    std::vector<StageModelAssignment> assignments;
    ProviderExecutionBudgetOverrides providerBudget;
};

struct UpdateExecutionProfileRequest {
    ExecutionProfileId id;
    std::string name;
    std::string workflowId;
    std::vector<StageModelAssignment> assignments;
    ProviderExecutionBudgetOverrides providerBudget;
};

} // namespace ai::application
