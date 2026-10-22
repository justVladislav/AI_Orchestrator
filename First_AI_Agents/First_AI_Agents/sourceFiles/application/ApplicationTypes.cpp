#include "ai/application/ApplicationTypes.hpp"

namespace ai::application {

// Centralized string conversion prevents UI/storage adapters from inventing
// incompatible spellings for the same lifecycle state.
const char* toString(ProjectStatus status) noexcept
{
    switch (status) {
    case ProjectStatus::Active: return "active";
    case ProjectStatus::Archived: return "archived";
    case ProjectStatus::Unavailable: return "unavailable";
    }
    return "unknown";
}

const char* toString(RunStatus status) noexcept
{
    switch (status) {
    case RunStatus::Prepared: return "prepared";
    case RunStatus::Queued: return "queued";
    case RunStatus::Running: return "running";
    case RunStatus::Cancelling: return "cancelling";
    case RunStatus::Completed: return "completed";
    case RunStatus::Failed: return "failed";
    case RunStatus::Cancelled: return "cancelled";
    case RunStatus::Interrupted: return "interrupted";
    }
    return "unknown";
}

const char* toString(PlanningCompletionPolicy policy) noexcept
{
    switch (policy) {
    case PlanningCompletionPolicy::RequireManifest: return "require_manifest";
    case PlanningCompletionPolicy::AllowEvidenceBackedNoChanges:
        return "allow_evidence_backed_no_changes";
    }
    return "require_manifest";
}

std::optional<PlanningCompletionPolicy> planningCompletionPolicyFromString(
    std::string_view value) noexcept
{
    if (value == "require_manifest")
        return PlanningCompletionPolicy::RequireManifest;
    if (value == "allow_evidence_backed_no_changes")
        return PlanningCompletionPolicy::AllowEvidenceBackedNoChanges;
    return std::nullopt;
}

const char* toString(VerificationRepairPolicy policy) noexcept
{
    switch (policy) {
    case VerificationRepairPolicy::OneBoundedWave: return "one_bounded_wave";
    case VerificationRepairPolicy::Disabled: return "disabled";
    }
    return "one_bounded_wave";
}

std::optional<VerificationRepairPolicy> verificationRepairPolicyFromString(
    std::string_view value) noexcept
{
    if (value == "one_bounded_wave")
        return VerificationRepairPolicy::OneBoundedWave;
    if (value == "disabled")
        return VerificationRepairPolicy::Disabled;
    return std::nullopt;
}

const char* toString(StageStatus status) noexcept
{
    switch (status) {
    case StageStatus::Pending: return "pending";
    case StageStatus::Running: return "running";
    case StageStatus::Completed: return "completed";
    case StageStatus::Failed: return "failed";
    case StageStatus::Skipped: return "skipped";
    case StageStatus::Cancelled: return "cancelled";
    }
    return "unknown";
}

const char* toString(TaskStatus status) noexcept
{
    switch (status) {
    case TaskStatus::Backlog: return "backlog";
    case TaskStatus::Planned: return "planned";
    case TaskStatus::InProgress: return "in_progress";
    case TaskStatus::Blocked: return "blocked";
    case TaskStatus::Done: return "done";
    case TaskStatus::Cancelled: return "cancelled";
    }
    return "backlog";
}

const char* toString(TaskPriority priority) noexcept
{
    switch (priority) {
    case TaskPriority::Low: return "low";
    case TaskPriority::Normal: return "normal";
    case TaskPriority::High: return "high";
    case TaskPriority::Critical: return "critical";
    }
    return "normal";
}

const char* toString(RunLaunchMode mode) noexcept
{
    switch (mode) {
    case RunLaunchMode::Mock: return "mock";
    case RunLaunchMode::SavedExecutionProfile: return "saved_execution_profile";
    case RunLaunchMode::CustomRoster: return "custom_roster";
    }
    return "mock";
}

bool isTerminal(RunStatus status) noexcept
{
    return status == RunStatus::Completed
        || status == RunStatus::Failed
        || status == RunStatus::Cancelled
        || status == RunStatus::Interrupted;
}

} // namespace ai::application
