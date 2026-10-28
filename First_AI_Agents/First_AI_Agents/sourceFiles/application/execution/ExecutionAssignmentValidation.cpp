#include "ai/application/execution/ExecutionAssignmentValidation.hpp"

#include "ai/application/execution/IWorkflowRunner.hpp"
#include "ai/application/execution/ManifestRouting.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ai::application {

namespace {

common::Error error(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

common::Result<void> validateExecutionPolicy(
    const StageModelAssignment& assignment,
    const IProviderRepository& providers,
    const IModelRepository& models)
{
    if (assignment.maxAttempts == 0 || assignment.maxAttempts > 10) {
        return common::Result<void>::failure(error(
            "run.invalid_max_attempts", "Agent attempts must be between 1 and 10"));
    }
    // AgentToolTurnLoopRunner has one primary attempt plus one narrow format-
    // repair retry, not the general attempt-loop below - rather than
    // silently ignore a configured retry/fallback policy for a
    // workspaceAccess agent, reject the combination outright so a
    // misconfigured roster fails fast at submission time.
    if (assignment.workspaceAccess
        && (assignment.maxAttempts > 1 || assignment.fallbackProviderId.has_value())) {
        return common::Result<void>::failure(error(
            "run.workspace_access_no_retry",
            "A workspace-access agent does not support maxAttempts > 1 or a fallback provider/model"));
    }
    if (assignment.fallbackProviderId.has_value()
        != assignment.fallbackModelId.has_value()) {
        return common::Result<void>::failure(error(
            "run.incomplete_fallback",
            "Fallback provider and model must be configured together"));
    }
    if (!assignment.fallbackProviderId.has_value()) {
        return common::Result<void>::ok();
    }

    const auto provider = providers.findById(*assignment.fallbackProviderId);
    if (!provider.success()) return common::Result<void>::failure(*provider.error());
    if (!provider.value().has_value()) {
        return common::Result<void>::failure(
            error("provider.not_found", "Fallback provider not found"));
    }
    if (!provider.value()->enabled) {
        return common::Result<void>::failure(
            error("provider.disabled", "Fallback provider is disabled"));
    }
    const auto model = models.findById(*assignment.fallbackModelId);
    if (!model.success()) return common::Result<void>::failure(*model.error());
    if (!model.value().has_value()) {
        return common::Result<void>::failure(
            error("model.not_found", "Fallback model not found"));
    }
    if (!model.value()->enabled) {
        return common::Result<void>::failure(
            error("model.disabled", "Fallback model is disabled"));
    }
    if (model.value()->providerId != *assignment.fallbackProviderId) {
        return common::Result<void>::failure(error(
            "model.provider_mismatch",
            "Fallback model does not belong to the fallback provider"));
    }
    return common::Result<void>::ok();
}

common::Result<void> validateDependencyGraph(
    const std::vector<StageModelAssignment>& assignments)
{
    std::unordered_map<std::string, const StageModelAssignment*> byId;
    for (const auto& assignment : assignments) byId.emplace(assignment.stageType, &assignment);

    for (const auto& assignment : assignments) {
        std::unordered_set<std::string> unique;
        for (const auto& dependency : assignment.dependencies) {
            if (dependency == assignment.stageType || !byId.contains(dependency)
                || !unique.insert(dependency).second) {
                return common::Result<void>::failure(error(
                    "run.invalid_dependency",
                    "Dependencies must be unique existing task ids and cannot reference themselves"));
            }
        }
    }

    std::unordered_map<std::string, int> state;
    std::function<bool(const std::string&)> hasCycle = [&](const std::string& id) {
        int& current = state[id];
        if (current == 1) return true;
        if (current == 2) return false;
        current = 1;
        for (const auto& dependency : byId.at(id)->dependencies) {
            if (hasCycle(dependency)) return true;
        }
        current = 2;
        return false;
    };
    for (const auto& [id, assignment] : byId) {
        static_cast<void>(assignment);
        if (hasCycle(id)) {
            return common::Result<void>::failure(
                error("run.dependency_cycle", "Agent dependency graph must be acyclic"));
        }
    }
    return common::Result<void>::ok();
}

} // namespace

common::Result<void> validateExecutionAssignments(
    const std::vector<StageModelAssignment>& assignments,
    const IProviderRepository& providers,
    const IModelRepository& models)
{
    std::unordered_set<std::string> seenStageTypes;
    for (const auto& assignment : assignments) {
        if (!isKnownExecutionStageType(assignment.stageType)) {
            return common::Result<void>::failure(error(
                "execution_profile.unknown_stage_type",
                "Stage type must be one of: architect, implementation, documentation"));
        }
        if (!seenStageTypes.insert(assignment.stageType).second) {
            return common::Result<void>::failure(error(
                "execution_profile.duplicate_stage_type",
                "A stage type cannot appear more than once in one profile"));
        }
        if (assignment.timeout <= std::chrono::milliseconds {0}) {
            return common::Result<void>::failure(error(
                "execution_profile.invalid_timeout", "Stage timeout must be positive"));
        }
        const auto policy = validateExecutionPolicy(assignment, providers, models);
        if (!policy.success()) return policy;

        const auto provider = providers.findById(assignment.providerId);
        if (!provider.success()) {
            return common::Result<void>::failure(*provider.error());
        }
        if (!provider.value().has_value()) {
            return common::Result<void>::failure(error("provider.not_found", "Provider not found"));
        }
        if (!provider.value()->enabled) {
            return common::Result<void>::failure(
                error("provider.disabled", "Provider is disabled"));
        }

        const auto model = models.findById(assignment.modelId);
        if (!model.success()) {
            return common::Result<void>::failure(*model.error());
        }
        if (!model.value().has_value()) {
            return common::Result<void>::failure(error("model.not_found", "Model not found"));
        }
        if (model.value()->providerId != assignment.providerId) {
            return common::Result<void>::failure(error(
                "model.provider_mismatch", "Model does not belong to the assignment's provider"));
        }
        if (!model.value()->enabled) {
            return common::Result<void>::failure(error("model.disabled", "Model is disabled"));
        }
    }
    return validateDependencyGraph(assignments);
}

common::Result<void> validateExecutionProfileWorkflow(
    std::string_view workflowId,
    const std::vector<StageModelAssignment>& assignments)
{
    if (workflowId != kExecutionManifestWorkflowId) {
        return common::Result<void>::ok();
    }
    // A saved profile persists only stage/provider/model/timeout, so it can only
    // ever be the historical one-Architect form: a multi-member roster would lose
    // its lead flag, workspace access, positions, roles and instructions on the
    // way to SQLite and then fail at Run start with a confusing code. A roster is
    // configured per Run (validateManifestRoster).
    if (assignments.size() == 1 && assignments.front().stageType == "architect") {
        return common::Result<void>::ok();
    }
    return common::Result<void>::failure(error(
        "execution_profile.manifest_requires_single_architect",
        "A saved Manifest profile requires exactly one Architect assignment; configure a "
        "multi-model roster for a single Run instead"));
}

common::Result<void> validateManifestRoster(
    const std::vector<StageModelAssignment>& assignments)
{
    if (assignments.empty()) {
        return common::Result<void>::failure(error(
            "run.manifest_roster_empty", "A Manifest roster needs at least a lead Architect"));
    }
    // One member is the single-assignment Manifest (that member plans and executes;
    // it is the lead by construction).
    if (assignments.size() == 1) return common::Result<void>::ok();

    const auto leadCount = std::count_if(assignments.begin(), assignments.end(),
        [](const StageModelAssignment& assignment) { return assignment.leadArchitect; });
    if (leadCount != 1) {
        return common::Result<void>::failure(error(
            "run.manifest_roster_requires_one_lead",
            "A multi-member Manifest roster requires exactly one lead Architect"));
    }
    const auto routing = ManifestRouting::fromAssignments(assignments);
    if (const auto problem = ManifestRouting::recordProblem(routing)) {
        return common::Result<void>::failure(error("run.manifest_roster_invalid", *problem));
    }
    // A roster is a pool of AVAILABLE workers, but a Manifest can only assign
    // provider-backed tasks to a non-lead worker with workspace access. Without one
    // no plan could ever be valid, so refuse the roster now rather than after a
    // Run, a planning-retry authorization and a Planner accounting row exist.
    if (ManifestRouting::assignableMemberIds(routing).empty()) {
        return common::Result<void>::failure(error(
            "run.manifest_roster_requires_workspace_worker",
            "A multi-member Manifest roster needs at least one non-lead worker with "
            "workspace access, otherwise no task could be assigned"));
    }
    return common::Result<void>::ok();
}

common::Result<void> validateRunRoster(
    const std::vector<StageModelAssignment>& assignments,
    const IProviderRepository& providers,
    const IModelRepository& models)
{
    if (assignments.empty() || assignments.size() > 64) {
        return common::Result<void>::failure(error(
            "run.invalid_roster_size", "A production run requires between 1 and 64 agents"));
    }

    std::unordered_set<std::string> stageIds;
    std::unordered_set<std::uint32_t> positions;
    std::size_t architectCount = 0;
    for (const auto& assignment : assignments) {
        if (assignment.stageType.empty() || assignment.stageType.size() > 80) {
            return common::Result<void>::failure(error(
                "run.invalid_agent_id", "Every agent requires a stable stage id"));
        }
        if (!stageIds.insert(assignment.stageType).second
                || !positions.insert(assignment.position).second) {
            return common::Result<void>::failure(error(
                "run.duplicate_agent", "Agent stage ids and positions must be unique"));
        }
        if (assignment.role.empty() || assignment.role.size() > 160
                || assignment.agentName.size() > 160
                || assignment.instructions.size() > 16000) {
            return common::Result<void>::failure(error(
                "run.invalid_agent_metadata", "Agent role, name, or instructions are invalid"));
        }
        if (assignment.leadArchitect) ++architectCount;
        if (assignment.timeout <= std::chrono::milliseconds {0}) {
            return common::Result<void>::failure(error(
                "execution_profile.invalid_timeout", "Agent timeout must be positive"));
        }
        const auto policy = validateExecutionPolicy(assignment, providers, models);
        if (!policy.success()) return policy;

        const auto provider = providers.findById(assignment.providerId);
        if (!provider.success()) return common::Result<void>::failure(*provider.error());
        if (!provider.value().has_value()) {
            return common::Result<void>::failure(error("provider.not_found", "Provider not found"));
        }
        if (!provider.value()->enabled) {
            return common::Result<void>::failure(error("provider.disabled", "Provider is disabled"));
        }
        const auto model = models.findById(assignment.modelId);
        if (!model.success()) return common::Result<void>::failure(*model.error());
        if (!model.value().has_value()) {
            return common::Result<void>::failure(error("model.not_found", "Model not found"));
        }
        if (!model.value()->enabled) {
            return common::Result<void>::failure(error("model.disabled", "Model is disabled"));
        }
        if (model.value()->providerId != assignment.providerId) {
            return common::Result<void>::failure(error(
                "model.provider_mismatch", "Model does not belong to the assignment's provider"));
        }
    }
    if (architectCount != 1) {
        return common::Result<void>::failure(error(
            "run.invalid_architect_count", "A production run requires exactly one lead architect"));
    }
    return validateDependencyGraph(assignments);
}

} // namespace ai::application
