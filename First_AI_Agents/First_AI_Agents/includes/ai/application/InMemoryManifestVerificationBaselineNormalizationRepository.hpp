#pragma once

#include "ai/application/IManifestVerificationBaselineNormalizationRepository.hpp"

#include <map>
#include <mutex>

namespace ai::application {

class InMemoryManifestVerificationBaselineNormalizationRepository final
    : public IManifestVerificationBaselineNormalizationRepository {
public:
    common::Result<void> insert(ManifestVerificationBaselineNormalization normalization) override;
    [[nodiscard]] common::Result<std::optional<ManifestVerificationBaselineNormalization>>
    findBySourceRun(const RunId& sourceRunId) const override;

private:
    mutable std::mutex mutex_;
    std::map<RunId, ManifestVerificationBaselineNormalization> bySourceRun_;
};

} // namespace ai::application
