#include "ai/application/TaskRouteResolver.hpp"

#include "ai/application/agents/AgentToolTurnLoopRunner.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <set>
#include <unordered_set>

namespace ai::application {

namespace {

common::Result<TaskRouteResolver> failure(const char* code, std::string message)
{
    return common::Result<TaskRouteResolver>::failure({code, std::move(message)});
}

std::uint64_t outputCeilingFor(const Model& model, std::uint64_t runMaxOutputTokens)
{
    // The same formula ManifestWorkflowRunner has applied since the max_output_tokens
    // incident (Run 38f03ba5): never below the worker floor, still capped by the Run.
    const std::uint64_t modelCeiling = model.maxOutputTokens > 0
        ? model.maxOutputTokens : kAgentToolTurnLoopMaxOutputTokens;
    return std::min<std::uint64_t>(
        std::max<std::uint64_t>(modelCeiling, kWorkerProposeMinOutputTokens), runMaxOutputTokens);
}

// Why a routing member's frozen provider/model cannot serve a Run NOW, or nullopt when both can.
// The two reasons share the one umbrella code (kRouteResolutionFailedCode) and are told apart by
// the message: the catalog entry is gone, or an operator switched it OFF. `enabled=false` means no
// new provider session, and a Run that replays a frozen snapshot (a Retry) does not get to bypass
// the switch.
std::optional<std::string> catalogProblem(
    const std::string& memberId,
    const common::Result<std::optional<Provider>>& provider,
    const common::Result<std::optional<Model>>& model)
{
    if (!provider.success() || !provider.value().has_value()
        || !model.success() || !model.value().has_value()) {
        return "The provider or model of routing member '" + memberId
            + "' disappeared after plan resolution";
    }
    if (!provider.value()->enabled) {
        return "The provider of routing member '" + memberId + "' is disabled";
    }
    if (!model.value()->enabled) {
        return "The model of routing member '" + memberId + "' is disabled";
    }
    return std::nullopt;
}

} // namespace

namespace {

// The member that executes a provider-backed task, from the frozen routing record
// alone. TaskRouteResolver::resolve and the record-only position allocation share it, so
// "which member runs this task" is decided in exactly one place.
common::Result<const ManifestRoutingMember*> routingMemberForTask(
    const ManifestRoutingRecord& routing, const ExecutionManifestTask& task)
{
    const auto unresolved = [&](const std::string& why) {
        return common::Result<const ManifestRoutingMember*>::failure({kRouteUnresolvedCode,
            "Task '" + task.id + "' has no resolvable route: " + why});
    };
    if (task.requestedCapability == WorkspaceCapability::None) {
        return unresolved("a coordination task executes no provider work");
    }
    const auto* member = ManifestRouting::assigneeOf(routing, task);
    if (member == nullptr) {
        return unresolved(task.assigneeMemberId.has_value()
            ? "its assignee is not a member of the frozen routing record"
            : "it names no assignee in a roster Manifest");
    }
    if (routing.mode == ManifestRoutingMode::Roster) {
        if (member->leadArchitect) return unresolved("the lead Architect only plans in a roster");
        if (!member->workspaceAccess) return unresolved("its assignee has no workspace access");
    }
    return common::Result<const ManifestRoutingMember*>::ok(member);
}

// base + Manifest task ordinal for every provider-backed task: the allocation every
// pre-roster Manifest has always used. With a routing record each task's assignee is also
// required to resolve; without one (a Manifest frozen before AG-17.1 has no routing
// record) the allocation is unchanged.
common::Result<TaskModelExecutionPositionPlan> ordinalPositionPlan(
    const ExecutionManifest& manifest, std::uint32_t base, const ManifestRoutingRecord* routing)
{
    TaskModelExecutionPositionPlan plan;
    const auto reservedLast = static_cast<std::uint64_t>(base)
        + (manifest.tasks.empty() ? 0 : manifest.tasks.size() - 1);
    if (reservedLast > std::numeric_limits<std::uint32_t>::max()) {
        return common::Result<TaskModelExecutionPositionPlan>::failure({
            "model_execution.position_invalid",
            "The Manifest task set does not fit in the model-execution position range"});
    }
    plan.lastPosition = static_cast<std::uint32_t>(reservedLast);
    for (std::size_t index = 0; index < manifest.tasks.size(); ++index) {
        const auto& task = manifest.tasks[index];
        if (task.requestedCapability == WorkspaceCapability::None) continue;
        if (routing != nullptr) {
            const auto resolved = routingMemberForTask(*routing, task);
            if (!resolved.success()) {
                return common::Result<TaskModelExecutionPositionPlan>::failure(*resolved.error());
            }
        }
        plan.byTaskId.emplace(task.id, static_cast<std::uint32_t>(base + index));
    }
    return common::Result<TaskModelExecutionPositionPlan>::ok(std::move(plan));
}

} // namespace

common::Result<TaskRouteResolver> TaskRouteResolver::create(
    ManifestRoutingRecord routing,
    std::vector<TaskRouteInput> inputs,
    std::uint64_t runMaxOutputTokens)
{
    if (const auto problem = ManifestRouting::recordProblem(routing)) {
        return failure(kRouteResolutionFailedCode,
            "The frozen routing record is defective: " + *problem);
    }
    if (inputs.size() != routing.members.size()) {
        return failure(kRouteResolutionFailedCode,
            "The route inputs do not correspond one to one to the routing members");
    }
    std::set<std::string> seen;
    for (const auto& input : inputs) {
        if (!seen.insert(input.memberId).second) {
            return failure(kRouteResolutionFailedCode, "A routing member has two route inputs");
        }
    }

    TaskRouteResolver resolver;
    resolver.routing_ = std::move(routing);
    resolver.routes_.reserve(resolver.routing_.members.size());
    for (const auto& member : resolver.routing_.members) {
        const auto input = std::find_if(inputs.begin(), inputs.end(),
            [&](const TaskRouteInput& candidate) { return candidate.memberId == member.memberId; });
        if (input == inputs.end()) {
            return failure(kRouteResolutionFailedCode,
                "Routing member '" + member.memberId + "' has no route input");
        }
        if (input->provider.id != member.providerId || input->model.id != member.modelId
            || input->model.providerId != member.providerId) {
            return failure(kRouteIdentityMismatchCode,
                "Routing member '" + member.memberId + "' no longer resolves to its frozen "
                "provider/model");
        }
        const bool effectiveLead = resolver.routing_.mode == ManifestRoutingMode::SingleAssignment
            ? true : input->leadArchitect;
        if (input->role != member.role) {
            return failure(kRouteIdentityMismatchCode,
                "Routing member '" + member.memberId + "' no longer carries its frozen role");
        }
        if (ManifestRouting::instructionsHash(input->instructions) != member.instructionsHash) {
            return failure(kRouteIdentityMismatchCode,
                "Routing member '" + member.memberId + "' no longer carries its frozen "
                "instructions");
        }
        if (effectiveLead != member.leadArchitect
            || input->workspaceAccess != member.workspaceAccess
            || input->position != member.position
            || input->timeoutMs != member.timeoutMs) {
            return failure(kRouteIdentityMismatchCode,
                "Routing member '" + member.memberId + "' no longer carries its frozen "
                "capability, position, or timeout identity");
        }
        if (input->provider.kind.empty()) {
            return failure(kRouteResolutionFailedCode,
                "Routing member '" + member.memberId + "' resolves to a provider without a kind");
        }

        ResolvedTaskRoute route;
        route.memberId = member.memberId;
        route.providerId = member.providerId;
        route.modelId = member.modelId;
        route.apiModelId = input->model.apiModelId;
        route.providerKind = input->provider.kind;
        route.agentRole = member.role;
        route.maxOutputTokens = outputCeilingFor(input->model, runMaxOutputTokens);
        route.pricingModel = input->model;
        if (resolver.routing_.mode == ManifestRoutingMode::Roster) {
            route.modelExecutionPosition = member.position;
            route.turnTimeout = std::max(kWorkerProposeTurnTimeout,
                std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(member.timeoutMs)));
            route.workerInstructions = input->instructions;
        } else {
            route.turnTimeout = kWorkerProposeTurnTimeout;
        }
        resolver.routes_.push_back(std::move(route));
    }
    return common::Result<TaskRouteResolver>::ok(std::move(resolver));
}

