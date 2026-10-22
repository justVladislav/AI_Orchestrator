#pragma once

#include "ai/application/ExecutionManifestTypes.hpp"

#include <vector>

namespace ai::application {

struct WorkspaceReadPolicy;

class ExecutionManifestValidator final {
public:
    // AG-15: `denyPolicy`, when supplied, re-checks every active
    // workspace-scope path (ownedPaths/plannedCreatePaths/requiredReadPaths/
    // referencePaths/excludedPaths/deferredPaths, across all tasks) against
    // the Run's real WorkspaceReadPolicy deny-lists via the already-existing
    // WorkspaceReadPolicyValidator::validateRelativePath - reused, never
    // duplicated - reporting manifest.excluded_path_activated for any hit.
    // nullptr (the default, used by structural-only call sites/tests) skips
    // this check entirely. Baseline-existence and file-role checks require
    // WorkspaceBaselineSnapshot, so ExecutionManifestPlanner invokes
    // RunWorkspaceScopeService::validateManifestAgainstBaseline before its
    // bounded repair/freeze; freezeForManifest repeats them defensively.
    //
    // AG-18.2: `requireResolvedVerificationSelections` decides which shape
    // each task's acceptanceContract requirements' verification selections
    // must satisfy. false (the default, used by the Planner's own pre-
    // binding check and by structural-only call sites/tests) accepts either
    // the fresh provider draft shape or an already Backend-resolved shape.
    // true accepts ONLY the fully Backend-resolved shape - this is what
    // ExecutionManifestService::freeze() itself must pass, so the freeze
    // boundary can never persist an unbound draft regardless of which
    // caller reaches it (not just the Planner's own evaluateSubmission
    // path). See ManifestVerificationProfileBinder for the actual binding
    // that produces the resolved shape.
    [[nodiscard]] static std::vector<ExecutionManifestFinding> validate(
        const ExecutionManifest& manifest, const WorkspaceReadPolicy* denyPolicy = nullptr,
        bool requireResolvedVerificationSelections = false);
};

} // namespace ai::application
