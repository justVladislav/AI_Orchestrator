#include "ai/application/execution/RunWorkspaceScopeService.hpp"

#include "ai/application/execution/ExecutionManifestDependencyGraph.hpp"
#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/IdGenerator.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

namespace ai::application {

namespace {

// AG-15: the same lowercase+forward-slash normalization
// ExecutionManifestValidator::diagnoseOwnedPath already applies for safety/
// overlap comparison - reused here (not re-implemented) so a path this
// service persists is byte-identical to how the validator already treated
// it. Only called on paths that have already passed validator safety
// checks (freezeForManifest/requestExpansion are only ever reached for an
// already-validated manifest / an already-safety-checked expansion
// candidate), so this is normalization for lookup, not a second safety
// gate.
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

std::unordered_map<std::string, std::string> buildBaselineHashIndex(
    const WorkspaceBaselineSnapshot& baseline)
{
    std::unordered_map<std::string, std::string> index;
    index.reserve(baseline.files.size());
    for (const auto& file : baseline.files) {
        index.emplace(normalizeForLookup(std::filesystem::path(file.relativePath)), file.sha256);
    }
    return index;
}

std::string synthesizeReason(WorkspaceScopeRole role, const std::string& taskId)
{
    return std::string("Declared as ") + toString(role) + " by task '" + taskId + "'";
}

} // namespace

RunWorkspaceScopeService::RunWorkspaceScopeService(IRunWorkspaceScopeRepository& repository)
    : repository_(repository)
{
}

std::vector<ExecutionManifestFinding>
RunWorkspaceScopeService::validateManifestAgainstBaseline(
    const ExecutionManifest& manifest, const WorkspaceBaselineSnapshot& baseline)
{
    const auto baselineIndex = buildBaselineHashIndex(baseline);
    std::unordered_map<std::string, std::string> writeOwnerByPath;
    for (const auto& task : manifest.tasks) {
        for (const auto& path : task.ownedPaths)
            writeOwnerByPath.emplace(normalizeForLookup(path), task.id);
        for (const auto& path : task.plannedCreatePaths)
            writeOwnerByPath.emplace(normalizeForLookup(path), task.id);
    }

    std::vector<ExecutionManifestFinding> findings;
    const auto add = [&](std::string code, std::string message, const std::string& taskId) {
        findings.push_back({std::move(code), std::move(message), taskId});
    };
    const auto isExistingDirectory = [&](const std::string& key) {
        const std::string prefix = key + "/";
        return std::any_of(baselineIndex.begin(), baselineIndex.end(),
            [&](const auto& item) { return item.first.starts_with(prefix); });
    };

    for (const auto& task : manifest.tasks) {
        for (const auto& path : task.ownedPaths) {
            const std::string key = normalizeForLookup(path);
            if (!baselineIndex.contains(key)) {
                add("workspace_scope.owned_write_path_missing_from_baseline",
                    "An owned_write path is not an existing baseline file: " + key,
                    task.id);
            }
        }
        for (const auto& path : task.plannedCreatePaths) {
            const std::string key = normalizeForLookup(path);
            if (baselineIndex.contains(key)) {
                add("workspace_scope.planned_create_path_already_exists",
                    "A planned_create path already exists in the captured baseline: " + key,
                    task.id);
            } else if (isExistingDirectory(key)) {
                add("workspace_scope.planned_create_path_is_directory",
                    "A planned_create path names an existing directory rather than one file: " + key,
                    task.id);
            }
        }

        const auto validateReadable = [&](const std::filesystem::path& path,
                                          std::string_view role) {
            const std::string key = normalizeForLookup(path);
            const auto owner = writeOwnerByPath.find(key);
            if (!baselineIndex.contains(key) && owner == writeOwnerByPath.end()) {
                add("workspace_scope.read_path_missing_from_baseline",
                    std::string(role) + " path is neither a baseline file nor another task's output: "
                        + key,
                    task.id);
            }
            return owner;
        };
        // MW-P3A package 2 (Run a8afe6a0-7eb1-4c05-aaf3-46a1c5c90b6b): a
        // baselineOnlyReadPaths entry must never require a declared
        // dependency either, for exactly the same reason
        // deriveManifestDependencyGraph never derives an edge from one - it
        // is a read of the frozen baseline, not of another task's proposed
        // output, even when the same path also happens to be another
        // task's write target. Without this, forcing a declared dependency
        // here would silently recreate the same false-dependency defect via
        // `dependencies` instead of via a Backend-derived edge. Always empty
        // for a legacy-parsed manifest, so this changes nothing for that
        // pathway.
        std::unordered_set<std::string> baselineOnly;
        baselineOnly.reserve(task.baselineOnlyReadPaths.size());
        for (const auto& path : task.baselineOnlyReadPaths) {
            baselineOnly.insert(normalizeForLookup(path));
        }
        for (const auto& path : task.requiredReadPaths) {
            const auto owner = validateReadable(path, "A required_read");
            if (baselineOnly.contains(normalizeForLookup(path))) continue;
            if (owner != writeOwnerByPath.end() && owner->second != task.id
                && std::find(task.dependencies.begin(), task.dependencies.end(), owner->second)
                    == task.dependencies.end()) {
                // Bounded, leak-safe, and precise: names exactly the consumer
                // task, the producer task, and the already-normalized
                // workspace-relative path (never an absolute path, workspace
                // root, or provider payload) so a repair Planner can resolve
                // this in one bounded attempt instead of repeating the same
                // invalid dependency graph. The path is `owner->first`, the
                // normalized key already computed by validateReadable via
                // normalizeForLookup - never re-derived from raw input here.
                add("manifest.missing_scope_dependency",
                    "Task '" + task.id + "' requires path '" + owner->first
                        + "' produced by task '" + owner->second
                        + "' and must include '" + owner->second + "' in dependencies.",
                    task.id);
            }
        }
        for (const auto& path : task.referencePaths)
            static_cast<void>(validateReadable(path, "A reference_only"));
    }
    return findings;
}

common::Result<RunWorkspaceScope> RunWorkspaceScopeService::freezeForManifest(
    const RunId& runId, const ExecutionManifest& manifest, const WorkspaceBaselineSnapshot& baseline)
{
    const auto baselineFindings = validateManifestAgainstBaseline(manifest, baseline);
    if (!baselineFindings.empty()) {
        return common::Result<RunWorkspaceScope>::failure(
            {baselineFindings.front().code, baselineFindings.front().message});
    }
    const auto baselineIndex = buildBaselineHashIndex(baseline);

    RunWorkspaceScope scope;
    scope.runId = runId;
    scope.manifestSchemaVersion = manifest.schemaVersion;
    scope.frozenAt = std::chrono::system_clock::now();

    auto addEntry = [&](const std::filesystem::path& rawPath, WorkspaceScopeRole role,
                         const std::string& taskId) -> common::Result<void> {
        const std::string normalized = normalizeForLookup(rawPath);
        const auto baselineHashIt = baselineIndex.find(normalized);
        const bool existsInBaseline = baselineHashIt != baselineIndex.end();

        if (role == WorkspaceScopeRole::PlannedCreate && existsInBaseline) {
            return common::Result<void>::failure(
                {"workspace_scope.planned_create_path_already_exists",
                 "A planned_create path already exists in the captured baseline: " + normalized});
        }
        if (role == WorkspaceScopeRole::OwnedWrite && !existsInBaseline) {
            return common::Result<void>::failure(
                {"workspace_scope.owned_write_path_missing_from_baseline",
                 "An owned_write path is missing from the captured baseline: " + normalized});
        }

        RunWorkspaceScopeEntry entry;
        entry.relativePath = normalized;
        entry.role = role;
        entry.reason = synthesizeReason(role, taskId);
        entry.taskId = taskId;
        if (existsInBaseline) entry.baselineHash = baselineHashIt->second;
        entry.origin = WorkspaceScopeOrigin::FrozenInitial;
        scope.entries.push_back(std::move(entry));
        return common::Result<void>::ok();
    };

    for (const auto& task : manifest.tasks) {
        for (const auto& path : task.ownedPaths) {
            auto added = addEntry(path, WorkspaceScopeRole::OwnedWrite, task.id);
            if (!added.success()) return common::Result<RunWorkspaceScope>::failure(*added.error());
        }
        for (const auto& path : task.plannedCreatePaths) {
            auto added = addEntry(path, WorkspaceScopeRole::PlannedCreate, task.id);
            if (!added.success()) return common::Result<RunWorkspaceScope>::failure(*added.error());
        }
        for (const auto& path : task.requiredReadPaths) {
            auto added = addEntry(path, WorkspaceScopeRole::RequiredRead, task.id);
            if (!added.success()) return common::Result<RunWorkspaceScope>::failure(*added.error());
        }
        for (const auto& path : task.referencePaths) {
            auto added = addEntry(path, WorkspaceScopeRole::ReferenceOnly, task.id);
            if (!added.success()) return common::Result<RunWorkspaceScope>::failure(*added.error());
        }
        for (const auto& excluded : task.excludedPaths) {
            auto added = addEntry(excluded.path, WorkspaceScopeRole::Excluded, task.id);
            if (!added.success()) return common::Result<RunWorkspaceScope>::failure(*added.error());
        }
        for (const auto& deferred : task.deferredPaths) {
            auto added = addEntry(deferred.path, WorkspaceScopeRole::Deferred, task.id);
            if (!added.success()) return common::Result<RunWorkspaceScope>::failure(*added.error());
        }
    }

    // AG-15/MW-P1: file-level dependency edges are Backend-derived, never
    // Planner-declared - a task's required_read path P links to whichever
    // other task's owned_write/planned_create claims P. Reuses the exact
    // same derivation ExecutionManifestValidator's cycle detection already
    // walked before this manifest could ever reach freeze (see
    // ExecutionManifestDependencyGraph.hpp), so the two can never disagree
    // about which edges exist.
    for (const auto& edge : deriveManifestDependencyGraph(manifest)) {
        if (edge.origin != ManifestDependencyEdgeOrigin::BackendDerived) continue;
        scope.dependencyEdges.push_back(
            {edge.consumerTaskId, edge.producerTaskId, *edge.relativePath});
    }

    const auto frozen = repository_.freeze(scope);
    if (!frozen.success()) {
        if (frozen.error()->code == "workspace_scope.already_frozen") {
            const auto existing = repository_.findForRun(runId);
            if (existing.success() && existing.value().has_value()) {
                return common::Result<RunWorkspaceScope>::ok(*existing.value());
            }
        }
        return common::Result<RunWorkspaceScope>::failure(*frozen.error());
    }
    return common::Result<RunWorkspaceScope>::ok(std::move(scope));
}

common::Result<std::optional<RunWorkspaceScope>> RunWorkspaceScopeService::findForRun(
    const RunId& runId) const
{
    return repository_.findForRun(runId);
}

common::Result<RunWorkspaceScopeExpansion> RunWorkspaceScopeService::requestExpansion(
    const RunId& runId, const std::string& taskId, const std::string& rawRelativePath,
    WorkspaceScopeRole role, const std::string& reason, const WorkspaceReadPolicy& denyPolicy,
    const std::optional<WorkspaceBaselineSnapshot>& currentBaseline,
    std::size_t maxExpansionsForTask)
{
    if (role != WorkspaceScopeRole::RequiredRead && role != WorkspaceScopeRole::ReferenceOnly) {
        return common::Result<RunWorkspaceScopeExpansion>::failure(
            {"workspace_scope.expansion_role_not_allowed",
             "A scope expansion may only grant required_read or reference_only, never a write role"});
    }
    if (reason.empty() || reason.size() > kRunWorkspaceScopeMaxReasonBytes) {
        return common::Result<RunWorkspaceScopeExpansion>::failure(
            {"workspace_scope.expansion_invalid_reason",
             "A scope expansion reason must be nonblank and bounded"});
    }

    const auto checked = WorkspaceReadPolicyValidator::validateRelativePath(
        std::filesystem::path(rawRelativePath), denyPolicy);
    if (!checked.success()) {
        return common::Result<RunWorkspaceScopeExpansion>::failure(
            {"workspace_scope.expansion_path_denied",
             "The requested expansion path is unsafe or globally excluded"});
    }

    const auto frozenScope = repository_.findForRun(runId);
    if (!frozenScope.success())
        return common::Result<RunWorkspaceScopeExpansion>::failure(*frozenScope.error());
    if (!frozenScope.value().has_value()) {
        return common::Result<RunWorkspaceScopeExpansion>::failure(
            {"workspace_scope.not_frozen",
             "A scope expansion requires an existing frozen Run workspace scope"});
    }

    const std::string normalized = normalizeForLookup(checked.value());
    bool knownProducedPath = false;
    for (const auto& entry : frozenScope.value()->entries) {
        if (entry.relativePath != normalized) continue;
        if (entry.role == WorkspaceScopeRole::Excluded) {
            return common::Result<RunWorkspaceScopeExpansion>::failure(
                {"workspace_scope.expansion_excluded_path",
                 "A path frozen as excluded cannot be activated by a worker"});
        }
        if (entry.role == WorkspaceScopeRole::OwnedWrite
            || entry.role == WorkspaceScopeRole::PlannedCreate) {
            knownProducedPath = true;
        }
        if (entry.taskId == taskId && entry.role != WorkspaceScopeRole::Deferred) {
            return common::Result<RunWorkspaceScopeExpansion>::failure(
                {"workspace_scope.expansion_path_already_active",
                 "The requested path is already active for this task"});
        }
    }
    std::optional<std::string> observedHash;
    if (currentBaseline.has_value()) {
        const auto baselineIndex = buildBaselineHashIndex(*currentBaseline);
        const auto found = baselineIndex.find(normalized);
        if (found != baselineIndex.end()) observedHash = found->second;
    }
    if (!observedHash.has_value() && !knownProducedPath) {
        return common::Result<RunWorkspaceScopeExpansion>::failure(
            {"workspace_scope.expansion_path_missing",
             "A read-scope expansion must identify a baseline file or a frozen task output"});
    }

    const auto existingExpansions = repository_.listExpansionsForRun(runId);
    if (!existingExpansions.success()) {
        return common::Result<RunWorkspaceScopeExpansion>::failure(*existingExpansions.error());
    }
    const auto alreadyRecordedForThisPath = std::find_if(
        existingExpansions.value().begin(), existingExpansions.value().end(),
        [&](const RunWorkspaceScopeExpansion& expansion) {
            return expansion.taskId == taskId && expansion.relativePath == normalized;
        });
    if (alreadyRecordedForThisPath != existingExpansions.value().end()) {
        if (alreadyRecordedForThisPath->role != role) {
            return common::Result<RunWorkspaceScopeExpansion>::failure(
                {"workspace_scope.expansion_role_conflict",
                 "An existing expansion for this path has a different role"});
        }
        return common::Result<RunWorkspaceScopeExpansion>::ok(*alreadyRecordedForThisPath);
    }
    {
        const std::size_t existingCountForTask = static_cast<std::size_t>(std::count_if(
            existingExpansions.value().begin(), existingExpansions.value().end(),
            [&](const RunWorkspaceScopeExpansion& expansion) { return expansion.taskId == taskId; }));
        if (existingCountForTask >= maxExpansionsForTask) {
            return common::Result<RunWorkspaceScopeExpansion>::failure(
                {"workspace_scope.expansion_limit_exceeded",
                 "This task has already reached its bounded scope-expansion limit"});
        }
    }

    RunWorkspaceScopeExpansion expansion;
    expansion.id = generateUuid();
    expansion.runId = runId;
    expansion.taskId = taskId;
    expansion.relativePath = normalized;
    expansion.role = role;
    expansion.reason = reason;
    expansion.baselineHash = observedHash;
    expansion.approvedAt = std::chrono::system_clock::now();

    const auto recorded = repository_.recordExpansion(expansion);
    if (!recorded.success() && recorded.error()->code != "workspace_scope.expansion_already_recorded") {
        return common::Result<RunWorkspaceScopeExpansion>::failure(*recorded.error());
    }
    if (!recorded.success()) {
        const auto raced = repository_.listExpansionsForRun(runId);
        if (!raced.success())
            return common::Result<RunWorkspaceScopeExpansion>::failure(*raced.error());
        const auto existing = std::find_if(raced.value().begin(), raced.value().end(),
            [&](const RunWorkspaceScopeExpansion& item) {
                return item.taskId == taskId && item.relativePath == normalized;
            });
        if (existing == raced.value().end() || existing->role != role) {
            return common::Result<RunWorkspaceScopeExpansion>::failure(
                {"workspace_scope.expansion_role_conflict",
                 "A concurrent expansion recorded a conflicting role"});
        }
        return common::Result<RunWorkspaceScopeExpansion>::ok(*existing);
    }
    return common::Result<RunWorkspaceScopeExpansion>::ok(std::move(expansion));
}

common::Result<std::vector<RunWorkspaceScopeExpansion>> RunWorkspaceScopeService::listExpansionsForRun(
    const RunId& runId) const
{
    return repository_.listExpansionsForRun(runId);
}

common::Result<RunWorkspaceScopeDiagnosticRead> RunWorkspaceScopeService::requestDiagnosticRead(
    const RunId& runId, const std::string& taskId, const std::string& rawRelativePath,
    const std::string& findingCode, const std::string& reason,
    const WorkspaceReadPolicy& denyPolicy, const WorkspaceBaselineSnapshot& baseline,
    std::size_t maxDiagnosticReadsForTask)
{
    if (findingCode.empty() || findingCode.size() > kRunWorkspaceScopeMaxFindingCodeBytes) {
        return common::Result<RunWorkspaceScopeDiagnosticRead>::failure(
            {"workspace_scope.diagnostic_read_invalid_finding",
             "A diagnostic read must name a bounded, nonblank finding code"});
    }
    if (reason.empty() || reason.size() > kRunWorkspaceScopeMaxReasonBytes) {
        return common::Result<RunWorkspaceScopeDiagnosticRead>::failure(
            {"workspace_scope.diagnostic_read_invalid_finding",
             "A diagnostic read reason must be nonblank and bounded"});
    }

    const auto frozenScope = repository_.findForRun(runId);
    if (!frozenScope.success())
        return common::Result<RunWorkspaceScopeDiagnosticRead>::failure(*frozenScope.error());
    if (!frozenScope.value().has_value()) {
        return common::Result<RunWorkspaceScopeDiagnosticRead>::failure(
            {"workspace_scope.diagnostic_read_unavailable",
             "A diagnostic read requires an existing frozen Run workspace scope"});
    }

    // The path-safety/deny-list check below is the ONE authoritative
    // security boundary this call can never see past - unlike the frozen
    // scope's own Excluded classification (a Planner decision, deliberately
    // overridable here), a rejection from here is kept in a distinct error
    // code family so a caller (and a test) can tell "the Backend's security
    // policy denied this" apart from "the Planner had classified this path
    // differently." "workspace_path.unsafe" (absolute/traversal/reserved
    // component/empty) is a shape problem with the path itself, reported as
    // diagnostic_read_invalid_path; anything else the validator rejects
    // (an active deny-list entry, a restricted allowed-roots boundary) is a
    // genuine security denial, reported as diagnostic_read_policy_denied.
    const auto checked = WorkspaceReadPolicyValidator::validateRelativePath(
        std::filesystem::path(rawRelativePath), denyPolicy);
    if (!checked.success()) {
        if (checked.error()->code == "workspace_path.unsafe") {
            return common::Result<RunWorkspaceScopeDiagnosticRead>::failure(
                {"workspace_scope.diagnostic_read_invalid_path",
                 "The requested diagnostic-read path is not a safe workspace-relative path"});
        }
        return common::Result<RunWorkspaceScopeDiagnosticRead>::failure(
            {"workspace_scope.diagnostic_read_policy_denied",
             "The requested diagnostic-read path is denied by authoritative workspace policy"});
    }

    const std::string normalized = normalizeForLookup(checked.value());
    const auto baselineIndex = buildBaselineHashIndex(baseline);
    const auto foundInBaseline = baselineIndex.find(normalized);
    if (foundInBaseline == baselineIndex.end()) {
        return common::Result<RunWorkspaceScopeDiagnosticRead>::failure(
            {"workspace_scope.diagnostic_read_not_in_baseline",
             "A diagnostic read must name an existing baseline file"});
    }

    const auto existingGrants = repository_.listDiagnosticReadsForRun(runId);
    if (!existingGrants.success()) {
        return common::Result<RunWorkspaceScopeDiagnosticRead>::failure(*existingGrants.error());
    }
    const auto alreadyRecorded = std::find_if(
        existingGrants.value().begin(), existingGrants.value().end(),
        [&](const RunWorkspaceScopeDiagnosticRead& grant) {
            return grant.taskId == taskId && grant.relativePath == normalized;
        });
    if (alreadyRecorded != existingGrants.value().end()) {
        return common::Result<RunWorkspaceScopeDiagnosticRead>::ok(*alreadyRecorded);
    }
    {
        const std::size_t existingCountForTask = static_cast<std::size_t>(std::count_if(
            existingGrants.value().begin(), existingGrants.value().end(),
            [&](const RunWorkspaceScopeDiagnosticRead& grant) { return grant.taskId == taskId; }));
        if (existingCountForTask >= maxDiagnosticReadsForTask) {
            return common::Result<RunWorkspaceScopeDiagnosticRead>::failure(
                {"workspace_scope.diagnostic_read_limit_exceeded",
                 "This task has already reached its bounded diagnostic-read limit"});
        }
    }

    RunWorkspaceScopeDiagnosticRead grant;
    grant.id = generateUuid();
    grant.runId = runId;
    grant.taskId = taskId;
    grant.relativePath = normalized;
    grant.findingCode = findingCode;
    grant.reason = reason;
    grant.baselineHash = foundInBaseline->second;
    grant.grantedAt = std::chrono::system_clock::now();

    const auto recorded = repository_.recordDiagnosticRead(grant);
    if (!recorded.success()
        && recorded.error()->code != "workspace_scope.diagnostic_read_already_recorded") {
        return common::Result<RunWorkspaceScopeDiagnosticRead>::failure(*recorded.error());
    }
    if (!recorded.success()) {
        const auto raced = repository_.listDiagnosticReadsForRun(runId);
        if (!raced.success())
            return common::Result<RunWorkspaceScopeDiagnosticRead>::failure(*raced.error());
        const auto existing = std::find_if(raced.value().begin(), raced.value().end(),
            [&](const RunWorkspaceScopeDiagnosticRead& item) {
                return item.taskId == taskId && item.relativePath == normalized;
            });
        if (existing == raced.value().end()) {
            return common::Result<RunWorkspaceScopeDiagnosticRead>::failure(
                {"workspace_scope.diagnostic_read_unavailable",
                 "A concurrent write left no diagnostic-read grant to return"});
        }
        return common::Result<RunWorkspaceScopeDiagnosticRead>::ok(*existing);
    }
    return common::Result<RunWorkspaceScopeDiagnosticRead>::ok(std::move(grant));
}

common::Result<std::vector<RunWorkspaceScopeDiagnosticRead>>
RunWorkspaceScopeService::listDiagnosticReadsForRun(const RunId& runId) const
{
    return repository_.listDiagnosticReadsForRun(runId);
}

} // namespace ai::application