common::Result<ResolvedTaskRoute> TaskRouteResolver::resolve(const ExecutionManifestTask& task) const
{
    const auto resolvedMember = routingMemberForTask(routing_, task);
    if (!resolvedMember.success()) {
        return common::Result<ResolvedTaskRoute>::failure(*resolvedMember.error());
    }
    const auto* member = resolvedMember.value();
    const auto index = static_cast<std::size_t>(member - routing_.members.data());
    auto route = routes_.at(index);
    if (routing_.mode == ManifestRoutingMode::SingleAssignment) {
        route.agentRole = task.role;
    }
    return common::Result<ResolvedTaskRoute>::ok(std::move(route));
}

common::Result<TaskModelExecutionPositionPlan>
TaskRouteResolver::allocateModelExecutionPositions(
    const ExecutionManifest& manifest, std::uint32_t singleAssignmentBase) const
{
    return allocateModelExecutionPositionsFromRecord(routing_, manifest, singleAssignmentBase);
}

common::Result<TaskModelExecutionPositionPlan> allocateModelExecutionPositionsFromRecord(
    const ManifestRoutingRecord& routing,
    const ExecutionManifest& manifest,
    std::uint32_t singleAssignmentBase)
{
    TaskModelExecutionPositionPlan plan;
    if (routing.mode == ManifestRoutingMode::SingleAssignment) {
        return ordinalPositionPlan(manifest, singleAssignmentBase, &routing);
    }

    std::uint64_t maxMemberPosition = 0;
    for (const auto& member : routing.members) {
        maxMemberPosition = std::max<std::uint64_t>(maxMemberPosition, member.position);
    }
    std::unordered_set<std::string> seenMembers;
    std::uint64_t repeatedTaskOrdinal = 0;
    for (const auto& task : manifest.tasks) {
        if (task.requestedCapability == WorkspaceCapability::None) continue;
        const auto resolved = routingMemberForTask(routing, task);
        if (!resolved.success()) {
            return common::Result<TaskModelExecutionPositionPlan>::failure(*resolved.error());
        }
        const auto* member = resolved.value();
        const bool firstForMember = seenMembers.insert(member->memberId).second;
        // In a roster a member's first task keeps its frozen position (the route's
        // modelExecutionPosition is exactly this value).
        std::uint64_t position = member->position;
        if (!firstForMember) {
            position = maxMemberPosition + 1 + repeatedTaskOrdinal;
            ++repeatedTaskOrdinal;
        }
        if (position > std::numeric_limits<std::uint32_t>::max()) {
            return common::Result<TaskModelExecutionPositionPlan>::failure({
                "model_execution.position_invalid",
                "The roster task set does not fit in the model-execution position range"});
        }
        plan.byTaskId.emplace(task.id, static_cast<std::uint32_t>(position));
    }
    const auto reservedLast = maxMemberPosition + repeatedTaskOrdinal;
    if (reservedLast > std::numeric_limits<std::uint32_t>::max()) {
        return common::Result<TaskModelExecutionPositionPlan>::failure({
            "model_execution.position_invalid",
            "The roster task set does not fit in the model-execution position range"});
    }
    plan.lastPosition = static_cast<std::uint32_t>(reservedLast);
    return common::Result<TaskModelExecutionPositionPlan>::ok(std::move(plan));
}

