#include "ai/application/inmemory/InMemoryManifestVerificationContinuationRepository.hpp"

namespace ai::application {

common::Result<void> InMemoryManifestVerificationContinuationRepository::insert(
    ManifestVerificationContinuationCheckpoint checkpoint)
{
    std::lock_guard lock(mutex_);
    if (bySourceRun_.contains(checkpoint.sourceRunId)) {
        return common::Result<void>::failure({"manifest_verification_continuation.already_exists",
            "A verification-continuation checkpoint already exists for this Run"});
    }
    bySourceRun_[checkpoint.sourceRunId] = std::move(checkpoint);
    return common::Result<void>::ok();
}

common::Result<std::optional<ManifestVerificationContinuationCheckpoint>>
InMemoryManifestVerificationContinuationRepository::findBySourceRun(
    const RunId& sourceRunId) const
{
    std::lock_guard lock(mutex_);
    const auto it = bySourceRun_.find(sourceRunId);
    if (it == bySourceRun_.end()) {
        return common::Result<std::optional<ManifestVerificationContinuationCheckpoint>>::ok(
            std::nullopt);
    }
    return common::Result<std::optional<ManifestVerificationContinuationCheckpoint>>::ok(
        it->second);
}

common::Result<void> InMemoryManifestVerificationContinuationRepository::insertAttribution(
    ManifestVerificationAttributionCheckpoint checkpoint)
{
    const auto valid = validateManifestVerificationAttributionCheckpoint(checkpoint);
    if (!valid.success()) return valid;
    std::lock_guard lock(mutex_);
    if (attributionBySourceRun_.contains(checkpoint.sourceRunId)) {
        return common::Result<void>::failure({
            "manifest_verification_attribution.already_exists",
            "A verification-attribution checkpoint already exists for this Run"});
    }
    attributionBySourceRun_[checkpoint.sourceRunId] = std::move(checkpoint);
    return common::Result<void>::ok();
}

common::Result<std::optional<ManifestVerificationAttributionCheckpoint>>
InMemoryManifestVerificationContinuationRepository::findAttributionBySourceRun(
    const RunId& sourceRunId) const
{
    std::lock_guard lock(mutex_);
    const auto it = attributionBySourceRun_.find(sourceRunId);
    if (it == attributionBySourceRun_.end()) {
        return common::Result<std::optional<ManifestVerificationAttributionCheckpoint>>::ok(
            std::nullopt);
    }
    return common::Result<std::optional<ManifestVerificationAttributionCheckpoint>>::ok(it->second);
}

common::Result<void> InMemoryManifestVerificationContinuationRepository::insertInput(
    ManifestVerificationContinuationInput input)
{
    const auto valid = validateManifestVerificationContinuationInput(input);
    if (!valid.success()) return valid;
    std::lock_guard lock(mutex_);
    if (inputBySourceRun_.contains(input.sourceRunId)) {
        return common::Result<void>::failure({
            "manifest_verification_continuation.input_already_exists",
            "A continuation input already exists for this source Run"});
    }
    for (const auto& [sourceRunId, existing] : inputBySourceRun_) {
        static_cast<void>(sourceRunId);
        if (existing.childRunId == input.childRunId) {
            return common::Result<void>::failure({
                "manifest_verification_continuation.child_already_reserved",
                "The continuation child identity is already reserved"});
        }
    }
    inputBySourceRun_.emplace(input.sourceRunId, std::move(input));
    return common::Result<void>::ok();
}

common::Result<std::optional<ManifestVerificationContinuationInput>>
InMemoryManifestVerificationContinuationRepository::findInputBySourceRun(
    const RunId& sourceRunId) const
{
    std::lock_guard lock(mutex_);
    const auto found = inputBySourceRun_.find(sourceRunId);
    return common::Result<std::optional<ManifestVerificationContinuationInput>>::ok(
        found == inputBySourceRun_.end() ? std::nullopt
            : std::optional<ManifestVerificationContinuationInput>(found->second));
}

common::Result<std::optional<ManifestVerificationContinuationInput>>
InMemoryManifestVerificationContinuationRepository::findInputByChildRun(
    const RunId& childRunId) const
{
    std::lock_guard lock(mutex_);
    for (const auto& [sourceRunId, input] : inputBySourceRun_) {
        static_cast<void>(sourceRunId);
        if (input.childRunId == childRunId) {
            return common::Result<std::optional<ManifestVerificationContinuationInput>>::ok(input);
        }
    }
    return common::Result<std::optional<ManifestVerificationContinuationInput>>::ok(std::nullopt);
}

common::Result<std::vector<ManifestVerificationContinuationInput>>
InMemoryManifestVerificationContinuationRepository::listInputs() const
{
    std::lock_guard lock(mutex_);
    std::vector<ManifestVerificationContinuationInput> values;
    values.reserve(inputBySourceRun_.size());
    for (const auto& [sourceRunId, input] : inputBySourceRun_) {
        static_cast<void>(sourceRunId);
        values.push_back(input);
    }
    return common::Result<std::vector<ManifestVerificationContinuationInput>>::ok(
        std::move(values));
}

} // namespace ai::application
