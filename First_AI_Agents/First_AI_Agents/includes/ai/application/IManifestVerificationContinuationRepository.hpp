#pragma once

#include "ai/application/ManifestVerificationContinuationTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>

namespace ai::application {

// Persistence port for MW-P5.1's durable admission checkpoint. At most one
// checkpoint ever exists per source Run - see
// ManifestVerificationContinuationCheckpoint's own doc comment.
class IManifestVerificationContinuationRepository {
public:
    virtual ~IManifestVerificationContinuationRepository() = default;

    // Immutable on sourceRunId: a second call for the same source Run fails
    // with "manifest_verification_continuation.already_exists" rather than
    // silently overwriting or creating a second checkpoint.
    // ManifestVerificationContinuationService treats that code as a benign
    // idempotent case and returns the existing checkpoint instead.
    virtual common::Result<void> insert(
        ManifestVerificationContinuationCheckpoint checkpoint) = 0;

    [[nodiscard]] virtual common::Result<std::optional<ManifestVerificationContinuationCheckpoint>>
    findBySourceRun(const RunId& sourceRunId) const = 0;

    virtual common::Result<void> insertAttribution(
        ManifestVerificationAttributionCheckpoint checkpoint) = 0;

    [[nodiscard]] virtual common::Result<std::optional<ManifestVerificationAttributionCheckpoint>>
    findAttributionBySourceRun(const RunId& sourceRunId) const = 0;

    virtual common::Result<void> insertInput(
        ManifestVerificationContinuationInput input) = 0;
    [[nodiscard]] virtual common::Result<std::optional<ManifestVerificationContinuationInput>>
    findInputBySourceRun(const RunId& sourceRunId) const = 0;
    // QML-P4R.2: the reverse lookup manual handoff needs to resolve
    // originManifestRunId for an anchor that is itself a paid continuation
    // child, never the source. childRunId is already unique across every
    // input (insertInput's own duplicate-child-reservation check), so this
    // returns at most one match.
    [[nodiscard]] virtual common::Result<std::optional<ManifestVerificationContinuationInput>>
    findInputByChildRun(const RunId& childRunId) const = 0;
    [[nodiscard]] virtual common::Result<std::vector<ManifestVerificationContinuationInput>>
    listInputs() const = 0;
};

} // namespace ai::application
