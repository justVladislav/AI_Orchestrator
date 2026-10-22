#pragma once

#include "ai/application/ApplicationTypes.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ai::application {

using RunDraftId = std::string;
using RunDraftGroupId = std::string;

// PC-3B: drafts created together from one `next-run-plan.v1` plan form a group. A group and the provenance of its
// drafts are written once, in one transaction, and never change afterwards (see IRunDraftRepository::insertGroup).
// The most drafts a group holds; the same number as the plan limit (kNextRunPlanMaxDrafts).
inline constexpr std::size_t kRunDraftGroupMaxDrafts = 8;
inline constexpr std::size_t kRunDraftGroupMaxSourceRuns = 16;

// Where the plan came from. The idempotency key of a group is derived from this (see CODEX_CONTEXT "PC-3 design").
enum class RunDraftGroupOrigin {
    ChatMessage,     // an Assistant message of a conversation, with its content fingerprint
    ManualImport,    // a manual paste, previewed and confirmed with an opaque token
    FollowUpRequest, // PC-5: the stored plan of a paid follow-up composer request, saved by its atomic Confirm
};

// One immutable row per group. A group exists only while it has at least one draft: removing the last draft removes
// the group in the same transaction, so a repeated import of the same plan creates a fresh group.
struct RunDraftGroup {
    RunDraftGroupId id;
    ProjectId projectId;
    RunDraftGroupOrigin origin {RunDraftGroupOrigin::ManualImport};
    // sha256 hex, unique. Message path: (project, message, message content, plan); manual paste: the preview token.
    std::string idempotencyKey;
    std::string planTitle;
    std::string planFingerprint;   // NextRunPlan::fingerprint
    // ChatMessage origin only (all three present), absent for a manual import. Opaque ids: a deleted conversation
    // or message never removes the group.
    std::optional<std::string> sourceConversationId;
    std::optional<std::string> sourceMessageId;
    std::optional<std::string> sourceMessageFingerprint;
    // FollowUpRequest origin only: the id of the follow-up plan request this group was saved from (unique: one
    // request saves at most one group). An opaque id like the message ids above.
    std::optional<std::string> sourceRequestId;
    // Only for a snapshot that was really attached to the source Chat request or composer request (never built at
    // import time): the chat context fingerprint, or the sealed follow-up context fingerprint.
    std::optional<std::string> contextFingerprint;
    // Runs the user named as the source of the plan; chosen in the UI and checked against the project, never
    // taken from the plan.
    std::vector<RunId> sourceRunIds;
    std::chrono::system_clock::time_point createdAt;
};

// What makes a draft part of a group. Absent on a manual draft. Set once by insertGroup and never changed:
// updateDraft refuses a different value instead of ignoring it.
struct RunDraftProvenance {
    RunDraftGroupId groupId;
    std::uint32_t ordinal {0};                 // 0-based position in the group
    std::string planKey;                       // the handle other drafts of the plan depend on
    std::vector<std::string> dependsOn;        // plan keys of drafts of the same group; advisory only
    // nextRunPlanDraftContentFingerprint of the draft as created: the "edited since creation" indicator recomputes
    // it from the current draft and compares.
    std::string createdContentFingerprint;

    [[nodiscard]] bool operator==(const RunDraftProvenance&) const = default;
};

/**
 * A saved, not-yet-submitted New Run configuration: project, objective text,
 * launch mode and (for CustomRoster/SavedExecutionProfile) the full roster
 * the user was editing, persisted so it can be reopened and resubmitted or
 * further edited later without losing work.
 *
 * Unlike ExecutionProfile - whose assignments are deliberately restricted to
 * the fixed architect/implementation/documentation stage triple (see
 * ExecutionProfileTypes.hpp) - a RunDraft's roster carries the full,
 * arbitrary-length StageModelAssignment shape a real Run's assignmentSnapshot
 * uses (agentName/role/instructions/dependencies/workspaceAccess/etc.),
 * because it must be able to round-trip anything NewRunDialog's custom
 * roster editor can produce.
 *
 * Reopening a draft never deletes it (only an explicit remove() does), so a
 * frequently-reused draft doubles as a lightweight, project/task-specific
 * template - a deliberate simplification instead of extending the narrow
 * ExecutionProfile schema to carry rich per-agent metadata.
 */
struct RunDraft {
    RunDraftId id;
    std::string name;
    ProjectId projectId;
    std::string title;
    std::string task;
    std::string workflowId;
    RunLaunchMode launchMode {RunLaunchMode::Mock};
    std::optional<ExecutionProfileId> executionProfileId;
    std::vector<StageModelAssignment> roster;
    std::uint32_t maxParallelism {4};
    std::optional<std::filesystem::path> workspacePath;
    VerificationRepairPolicy verificationRepairPolicy {
        VerificationRepairPolicy::OneBoundedWave};
    // Explicit per-Run provider limits (custom roster only); empty = the Backend defaults.
    ProviderExecutionBudgetOverrides providerBudget;
    // PC-2: saved with the draft so Edit & rerun and a reopened draft keep the authoritative contracts.
    std::vector<RunInterfaceContract> interfaceContracts;
    // PC-3B: set only for a draft created from a plan (IRunDraftRepository::insertGroup); a manual draft has none.
    std::optional<RunDraftProvenance> provenance;
    std::chrono::system_clock::time_point createdAt;
    std::chrono::system_clock::time_point updatedAt;
};

struct CreateRunDraftRequest {
    std::string name;
    ProjectId projectId;
    std::string title;
    std::string task;
    std::string workflowId;
    RunLaunchMode launchMode {RunLaunchMode::Mock};
    std::optional<ExecutionProfileId> executionProfileId;
    std::vector<StageModelAssignment> roster;
    std::uint32_t maxParallelism {4};
    std::optional<std::filesystem::path> workspacePath;
    VerificationRepairPolicy verificationRepairPolicy {
        VerificationRepairPolicy::OneBoundedWave};
    ProviderExecutionBudgetOverrides providerBudget;
    std::vector<RunInterfaceContract> interfaceContracts;
};

struct UpdateRunDraftRequest {
    RunDraftId id;
    std::string name;
    ProjectId projectId;
    std::string title;
    std::string task;
    std::string workflowId;
    RunLaunchMode launchMode {RunLaunchMode::Mock};
    std::optional<ExecutionProfileId> executionProfileId;
    std::vector<StageModelAssignment> roster;
    std::uint32_t maxParallelism {4};
    std::optional<std::filesystem::path> workspacePath;
    VerificationRepairPolicy verificationRepairPolicy {
        VerificationRepairPolicy::OneBoundedWave};
    ProviderExecutionBudgetOverrides providerBudget;
    std::vector<RunInterfaceContract> interfaceContracts;
};

} // namespace ai::application
