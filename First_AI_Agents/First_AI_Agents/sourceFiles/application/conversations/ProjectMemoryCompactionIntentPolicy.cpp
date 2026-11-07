#include "ai/application/conversations/ProjectMemoryCompactionIntentPolicy.hpp"

#include <chrono>
#include <cstdint>

namespace ai::application {

namespace {

std::int64_t wholeMilliseconds(std::chrono::system_clock::time_point value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

} // namespace

CompactionIntentIdentity compactionIntentIdentityOf(const ProjectMemoryCompactionIntent& intent)
{
    return CompactionIntentIdentity {intent.id, intent.projectId, intent.providerId, intent.modelId, intent.sourceSegmentIds,
        intent.sourceContentHash, intent.createdAt};
}

CompactionIntentIdentity compactionIntentIdentityOf(const ProjectMemoryCompactionRecord& record)
{
    return CompactionIntentIdentity {record.id, record.projectId, record.providerId, record.modelId, record.sourceSegmentIds,
        record.sourceContentHash, record.createdAt};
}

std::optional<CompactionIntentIdentityField> compactionIntentIdentityDifference(
    const CompactionIntentIdentity& expected, const CompactionIntentIdentity& actual)
{
    if (expected.id != actual.id) return CompactionIntentIdentityField::Id;
    if (expected.projectId != actual.projectId) return CompactionIntentIdentityField::ProjectId;
    if (expected.providerId != actual.providerId) return CompactionIntentIdentityField::ProviderId;
    if (expected.modelId != actual.modelId) return CompactionIntentIdentityField::ModelId;
    if (expected.sourceSegmentIds != actual.sourceSegmentIds) return CompactionIntentIdentityField::SourceSegmentIds;
    if (expected.sourceContentHash != actual.sourceContentHash) return CompactionIntentIdentityField::SourceContentHash;
    if (wholeMilliseconds(expected.createdAt) != wholeMilliseconds(actual.createdAt)) return CompactionIntentIdentityField::CreatedAt;
    return std::nullopt;
}

std::optional<CompactionIntentIdentityField> compactionIntentIdentityDifference(
    const ProjectMemoryCompactionIntent& intent, const ProjectMemoryCompactionRecord& record)
{
    return compactionIntentIdentityDifference(compactionIntentIdentityOf(intent), compactionIntentIdentityOf(record));
}

common::Result<void> validateNewCompactionIntent(const ProjectMemoryCompactionIntent& intent)
{
    if (intent.id.empty() || intent.projectId.empty()) {
        return common::Result<void>::failure({kIntentInvalidCode, "A compaction intent needs an operation id and a project id"});
    }
    if (intent.state != CompactionIntentState::Open || intent.unresolvedMarkedAt.has_value()) {
        return common::Result<void>::failure({kIntentInvalidCode, "A new compaction intent must be open and unmarked"});
    }
    return common::Result<void>::ok();
}

bool mayDispatch(const CompactionIntentWriteOutcome& outcome) noexcept
{
    return outcome.newIntentInserted && outcome.persist.result.success() && outcome.persist.state == CompactionPersistState::Committed;
}

CompactionIntentAdmission compactionIntentAdmissionOf(const CompactionIntentWriteOutcome& outcome) noexcept
{
    return mayDispatch(outcome) ? CompactionIntentAdmission::Admitted : CompactionIntentAdmission::NotAdmitted;
}

CompactionIntentObservation classifyCompactionOperation(const CompactionIntentIdentity& expected,
    const std::optional<ProjectMemoryCompactionIntent>& intent, const std::optional<CompactionIntentIdentity>& record)
{
    if (intent.has_value() && record.has_value()) return CompactionIntentObservation::Inconsistent;
    if (intent.has_value()) {
        return compactionIntentIdentityDifference(expected, compactionIntentIdentityOf(*intent)).has_value()
            ? CompactionIntentObservation::Inconsistent
            : CompactionIntentObservation::IntentPresent;
    }
    if (record.has_value()) {
        return compactionIntentIdentityDifference(expected, *record).has_value()
            ? CompactionIntentObservation::Inconsistent
            : CompactionIntentObservation::FinalRecordPresent;
    }
    return CompactionIntentObservation::NeitherPresent;
}

} // namespace ai::application
