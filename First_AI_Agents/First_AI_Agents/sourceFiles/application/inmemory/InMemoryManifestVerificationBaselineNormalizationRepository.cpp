#include "ai/application/inmemory/InMemoryManifestVerificationBaselineNormalizationRepository.hpp"

namespace ai::application {

common::Result<void> InMemoryManifestVerificationBaselineNormalizationRepository::insert(
    ManifestVerificationBaselineNormalization normalization)
{
    const auto valid = validateManifestVerificationBaselineNormalization(normalization);
    if (!valid.success()) return valid;
    std::lock_guard lock(mutex_);
    if (bySourceRun_.contains(normalization.sourceRunId)) {
        return common::Result<void>::failure({
            "manifest_verification_baseline_normalization.already_exists",
            "A baseline-normalization proof already exists for this source Run"});
    }
    bySourceRun_.emplace(normalization.sourceRunId, std::move(normalization));
    return common::Result<void>::ok();
}

common::Result<std::optional<ManifestVerificationBaselineNormalization>>
InMemoryManifestVerificationBaselineNormalizationRepository::findBySourceRun(
    const RunId& sourceRunId) const
{
    std::lock_guard lock(mutex_);
    const auto found = bySourceRun_.find(sourceRunId);
    return common::Result<std::optional<ManifestVerificationBaselineNormalization>>::ok(
        found == bySourceRun_.end() ? std::nullopt
            : std::optional<ManifestVerificationBaselineNormalization>(found->second));
}

} // namespace ai::application
