#pragma once

#include "ai/models/TokenUsage.hpp"

#include <string>

namespace ai::models {

/**
 * @brief Provider-neutral response consumed by agents and the orchestrator.
 *
 * Providers normalize successful generations and failures into this structure
 * so higher layers can make decisions without knowing vendor-specific response
 * schemas or exception types.
 */
struct ModelResponse {
    /// Generated assistant text. Empty on provider or transport failure.
    std::string text;

    /// Stable provider id, for example "mock" or "openai".
    std::string provider;

    /// Concrete model used after provider fallback/configuration resolution.
    std::string model;

    /// Token counters when the provider returned them.
    TokenUsage usage;

    /// True when generated text is usable.
    bool success {false};

    /// Human-readable error when success is false.
    std::string errorMessage;
};

} // namespace ai::models