common::Result<TaskModelExecutionPositionPlan> allocateWorkerModelExecutionPositions(
    const TaskRouteResolver* resolver,
    const ExecutionManifest& manifest,
    std::uint32_t singleAssignmentBase)
{
    if (resolver != nullptr) {
        return allocateModelExecutionPositionsFromRecord(
            resolver->routing(), manifest, singleAssignmentBase);
    }
    return ordinalPositionPlan(manifest, singleAssignmentBase, nullptr);
}

common::Result<TaskRouteResolver> buildTaskRouteResolver(
    const ManifestRoutingRecord& routing,
    const std::vector<StageModelAssignment>& assignments,
    const IProviderRepository& providers,
    const IModelRepository& models,
    std::uint64_t runMaxOutputTokens)
{
    std::vector<TaskRouteInput> inputs;
    inputs.reserve(routing.members.size());
    for (const auto& member : routing.members) {
        const auto assignment = std::find_if(assignments.begin(), assignments.end(),
            [&](const StageModelAssignment& candidate) { return candidate.stageType == member.memberId; });
        if (assignment == assignments.end()) {
            return failure(kRouteResolutionFailedCode,
                "Routing member '" + member.memberId + "' is not in the Run's assignment snapshot");
        }
        const auto provider = providers.findById(assignment->providerId);
        const auto model = models.findById(assignment->modelId);
        if (const auto problem = catalogProblem(member.memberId, provider, model)) {
            return failure(kRouteResolutionFailedCode, *problem);
        }
        TaskRouteInput input;
        input.memberId = member.memberId;
        input.provider = *provider.value();
        input.model = *model.value();
        input.role = ManifestRouting::effectiveRole(*assignment);
        input.instructions = assignment->instructions;
        input.leadArchitect = assignment->leadArchitect;
        input.workspaceAccess = assignment->workspaceAccess;
        input.position = assignment->position;
        input.timeoutMs = ManifestRouting::effectiveTimeoutMs(*assignment);
        inputs.push_back(std::move(input));
    }
    return TaskRouteResolver::create(routing, std::move(inputs), runMaxOutputTokens);
}

