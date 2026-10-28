#pragma once

#include "ai/application/execution/ExecutionManifestTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>

namespace ai::application {

/** Immutable one-per-Run persistence port for accepted execution manifests. */
class IExecutionManifestRepository {
public:
    virtual ~IExecutionManifestRepository() = default;

    virtual common::Result<ExecutionManifest> insert(ExecutionManifest manifest) = 0;
    [[nodiscard]] virtual common::Result<std::optional<ExecutionManifest>> findForRun(
        const RunId& runId) const = 0;
};

} // namespace ai::application
