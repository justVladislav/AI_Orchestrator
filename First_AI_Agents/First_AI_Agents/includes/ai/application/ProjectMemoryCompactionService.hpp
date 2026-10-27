#pragma once

#include "ai/application/IProjectMemoryCompactionRepository.hpp"
#include "ai/application/IProjectMemoryRepository.hpp"
#include "ai/application/IProjectRepository.hpp"
#include "ai/application/IProviderClient.hpp"
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
// Every call that reaches the provider produces exactly one durable
// ProjectMemoryCompactionRecord, regardless of whether the attempt completed,
// failed, or was cancelled - see ProjectMemoryCompactionTypes.hpp. compact()
// itself only returns common::Result::failure() for validation problems that
// precede the attempt (project/provider/model not found, fewer than two
// segments to compact) or for a persistence-layer failure while recording the
// outcome; a provider-side failure or cancellation is reported through the
// returned record's status, not through Result::failure().
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

    common::Result<ProjectMemoryCompactionRecord> compact(
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
