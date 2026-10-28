#pragma once

#include "ai/application/execution/ExecutionManifestTypes.hpp"
#include "ai/application/execution/IRunWorkspaceScopeRepository.hpp"
#include "ai/application/execution/RunWorkspaceScopeTypes.hpp"
#include "ai/application/workspace/WorkspaceBaselineProvenance.hpp"
#include "ai/application/workspace/WorkspaceReadPolicy.hpp"
#include "ai/common/Result.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace ai::application {

// AG-15: builds, freezes, and grows (via audited expansion) the per-Run
// workspace scope. See ARCHITECTURE_GAPS.md's AG-15 and
// AI_ORCHESTRATOR_TARGET_ARCHITECTURE.md's "Cross-Run continuity and
// workspace scoping" section.
class RunWorkspaceScopeService {
public:
    explicit RunWorkspaceScopeService(IRunWorkspaceScopeRepository& repository);

    // Pure baseline-aware validation used by the Planner before its
    // terminal candidate is frozen and repeated defensively by
    // freezeForManifest. Keeping this phase side-effect free lets the
    // Planner include these findings in its single bounded repair attempt.
    [[nodiscard]] static std::vector<ExecutionManifestFinding>
    validateManifestAgainstBaseline(
        const ExecutionManifest& manifest,
        const WorkspaceBaselineSnapshot& baseline);

    // Builds and persists the frozen RunWorkspaceScope for runId from an
    // already-frozen, already-validated ExecutionManifest and the Run's own
    // captured WorkspaceBaselineSnapshot. Cross-checks the baseline
    // existence rules ExecutionManifestValidator cannot perform on its own
    // (planned_create must NOT exist in baseline, owned_write MUST exist),
    // attaches real baseline hashes, derives file-level dependency edges
    // (a task's required_read path linked to whichever other task's
    // owned_write/planned_create claims it - never Planner-declared), and
    // persists exactly once. A second call for the same runId is a benign
    // idempotent success (see IRunWorkspaceScopeRepository::freeze); any
    // other persistence failure propagates, never silently swallowed.
    common::Result<RunWorkspaceScope> freezeForManifest(
        const RunId& runId,
        const ExecutionManifest& manifest,
        const WorkspaceBaselineSnapshot& baseline);

    [[nodiscard]] common::Result<std::optional<RunWorkspaceScope>> findForRun(
        const RunId& runId) const;

    // Validates and persists one audited scope-expansion request. `role`
    // must be RequiredRead or ReferenceOnly - an expansion can never grant a
    // new write target (see RunWorkspaceScopeExpansion's own doc comment).
    // Re-validates the raw path with the same safety rules used at freeze
    // time, rejects it if `denyPolicy`'s deny-lists exclude it, and enforces
    // `maxExpansionsForTask` against the count already recorded for
    // (runId, taskId). A duplicate (runId, taskId, relativePath) is a benign
    // idempotent success.
    common::Result<RunWorkspaceScopeExpansion> requestExpansion(
        const RunId& runId,
        const std::string& taskId,
        const std::string& rawRelativePath,
        WorkspaceScopeRole role,
        const std::string& reason,
        const WorkspaceReadPolicy& denyPolicy,
        const std::optional<WorkspaceBaselineSnapshot>& currentBaseline,
        std::size_t maxExpansionsForTask);

    [[nodiscard]] common::Result<std::vector<RunWorkspaceScopeExpansion>> listExpansionsForRun(
        const RunId& runId) const;

    // Verification-repair reliability hardening: the audited, repair-only
    // counterpart to requestExpansion above. A real incident showed a
    // Planner-frozen WorkspaceScopeRole::Excluded classification (correct at
    // Manifest-freeze time) could later make an actionable verification
    // diagnostic unrepairable - the repair worker could not inspect the
    // authoritative declaration a diagnostic concerned (e.g. a QML singleton
    // whose real property names differ from what a consumer used) and
    // guessed another invalid replacement instead. This grants narrow,
    // read-only, durably-audited access to an EXISTING baseline file for
    // that one bounded repair attempt, without ever mutating the frozen
    // scope, the original Excluded entry, or any write allow-list.
    //
    // Unlike requestExpansion, this call:
    //   - MAY target a path frozen WorkspaceScopeRole::Excluded (the whole
    //     point - see RunWorkspaceScopeDiagnosticRead's own doc comment);
    //   - requires the path to already exist in `baseline` (never a
    //     not-yet-created planned_create/owned_write target - a diagnostic
    //     read is about inspecting an existing authoritative declaration,
    //     never a forward-looking write target) ->
    //     "workspace_scope.diagnostic_read_not_in_baseline" otherwise;
    //   - still fails closed on the SAME authoritative `denyPolicy`
    //     WorkspaceReadPolicy safety/deny-list check requestExpansion uses -
    //     this is the one thing that can NEVER be overridden, keeping a
    //     genuine Backend security denial (secret/build/generated paths,
    //     traversal, absolute paths - "workspace_scope.
    //     diagnostic_read_invalid_path" for an unsafe path shape,
    //     "workspace_scope.diagnostic_read_policy_denied" for an active
    //     deny-list/allowed-roots rejection) structurally distinct from the
    //     Planner's own task-level Excluded classification, which this
    //     mechanism is explicitly allowed to see past;
    //   - never accepts a write role - there is no role parameter at all,
    //     since a diagnostic read is always read-only by construction;
    //   - is bounded per (runId, taskId) by maxDiagnosticReadsForTask,
    //     independent of and never shared with requestExpansion's own
    //     per-task expansion budget -> "workspace_scope.
    //     diagnostic_read_limit_exceeded";
    //   - requires a frozen RunWorkspaceScope to already exist for runId
    //     (this is a repair-only refinement of an already-frozen Run, never
    //     a way to bootstrap one) and a bounded, nonblank `findingCode` and
    //     `reason` -> "workspace_scope.diagnostic_read_unavailable" /
    //     "workspace_scope.diagnostic_read_invalid_finding" otherwise;
    //   - is idempotent per (runId, taskId, relativePath), exactly like
    //     requestExpansion: a repeated call for the same triple returns the
    //     original durable grant rather than erroring or duplicating it.
    //
    // Deliberately does NOT cross-validate findingCode/reason against the
    // Run's actual verification findings, and does NOT infer which file a
    // diagnostic concerns from the diagnostic text itself - both would
    // require parsing/symbol-resolution machinery this package explicitly
    // avoids (see AgentToolTurnLoopRunner's own same-attempt diagnostic-read
    // inspection gate for how "was the granted file actually read" is
    // verified generically instead, without any such inference). The model
    // chooses the path from the bounded frozen-scope/baseline catalog it is
    // already given; this call only ever enforces safety, existence, and
    // audit-trail bounds on that choice.
    common::Result<RunWorkspaceScopeDiagnosticRead> requestDiagnosticRead(
        const RunId& runId,
        const std::string& taskId,
        const std::string& rawRelativePath,
        const std::string& findingCode,
        const std::string& reason,
        const WorkspaceReadPolicy& denyPolicy,
        const WorkspaceBaselineSnapshot& baseline,
        std::size_t maxDiagnosticReadsForTask);

    [[nodiscard]] common::Result<std::vector<RunWorkspaceScopeDiagnosticRead>>
    listDiagnosticReadsForRun(const RunId& runId) const;

private:
    IRunWorkspaceScopeRepository& repository_;
};

} // namespace ai::application
