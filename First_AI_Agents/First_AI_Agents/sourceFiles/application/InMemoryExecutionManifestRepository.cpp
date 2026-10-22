#include "ai/application/InMemoryExecutionManifestRepository.hpp"

#include <utility>

namespace ai::application {

common::Result<ExecutionManifest> InMemoryExecutionManifestRepository::insert(
    ExecutionManifest manifest)
{
    std::lock_guard lock(mutex_);
    if (manifests_.contains(manifest.runId)) {
        return common::Result<ExecutionManifest>::failure(
            {{"manifest.already_frozen"}, {"Execution manifest is already frozen for this Run"}});
    }
    const RunId runId = manifest.runId;
    manifests_.emplace(runId, manifest);
    return common::Result<ExecutionManifest>::ok(std::move(manifest));
}

common::Result<std::optional<ExecutionManifest>>
InMemoryExecutionManifestRepository::findForRun(const RunId& runId) const
{
    std::lock_guard lock(mutex_);
    const auto found = manifests_.find(runId);
    if (found == manifests_.end()) {
        return common::Result<std::optional<ExecutionManifest>>::ok(std::nullopt);
    }
    return common::Result<std::optional<ExecutionManifest>>::ok(found->second);
}

} // namespace ai::application
