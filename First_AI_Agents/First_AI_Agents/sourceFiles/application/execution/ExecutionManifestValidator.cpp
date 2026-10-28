#include "ai/application/execution/ExecutionManifestValidator.hpp"

#include "ai/application/execution/ExecutionManifestDependencyGraph.hpp"
#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/verification/ManifestBootstrapValidator.hpp"
#include "ai/application/verification/ManifestMandatoryVerificationPolicy.hpp"
#include "ai/application/ManifestRouting.hpp"
#include "ai/application/RunWorkspaceScopeTypes.hpp"
#include "ai/application/workspace/WorkspaceReadPolicy.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace ai::application {

namespace {

constexpr std::uint32_t absoluteMaxTasks = 64;
constexpr std::uint32_t absoluteMaxDepth = 32;
constexpr std::uint32_t absoluteMaxParallelism = 32;
constexpr std::uint32_t absoluteMaxItemsPerTask = 256;

// MW-P1: renders one representative reason a cycle edge exists, preferring
// a Backend-derived edge (it carries the actual path a repair can act on)
// over a bare Planner-declared one when both exist between the same pair.
// When several distinct requiredReadPaths independently derive the same
// producer, only the first (manifest task/path order) is named - bounded,
// like every other diagnostic message in this file; the cycle is the same
// graph edge regardless of how many paths caused it.
std::string describeDependencyEdge(
    const std::vector<ManifestDependencyEdge>& edges,
    const std::string& consumerTaskId, const std::string& producerTaskId)
{
    const ManifestDependencyEdge* declared = nullptr;
    for (const auto& edge : edges) {
        if (edge.consumerTaskId != consumerTaskId || edge.producerTaskId != producerTaskId) continue;
        if (edge.origin == ManifestDependencyEdgeOrigin::BackendDerived) {
            return std::string(toString(edge.origin)) + ": " + *edge.relativePath;
        }
        if (declared == nullptr) declared = &edge;
    }
    return declared != nullptr ? toString(declared->origin) : "unknown";
}

bool blank(std::string_view value)
{
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
}

int capabilityRank(WorkspaceCapability capability)
{
    switch (capability) {
    case WorkspaceCapability::None: return 0;
    case WorkspaceCapability::Inspect: return 1;
    case WorkspaceCapability::Propose: return 2;
    }
    return 3;
}

// Safe, fixed diagnostic category for a rejected owned path. Never carries
// the raw or absolute input - only a stable reason the finding message can
// quote verbatim. `ownedPaths` reaches this validator as an already-
// constructed std::filesystem::path (built in ExecutionManifestParser.cpp's
// pathFromUtf8()); on this project's primary verified platform (Windows),
// std::filesystem::path treats '\\' as a native separator exactly like '/'
// at construction time, so a raw backslash is already indistinguishable
// from a forward slash by the time a path object reaches this file - there
// is no reachable, distinct "backslash" rejection branch to report here
// without changing accept/reject behavior. Every branch below instead maps
// 1:1 onto a real, currently-existing rejection point.
enum class OwnedPathIssue {
    Empty,
    AbsoluteOrDriveQualified,
    RepositoryRootOrDotSegment,
    ParentTraversal,
    NonCanonicalSeparatorForm,
    Other,
};

std::string_view ownedPathIssueDescription(OwnedPathIssue issue)
{
    switch (issue) {
    case OwnedPathIssue::Empty:
        return "the path is empty";
    case OwnedPathIssue::AbsoluteOrDriveQualified:
        return "the path is absolute or drive/UNC-qualified";
    case OwnedPathIssue::RepositoryRootOrDotSegment:
        return "the path contains a repository-root (\".\") segment";
    case OwnedPathIssue::ParentTraversal:
        return "the path contains a parent-directory (\"..\") traversal segment";
    case OwnedPathIssue::NonCanonicalSeparatorForm:
        return "the path contains an empty segment from a trailing or repeated separator";
    case OwnedPathIssue::Other:
        return "the path is not a canonical workspace-relative form";
    }
    return "the path is not a canonical workspace-relative form";
}

// Fixed phrase only - the caller never appends the raw path, an absolute
// form, or any other unbounded/untrusted text to this message. `roleLabel`
// is always one of a small set of Backend-known constants (see the six call
// sites in validate() below), never provider-controlled text.
std::string invalidScopePathMessage(std::string_view roleLabel, OwnedPathIssue issue)
{
    return std::string(roleLabel) + " path rejected because "
        + std::string(ownedPathIssueDescription(issue))
        + "; only clean workspace-relative paths are accepted";
}

struct OwnedPathDiagnosis {
    // Set exactly when normalized has no value.
    std::optional<OwnedPathIssue> issue;
    std::optional<std::string> normalized;
};

// Behaviorally identical to the prior single-branch normalizedOwnedPath():
// every condition below, evaluated in the same left-to-right order as
// before, previously collapsed into one `return std::nullopt`. Splitting
// each into its own return only attributes a category; it accepts and
// rejects exactly the same inputs as before.
OwnedPathDiagnosis diagnoseOwnedPath(const std::filesystem::path& path)
{
    if (path.empty()) {
        return {OwnedPathIssue::Empty, std::nullopt};
    }
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return {OwnedPathIssue::AbsoluteOrDriveQualified, std::nullopt};
    }

