#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/common/Result.hpp"

#include <vector>

namespace ai::application {

// Backend-owned secondary outcome for Runs history. The original RunStatus stays immutable; this
// projection answers the separate question whether manual verification recovery was completed and
// whether the exact authoritative proposal set was applied.
enum class ManifestVerificationManualHandoffResolution {
    NotApplicable,
    Unresolved,
    VerifiedAwaitingApply,
    PartiallyApplied,
    ResolvedManuallyApplied,
};

[[nodiscard]] const char* toString(
    ManifestVerificationManualHandoffResolution resolution) noexcept;

struct ManifestVerificationManualHandoffRunResolution {
    RunId runId;
    ManifestVerificationManualHandoffResolution resolution {
        ManifestVerificationManualHandoffResolution::NotApplicable};
};

// Narrow read-only port used by ProjectContextSnapshotService. The existing manual-handoff read
// model implements it, while focused Application tests can supply the same authoritative projection
// without constructing the command-side handoff services.
class IManifestVerificationManualHandoffResolutionReader {
public:
    virtual ~IManifestVerificationManualHandoffResolutionReader() = default;

    [[nodiscard]] virtual common::Result<
        std::vector<ManifestVerificationManualHandoffRunResolution>>
    resolutionsForRuns(const std::vector<Run>& runs) const = 0;
};

} // namespace ai::application
