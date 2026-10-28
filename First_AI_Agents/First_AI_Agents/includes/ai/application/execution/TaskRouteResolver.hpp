#pragma once

#include "ai/application/execution/ExecutionManifestTypes.hpp"
#include "ai/application/catalog/ExecutionProfileTypes.hpp"
#include "ai/application/catalog/IModelRepository.hpp"
#include "ai/application/catalog/IProviderRepository.hpp"
#include "ai/application/execution/ManifestRouting.hpp"
#include "ai/common/Result.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ai::application {

// AG-17.2: the one place that turns a frozen Manifest task into the concrete route
// its worker session runs on - provider, model, output ceiling, turn timeout,
// pricing and (in a roster) the member's own instructions.
//
// Authority: the provider never chooses any of it. The task names a member
// (ExecutionManifestTask::assigneeMemberId, or the sole worker of a
// single-assignment Manifest), the frozen ManifestRoutingRecord identifies that
// member, and the Backend resolved the facts behind it once, from the Run's frozen
// assignment snapshot and the provider/model repositories, then VERIFIED them
// against the record (provider, model, role, SHA-256 of the instructions,
// lead/workspace flags, position and effective timeout). A resolver therefore
// cannot silently drift from the identity that was frozen.
//
// The resolver is an immutable value: it reads nothing after construction, calls no
// provider and runs no command. ExecutionManifestExecutor consumes it per task;
// every Run-wide guard (budget, capacity reservation, terminal forcing) stays shared.

inline constexpr const char* kRouteUnresolvedCode = "manifest.route_unresolved";
inline constexpr const char* kRouteIdentityMismatchCode = "manifest.route_identity_mismatch";
inline constexpr const char* kRouteResolutionFailedCode = "manifest.route_resolution_failed";

struct ResolvedTaskRoute {
    // The roster member that executes the task.
    std::string memberId;
    ProviderId providerId;
    ModelId modelId;
    std::string apiModelId;
    std::string providerKind;
    // Durable agent identity used by ModelExecution and proposals. In a roster
    // this is the selected member's frozen role; in single_assignment resolve()
    // substitutes the task role to preserve the pre-AG-17 execution contract.
    std::string agentRole;
    // A roster member's first ModelExecution retains this stable position. If the
    // member owns more than one task, the Executor deterministically places later
    // tasks after the greatest frozen member position, preserving SQLite's unique
    // (source, sourceId, position, attempt) identity. Single-assignment leaves this
    // empty and keeps the existing task-ordinal allocation.
    std::optional<std::uint32_t> modelExecutionPosition;
    // Output-token ceiling of this route's worker turns:
    // min(max(model max_output_tokens or the Backend default, kWorkerProposeMinOutputTokens),
    //     the Run provider budget's ceiling).
    std::uint64_t maxOutputTokens {0};
    // The route's own model record: its pricing prices this route's executions.
    std::optional<Model> pricingModel;
    // single_assignment: exactly kWorkerProposeTurnTimeout (unchanged behaviour).
    // roster: max(member timeout, kWorkerProposeTurnTimeout) - a member can raise the
    // Backend's incident-driven floor, never lower it.
    std::chrono::milliseconds turnTimeout {0};
    // Appended to the worker system prompt as "Additional instructions". Empty for
    // single_assignment (the Architect's instructions never reached workers before
    // AG-17); the member's own text in a roster.
    std::string workerInstructions;
};

struct TaskModelExecutionPositionPlan {
    std::unordered_map<std::string, std::uint32_t> byTaskId;
    // The first free caller-owned position is lastPosition + 1. The plan also
    // reserves gaps implied by the frozen roster or legacy Manifest order.
    std::uint32_t lastPosition {0};
};

// Backend-resolved facts about one routing member. Built by buildTaskRouteResolver
// from the Run's frozen assignment snapshot; never provider-authored.
struct TaskRouteInput {
    std::string memberId;
    Provider provider;
    Model model;
    std::string role;         // ManifestRouting::effectiveRole(assignment)
    std::string instructions; // the exact text whose SHA-256 the record holds
    bool leadArchitect {false};
    bool workspaceAccess {false};
    std::uint32_t position {0};
    std::uint64_t timeoutMs {0}; // ManifestRouting::effectiveTimeoutMs(assignment)
};

class TaskRouteResolver final {
public:
    // Fails with kRouteIdentityMismatchCode when an input disagrees with the frozen
    // record (provider id, model id, the model's owning provider, role, instruction
    // hash, lead/workspace flags, position and effective timeout) or
    // kRouteResolutionFailedCode when members and inputs do not correspond one to
    // one, a provider has no kind, or the record itself is defective.
    [[nodiscard]] static common::Result<TaskRouteResolver> create(
        ManifestRoutingRecord routing,
        std::vector<TaskRouteInput> inputs,
        std::uint64_t runMaxOutputTokens);

