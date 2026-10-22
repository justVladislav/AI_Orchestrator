#pragma once

#include "ai/application/ExecutionManifestTypes.hpp"
#include "ai/common/Result.hpp"

#include <string_view>

namespace ai::application {

// Parses the planner-owned portion of execution-manifest.v1. Run identity,
// policy and creation time are trusted Backend inputs and are never accepted
// from provider JSON.
class ExecutionManifestParser final {
public:
    [[nodiscard]] static common::Result<ExecutionManifest> parse(
        std::string_view rawSubmission,
        const RunId& trustedRunId);
};

} // namespace ai::application
