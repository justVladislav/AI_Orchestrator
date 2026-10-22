#include "ai/application/ManifestBootstrapValidator.hpp"
#include "ai/application/FilesystemPathUtils.hpp"
#include "ai/application/VerificationBootstrapTypes.hpp"

#include <algorithm>
#include <unordered_set>

namespace ai::application {

namespace {

namespace fs = std::filesystem;

void addFinding(std::vector<ExecutionManifestFinding>& findings,
    std::string code, std::string message, std::optional<std::string> taskId = std::nullopt)
{
    findings.push_back({std::move(code), std::move(message), std::move(taskId)});
}

// The only two repository-root CMake preset files this Backend understands
// as bootstrap setup targets - never a subdirectory path, never any other
// filename.
bool isSupportedBootstrapFilename(const fs::path& path)
{
    if (path.has_parent_path() && path.parent_path() != fs::path()) return false;
    const auto name = path.filename().generic_string();
    return name == "CMakePresets.json" || name == "CMakeUserPresets.json";
}

// AG-18.3 hardening (blocker 5): compares via the same authoritative
// comparison-key normalization every other cross-producer path match in
// this codebase already uses (detail::normalizedPathKey) - never a second,
// weaker ad hoc comparison. This catches a differently-formatted but
// logically identical path (case, separator style) claiming the bootstrap
// setup file in another task's role list.
bool pathInList(const fs::path& path, const std::vector<fs::path>& list)
{
    const auto key = detail::normalizedPathKey(path);
    return std::any_of(list.begin(), list.end(),
        [&](const fs::path& entry) { return detail::normalizedPathKey(entry) == key; });
}

bool pathInReasonList(const fs::path& path, const std::vector<ManifestExcludedOrDeferredPath>& list)
{
    const auto key = detail::normalizedPathKey(path);
    return std::any_of(list.begin(), list.end(),
        [&](const ManifestExcludedOrDeferredPath& entry) {
            return detail::normalizedPathKey(entry.path) == key;
        });
}

// AG-18.3 hardening (blocker 5): counts every occurrence of setupPath across
// every scope-role list of every task in the Manifest - not just the
// bootstrap-owning task's own lists. AG-15's per-task structural check only
// forbids a path from appearing in more than one role of the SAME task; it
// says nothing about a different task independently claiming the same path
// in any role (ownership, a read dependency, a reference, or even an
// exclusion/deferral note), all of which would let that task's own
// validated content silently stand in for - or contradict - the bootstrap's
// own declared setup file.
std::size_t countOccurrencesAcrossManifest(
    const ExecutionManifest& manifest, const fs::path& setupPath, const std::string& bootstrapTaskId,
    bool& ownedByBootstrapTask, bool& plannedCreatedByBootstrapTask)
{
    std::size_t total = 0;
    ownedByBootstrapTask = false;
    plannedCreatedByBootstrapTask = false;
    for (const auto& task : manifest.tasks) {
        const bool owned = pathInList(setupPath, task.ownedPaths);
        const bool plannedCreate = pathInList(setupPath, task.plannedCreatePaths);
        const bool requiredRead = pathInList(setupPath, task.requiredReadPaths);
        const bool reference = pathInList(setupPath, task.referencePaths);
        const bool excluded = pathInReasonList(setupPath, task.excludedPaths);
        const bool deferred = pathInReasonList(setupPath, task.deferredPaths);
        const int hits = static_cast<int>(owned) + static_cast<int>(plannedCreate)
            + static_cast<int>(requiredRead) + static_cast<int>(reference)
            + static_cast<int>(excluded) + static_cast<int>(deferred);
        total += static_cast<std::size_t>(hits);
        if (task.id == bootstrapTaskId) {
            ownedByBootstrapTask = owned;
            plannedCreatedByBootstrapTask = plannedCreate;
        }
    }
    return total;
}

const ExecutionManifestTask* findTask(
    const ExecutionManifest& manifest, const std::string& taskId)
{
    for (const auto& task : manifest.tasks) {
        if (task.id == taskId) return &task;
    }
    return nullptr;
}

bool anyRequirementIsBootstrapPending(const ExecutionManifestTask& task)
{
    if (!task.acceptanceContract.has_value()) return false;
    return std::any_of(task.acceptanceContract->requirements.begin(),
        task.acceptanceContract->requirements.end(),
        [](const ExecutionAcceptanceRequirement& requirement) {
            return requirement.verificationBootstrapPending;
        });
}

} // namespace

std::vector<ExecutionManifestFinding> ManifestBootstrapValidator::validate(
    const ExecutionManifest& manifest)
{
    std::vector<ExecutionManifestFinding> findings;

    std::vector<std::string> dependentTaskIds;
    for (const auto& task : manifest.tasks) {
        if (anyRequirementIsBootstrapPending(task)) dependentTaskIds.push_back(task.id);
    }

    if (dependentTaskIds.empty()) {
        if (manifest.bootstrapDeclaration.has_value()) {
            addFinding(findings, "verification_environment.bootstrap_scope_invalid",
                "A bootstrapDeclaration was present but no requirement selected "
                "verificationBootstrapPending");
        }
        return findings;
    }

    if (!manifest.bootstrapDeclaration.has_value()) {
        // Absence of an explicit bootstrapDeclaration must never be
        // inferred as consent - a requirement cannot be deferred pending a
        // bootstrap the Manifest never actually declared.
        addFinding(findings, "verification_environment.bootstrap_scope_invalid",
            "A requirement selected verificationBootstrapPending without an explicit "
            "manifest-level bootstrapDeclaration");
        return findings;
    }

    const auto& declaration = *manifest.bootstrapDeclaration;
    const auto* bootstrapTask = findTask(manifest, declaration.bootstrapTaskId);
    if (bootstrapTask == nullptr) {
        addFinding(findings, "verification_environment.bootstrap_scope_invalid",
            "bootstrapDeclaration.bootstrapTaskId does not name a task in this Manifest");
        return findings;
    }

    if (declaration.setupPaths.empty()
        || declaration.setupPaths.size() > kVerificationBootstrapMaxSetupPaths) {
        addFinding(findings, "verification_environment.bootstrap_scope_invalid",
            "bootstrapDeclaration.setupPaths must be nonempty and bounded", bootstrapTask->id);
        return findings;
    }

    std::unordered_set<std::string> seenSetupPaths;
    for (const auto& setupPath : declaration.setupPaths) {
        const auto key = setupPath.generic_string();
        if (!seenSetupPaths.emplace(key).second) {
            addFinding(findings, "verification_environment.bootstrap_scope_invalid",
                "bootstrapDeclaration.setupPaths contains a duplicate path", bootstrapTask->id);
            continue;
        }
        if (!isSupportedBootstrapFilename(setupPath)) {
            addFinding(findings, "verification_environment.bootstrap_scope_invalid",
                "Only repository-root CMakePresets.json/CMakeUserPresets.json are supported "
                "bootstrap setup files", bootstrapTask->id);
            continue;
        }
        // AG-18.3 hardening (blocker 5): a path may not masquerade as
        // bootstrap-owned via ANY role of ANY other task, nor via any role
        // of the bootstrap task itself other than exactly one of
        // ownedPaths/plannedCreatePaths - checked across the whole
        // Manifest, not just the bootstrap-owning task's own six lists.
        bool ownedByBootstrapTask = false;
        bool plannedCreatedByBootstrapTask = false;
        const auto occurrences = countOccurrencesAcrossManifest(
            manifest, setupPath, bootstrapTask->id, ownedByBootstrapTask, plannedCreatedByBootstrapTask);
        const bool exactlyOneValidRole =
            occurrences == 1 && (ownedByBootstrapTask != plannedCreatedByBootstrapTask);
        if (!exactlyOneValidRole) {
            addFinding(findings, "verification_environment.bootstrap_scope_invalid",
                "Each bootstrap setup file must appear exactly once across the whole Manifest, "
                "as exactly one of the bootstrap task's ownedPaths or plannedCreatePaths, and in "
                "no scope-role list of any other task", bootstrapTask->id);
        }
    }

    for (const auto& dependentTaskId : dependentTaskIds) {
        if (dependentTaskId == bootstrapTask->id) continue;
        const auto* dependentTask = findTask(manifest, dependentTaskId);
        if (dependentTask == nullptr) continue; // reported elsewhere as an unknown task
        const bool hasEdge = std::find(dependentTask->dependencies.begin(),
            dependentTask->dependencies.end(), bootstrapTask->id)
            != dependentTask->dependencies.end();
        if (!hasEdge) {
            addFinding(findings, "verification_environment.bootstrap_dependency_invalid",
                "A task with a bootstrap-pending verification requirement has no explicit "
                "dependency edge on the bootstrap-owning task", dependentTaskId);
        }
    }

    return findings;
}

} // namespace ai::application
