#pragma once

#include "ai/application/RunWorkspaceScopeTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <vector>

namespace ai::application {

// AG-15: persistence port for the frozen per-Run workspace scope and its
// audited expansions. Deliberately two separate stores, the same
// "immutable terminal row vs. append-only evidence" split
// IManifestWorkLedgerRepository.hpp already documents: a RunWorkspaceScope
// is written exactly once, immediately after its owning ExecutionManifest is
// frozen, while expansions accumulate afterward as workers discover genuine
// dependencies - folding both into one mutable aggregate would make the
// frozen scope not-actually-immutable.
class IRunWorkspaceScopeRepository {
public:
    virtual ~IRunWorkspaceScopeRepository() = default;

    // Immutable on runId: a second call for the same Run fails with
    // "workspace_scope.already_frozen" rather than replacing the earlier
    // scope. RunWorkspaceScopeService treats that code as a benign
    // idempotent no-op.
    virtual common::Result<void> freeze(RunWorkspaceScope scope) = 0;

    [[nodiscard]] virtual common::Result<std::optional<RunWorkspaceScope>> findForRun(
        const RunId& runId) const = 0;

    // Append-only; immutable per (runId, taskId, relativePath) - a second
    // call for the same triple fails with "workspace_scope.expansion_already_recorded",
    // treated as a benign idempotent no-op by RunWorkspaceScopeService.
    virtual common::Result<void> recordExpansion(RunWorkspaceScopeExpansion expansion) = 0;

    [[nodiscard]] virtual common::Result<std::vector<RunWorkspaceScopeExpansion>>
    listExpansionsForRun(const RunId& runId) const = 0;

    // Append-only; immutable per (runId, taskId, relativePath) - same
    // idempotent-duplicate contract as recordExpansion, but a SEPARATE
    // durable store: a diagnostic read is never mixed into
    // run_workspace_scope_expansions - see RunWorkspaceScopeDiagnosticRead's
    // own doc comment for why this stays structurally distinct. A second
    // call for the same (runId, taskId, relativePath) fails with
    // "workspace_scope.diagnostic_read_already_recorded", treated as a
    // benign idempotent no-op by RunWorkspaceScopeService.
    virtual common::Result<void> recordDiagnosticRead(
        RunWorkspaceScopeDiagnosticRead grant) = 0;

    [[nodiscard]] virtual common::Result<std::vector<RunWorkspaceScopeDiagnosticRead>>
    listDiagnosticReadsForRun(const RunId& runId) const = 0;
};

} // namespace ai::application