    std::string value = detail::genericUtf8(path);
    std::replace(value.begin(), value.end(), '\\', '/');
    if (value.empty()) {
        return {OwnedPathIssue::Empty, std::nullopt};
    }
    if (value.front() == '/' || value.find(':') != std::string::npos) {
        return {OwnedPathIssue::AbsoluteOrDriveQualified, std::nullopt};
    }

    std::string normalized;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const std::size_t end = value.find('/', begin);
        const std::string_view component(
            value.data() + begin,
            (end == std::string::npos ? value.size() : end) - begin);
        if (component.empty()) {
            return {OwnedPathIssue::NonCanonicalSeparatorForm, std::nullopt};
        }
        if (component == ".") {
            return {OwnedPathIssue::RepositoryRootOrDotSegment, std::nullopt};
        }
        if (component == "..") {
            return {OwnedPathIssue::ParentTraversal, std::nullopt};
        }
        if (!normalized.empty()) normalized.push_back('/');
        for (const unsigned char character : component) {
            normalized.push_back(static_cast<char>(std::tolower(character)));
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return {std::nullopt, normalized};
}

// Both paths are already-validated, normalized workspace-relative forms by
// the time this is called (never the raw, unvalidated model input), so it
// is safe to name them alongside the two conflicting task IDs.
std::string ownershipOverlapMessage(
    const std::string& taskId,
    const std::string& taskPath,
    const std::string& existingTaskId,
    const std::string& existingPath)
{
    return "Task '" + taskId + "' owned path '" + taskPath
        + "' overlaps with task '" + existingTaskId + "' owned path '" + existingPath + "'";
}

bool pathsOverlap(std::string_view left, std::string_view right)
{
    if (left == right) return true;
    if (left.size() < right.size()) {
        return right.starts_with(left) && right[left.size()] == '/';
    }
    return left.starts_with(right) && left[right.size()] == '/';
}

void addFinding(
    std::vector<ExecutionManifestFinding>& findings,
    std::string code,
    std::string message,
    std::optional<std::string> taskId = std::nullopt)
{
    findings.push_back(
        {std::move(code), std::move(message), std::move(taskId)});
}

} // namespace

