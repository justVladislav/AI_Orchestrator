#include "ai/application/inmemory/InMemoryVerificationEnvironmentRepository.hpp"

namespace ai::application {

common::Result<void> InMemoryVerificationEnvironmentRepository::recordSnapshot(
    VerificationEnvironmentSnapshot snapshot)
{
    std::lock_guard lock(mutex_);
    const RunId runId = snapshot.runId;
    if (snapshots_.contains(runId)) {
        return common::Result<void>::failure({"verification_environment.already_recorded",
            "A verification environment snapshot already exists for this Run"});
    }
    snapshots_[runId] = std::move(snapshot);
    return common::Result<void>::ok();
}

common::Result<std::optional<VerificationEnvironmentSnapshot>>
InMemoryVerificationEnvironmentRepository::findForRun(const RunId& runId) const
{
    std::lock_guard lock(mutex_);
    const auto it = snapshots_.find(runId);
    if (it == snapshots_.end()) {
        return common::Result<std::optional<VerificationEnvironmentSnapshot>>::ok(std::nullopt);
    }
    return common::Result<std::optional<VerificationEnvironmentSnapshot>>::ok(it->second);
}

} // namespace ai::application
