#pragma once

#include "ai/application/conversations/ProjectMemoryCompactionPersistence.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <utility>
#include <vector>

namespace ai::application {

// Persistence port for AG-11 compaction provenance. Records are immutable
// once inserted - there is no update method. Deliberately separate from
// IProjectMemoryRepository (which owns the segments themselves): a
// compaction attempt is an audit fact about *how* segments changed, not a
// segment itself, so a failed/cancelled attempt can be recorded without a
// segment ever being touched.
class IProjectMemoryCompactionRepository {
public:
    virtual ~IProjectMemoryCompactionRepository() = default;

    // Failed/Cancelled attempts only. Completed records must use the atomic
    // commit below so durable provenance can never get ahead of memory.
    virtual common::Result<void> insertCompactionRecord(
        const ProjectMemoryCompactionRecord& record) = 0;

    // The only successful-compaction commit point. Implementations must
    // atomically verify that every source id still belongs to record.projectId,
    // remove exactly those rows, insert resultSegment, and insert record. A
    // stale/concurrent source set must fail closed without changing either
    // memory or provenance.
    virtual common::Result<void> commitSuccessfulCompaction(
        const ProjectMemoryCompactionRecord& record,
        ProjectMemorySegment resultSegment) = 0;

    // Ordered by createdAt ascending.
    [[nodiscard]] virtual common::Result<std::vector<ProjectMemoryCompactionRecord>>
    listCompactionRecords(const ProjectId& projectId) const = 0;

    // C2-B/S2b: the same writes as above, reporting WHAT HAPPENED to them (ProjectMemoryCompactionPersistence.hpp), so that a
    // caller can tell "proven not stored" from "stored" from "not proven" instead of reading a failure string. NON-PURE: an
    // implementation that does not override them keeps its behaviour, and the default is deliberately conservative - exactly ONE call
    // of the old method; success = Committed; any failure = Unknown (an implementation that cannot prove a rollback must not claim
    // one); the connection is not observed (nullopt, never "healthy").
    virtual CompactionPersistOutcome commitSuccessfulCompactionObserved(
        const ProjectMemoryCompactionRecord& record,
        ProjectMemorySegment resultSegment)
    {
        CompactionPersistOutcome outcome;
        outcome.result = commitSuccessfulCompaction(record, std::move(resultSegment));
        outcome.state = outcome.result.success() ? CompactionPersistState::Committed : CompactionPersistState::Unknown;
        return outcome;
    }

    virtual CompactionPersistOutcome insertCompactionRecordObserved(const ProjectMemoryCompactionRecord& record)
    {
        CompactionPersistOutcome outcome;
        outcome.result = insertCompactionRecord(record);
        outcome.state = outcome.result.success() ? CompactionPersistState::Committed : CompactionPersistState::Unknown;
        return outcome;
    }

    // One stored record by its GLOBAL id (the primary key), every stored field. Success with no value = no such row. The default
    // is "not supported", never an empty success: a caller must not read "unsupported" as "absent".
    [[nodiscard]] virtual common::Result<std::optional<ProjectMemoryCompactionRecord>> findCompactionRecord(
        const ProjectMemoryCompactionId&) const
    {
        return common::Result<std::optional<ProjectMemoryCompactionRecord>>::failure(
            {"project_memory.lookup_unsupported", "This compaction repository cannot look a record up by id"});
    }
};

} // namespace ai::application
