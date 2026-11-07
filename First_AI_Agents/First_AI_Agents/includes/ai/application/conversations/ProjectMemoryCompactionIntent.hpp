#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionTypes.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace ai::application {

// B3 phase I: the durable "compaction intent" - ONE row written right before the paid provider request of a memory compaction is
// issued, closed in the same transaction as the final compaction record, or marked unresolved by a restart that finds it still open.
//
// It is NOT a ledger entry, NOT a Pending record and NOT a retry queue: it holds no usage, no cost, no prompt, no answer and no HTTP
// detail. Its presence or absence says nothing about whether a request was delivered or what it cost (see the wording constants
// below and ProjectMemoryCompactionIntentPolicy.hpp).

// open        the operation has written its intent and has not (yet) recorded its final record.
// unresolved  a later start of the owning process found the intent still open and marked it; it is never deleted automatically.
enum class CompactionIntentState { Open, Unresolved };

[[nodiscard]] inline const char* toString(CompactionIntentState state) noexcept
{
    switch (state) {
    case CompactionIntentState::Open: return "open";
    case CompactionIntentState::Unresolved: return "unresolved";
    }
    return "open";
}

// The seven fields an intent shares, immutably, with the final compaction record of the same operation. Nothing else is compared
// between the two (status, result segment, usage, cost and error text exist only on the record).
enum class CompactionIntentIdentityField {
    Id,
    ProjectId,
    ProviderId,
    ModelId,
    SourceSegmentIds,   // the ordered list, duplicates included
    SourceContentHash,
    CreatedAt           // compared in milliseconds, exactly as it is stored
};

[[nodiscard]] inline const char* toString(CompactionIntentIdentityField field) noexcept
{
    switch (field) {
    case CompactionIntentIdentityField::Id: return "id";
    case CompactionIntentIdentityField::ProjectId: return "project_id";
    case CompactionIntentIdentityField::ProviderId: return "provider_id";
    case CompactionIntentIdentityField::ModelId: return "model_id";
    case CompactionIntentIdentityField::SourceSegmentIds: return "source_segment_ids";
    case CompactionIntentIdentityField::SourceContentHash: return "source_content_hash";
    case CompactionIntentIdentityField::CreatedAt: return "created_at";
    }
    return "id";
}

struct CompactionIntentIdentity {
    ProjectMemoryCompactionId id;
    ProjectId projectId;
    ProviderId providerId;
    ModelId modelId;
    std::vector<ProjectMemorySegmentId> sourceSegmentIds;
    std::string sourceContentHash;
    std::chrono::system_clock::time_point createdAt;
};

struct ProjectMemoryCompactionIntent {
    ProjectMemoryCompactionId id;
    ProjectId projectId;
    ProviderId providerId;
    ModelId modelId;
    std::vector<ProjectMemorySegmentId> sourceSegmentIds;
    std::string sourceContentHash;
    std::chrono::system_clock::time_point createdAt;

    CompactionIntentState state {CompactionIntentState::Open};
    // Set exactly when state == Unresolved. Written once; a repeated mark never rewrites it.
    std::optional<std::chrono::system_clock::time_point> unresolvedMarkedAt;
};

// Stable machine codes of this feature. Read and write failures that are not listed keep their persistence.* codes.
inline constexpr const char* kIntentNotRecordedCode = "project_memory.intent_not_recorded";
inline constexpr const char* kIntentDuplicateIdCode = "project_memory.intent_duplicate_id";
inline constexpr const char* kIntentIdentityConflictCode = "project_memory.intent_identity_conflict";
inline constexpr const char* kIntentRecordConflictCode = "project_memory.intent_record_conflict";
inline constexpr const char* kIntentObservationInconsistentCode = "project_memory.intent_observation_inconsistent";
inline constexpr const char* kIntentRecoveryForbiddenCode = "project_memory.intent_recovery_forbidden";
inline constexpr const char* kIntentInvalidCode = "project_memory.invalid_intent";

// Neutral English texts. The first states ONLY a control-flow fact of one call (its request was never issued); none of them says
// that a request was or was not delivered, that anything was billed, or that an answer can be recovered.
inline constexpr const char* kIntentNotRecordedMessage =
    "The compaction could not be started because its intent was not confirmed saved.";
inline constexpr const char* kIntentDuplicateIdMessage =
    "The compaction operation ID is already in use. No request was sent by this attempt.";
inline constexpr const char* kIntentUnresolvedMessage =
    "An earlier compaction did not complete its recording protocol. Delivery and cost are unconfirmed; the request may not have started.";

} // namespace ai::application
