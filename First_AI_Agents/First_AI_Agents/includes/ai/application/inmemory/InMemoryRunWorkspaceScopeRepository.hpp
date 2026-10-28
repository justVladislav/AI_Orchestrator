#pragma once

#include "ai/application/execution/IRunWorkspaceScopeRepository.hpp"

#include <map>
#include <mutex>
#include <vector>

namespace ai::application {

class InMemoryRunWorkspaceScopeRepository final : public IRunWorkspaceScopeRepository {
public:
    common::Result<void> freeze(RunWorkspaceScope scope) override;
    [[nodiscard]] common::Result<std::optional<RunWorkspaceScope>> findForRun(
        const RunId& runId) const override;

    common::Result<void> recordExpansion(RunWorkspaceScopeExpansion expansion) override;
    [[nodiscard]] common::Result<std::vector<RunWorkspaceScopeExpansion>>
    listExpansionsForRun(const RunId& runId) const override;

    common::Result<void> recordDiagnosticRead(
        RunWorkspaceScopeDiagnosticRead grant) override;
    [[nodiscard]] common::Result<std::vector<RunWorkspaceScopeDiagnosticRead>>
    listDiagnosticReadsForRun(const RunId& runId) const override;

private:
    mutable std::mutex mutex_;
    std::map<RunId, RunWorkspaceScope> scopes_;
    // Insertion order preserved (a std::vector, not a map) - listExpansionsForRun's
    // contract is "in the order they were recorded."
    std::vector<RunWorkspaceScopeExpansion> expansions_;
    std::vector<RunWorkspaceScopeDiagnosticRead> diagnosticReads_;
};

} // namespace ai::application
