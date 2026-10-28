#pragma once

#include "ai/application/ApplicationTypes.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

inline constexpr std::string_view kRunWorkspaceScopeSchemaVersion =
    "run-workspace-scope.v1";

// AG-15: bounded caps, matching the same "Backend-authored context is
// always bounded" convention as ExecutionManifestPolicy/ManifestWorkLedger.
inline constexpr std::size_t kRunWorkspaceScopeMaxEntries = 4096;
inline constexpr std::size_t kRunWorkspaceScopeMaxDependencyEdges = 4096;
inline constexpr std::size_t kRunWorkspaceScopeMaxReasonBytes = 512;

// One active file's role within a frozen Run Workspace Scope. Unlisted paths
// are inactive by default - see ARCHITECTURE_GAPS.md's AG-15.
enum class WorkspaceScopeRole {
    OwnedWrite,
    PlannedCreate,
    RequiredRead,
    ReferenceOnly,
    Excluded,
    Deferred,
};

[[nodiscard]] const char* toString(WorkspaceScopeRole value) noexcept;
[[nodiscard]] std::optional<WorkspaceScopeRole> workspaceScopeRoleFromString(
    std::string_view value) noexcept;

// Whether an entry was part of the scope frozen immediately after the
// Manifest was accepted, or was added later via an audited
// request_scope_expansion. FrozenInitial entries are immutable; an
// ApprovedExpansion entry lives in a separate append-only table and never
// mutates the frozen row - see IRunWorkspaceScopeRepository.
enum class WorkspaceScopeOrigin {
    FrozenInitial,
    ApprovedExpansion,
};

[[nodiscard]] const char* toString(WorkspaceScopeOrigin value) noexcept;
[[nodiscard]] std::optional<WorkspaceScopeOrigin> workspaceScopeOriginFromString(
    std::string_view value) noexcept;

// A Backend-derived file-level dependency edge: `dependentTaskId` declared
// `relativePath` as a required_read, and `providerTaskId` is the task whose
// owned_write/planned_create claims that same path. Never Planner-declared -
// always derived from already-validated role data, so it can never
// contradict the roles it is derived from and always references known scope
// entries/tasks by construction.
struct WorkspaceScopeDependencyEdge {
    std::string dependentTaskId;
    std::string providerTaskId;
    std::string relativePath;
};

// One row of the frozen scope. `reason` is Backend-synthesized and
// deterministic (e.g. "Declared as required_read by task <id>") - never
// free provider text, so it stays stable and testable; the one workspace-
// scope field that IS genuinely worker/planner-authored free text is
// RunWorkspaceScopeExpansion::reason below, which is only ever an audit
// annotation, never an authorization input.
struct RunWorkspaceScopeEntry {
    std::string relativePath;
    WorkspaceScopeRole role {WorkspaceScopeRole::ReferenceOnly};
    std::string reason;
    std::string taskId;
    // Present iff relativePath existed in the captured baseline at freeze
    // time - the real WorkspaceBaselineSnapshot::fingerprint-family hash,
    // never re-derived or provider-supplied.
    std::optional<std::string> baselineHash;
    WorkspaceScopeOrigin origin {WorkspaceScopeOrigin::FrozenInitial};
};

// The immutable, versioned, per-Run frozen workspace scope. Written exactly
// once (RunWorkspaceScopeService::freezeForManifest), immediately after the
// owning ExecutionManifest is itself frozen - see
// IRunWorkspaceScopeRepository::freeze's "insert once, never replace"
// contract.
struct RunWorkspaceScope {
    std::string schemaVersion {std::string(kRunWorkspaceScopeSchemaVersion)};
    RunId runId;
    std::string manifestSchemaVersion;
    std::vector<RunWorkspaceScopeEntry> entries;
    std::vector<WorkspaceScopeDependencyEdge> dependencyEdges;
    std::chrono::system_clock::time_point frozenAt;
};

// One durable, append-only audited scope-expansion record. Never mutates
// the frozen RunWorkspaceScope; only ever widens the *effective* scope a
// task's worker attempt is allowed to read (role is always RequiredRead or
// ReferenceOnly - an expansion can never grant a new write target). `reason`
// here is genuinely worker-authored bounded free text explaining the
// discovered dependency - an audit annotation only, never itself an
// authorization input (the actual gate is the safe-path/deny-list/limit
// checks RunWorkspaceScopeService::requestExpansion performs before this
// record is ever persisted).
struct RunWorkspaceScopeExpansion {
    std::string id;
    RunId runId;
    std::string taskId;
    std::string relativePath;
    WorkspaceScopeRole role {WorkspaceScopeRole::RequiredRead};
    std::string reason;
    std::optional<std::string> baselineHash;
    std::chrono::system_clock::time_point approvedAt;
};

inline constexpr std::size_t kRunWorkspaceScopeMaxFindingCodeBytes = 128;

// Audited diagnostic read scope (verification-repair reliability
// hardening): one durable, append-only, read-only grant letting the ONE
// bounded combined-verification repair attempt inspect an existing baseline
// file it needs to resolve a concrete verification finding - most notably a
// file the Planner froze as WorkspaceScopeRole::Excluded, which
// RunWorkspaceScopeService::requestExpansion (the ordinary worker-facing
// mechanism above) always refuses outright.
//
// Deliberately a SEPARATE record type from RunWorkspaceScopeExpansion, never
// folded into it or into the frozen RunWorkspaceScope's own entries:
//   - it is granted only during the bounded verification-repair attempt,
//     never during ordinary primary dispatch (see
//     AgentToolTurnLoopRequest::diagnosticReadEligible);
//   - unlike an expansion, it may target a path frozen Excluded - the
//     Planner's original excludedPaths entry is never mutated, replaced, or
//     silently reinterpreted as required_read/reference_only; this grant is
//     the explicit, durable exception, kept structurally distinct precisely
//     so the frozen scope's own record of "the Planner excluded this" stays
//     intact and auditable;
//   - it can never carry OwnedWrite/PlannedCreate (there is no role field at
//     all - a diagnostic read is always read-only, by construction, never a
//     write grant of any kind);
//   - it still passes through the SAME authoritative WorkspaceReadPolicy
//     deny-list/security check an ordinary expansion does - see
//     RunWorkspaceScopeService::requestDiagnosticRead's own doc comment for
//     exactly how a Backend security denial (workspace_scope.
//     diagnostic_read_policy_denied) is kept distinct from the Planner's own
//     task-level Excluded classification (which this mechanism may
//     override, precisely because it is not a security boundary).
//
// `findingCode` is genuinely worker-authored bounded free text identifying
// which verification finding this read is meant to resolve (e.g.
// "verification.qml_warning") - like RunWorkspaceScopeExpansion::reason,
// this is an audit annotation only, never itself an authorization input;
// requestDiagnosticRead never cross-validates it against the Run's actual
// verification findings (doing so would require parsing/inferring
// diagnostic-to-file relationships, which this package deliberately does
// not attempt - see requestDiagnosticRead's own doc comment).
struct RunWorkspaceScopeDiagnosticRead {
    std::string id;
    RunId runId;
    std::string taskId;
    std::string relativePath;
    std::string findingCode;
    std::string reason;
    std::optional<std::string> baselineHash;
    std::chrono::system_clock::time_point grantedAt;
};

} // namespace ai::application
