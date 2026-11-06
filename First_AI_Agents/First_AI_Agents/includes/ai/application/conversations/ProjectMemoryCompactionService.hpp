#pragma once

#include "ai/application/conversations/IProjectMemoryCompactionRepository.hpp"
#include "ai/application/conversations/IProjectMemoryRepository.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionOutcome.hpp"
#include "ai/application/projects/IProjectRepository.hpp"
#include "ai/application/providers/IProviderClient.hpp"
#include "ai/application/catalog/IProviderRepository.hpp"
#include "ai/application/catalog/IModelRepository.hpp"

#include <cstddef>
#include <stop_token>

namespace ai::application {

// AG-11: provider-driven summarization of a project's accumulated memory
// segments into one consolidated segment, freeing an operator from an
// unbounded, ever-growing RunContextAssembler input. Unlike ProjectMemoryService
// (deterministic, LLM-free, one segment per completed Run), this service makes
// exactly one real provider call per compact() invocation and is never called
// automatically - an operator (or a future policy) decides when a project's
// memory is worth compacting.
//
// Every call that reaches the provider TRIES to write exactly one
// ProjectMemoryCompactionRecord - completed, failed or cancelled, see
// ProjectMemoryCompactionTypes.hpp - and the record is durable when the write
// succeeds. Storage can fail, and then no row is guaranteed. compact() only
// returns common::Result::failure() for validation problems that precede the
// attempt (project/provider/model not found, fewer than two segments to
// compact) or for a persistence-layer failure while recording the outcome; a
// provider-side failure or cancellation is reported through the returned
// record's status, not through Result::failure().
//
// When the atomic commit of a COMPLETED compaction fails after the provider's
// response was received (so the usage was already charged), the service may
// write ONE Failed accounting row with the same id and the same usage/amount/
// flag - only when the storage proves that the commit left nothing behind and
// the connection is explicitly healthy. That never turns the failure into a
// success: compact() still fails with the original error. What happened to the
// usage (Saved / NotSaved / Unknown / NotApplicable) is reported by
// compactObserved(), together with the storage health, the native codes and the
// secondary causes. See ProjectMemoryCompactionOutcome.hpp.
class ProjectMemoryCompactionService final {
public:
    static constexpr std::size_t kMaxSourceSegments = 256;
    static constexpr std::size_t kMaxSourceBytes = 512 * 1024;
    static constexpr std::size_t kMaxSummaryBytes = 256 * 1024;

    ProjectMemoryCompactionService(
        IProjectMemoryRepository& memoryRepository,
        IProjectMemoryCompactionRepository& compactionRepository,
        IProjectRepository& projects,
        IProviderRepository& providers,
        IModelRepository& models,
        IProviderClientFactory& clients);

    // A thin view of compactObserved(): exactly the .result of ONE call of it (one provider request at most, never two).
    common::Result<ProjectMemoryCompactionRecord> compact(
        const ProjectId& projectId,
        const ProviderId& providerId,
        const ModelId& modelId,
        std::stop_token stopToken);

    // The operation itself: the same call as compact(), reporting what happened to the received usage and to the storage.
    ProjectMemoryCompactionOutcome compactObserved(
        const ProjectId& projectId,
        const ProviderId& providerId,
        const ModelId& modelId,
        std::stop_token stopToken);

    [[nodiscard]] common::Result<std::vector<ProjectMemoryCompactionRecord>> listCompactionRecords(
        const ProjectId& projectId) const;

private:
    IProjectMemoryRepository& memoryRepository_;
    IProjectMemoryCompactionRepository& compactionRepository_;
    IProjectRepository& projects_;
    IProviderRepository& providers_;
    IModelRepository& models_;
    IProviderClientFactory& clients_;
};

} // namespace ai::application
