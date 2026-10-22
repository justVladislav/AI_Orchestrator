#pragma once

#include "ai/application/IVerificationEnvironmentRepository.hpp"

#include <map>
#include <mutex>

namespace ai::application {

class InMemoryVerificationEnvironmentRepository final
    : public IVerificationEnvironmentRepository {
public:
    common::Result<void> recordSnapshot(VerificationEnvironmentSnapshot snapshot) override;
    [[nodiscard]] common::Result<std::optional<VerificationEnvironmentSnapshot>> findForRun(
        const RunId& runId) const override;

private:
    mutable std::mutex mutex_;
    std::map<RunId, VerificationEnvironmentSnapshot> snapshots_;
};

} // namespace ai::application
