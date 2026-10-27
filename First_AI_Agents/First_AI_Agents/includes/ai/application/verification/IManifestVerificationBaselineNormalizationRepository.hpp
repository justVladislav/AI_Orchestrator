#pragma once

#include "ai/application/verification/ManifestVerificationContinuationTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>

namespace ai::application {

// MW-P5.6D: persistence port for the immutable legacy-baseline-drift
// normalization proof - see ManifestVerificationBaselineNormalization's own
// doc comment. Same "insert once, never replace" contract as
// IManifestVerificationContinuationRepository's admission/attribution/input
// rows.
class IManifestVerificationBaselineNormalizationRepository {
public:
    virtual ~IManifestVerificationBaselineNormalizationRepository() = default;

    // Immutable on sourceRunId: a second call for the same Run fails with
    // "manifest_verification_baseline_normalization.already_exists" rather
    // than replacing the earlier proof.
    virtual common::Result<void> insert(ManifestVerificationBaselineNormalization normalization) = 0;

    [[nodiscard]] virtual common::Result<std::optional<ManifestVerificationBaselineNormalization>>
    findBySourceRun(const RunId& sourceRunId) const = 0;
};

} // namespace ai::application
