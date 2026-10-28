#include "ai/application/execution/ExecutionManifestDependencyGraph.hpp"
#include "ai/application/workspace/FilesystemPathUtils.hpp"

#include <cctype>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace ai::application {

namespace {

// Mirrors RunWorkspaceScopeService.cpp's own `normalizeForLookup` exactly
// (lowercase, forward slashes) - intentionally duplicated rather than
// shared, since that file's version is a general-purpose utility used for
// many unrelated baseline/expansion lookups this module has no business
// depending on; this one pure, stable, six-line function is the only path
// normalization this module needs.
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

} // namespace

const char* toString(ManifestDependencyEdgeOrigin origin) noexcept
{
    switch (origin) {
    case ManifestDependencyEdgeOrigin::PlannerDeclared: return "planner_declared";
    case ManifestDependencyEdgeOrigin::BackendDerived: return "backend_derived";
    }
    return "planner_declared";
}

std::vector<ManifestDependencyEdge> deriveManifestDependencyGraph(
    const ExecutionManifest& manifest)
{
    std::unordered_set<std::string> knownTaskIds;
    knownTaskIds.reserve(manifest.tasks.size());
    for (const auto& task : manifest.tasks) knownTaskIds.insert(task.id);

    // Last write wins on a duplicate ownership claim - a manifest with two
    // tasks claiming the same write path is separately, fully reported by
    // the validator's own ownership-overlap pass; this function only needs
    // to stay deterministic (manifest task order) on that malformed input,
    // not to arbitrate it.
    std::unordered_map<std::string, std::string> writeOwnerByPath;
    for (const auto& task : manifest.tasks) {
        for (const auto& path : task.ownedPaths) {
            writeOwnerByPath[normalizeForLookup(path)] = task.id;
        }
        for (const auto& path : task.plannedCreatePaths) {
            writeOwnerByPath[normalizeForLookup(path)] = task.id;
        }
    }

    std::vector<ManifestDependencyEdge> edges;
    for (const auto& task : manifest.tasks) {
        // Declared edges first, in declaration order. Unknown/self
        // dependencies are silently skipped here - each already has its
        // own dedicated validator finding and must not be double-reported
        // or turned into a phantom graph edge by this pure helper.
        for (const auto& dependency : task.dependencies) {
            if (dependency == task.id) continue;
            if (!knownTaskIds.contains(dependency)) continue;
            edges.push_back({task.id, dependency,
                ManifestDependencyEdgeOrigin::PlannerDeclared, std::nullopt});
        }
        // MW-P3A package 2 (Run a8afe6a0-7eb1-4c05-aaf3-46a1c5c90b6b): a
        // requiredReadPaths entry the task itself marked
        // baselineOnlyReadPaths is a read of the frozen baseline, never of
        // another task's proposed output - it must never derive an edge
        // even when the same normalized path also happens to be another
        // task's write target. Always empty for a legacy-parsed manifest,
        // so this changes nothing for that pathway.
        std::unordered_set<std::string> baselineOnly;
        baselineOnly.reserve(task.baselineOnlyReadPaths.size());
        for (const auto& path : task.baselineOnlyReadPaths) {
            baselineOnly.insert(normalizeForLookup(path));
        }
        // Derived edges, in requiredReadPaths order. An unresolvable path
        // (no known owner, or the task's own output) is silently skipped -
        // "no owner" already has its own dedicated validator/scope finding
        // (workspace_scope.read_path_missing_from_baseline), and a task
        // reading its own output is not a cross-task dependency at all.
        for (const auto& path : task.requiredReadPaths) {
            const auto normalized = normalizeForLookup(path);
            if (baselineOnly.contains(normalized)) continue;
            const auto owner = writeOwnerByPath.find(normalized);
            if (owner == writeOwnerByPath.end() || owner->second == task.id) continue;
            edges.push_back({task.id, owner->second,
                ManifestDependencyEdgeOrigin::BackendDerived, normalized});
        }
    }
    return edges;
}

} // namespace ai::application