std::vector<ExecutionManifestFinding> ExecutionManifestValidator::validate(
    const ExecutionManifest& manifest, const WorkspaceReadPolicy* denyPolicy,
    bool requireResolvedVerificationSelections)
{
    std::vector<ExecutionManifestFinding> findings;
    if (manifest.schemaVersion != kExecutionManifestSchemaVersion) {
        addFinding(findings, "manifest.unsupported_schema",
            "Execution manifest schema version is not supported");
    }
    if (manifest.runId.empty()) {
        addFinding(findings, "manifest.empty_run_id", "Execution manifest runId cannot be empty");
    }

    const auto& policy = manifest.policy;
    if (policy.maxTasks == 0 || policy.maxTasks > absoluteMaxTasks
        || policy.maxDepth == 0 || policy.maxDepth > absoluteMaxDepth
        || policy.maxParallelism == 0 || policy.maxParallelism > absoluteMaxParallelism
        || policy.maxOwnedPathsPerTask == 0
        || policy.maxOwnedPathsPerTask > absoluteMaxItemsPerTask
        || policy.maxExcludedOrDeferredPathsPerTask == 0
        || policy.maxExcludedOrDeferredPathsPerTask > absoluteMaxItemsPerTask
        || policy.maxAcceptanceCriteriaPerTask == 0
        || policy.maxAcceptanceCriteriaPerTask > absoluteMaxItemsPerTask
        || policy.maxContextReferencesPerTask == 0
        || policy.maxContextReferencesPerTask > absoluteMaxItemsPerTask) {
        addFinding(findings, "manifest.invalid_policy",
            "Trusted manifest policy contains a zero or unsupported ceiling");
    }
    if (manifest.tasks.empty()) {
        addFinding(findings, "manifest.empty_tasks", "Execution manifest must contain at least one task");
    }
    if (manifest.tasks.size() > policy.maxTasks
        || manifest.tasks.size() > absoluteMaxTasks) {
        addFinding(findings, "manifest.task_limit_exceeded",
            "Execution manifest exceeds the trusted task ceiling");
    }
    if (manifest.requestedMaxParallelism == 0
        || manifest.requestedMaxParallelism > policy.maxParallelism
        || manifest.requestedMaxParallelism > absoluteMaxParallelism) {
        addFinding(findings, "manifest.parallelism_exceeded",
            "Requested parallelism exceeds the trusted ceiling");
    }
    if (manifest.requestedTokenBudget.has_value()
        && (!policy.tokenBudgetCeiling.has_value()
            || *manifest.requestedTokenBudget > *policy.tokenBudgetCeiling)) {
        addFinding(findings, "manifest.token_budget_exceeded",
            "Requested token budget is not allowed by the trusted policy");
    }
    if (manifest.requestedCostMicros.has_value()
        && (!policy.costMicrosCeiling.has_value()
            || *manifest.requestedCostMicros > *policy.costMicrosCeiling)) {
        addFinding(findings, "manifest.cost_budget_exceeded",
            "Requested cost budget is not allowed by the trusted policy");
    }

    std::unordered_map<std::string, std::size_t> taskIndexes;
    for (std::size_t index = 0; index < manifest.tasks.size(); ++index) {
        const auto& task = manifest.tasks[index];
        if (task.id.empty() || blank(task.id)) {
            addFinding(findings, "manifest.empty_task_id", "Manifest task id cannot be empty");
        } else if (!taskIndexes.emplace(task.id, index).second) {
            addFinding(findings, "manifest.duplicate_task_id",
                "Manifest task ids must be unique", task.id);
        }
        if (task.objective.empty() || blank(task.objective)) {
            addFinding(findings, "manifest.empty_objective",
                "Manifest task objective cannot be empty", task.id);
        }
        if (task.role.empty() || blank(task.role)) {
            addFinding(findings, "manifest.empty_role",
                "Manifest task role cannot be empty", task.id);
        }
        if (capabilityRank(task.requestedCapability)
            > capabilityRank(policy.maximumCapability)) {
            addFinding(findings, "manifest.capability_exceeded",
                "Task requests a workspace capability above the trusted ceiling", task.id);
        }
        if (task.ownedPaths.size() > policy.maxOwnedPathsPerTask
            || task.ownedPaths.size() > absoluteMaxItemsPerTask) {
            addFinding(findings, "manifest.owned_path_limit_exceeded",
                "Task exceeds the trusted owned-path ceiling", task.id);
        }
        if (task.plannedCreatePaths.size() > policy.maxOwnedPathsPerTask
            || task.plannedCreatePaths.size() > absoluteMaxItemsPerTask) {
            addFinding(findings, "manifest.planned_create_path_limit_exceeded",
                "Task exceeds the trusted planned-create-path ceiling", task.id);
        }
        if (task.requiredReadPaths.size() > policy.maxOwnedPathsPerTask
            || task.requiredReadPaths.size() > absoluteMaxItemsPerTask) {
            addFinding(findings, "manifest.required_read_path_limit_exceeded",
                "Task exceeds the trusted required-read-path ceiling", task.id);
        }
        if (task.referencePaths.size() > policy.maxOwnedPathsPerTask
            || task.referencePaths.size() > absoluteMaxItemsPerTask) {
            addFinding(findings, "manifest.reference_path_limit_exceeded",
                "Task exceeds the trusted reference-path ceiling", task.id);
        }
        if (task.excludedPaths.size() > policy.maxExcludedOrDeferredPathsPerTask) {
            addFinding(findings, "manifest.excluded_path_limit_exceeded",
                "Task exceeds the trusted excluded-path ceiling", task.id);
        }
        if (task.deferredPaths.size() > policy.maxExcludedOrDeferredPathsPerTask) {
            addFinding(findings, "manifest.deferred_path_limit_exceeded",
                "Task exceeds the trusted deferred-path ceiling", task.id);
        }
        if (task.acceptanceCriteria.size() > policy.maxAcceptanceCriteriaPerTask
            || task.acceptanceCriteria.size() > absoluteMaxItemsPerTask) {
            addFinding(findings, "manifest.acceptance_limit_exceeded",
                "Task exceeds the trusted acceptance-criteria ceiling", task.id);
        }
        if (task.contextReferences.size() > policy.maxContextReferencesPerTask
            || task.contextReferences.size() > absoluteMaxItemsPerTask) {
            addFinding(findings, "manifest.context_limit_exceeded",
                "Task exceeds the trusted context-reference ceiling", task.id);
        }
        for (const auto& criterion : task.acceptanceCriteria) {
            if (criterion.empty() || blank(criterion)) {
                addFinding(findings, "manifest.empty_acceptance_criterion",
                    "Acceptance criteria cannot be blank", task.id);
            }
        }
        if (task.acceptanceContract.has_value()) {
            const auto contractResult =
                validateExecutionAcceptanceContract(*task.acceptanceContract);
            if (!contractResult.success()) {
                addFinding(findings, contractResult.error()->code,
                    contractResult.error()->message, task.id);
            } else {
                // AG-18.2: this validate() call runs at two different
                // points in the Planner's flow - pre-binding
                // (evaluateSubmission, verificationArguments still empty
                // for cmake_build/qml_lint) and again inside
                // ExecutionManifestService::freeze() itself, after
                // ManifestVerificationProfileBinder has already resolved
                // them. requireResolvedVerificationSelections is false for
                // the pre-binding call (either the fresh draft shape or an
                // already-resolved shape is acceptable there), but
                // freeze() itself always passes true: the freeze boundary
                // must accept ONLY the fully Backend-resolved shape, so a
                // caller other than the Planner's own evaluateSubmission
                // path can never persist an unbound draft by reaching
                // freeze() directly. The parser remains the actual
                // untrusted-input boundary enforcing the strict draft-only
                // shape a fresh provider submission must have.
                for (const auto& requirement :
                    task.acceptanceContract->requirements) {
                    const auto stateResult = requireResolvedVerificationSelections
                        ? validateExecutionVerificationProfileArguments(requirement)
                        : validateExecutionVerificationRequirementState(requirement);
                    if (!stateResult.success()) {
                        addFinding(findings, stateResult.error()->code,
                            stateResult.error()->message, task.id);
                        break;
                    }
                }
            }
        }
        if (task.requestedCapability == WorkspaceCapability::None
            && (!task.ownedPaths.empty() || !task.acceptanceCriteria.empty()
                || (task.acceptanceContract.has_value()
                    && !task.acceptanceContract->requirements.empty()))) {
            addFinding(findings, "manifest.none_task_not_empty",
                "A none-capability task must be an empty coordination node",
                task.id);
        }
        // Real-Run incident (Run 2f26ecc5-2723-4f9c-98fd-9ff317f13387,
        // 2026-09-11): an inspect-capability task's own worker session can
        // only self-verify via submit_task_verification, whose evidence a
        // model self-reports from its own read-only inspection. A command-
        // backed verificationKind (cmake_build/qml_lint/
        // cmake_target_consistency) can never be legitimately reported
        // Passed that way - AgentToolTurnLoopRunner::parseTaskVerification
        // already fails closed on exactly that ("task_verification.
        // profile_requires_backend"), and the ordinary required-passed gate
        // then fails every remaining attempt identically
        // ("task_verification.required_not_passed"), regardless of the
        // worker's competence or the real state of the project - the task
        // was unrepairable from the moment this Manifest was frozen. A
        // command-backed requirement is legitimate only on a propose-
        // capability task's acceptanceContract, evaluated once across the
        // whole Manifest by the Backend's own combined verification
        // (ManifestProposalSetVerifier), never by an individual worker
        // session of any capability. Every offending requirement is
        // reported (not just the first) so the one bounded Planner repair
        // can fix every instance in a single pass.
        if (task.requestedCapability == WorkspaceCapability::Inspect
            && task.acceptanceContract.has_value()) {
            for (const auto& requirement : task.acceptanceContract->requirements) {
                if (requirement.verificationKind != ExecutionVerificationKind::InspectPaths) {
                    addFinding(findings, "manifest.inspect_task_command_verification",
                        "Task '" + task.id + "' has requestedCapability inspect but "
                        "requirement '" + requirement.id + "' uses verificationKind '"
                        + toString(requirement.verificationKind) + "' - an inspect-"
                        "capability task may only self-verify inspect_paths "
                        "requirements through submit_task_verification; command-backed "
                        "checks (cmake_build/qml_lint/cmake_target_consistency) are "
                        "evaluated once across the whole Manifest by the Backend's own "
                        "combined verification and must be moved to a propose-"
                        "capability task's acceptanceContract instead.",
                        task.id);
                }
            }
        }
        for (const auto& reference : task.contextReferences) {
            if (reference.empty() || blank(reference)) {
                addFinding(findings, "manifest.empty_context_reference",
                    "Context references cannot be blank", task.id);
            }
        }
        // MW-P3A package 2: baselineOnlyReadPaths is a Backend-only marker
        // on entries already present in requiredReadPaths (see
        // ExecutionManifestTask's own field comment) - never a separate
        // provider-facing role, so it is deliberately not part of the AG-15
        // path-role loop below. This is the one invariant check for it:
        // every marked entry must genuinely be a requiredReadPaths entry,
        // defense-in-depth against a future caller populating it
        // incorrectly (today only ExecutionManifestDraftCompiler ever sets
        // it, always as a real subset by construction).
        if (!task.baselineOnlyReadPaths.empty()) {
            std::unordered_set<std::string> requiredReadKeys;
            for (const auto& path : task.requiredReadPaths) {
                requiredReadKeys.insert(diagnoseOwnedPath(path).normalized.value_or(std::string()));
            }
            for (const auto& path : task.baselineOnlyReadPaths) {
                const auto normalized = diagnoseOwnedPath(path).normalized;
                if (!normalized.has_value() || !requiredReadKeys.contains(*normalized)) {
                    addFinding(findings, "manifest.baseline_only_read_path_not_required",
                        "A baseline-only read path must also be a requiredReadPaths entry "
                        "of the same task", task.id);
                }
            }
        }
    }

    for (const auto& task : manifest.tasks) {
        std::unordered_set<std::string> dependencies;
        for (const auto& dependency : task.dependencies) {
            if (!dependencies.insert(dependency).second) {
                addFinding(findings, "manifest.duplicate_dependency",
                    "Task dependencies must be unique", task.id);
            }
            if (dependency == task.id) {
                addFinding(findings, "manifest.self_dependency",
                    "Task cannot depend on itself", task.id);
            } else if (!taskIndexes.contains(dependency)) {
                addFinding(findings, "manifest.unknown_dependency",
                    "Task dependency does not exist in the manifest", task.id);
            }
        }
    }

    std::vector<int> colors(manifest.tasks.size(), 0);
    std::vector<std::uint32_t> depths(manifest.tasks.size(), 0);
    // MW-P1 (real-Run incident, three OpenAI "Stage 2B" Runs, 2026-09-13):
    // walk the EFFECTIVE graph - Planner-declared dependencies union
    // Backend-derived ones (a requiredReadPaths entry matching another
    // task's write scope) - not declared edges alone. Two tasks that each
    // genuinely require a path the other produces form a real cycle the
    // moment both derived edges exist, even before either is declared; the
    // previous declared-only check could miss that shape here and only
    // catch it later in RunWorkspaceScopeService, after the manifest is
    // already immutably frozen and no repair chance remains. This is the
    // one deterministic construction shared with RunWorkspaceScopeService's
    // own edge derivation (see ExecutionManifestDependencyGraph.hpp) so the
    // two can never drift apart again.
    const auto dependencyEdges = deriveManifestDependencyGraph(manifest);
    std::vector<std::vector<std::size_t>> adjacency(manifest.tasks.size());
    for (std::size_t edgeIndex = 0; edgeIndex < dependencyEdges.size(); ++edgeIndex) {
        const auto consumerIt = taskIndexes.find(dependencyEdges[edgeIndex].consumerTaskId);
        if (consumerIt != taskIndexes.end()) adjacency[consumerIt->second].push_back(edgeIndex);
    }
    // Real-Run incident (Runs fcd484b8-.../cebcd3cf-.../c5f7d3da-...,
    // 2026-09-11): the current DFS path, by task id, in visit order - lets a
    // detected cycle be reported as the exact chain that closes it, not just
    // the single task where re-entry was observed. A Planner repair given
    // only "the graph must be acyclic" plus one task id had no way to tell
    // which edge to actually fix and repeated the same cycle.
    std::vector<std::string> pathStack;
    bool cycleReported = false;
    std::function<std::uint32_t(std::size_t)> visit = [&](std::size_t index) {
        if (colors[index] == 2) return depths[index];
        if (colors[index] == 1) {
            if (!cycleReported) {
                const auto& target = manifest.tasks[index].id;
                const auto start = std::find(pathStack.begin(), pathStack.end(), target);
                std::string chain;
                for (auto it = start; it != pathStack.end(); ++it) {
                    if (it != start) chain += " -> ";
                    chain += *it;
                }
                chain += " -> " + target;
                // MW-P1: name every edge in the cycle - consumer, producer,
                // origin, and (when derived) the exact path that created it
                // - so the bounded repair can tell a genuine mutual
                // producer/consumer relationship (merge the tasks) from a
                // simple wrong-direction declared edge (flip it), instead
                // of repeating the same cycle blind.
                std::vector<std::string> hopTaskIds(start, pathStack.end());
                hopTaskIds.push_back(target);
                std::string edgeDetail;
                for (std::size_t hop = 0; hop + 1 < hopTaskIds.size(); ++hop) {
                    if (!edgeDetail.empty()) edgeDetail += "; ";
                    edgeDetail += hopTaskIds[hop] + " -> " + hopTaskIds[hop + 1] + " ["
                        + describeDependencyEdge(
                              dependencyEdges, hopTaskIds[hop], hopTaskIds[hop + 1])
                        + "]";
                }
                addFinding(findings, "manifest.dependency_cycle",
                    "Manifest dependency graph must be acyclic: " + chain
                        + ". Edges: " + edgeDetail + ".",
                    manifest.tasks[index].id);
                cycleReported = true;
            }
            return std::uint32_t {0};
        }
        colors[index] = 1;
        pathStack.push_back(manifest.tasks[index].id);
        std::uint32_t depth = 1;
        for (const auto edgeIndex : adjacency[index]) {
            const auto found = taskIndexes.find(dependencyEdges[edgeIndex].producerTaskId);
            if (found != taskIndexes.end() && found->second != index) {
                depth = std::max(depth, visit(found->second) + 1);
            }
        }
        pathStack.pop_back();
        colors[index] = 2;
        depths[index] = depth;
        return depth;
    };
    for (std::size_t index = 0; index < manifest.tasks.size(); ++index) {
        if (visit(index) > policy.maxDepth || depths[index] > absoluteMaxDepth) {
            addFinding(findings, "manifest.depth_exceeded",
                "Manifest dependency depth exceeds the trusted ceiling",
                manifest.tasks[index].id);
        }
    }

    // AG-15: one unified pass over every workspace-scope path list a task
    // may declare. Processing order per task is ownedPaths, then
    // plannedCreatePaths, then requiredReadPaths, then referencePaths, then
    // excludedPaths, then deferredPaths - ownedPaths-only fixtures therefore
    // produce byte-identical findings/order to before this pass was
    // generalized (see testOwnedPathFindingOrderAndTaskAssociationAreDeterministic).
    struct WriteClaim { std::string value; std::string taskId; };
    std::vector<WriteClaim> writeClaims; // owned_write UNION planned_create - cross-task exclusive

    for (const auto& task : manifest.tasks) {
        // Within-task role tracking: a single path may not be declared under
        // more than one of this task's own six lists.
        std::unordered_map<std::string, WorkspaceScopeRole> taskRoleByPath;

        auto diagnoseAndTrack = [&](const std::filesystem::path& path, WorkspaceScopeRole role,
                                     const char* invalidCode,
                                     std::string_view roleLabel) -> std::optional<std::string> {
            const auto diagnosis = diagnoseOwnedPath(path);
            if (!diagnosis.normalized.has_value()) {
                addFinding(findings, invalidCode,
                    invalidScopePathMessage(roleLabel, *diagnosis.issue), task.id);
                return std::nullopt;
            }
            const auto& normalized = *diagnosis.normalized;
            const auto existing = taskRoleByPath.find(normalized);
            if (existing != taskRoleByPath.end()) {
                addFinding(findings,
                    existing->second == role ? "manifest.duplicate_scope_path"
                                              : "manifest.conflicting_path_role",
                    existing->second == role
                        ? "A task cannot declare the same path twice under the same role"
                        : "Path '" + normalized + "' cannot be both '"
                            + toString(existing->second) + "' and '" + toString(role)
                            + "' in the same task",
                    task.id);
                return std::nullopt;
            }
            taskRoleByPath.emplace(normalized, role);
            if (denyPolicy != nullptr) {
                const auto checked = WorkspaceReadPolicyValidator::validateRelativePath(
                    std::filesystem::path(normalized), *denyPolicy);
                if (!checked.success()) {
                    addFinding(findings, "manifest.excluded_path_activated",
                        "An active workspace-scope path is globally excluded "
                        "(build/generated/secret/denied)",
                        task.id);
                    return std::nullopt;
                }
            }
            return normalized;
        };

        auto trackWriteClaim = [&](const std::string& normalized) {
            for (const auto& existing : writeClaims) {
                if (existing.taskId != task.id && pathsOverlap(existing.value, normalized)) {
                    addFinding(findings, "manifest.ownership_overlap",
                        ownershipOverlapMessage(
                            task.id, normalized, existing.taskId, existing.value),
                        task.id);
                    break;
                }
            }
            writeClaims.push_back({normalized, task.id});
        };

        for (const auto& path : task.ownedPaths) {
            // "Owned" (not "Owned-write") preserves the exact pre-AG-15
            // diagnostic message text this list has always produced.
            auto normalized = diagnoseAndTrack(path, WorkspaceScopeRole::OwnedWrite,
                "manifest.invalid_owned_path", "Owned");
            if (normalized.has_value()) trackWriteClaim(*normalized);
        }
        for (const auto& path : task.plannedCreatePaths) {
            auto normalized = diagnoseAndTrack(path, WorkspaceScopeRole::PlannedCreate,
                "manifest.invalid_planned_create_path", "Planned-create");
            if (normalized.has_value()) trackWriteClaim(*normalized);
        }
        for (const auto& path : task.requiredReadPaths) {
            diagnoseAndTrack(path, WorkspaceScopeRole::RequiredRead,
                "manifest.invalid_required_read_path", "Required-read");
        }
        for (const auto& path : task.referencePaths) {
            diagnoseAndTrack(path, WorkspaceScopeRole::ReferenceOnly,
                "manifest.invalid_reference_path", "Reference-only");
        }
        for (const auto& excluded : task.excludedPaths) {
            diagnoseAndTrack(excluded.path, WorkspaceScopeRole::Excluded,
                "manifest.invalid_excluded_path", "Excluded");
        }
        for (const auto& deferred : task.deferredPaths) {
            diagnoseAndTrack(deferred.path, WorkspaceScopeRole::Deferred,
                "manifest.invalid_deferred_path", "Deferred");
        }
    }

    // AG-18.3: the full manifest-level bootstrap admission predicate - runs
    // regardless of requireResolvedVerificationSelections, both pre-binding
    // (so a bad declaration participates in the Planner's existing bounded
    // repair) and again at the freeze boundary.
    auto bootstrapFindings = ManifestBootstrapValidator::validate(manifest);
    findings.insert(findings.end(),
        std::make_move_iterator(bootstrapFindings.begin()),
        std::make_move_iterator(bootstrapFindings.end()));

    // AG-17.1: the Backend-owned routing record and every task's assignee against
    // it. A record defect (manifest.routing_invalid) is a Backend bug; assignee
    // findings are shaped so a Planner repair can fix them.
    {
        auto routingFindings = ManifestRouting::findings(manifest);
        findings.insert(findings.end(),
            std::make_move_iterator(routingFindings.begin()),
            std::make_move_iterator(routingFindings.end()));
    }

    // AG-17.0: a frozen mandatory-verification record must agree with the
    // Manifest's own requirements (no dangling requirement/task, no kind or
    // provenance contradiction). Only the Backend ever produces a record, so a
    // finding here is a Backend defect, never something a provider can repair.
    if (const auto problem = ManifestMandatoryVerificationPolicy::recordIntegrityProblem(manifest)) {
        addFinding(findings, "manifest.mandatory_verification_inconsistent", *problem);
    }

    return findings;
}

} // namespace ai::application
