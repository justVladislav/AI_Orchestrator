#pragma once

#include "ai/application/verification/IManifestVerificationContinuationRepository.hpp"

#include <map>
#include <mutex>

namespace ai::application {

class InMemoryManifestVerificationContinuationRepository final
    : public IManifestVerificationContinuationRepository {
public:
    common::Result<void> insert(
        ManifestVerificationContinuationCheckpoint checkpoint) override;
    [[nodiscard]] common::Result<std::optional<ManifestVerificationContinuationCheckpoint>>
    findBySourceRun(const RunId& sourceRunId) const override;
    common::Result<void> insertAttribution(
        ManifestVerificationAttributionCheckpoint checkpoint) override;
    [[nodiscard]] common::Result<std::optional<ManifestVerificationAttributionCheckpoint>>
    findAttributionBySourceRun(const RunId& sourceRunId) const override;
    common::Result<void> insertInput(ManifestVerificationContinuationInput input) override;
    [[nodiscard]] common::Result<std::optional<ManifestVerificationContinuationInput>>
    findInputBySourceRun(const RunId& sourceRunId) const override;
    [[nodiscard]] common::Result<std::optional<ManifestVerificationContinuationInput>>
    findInputByChildRun(const RunId& childRunId) const override;
    [[nodiscard]] common::Result<std::vector<ManifestVerificationContinuationInput>>
    listInputs() const override;

private:
    mutable std::mutex mutex_;
    std::map<RunId, ManifestVerificationContinuationCheckpoint> bySourceRun_;
    std::map<RunId, ManifestVerificationAttributionCheckpoint> attributionBySourceRun_;
    std::map<RunId, ManifestVerificationContinuationInput> inputBySourceRun_;
};

} // namespace ai::application
