#pragma once

#include "ai/application/ProjectMemoryCompactionTypes.hpp"
#include "ai/common/Result.hpp"

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
};

} // namespace ai::application
