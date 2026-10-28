#pragma once

// MW-P1 (manifest DAG provenance and repair): a single, deterministic,
// pure representation of a Manifest's EFFECTIVE task-dependency graph -
// the union of every Planner-declared edge (`ExecutionManifestTask::
// dependencies`) and every Backend-derived edge (a task's
// `requiredReadPaths` entry matching another task's `ownedPaths`/
// `plannedCreatePaths`).
//
// Real-Run incident (three OpenAI "Stage 2B" Runs, 2026-09-13, all
// `manifest.repair_exhausted` with primary AND final rejection
// `manifest.dependency_cycle`): two tasks each genuinely required a path
// the other produced, so the Planner correctly declared both directions to
// satisfy `manifest.missing_scope_dependency` - and that created a real
// 2-cycle in the DECLARED graph. The bounded repair, given only the task-id
// chain, had no way to tell which `requiredReadPaths` entry produced each
// direction, so it could not judge whether to merge the two tasks or
// redirect one edge, and repeated the same cycle.
//
// Before this, cycle detection (ExecutionManifestValidator) and dependency-
// edge derivation (RunWorkspaceScopeService) were two separate, duplicated
// implementations: the validator's cycle check only ever walked declared
// edges, so a cycle formed purely from derived edges (no declaration at
// all) could slip past the Planner's one bounded repair chance and only
// surface later, post-freeze, in RunWorkspaceScopeService - too late for
// any repair. This single helper is now the one deterministic source both
// call, so neither can drift from the other and a combined-origin cycle is
// always caught while a repair chance still exists.
//
// This module is diagnostic/derivation only. It does not change what a
// Planner must declare (`manifest.missing_scope_dependency` is unchanged -
// see RunWorkspaceScopeService.cpp) and it is never consulted by
// ExecutionManifestExecutor's own dispatch ordering, which continues to
// use only the declared `dependencies` field exactly as before. Widening
// the executor's scheduling authority to derived edges is explicitly out
// of this package's scope.

#include "ai/application/execution/ExecutionManifestTypes.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ai::application {

enum class ManifestDependencyEdgeOrigin {
    // The consumer task explicitly listed the producer task id in its own
    // `dependencies`.
    PlannerDeclared,
    // The consumer task's `requiredReadPaths` names a path the producer
    // task's `ownedPaths`/`plannedCreatePaths` claims - never Planner-
    // declared, always derived from already-validated role data.
    BackendDerived,
};

[[nodiscard]] const char* toString(ManifestDependencyEdgeOrigin origin) noexcept;

// One effective edge in the dependency graph: `consumerTaskId` depends on
// `producerTaskId`. `relativePath` is set only for a `BackendDerived` edge
// (the exact normalized path that created it) and is always `nullopt` for
// a `PlannerDeclared` edge, which carries no path of its own. A single
// (consumer, producer) task pair may appear more than once - once per
// distinct origin, and once per distinct `requiredReadPaths` entry that
// independently derives the same producer - so no real cause is silently
// collapsed away.
struct ManifestDependencyEdge {
    std::string consumerTaskId;
    std::string producerTaskId;
    ManifestDependencyEdgeOrigin origin {ManifestDependencyEdgeOrigin::PlannerDeclared};
    std::optional<std::string> relativePath;
};

// Deterministic and pure: depends only on the manifest's own task list, in
// manifest task order, then declared-edges-before-derived-edges, then
// requiredReadPaths order - never on wall-clock time, map iteration order,
// or any external state (no baseline access; existence-in-baseline is a
// separate concern already checked elsewhere). Unknown/self dependencies
// and unresolvable required-read paths are silently omitted here - each
// already has its own dedicated validator finding
// (`manifest.unknown_dependency`, `manifest.self_dependency`,
// `workspace_scope.read_path_missing_from_baseline`) and must not be
// double-reported by graph construction.
[[nodiscard]] std::vector<ManifestDependencyEdge> deriveManifestDependencyGraph(
    const ExecutionManifest& manifest);

} // namespace ai::application
