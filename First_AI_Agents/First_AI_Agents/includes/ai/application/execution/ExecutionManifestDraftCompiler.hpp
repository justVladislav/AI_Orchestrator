#pragma once

#include "ai/application/execution/ExecutionManifestDraftTypes.hpp"
#include "ai/application/execution/ExecutionManifestTypes.hpp"
#include "ai/application/workspace/WorkspaceBaselineProvenance.hpp"

#include <vector>

// MW-P3A package 2 ("Backend Compiler + Read Semantics"): turns a parsed,
// already-structurally-safe ExecutionManifestDraft into a real
// execution-manifest.v1 ExecutionManifest, making the two deterministic,
// Backend-owned decisions the draft deliberately leaves unmade:
// - which writePaths entries are existing (owned_write) versus new
//   (planned_create), from the Run's own frozen WorkspaceBaselineSnapshot -
//   never the provider's guess;
// - which required-read entries the provider explicitly typed as a
//   baseline-only read (never edge-forming) versus a proposed-dependency
//   read (must name a real producer among this draft's own work tasks, and
//   derives a real cross-task dependency edge).
//
// This is not a second validator: ExecutionManifestValidator::validate()
// (structural safety, role conflicts, cycles, capability/acceptance rules)
// and RunWorkspaceScopeService::validateManifestAgainstBaseline (the same
// existing/absent baseline re-check, defense-in-depth) still run afterward
// on the compiled manifest exactly as they do for a legacy submission - the
// caller (ExecutionManifestPlanner) is responsible for running them and
// folding this compiler's own findings into the same bounded repair pass.
namespace ai::application {

struct ExecutionManifestDraftCompilerRequest {
    ExecutionManifestDraft draft;
    RunId runId;
    // Policy is deliberately not part of this request: exactly like
    // ExecutionManifestParser::parse(), the compiled manifest's `policy`
    // field is left default-constructed - ExecutionManifestService::validate/
    // freeze() already overwrite it unconditionally from the caller's own
    // trusted policy, ignoring anything set here.
    WorkspaceBaselineSnapshot baseline;
};

struct ExecutionManifestDraftCompilationResult {
    // Always populated (best-effort) even when findings is non-empty, so a
    // caller building one combined repair prompt can still describe the
    // shape the model actually submitted.
    ExecutionManifest manifest;
    std::vector<ExecutionManifestFinding> findings;
};

class ExecutionManifestDraftCompiler {
public:
    [[nodiscard]] static ExecutionManifestDraftCompilationResult compile(
        ExecutionManifestDraftCompilerRequest request);
};

} // namespace ai::application
