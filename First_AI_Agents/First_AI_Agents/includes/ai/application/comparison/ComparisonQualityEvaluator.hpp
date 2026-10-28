#pragma once

#include "ai/application/agents/ModelExecutionService.hpp"

#include <string_view>

namespace ai::application {

/**
 * Reproducible baseline evaluator for Comparison candidates.
 *
 * It deliberately uses only persisted prompt/output text and a versioned
 * fixed rubric. A later blinded-model review may be added as another
 * QualityEvaluation, but it must not replace this deterministic baseline.
 */
class ComparisonQualityEvaluator final {
public:
    [[nodiscard]] CreateDeterministicQualityEvaluationRequest evaluate(
        const ModelExecution& execution,
        std::string_view prompt) const;

    [[nodiscard]] static constexpr std::string_view rubricVersion() noexcept
    {
        return "comparison-deterministic-v1";
    }
};

} // namespace ai::application
