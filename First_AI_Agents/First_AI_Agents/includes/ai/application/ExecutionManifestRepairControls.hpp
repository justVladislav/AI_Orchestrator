#pragma once

// Backend-owned Planner-boundary normalizations that carry no authority:
//   1. the numeric top-level controls of a Planner validation repair (below), and
//   2. canonicalizeExcludedReadConflicts, a task's own excludedPaths note versus its own read.
//
// Part 1.
// requestedMaxParallelism, requestedTokenBudget and requestedCostMicros are the model's REQUESTS,
// not capabilities: the trusted policy ceilings, the Run's parallelism clamp and the frozen
// ProviderExecutionBudgetPolicy decide what is really allowed (see the comment on
// ExecutionManifest::requestedTokenBudget). A bounded repair that is asked to fix one task must
// leave them alone, but a model that re-emits the whole draft sometimes nudges one of them, and
// the monotonic guard then rejected the whole repair (Run de6d81fb). Restoring them from the
// primary draft costs nothing: it grants no authority and the structural guard and the full
// validator still judge the result.

#include "ai/application/ExecutionManifestDraftTypes.hpp"
#include "ai/application/ExecutionManifestTypes.hpp"

#include <string>
#include <vector>

namespace ai::application {

// True when the primary findings ask for a graph-wide repair: some finding names no task, or is a
// graph-level code whose fix can reshape more than the task it names. Such a repair may legitimately
// change anything, so neither the monotonic guard nor the control restoration constrains it.
[[nodiscard]] bool isGraphWideRepair(const std::vector<ExecutionManifestFinding>& primaryFindings);

// Restores the three numeric controls of `repair` from `primary` and returns one audit line per
// restored field. Applies only to a task-scoped repair (non-empty findings, not graph-wide). A field
// whose own finding code is among the primary findings is left as the model submitted it, so the
// model can still fix exactly the control that caused the rejection. Finding codes are compared
// explicitly; message text is never parsed. schemaVersion, bootstrapDeclaration, the task counts
// and every task are untouched. Idempotent: a second call restores and reports nothing.
[[nodiscard]] std::vector<std::string> restoreNonAuthoritativeRepairControls(
    const ExecutionManifestDraft& primary,
    ExecutionManifestDraft& repair,
    const std::vector<ExecutionManifestFinding>& primaryFindings);

// Part 2 (Run de6d81fb: the Planner listed MachineMetricPanel.qml, MachineTrendChart.qml and
// AlertSeverityBadge.qml as required reads AND as excluded in the same task, the validator rejected
// manifest.conflicting_path_role, and the one repair regressed).
//
// excludedPaths is the Planner's own decision note, not a Backend security boundary: the global
// WorkspaceReadPolicy (build/generated/secret/denied) is enforced separately and stays mandatory.
// When the SAME task still actively reads a path after the active roles were canonicalized
// (required_read, then reference_only - the existing precedence), the matching excludedPaths
// entries of that task are removed, all duplicates of the path at once, with one audit line per
// path. The path stays read-only and no write authority appears. Deliberately NOT touched:
//   - a path the task writes (owned_write / planned_create): that conflict stays a hard error;
//   - deferredPaths, which express a separate "not now" decision;
//   - every other task's entries (a path excluded in one task and read in another is no conflict).
// Paths are compared case- and separator-insensitively on the exact path; the full validator still
// runs afterwards and judges anything this pass did not resolve. Idempotent.
[[nodiscard]] std::vector<std::string> canonicalizeExcludedReadConflicts(ExecutionManifest& manifest);

} // namespace ai::application
