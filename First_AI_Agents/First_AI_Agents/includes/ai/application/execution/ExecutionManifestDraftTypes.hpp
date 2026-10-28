#pragma once

#include "ai/application/ExecutionAcceptanceContract.hpp"
#include "ai/application/execution/ExecutionManifestTypes.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// MW-P3A package 1 ("Typed Draft"): a smaller provider-facing shape the
// Planner actually submits, distinct from ExecutionManifest/
// ExecutionManifestTask (the Backend's own normalized domain object, used
// unchanged by every downstream component - validator, executor, read
// model, work ledger). ExecutionManifestDraftCompiler (package 2) turns a
// parsed draft plus the frozen WorkspaceBaselineSnapshot into a real
// ExecutionManifest; nothing here is consumed directly by execution.
//
// Motivation (ARCHITECTURE_GAPS.md's MW-P3A, Runs
// 3ffb52df-98ca-472f-9b1c-537ab134cf18 and a8afe6a0-7eb1-4c05-aaf3-46a1c5c90b6b,
// 2026-09-14): the prior submit_execution_manifest shape asked the provider
// to author fourteen fields per task, split write intent into two roles it
// cannot reliably classify against a baseline it cannot see in full
// (owned_write vs planned_create - a real Stage 2B Run declared an absent
// file owned_write), and required an `acceptanceContract` key whose *value*
// was schema-nullable but runtime-mandatory for inspect/propose tasks. This
// draft removes both defect classes by construction rather than by prose:
// - a task is either a ManifestDraftCoordinationTask (no paths, no
//   acceptance - literally cannot carry either) or a ManifestDraftWorkTask
//   (acceptanceContract is a real, non-optional ExecutionAcceptanceContract
//   member - there is no representable "null" state);
// - a work task declares one undifferentiated writePaths list; the Backend
//   compiler alone classifies each entry as owned_write/planned_create from
//   the real frozen baseline, so the provider can no longer misclassify an
//   absent file as already existing (or vice versa).
//
// Package 2 ("Read Semantics") adds a second structural fix, for Run
// a8afe6a0-7eb1-4c05-aaf3-46a1c5c90b6b: a mandatory read of the Run's
// immutable baseline is not the same as consuming another task's proposed
// output, so a work task now declares two typed read lists instead of one
// requiredReadPaths - see ManifestDraftWorkTask's own comment. The provider
// states the distinction explicitly; the Backend compiler never infers it
// from filenames, extensions, or prose.
namespace ai::application {

inline constexpr std::string_view kExecutionManifestDraftSchemaVersion =
    "execution-manifest-draft.v1";

// A pure coordination/dependency-anchor task: no writes, no reads, no
// acceptance criteria. Replaces the prior requestedCapability=="none" task
// shape, which was representable with paths/acceptance criteria attached
// (rejected only after parsing, as manifest.none_task_not_empty) - here an
// equivalent malformed submission cannot parse into this type at all, since
// ManifestDraftCoordinationTask has no path/acceptance fields to populate.
struct ManifestDraftCoordinationTask {
    std::string id;
    std::string objective;
    std::string role;
    std::vector<std::string> dependencies;
};

enum class ManifestDraftCapability {
    Inspect,
    Propose
};

[[nodiscard]] const char* toString(ManifestDraftCapability capability) noexcept;
[[nodiscard]] std::optional<ManifestDraftCapability> manifestDraftCapabilityFromString(
    std::string_view value) noexcept;

// A real unit of work. `writePaths` is intentionally a single list - see
// this file's header comment; the Backend compiler alone decides the final
// owned_write/planned_create split.
//
// `baselineRequiredReadPaths`: paths this task must inspect the Run's
// immutable frozen baseline version of - the content as it exists NOW,
// regardless of whether some other task also writes the same path later.
// The compiler never derives a cross-task dependency edge from these, even
// when the path is also another task's writePaths entry, and rejects an
// entry that does not actually exist in the frozen baseline
// (manifest.baseline_required_read_path_not_in_baseline).
//
// `proposedDependencyReadPaths`: paths this task must inspect specifically
// because another task in THIS SAME draft is producing them - a genuine
// ordering dependency. The compiler derives a real dependency edge from
// each entry and rejects one that does not name any other work task's
// writePaths entry (manifest.proposed_dependency_read_path_has_no_producer).
//
// Both lists remain same-attempt auditable required reads in the compiled
// manifest exactly like the legacy requiredReadPaths - the split only
// changes which entries may derive a dependency edge, never the read-gate
// enforcement itself.
//
// `referencePaths`/`excludedPaths`/`deferredPaths` keep their existing
// execution-manifest.v1 meaning unchanged (see ExecutionManifestTask's own
// field comments). `acceptanceContract` is non-optional by construction:
// the draft schema's work-task shape requires a real object, and the JSON
// wire schema for a work task no longer offers a null literal for this
// field at all.
struct ManifestDraftWorkTask {
    std::string id;
    std::string objective;
    std::string role;
    std::vector<std::string> dependencies;
    std::vector<std::filesystem::path> writePaths;
    std::vector<std::filesystem::path> baselineRequiredReadPaths;
    std::vector<std::filesystem::path> proposedDependencyReadPaths;
    std::vector<std::filesystem::path> referencePaths;
    std::vector<ManifestExcludedOrDeferredPath> excludedPaths;
    std::vector<ManifestExcludedOrDeferredPath> deferredPaths;
    std::vector<std::string> acceptanceCriteria;
    std::vector<std::string> contextReferences;
    ManifestDraftCapability requestedCapability {ManifestDraftCapability::Inspect};
    ExecutionAcceptanceContract acceptanceContract;
    // AG-17.4: the roster member (ManifestRoutingMember::memberId) that executes
    // this task. Authored by the provider ONLY in a roster Run, where the tool
    // schema requires it and restricts it to the Backend's assignable member ids;
    // a single-assignment Run never advertises it. The compiler copies it onto
    // ExecutionManifestTask::assigneeMemberId and the validator judges it against
    // the frozen routing record. A coordination task has no such field.
    std::optional<std::string> assigneeMemberId;
};

struct ExecutionManifestDraft {
    std::string schemaVersion {std::string(kExecutionManifestDraftSchemaVersion)};
    std::vector<ManifestDraftCoordinationTask> coordinationTasks;
    std::vector<ManifestDraftWorkTask> workTasks;
    std::uint32_t requestedMaxParallelism {1};
    std::optional<std::uint64_t> requestedTokenBudget;
    std::optional<std::uint64_t> requestedCostMicros;
    std::optional<ManifestBootstrapDeclaration> bootstrapDeclaration;
};

} // namespace ai::application
