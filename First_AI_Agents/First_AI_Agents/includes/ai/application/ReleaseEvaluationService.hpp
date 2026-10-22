#pragma once

#include "ai/common/Result.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ai::application {

enum class ReleaseEvaluationMode {
    SolBaseline,
    ManualSolWorkers,
    ManifestSolWorkers,
    TerraWorkerSubstitution
};

struct ReleaseEvaluationMeasurement {
    ReleaseEvaluationMode mode {ReleaseEvaluationMode::SolBaseline};
    std::string taskFingerprint;
    std::string repositoryRevision;
    std::uint64_t wallTimeMs {0};
    std::uint64_t providerCalls {0};
    std::uint64_t auditedToolCalls {0};
    std::uint64_t inputTokens {0};
    std::uint64_t outputTokens {0};
    std::int64_t costMicros {0};
    std::uint64_t conflicts {0};
    std::uint64_t validationFailures {0};
    std::uint64_t missedIntegrationItems {0};
    std::uint64_t manualCorrections {0};
    std::uint32_t completenessBasisPoints {0};
    bool hiddenManualRepair {false};
};

struct ReleaseEvaluationReport {
    std::vector<ReleaseEvaluationMeasurement> primaryRuns;
    std::optional<ReleaseEvaluationMeasurement> terraSubstitution;
    bool releaseGatePassed {false};
    std::vector<std::string> gateFailures;

    [[nodiscard]] std::string toMarkdown() const;
};

// Builds the reproducible Phase 6 comparison. The first comparison is
// deliberately Sol-only; an optional Terra run is accepted solely as an
// isolated worker substitution after the primary contract has been supplied.
class ReleaseEvaluationService final {
public:
    [[nodiscard]] ai::common::Result<ReleaseEvaluationReport> evaluate(
        std::vector<ReleaseEvaluationMeasurement> measurements) const;
};

[[nodiscard]] const char* releaseEvaluationModeName(ReleaseEvaluationMode mode) noexcept;

} // namespace ai::application