std::string manifestRouteModelLabel(const Provider& provider, const Model& model)
{
    const std::string modelName = model.displayName.empty() ? model.apiModelId : model.displayName;
    return provider.name.empty() ? modelName : provider.name + " / " + modelName;
}

common::Result<ManifestRunRoutingContext> buildManifestRunRoutingContext(
    const ManifestRoutingRecord& routing,
    const std::vector<StageModelAssignment>& assignments,
    const IProviderRepository& providers,
    const IModelRepository& models,
    std::uint64_t runMaxOutputTokens)
{
    auto taskRoutes = buildTaskRouteResolver(
        routing, assignments, providers, models, runMaxOutputTokens);
    if (!taskRoutes.success()) {
        return common::Result<ManifestRunRoutingContext>::failure(*taskRoutes.error());
    }

    const auto leadMember = std::find_if(routing.members.begin(), routing.members.end(),
        [](const ManifestRoutingMember& member) { return member.leadArchitect; });
    if (leadMember == routing.members.end()) {
        return common::Result<ManifestRunRoutingContext>::failure({
            kRouteResolutionFailedCode,
            "The frozen routing record has no lead Architect"});
    }
    const auto leadAssignment = std::find_if(assignments.begin(), assignments.end(),
        [&](const StageModelAssignment& assignment) {
            return assignment.stageType == leadMember->memberId;
        });
    if (leadAssignment == assignments.end()) {
        return common::Result<ManifestRunRoutingContext>::failure({
            kRouteResolutionFailedCode,
            "The lead Architect is not in the Run's assignment snapshot"});
    }
    const auto leadProvider = providers.findById(leadAssignment->providerId);
    const auto leadModel = models.findById(leadAssignment->modelId);
    if (const auto problem = catalogProblem(leadMember->memberId, leadProvider, leadModel)) {
        return common::Result<ManifestRunRoutingContext>::failure({kRouteResolutionFailedCode, *problem});
    }

    std::vector<ManifestRosterMemberInfo> rosterCatalog;
    if (routing.mode == ManifestRoutingMode::Roster) {
        // Defense in depth for a snapshot that bypassed profile validation: with no
        // assignable worker no plan can be valid, so fail before the Planner, its
        // retry authorization and its accounting row exist.
        if (ManifestRouting::assignableMemberIds(routing).empty()) {
            return common::Result<ManifestRunRoutingContext>::failure({
                "planner.roster_no_assignable_member",
                "The roster has no worker with workspace access, so no task could be assigned"});
        }
        rosterCatalog.reserve(routing.members.size());
        for (const auto& member : routing.members) {
            const auto assignment = std::find_if(assignments.begin(), assignments.end(),
                [&](const StageModelAssignment& candidate) {
                    return candidate.stageType == member.memberId;
                });
            if (assignment == assignments.end()) {
                return common::Result<ManifestRunRoutingContext>::failure({
                    kRouteResolutionFailedCode,
                    "A routing member is absent from the Run's assignment snapshot"});
            }
            const auto provider = providers.findById(assignment->providerId);
            const auto model = models.findById(assignment->modelId);
            if (const auto problem = catalogProblem(member.memberId, provider, model)) {
                return common::Result<ManifestRunRoutingContext>::failure({kRouteResolutionFailedCode, *problem});
            }
            const std::string modelLabel =
                manifestRouteModelLabel(*provider.value(), *model.value());
            rosterCatalog.push_back(
                {member.memberId, modelLabel, assignment->instructions});
        }
        if (const auto problem = ManifestRouting::rosterCatalogProblem(
                routing, rosterCatalog)) {
            return common::Result<ManifestRunRoutingContext>::failure({
                "planner.roster_catalog_invalid", *problem});
        }
    }
    ManifestRunRoutingContext context {
        routing,
        std::move(taskRoutes.value()),
        *leadAssignment,
        *leadProvider.value(),
        *leadModel.value(),
        std::move(rosterCatalog)};
    return common::Result<ManifestRunRoutingContext>::ok(std::move(context));
}

} // namespace ai::application
