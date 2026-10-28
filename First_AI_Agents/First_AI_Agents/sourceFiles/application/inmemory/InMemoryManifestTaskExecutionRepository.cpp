#include "ai/application/inmemory/InMemoryManifestTaskExecutionRepository.hpp"

#include <algorithm>
#include <chrono>

namespace ai::application {

common::Result<void> InMemoryManifestTaskExecutionRepository::upsert(
    const RunId& runId, const std::string& taskId, const ManifestTaskState& state)
{
    std::lock_guard lock(mutex_);
    ManifestTaskExecutionRecord record;
    record.runId = runId;
    record.taskId = taskId;
    record.state = state;
    record.updatedAt = std::chrono::system_clock::now();
    records_[{runId, taskId}] = std::move(record);
    return common::Result<void>::ok();
}

common::Result<std::vector<ManifestTaskExecutionRecord>>
InMemoryManifestTaskExecutionRepository::listForRun(const RunId& runId) const
{
    std::lock_guard lock(mutex_);
    std::vector<ManifestTaskExecutionRecord> results;
    for (const auto& [key, record] : records_) {
        if (key.first == runId) results.push_back(record);
    }
    return common::Result<std::vector<ManifestTaskExecutionRecord>>::ok(std::move(results));
}

common::Result<void> InMemoryManifestTaskExecutionRepository::upsertNoChanges(
    const RunId& runId, const std::string& taskId, const ManifestWorkerNoChanges& evidence)
{
    std::lock_guard lock(mutex_);
    ManifestTaskNoChangesRecord record;
    record.runId = runId;
    record.taskId = taskId;
    record.evidence = evidence;
    record.updatedAt = std::chrono::system_clock::now();
    noChanges_[{runId, taskId}] = std::move(record);
    return common::Result<void>::ok();
}

common::Result<std::vector<ManifestTaskNoChangesRecord>>
InMemoryManifestTaskExecutionRepository::listNoChangesForRun(const RunId& runId) const
{
    std::lock_guard lock(mutex_);
    std::vector<ManifestTaskNoChangesRecord> results;
    for (const auto& [key, record] : noChanges_) {
        if (key.first == runId) results.push_back(record);
    }
    return common::Result<std::vector<ManifestTaskNoChangesRecord>>::ok(std::move(results));
}

} // namespace ai::application
