#pragma once

#include "ai/application/ExecutionProfileTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <vector>

namespace ai::application {

/**
 * Persistence port for execution profiles and their complete ordered
 * assignment list. insert/update must commit the profile and every
 * assignment atomically (all-or-nothing), the same aggregate guarantee
 * IRunRepository::insertRunWithStages already provides for Run+RunStage.
 */
class IExecutionProfileRepository {
public:
    virtual ~IExecutionProfileRepository() = default;

    virtual common::Result<ExecutionProfile> insert(ExecutionProfile profile) = 0;
    virtual common::Result<ExecutionProfile> update(ExecutionProfile profile) = 0;
    [[nodiscard]] virtual common::Result<std::optional<ExecutionProfile>> findById(
        const ExecutionProfileId& id) const = 0;
    // Deterministic order (createdAt, then id).
    [[nodiscard]] virtual common::Result<std::vector<ExecutionProfile>> listAll() const = 0;

    // Atomically clears any previously-default profile and marks profileId
    // as the new default, or leaves both untouched on failure - the same
    // single-default guarantee IModelRepository::setDefaultModel provides.
    virtual common::Result<void> setDefaultProfile(const ExecutionProfileId& profileId) = 0;
};

} // namespace ai::application
