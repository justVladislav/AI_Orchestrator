#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/ExecutionProfileTypes.hpp"
#include "ai/common/Result.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ai::application {

struct RunContextAssemblyRequest {
    std::vector<ProjectMemorySegment> memorySegments;
    std::uint64_t memoryTokenBudget {0};
};

struct RunContextAssembly {
    std::string projectMemorySection;
    std::vector<ProjectMemorySegmentId> includedMemorySegmentIds;
    std::uint64_t estimatedMemoryTokens {0};
};

// Selects complete project-memory segments under an explicit token budget.
// Newer segments have priority, while the accepted subset is rendered in
// stable chronological order (createdAt, id).
[[nodiscard]] common::Result<RunContextAssembly> assembleRunContext(
    RunContextAssemblyRequest request);

inline constexpr std::uint64_t kDefaultContextWindowTokens = 128'000;
inline constexpr std::uint64_t kProjectMemoryPercent = 15;
inline constexpr std::uint64_t kProjectMemoryTokenCap = 16'000;

// The one project-memory token budget formula shared by both the real
// ExecutionProfileWorkflowRunner (which feeds it straight into
// assembleRunContext() above) and any UI surface that wants to preview the
// same number before a Run starts (AG-11's New Run indication) - kept here,
// beside assembleRunContext(), specifically so neither caller can drift onto
// a different, undocumented heuristic. Takes the minimum candidate budget
// across every assignment's model and (if set) fallback model, capped at
// kProjectMemoryTokenCap; falls back to that cap untouched when no
// assignment's model/fallback resolves.
//
// Templated on the model lookup instead of a concrete IModelRepository so
// the one Application-layer port (ExecutionProfileWorkflowRunner's
// IModelRepository&) and a UI-facing Application Service
// (ProviderCatalogService::findModel(), which BackendRuntime's layering
// rules require the Qt Adapter to go through instead of a raw repository)
// can both drive the exact same formula. findModel must be callable as
// common::Result<std::optional<Model>>(const ModelId&).
template <class ModelLookup>
[[nodiscard]] std::uint64_t projectMemoryBudget(
    const std::vector<StageModelAssignment>& assignments,
    ModelLookup&& findModel)
{
    std::uint64_t budget = kProjectMemoryTokenCap;
    bool measuredAnyModel = false;
    auto measure = [&](const ModelId& modelId) {
        const auto model = findModel(modelId);
        if (!model.success() || !model.value().has_value()) return;
        const std::uint64_t contextWindow = model.value()->contextWindowTokens > 0
            ? model.value()->contextWindowTokens : kDefaultContextWindowTokens;
        const std::uint64_t outputReserve =
            std::min(contextWindow, model.value()->maxOutputTokens);
        const std::uint64_t inputCapacity = contextWindow - outputReserve;
        const std::uint64_t candidate =
            (inputCapacity / 100) * kProjectMemoryPercent
            + (inputCapacity % 100) * kProjectMemoryPercent / 100;
        budget = std::min(budget, candidate);
        measuredAnyModel = true;
    };
    for (const auto& assignment : assignments) {
        measure(assignment.modelId);
        if (assignment.fallbackModelId.has_value()) {
            measure(*assignment.fallbackModelId);
        }
    }
    return measuredAnyModel ? budget : kProjectMemoryTokenCap;
}

} // namespace ai::application
