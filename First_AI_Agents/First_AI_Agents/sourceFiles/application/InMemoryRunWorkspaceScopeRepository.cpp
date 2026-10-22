#include "ai/application/InMemoryRunWorkspaceScopeRepository.hpp"

#include <algorithm>

namespace ai::application {

common::Result<void> InMemoryRunWorkspaceScopeRepository::freeze(RunWorkspaceScope scope)
{
    std::lock_guard lock(mutex_);
    const RunId runId = scope.runId;
    if (scopes_.contains(runId)) {
        return common::Result<void>::failure({"workspace_scope.already_frozen",
            "A workspace scope already exists for this Run"});
    }
    scopes_[runId] = std::move(scope);
    return common::Result<void>::ok();
}

common::Result<std::optional<RunWorkspaceScope>>
InMemoryRunWorkspaceScopeRepository::findForRun(const RunId& runId) const
{
    std::lock_guard lock(mutex_);
    const auto it = scopes_.find(runId);
    if (it == scopes_.end()) return common::Result<std::optional<RunWorkspaceScope>>::ok(std::nullopt);
    return common::Result<std::optional<RunWorkspaceScope>>::ok(it->second);
}

common::Result<void> InMemoryRunWorkspaceScopeRepository::recordExpansion(
    RunWorkspaceScopeExpansion expansion)
{
    std::lock_guard lock(mutex_);
    const bool duplicate = std::any_of(expansions_.begin(), expansions_.end(),
        [&](const RunWorkspaceScopeExpansion& existing) {
            return existing.runId == expansion.runId && existing.taskId == expansion.taskId
                && existing.relativePath == expansion.relativePath;
        });
    if (duplicate) {
        return common::Result<void>::failure({"workspace_scope.expansion_already_recorded",
            "This path was already recorded as an expansion for this Run/task"});
    }
    expansions_.push_back(std::move(expansion));
    return common::Result<void>::ok();
}

common::Result<std::vector<RunWorkspaceScopeExpansion>>
InMemoryRunWorkspaceScopeRepository::listExpansionsForRun(const RunId& runId) const
{
    std::lock_guard lock(mutex_);
    std::vector<RunWorkspaceScopeExpansion> results;
    for (const auto& expansion : expansions_) {
        if (expansion.runId == runId) results.push_back(expansion);
    }
    return common::Result<std::vector<RunWorkspaceScopeExpansion>>::ok(std::move(results));
}

common::Result<void> InMemoryRunWorkspaceScopeRepository::recordDiagnosticRead(
    RunWorkspaceScopeDiagnosticRead grant)
{
    std::lock_guard lock(mutex_);
    const bool duplicate = std::any_of(diagnosticReads_.begin(), diagnosticReads_.end(),
        [&](const RunWorkspaceScopeDiagnosticRead& existing) {
            return existing.runId == grant.runId && existing.taskId == grant.taskId
                && existing.relativePath == grant.relativePath;
        });
    if (duplicate) {
        return common::Result<void>::failure({"workspace_scope.diagnostic_read_already_recorded",
            "This path was already recorded as a diagnostic read for this Run/task"});
    }
    diagnosticReads_.push_back(std::move(grant));
    return common::Result<void>::ok();
}

common::Result<std::vector<RunWorkspaceScopeDiagnosticRead>>
InMemoryRunWorkspaceScopeRepository::listDiagnosticReadsForRun(const RunId& runId) const
{
    std::lock_guard lock(mutex_);
    std::vector<RunWorkspaceScopeDiagnosticRead> results;
    for (const auto& grant : diagnosticReads_) {
        if (grant.runId == runId) results.push_back(grant);
    }
    return common::Result<std::vector<RunWorkspaceScopeDiagnosticRead>>::ok(std::move(results));
}

} // namespace ai::application
