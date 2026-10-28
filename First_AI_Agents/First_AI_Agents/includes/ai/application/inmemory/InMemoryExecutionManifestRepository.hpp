#pragma once

#include "ai/application/execution/IExecutionManifestRepository.hpp"

#include <mutex>
#include <unordered_map>

namespace ai::application {

class InMemoryExecutionManifestRepository final
    : public IExecutionManifestRepository {
public:
    common::Result<ExecutionManifest> insert(ExecutionManifest manifest) override;
    [[nodiscard]] common::Result<std::optional<ExecutionManifest>> findForRun(
        const RunId& runId) const override;

private:
    mutable std::mutex mutex_;
    std::unordered_map<RunId, ExecutionManifest> manifests_;
};

} // namespace ai::application
