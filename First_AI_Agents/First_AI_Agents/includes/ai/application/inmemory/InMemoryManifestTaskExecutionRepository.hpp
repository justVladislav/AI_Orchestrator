#pragma once

#include "ai/application/execution/IManifestTaskExecutionRepository.hpp"

#include <map>
#include <mutex>

namespace ai::application {

class InMemoryManifestTaskExecutionRepository final
    : public IManifestTaskExecutionRepository {
public:
    common::Result<void> upsert(
        const RunId& runId, const std::string& taskId, const ManifestTaskState& state) override;
    [[nodiscard]] common::Result<std::vector<ManifestTaskExecutionRecord>> listForRun(
        const RunId& runId) const override;

    common::Result<void> upsertNoChanges(
        const RunId& runId, const std::string& taskId, const ManifestWorkerNoChanges& evidence) override;
    [[nodiscard]] common::Result<std::vector<ManifestTaskNoChangesRecord>> listNoChangesForRun(
        const RunId& runId) const override;

private:
    mutable std::mutex mutex_;
    // Ordered by (runId, taskId) so listForRun returns a stable, deterministic
    // order for tests without needing a separate insertion-order tracker.
    std::map<std::pair<RunId, std::string>, ManifestTaskExecutionRecord> records_;
    std::map<std::pair<RunId, std::string>, ManifestTaskNoChangesRecord> noChanges_;
};

} // namespace ai::application
