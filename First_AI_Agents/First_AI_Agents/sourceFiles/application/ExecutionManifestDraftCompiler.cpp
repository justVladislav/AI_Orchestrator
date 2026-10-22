#include "ai/application/ExecutionManifestDraftCompiler.hpp"
#include "ai/application/FilesystemPathUtils.hpp"

#include <cctype>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ai::application {

namespace {

// Mirrors RunWorkspaceScopeService.cpp's and ExecutionManifestDependencyGraph.cpp's
// own `normalizeForLookup` exactly (lowercase, forward slashes) -
// intentionally duplicated rather than shared, per this codebase's
// established per-module convention for this one small, stable function.
std::string normalizeForLookup(const std::filesystem::path& path)
{
    std::string value = detail::genericUtf8(path);
    std::string normalized;
    normalized.reserve(value.size());
    for (const unsigned char ch : value) {
        normalized.push_back(ch == '\\' ? '/' : static_cast<char>(std::tolower(ch)));
    }
    return normalized;
}

WorkspaceCapability toWorkspaceCapability(ManifestDraftCapability capability)
{
    switch (capability) {
    case ManifestDraftCapability::Inspect: return WorkspaceCapability::Inspect;
    case ManifestDraftCapability::Propose: return WorkspaceCapability::Propose;
    }
    return WorkspaceCapability::Inspect;
}

void addFinding(
    std::vector<ExecutionManifestFinding>& findings,
    std::string code, std::string message, std::string taskId)
{
    findings.push_back({std::move(code), std::move(message), std::move(taskId)});
}

} // namespace

ExecutionManifestDraftCompilationResult ExecutionManifestDraftCompiler::compile(
    ExecutionManifestDraftCompilerRequest request)
{
    ExecutionManifestDraftCompilationResult result;
    result.manifest.schemaVersion = std::string(kExecutionManifestSchemaVersion);
    result.manifest.runId = request.runId;
    result.manifest.requestedMaxParallelism = request.draft.requestedMaxParallelism;
    result.manifest.requestedTokenBudget = request.draft.requestedTokenBudget;
    result.manifest.requestedCostMicros = request.draft.requestedCostMicros;
    result.manifest.bootstrapDeclaration = request.draft.bootstrapDeclaration;

    std::unordered_set<std::string> baselineIndex;
    baselineIndex.reserve(request.draft.workTasks.size() * 4);
    for (const auto& file : request.baseline.files) {
        baselineIndex.insert(normalizeForLookup(std::filesystem::path(file.relativePath)));
    }

    // Every work task's raw writePaths entries, indexed by normalized path
    // -> owning task id. Built once, over the whole draft, before any
    // per-task compilation, since proposedDependencyReadPaths validation
    // (below) must be able to name a producer regardless of task order.
    std::unordered_map<std::string, std::string> writePathOwnerByTask;
    for (const auto& workTask : request.draft.workTasks) {
        for (const auto& path : workTask.writePaths) {
            writePathOwnerByTask[normalizeForLookup(path)] = workTask.id;
        }
    }

    result.manifest.tasks.reserve(
        request.draft.coordinationTasks.size() + request.draft.workTasks.size());

    for (const auto& coordinationTask : request.draft.coordinationTasks) {
        ExecutionManifestTask task;
        task.id = coordinationTask.id;
        task.objective = coordinationTask.objective;
        task.role = coordinationTask.role;
        task.dependencies = coordinationTask.dependencies;
        task.requestedCapability = WorkspaceCapability::None;
        result.manifest.tasks.push_back(std::move(task));
    }

    for (const auto& workTask : request.draft.workTasks) {
        ExecutionManifestTask task;
        task.id = workTask.id;
        task.objective = workTask.objective;
        task.role = workTask.role;
        task.dependencies = workTask.dependencies;

        // Backend-owned classification (never the provider's guess): an
        // existing baseline path is owned_write, an absent one is
        // planned_create. This is the structural fix for Run
        // 3ffb52df-98ca-472f-9b1c-537ab134cf18's repair misclassifying an
        // absent qml/components/navigation/PageHost.qml as owned_write -
        // that specific misclassification can no longer happen, since the
        // provider never makes this choice at all.
        for (const auto& path : workTask.writePaths) {
            if (baselineIndex.contains(normalizeForLookup(path))) {
                task.ownedPaths.push_back(path);
            } else {
                task.plannedCreatePaths.push_back(path);
            }
        }

        // Read-version semantics (Run a8afe6a0-7eb1-4c05-aaf3-46a1c5c90b6b):
        // both lists remain same-attempt auditable required reads in the
        // compiled manifest (preserving the existing AG-15 read-gate
        // enforcement unchanged), but only proposedDependencyReadPaths
        // participates in dependency-edge derivation - baselineOnlyReadPaths
        // records exactly which entries must not.
        std::unordered_set<std::string> ownWriteKeys;
        ownWriteKeys.reserve(workTask.writePaths.size());
        for (const auto& path : workTask.writePaths) {
            ownWriteKeys.insert(normalizeForLookup(path));
        }
        task.requiredReadPaths.reserve(
            workTask.baselineRequiredReadPaths.size() + workTask.proposedDependencyReadPaths.size());
        for (const auto& path : workTask.baselineRequiredReadPaths) {
            task.requiredReadPaths.push_back(path);
            task.baselineOnlyReadPaths.push_back(path);
            const auto key = normalizeForLookup(path);
            // A path that duplicates this same task's own writePaths is a
            // harmless redundancy (the worker can already read anything it
            // writes) that canonicalizeRedundantActiveScopeRoles resolves
            // right after compilation - never an independent baseline claim
            // to validate here.
            if (!ownWriteKeys.contains(key) && !baselineIndex.contains(key)) {
                addFinding(result.findings, "manifest.baseline_required_read_path_not_in_baseline",
                    "A baselineRequiredReadPaths entry is not an existing baseline file",
                    task.id);
            }
        }
        for (const auto& path : workTask.proposedDependencyReadPaths) {
            task.requiredReadPaths.push_back(path);
            const auto key = normalizeForLookup(path);
            if (ownWriteKeys.contains(key)) continue;
            const auto owner = writePathOwnerByTask.find(key);
            if (owner == writePathOwnerByTask.end() || owner->second == task.id) {
                addFinding(result.findings, "manifest.proposed_dependency_read_path_has_no_producer",
                    "A proposedDependencyReadPaths entry does not name any other work "
                    "task's writePaths entry in this same submission",
                    task.id);
            }
        }

        task.referencePaths = workTask.referencePaths;
        task.excludedPaths = workTask.excludedPaths;
        task.deferredPaths = workTask.deferredPaths;
        task.acceptanceCriteria = workTask.acceptanceCriteria;
        task.contextReferences = workTask.contextReferences;
        task.requestedCapability = toWorkspaceCapability(workTask.requestedCapability);
        task.acceptanceContract = workTask.acceptanceContract;
        // AG-17.4: the provider's only routing statement. Whether the member
        // exists, is a workspace-capable worker and is not the lead is judged by
        // the validator against the Backend's frozen routing record.
        task.assigneeMemberId = workTask.assigneeMemberId;
        result.manifest.tasks.push_back(std::move(task));
    }

    return result;
}

} // namespace ai::application