    // The route of a provider-backed (Inspect/Propose) task. Fails with
    // kRouteUnresolvedCode for a coordination task, an unknown/missing assignee, or
    // - in a roster - an assignee that is the lead or has no workspace access (the
    // validator already rejects those at freeze; this re-check protects seeded,
    // restarted or legacy Manifests, before any worker is dispatched).
    [[nodiscard]] common::Result<ResolvedTaskRoute> resolve(
        const ExecutionManifestTask& task) const;

    // Sole route-aware allocator for durable ModelExecution positions.
    // Single-assignment preserves base + Manifest task ordinal. Roster keeps a
    // member's frozen position for its first task and assigns later tasks after
    // the greatest frozen member position. The result is independent of dispatch
    // order and safe for SQLite's unique (source, sourceId, position, attempt).
    [[nodiscard]] common::Result<TaskModelExecutionPositionPlan>
    allocateModelExecutionPositions(const ExecutionManifest& manifest,
        std::uint32_t singleAssignmentBase) const;

    [[nodiscard]] const ManifestRoutingRecord& routing() const noexcept { return routing_; }

private:
    TaskRouteResolver() = default;

    ManifestRoutingRecord routing_;
    // Parallel to routing_.members.
    std::vector<ResolvedTaskRoute> routes_;
};

// AG-17.5: one authoritative resolution of the Run's frozen routing identity.
// The lead route owns planning and optional integration; taskRoutes owns every
// provider-backed Manifest task. rosterCatalog is Backend-built display/prompt
// context and is empty for SingleAssignment. All values are copies so callers
// cannot observe later repository or assignment mutations through this object.
struct ManifestRunRoutingContext {
    ManifestRoutingRecord routing;
    TaskRouteResolver taskRoutes;
    StageModelAssignment leadAssignment;
    Provider leadProvider;
    Model leadModel;
    std::vector<ManifestRosterMemberInfo> rosterCatalog;
};

// AG-17.7B: the same allocation from the frozen routing record alone - no repository, no
// route inputs. TaskRouteResolver::allocateModelExecutionPositions is exactly this over its
// own record, so a read-side projection (member usage attribution) that inverts the
// allocation uses the writer's code, never a second copy of it.
[[nodiscard]] common::Result<TaskModelExecutionPositionPlan> allocateModelExecutionPositionsFromRecord(
    const ManifestRoutingRecord& routing,
    const ExecutionManifest& manifest,
    std::uint32_t singleAssignmentBase);

// The one entry point for worker ModelExecution positions, with or without a route
// resolver: with one it is TaskRouteResolver::allocateModelExecutionPositions; without
// one (a Manifest frozen before AG-17.1 carries no routing record) it is the
// unchanged base + Manifest task ordinal allocation.
[[nodiscard]] common::Result<TaskModelExecutionPositionPlan> allocateWorkerModelExecutionPositions(
    const TaskRouteResolver* resolver,
    const ExecutionManifest& manifest,
    std::uint32_t singleAssignmentBase);

// Backend builder over the Run's frozen assignment snapshot and the repositories
// (read-only). Fails with kRouteResolutionFailedCode when a routing member has no
// assignment, a provider or model is missing, and with kRouteIdentityMismatchCode
// when what the repositories/snapshot hold no longer matches the frozen record.
[[nodiscard]] common::Result<TaskRouteResolver> buildTaskRouteResolver(
    const ManifestRoutingRecord& routing,
    const std::vector<StageModelAssignment>& assignments,
    const IProviderRepository& providers,
    const IModelRepository& models,
    std::uint64_t runMaxOutputTokens);

// The one display label of a route: "Provider / Model", with the model API id when it has
// no display name and the model alone when the provider has no name. Shared by the
// Planner's roster catalog and the Run Details roster projection so they never differ.
[[nodiscard]] std::string manifestRouteModelLabel(const Provider& provider, const Model& model);

// Resolves the full frozen assignment snapshot before the Planner or any
// worker is dispatched. Besides TaskRouteResolver's identity checks, this
// selects the one frozen lead Architect and builds the exact catalog shown to
// the roster-constrained Planner from repository-owned provider/model labels.
[[nodiscard]] common::Result<ManifestRunRoutingContext> buildManifestRunRoutingContext(
    const ManifestRoutingRecord& routing,
    const std::vector<StageModelAssignment>& assignments,
    const IProviderRepository& providers,
    const IModelRepository& models,
    std::uint64_t runMaxOutputTokens);

} // namespace ai::application
