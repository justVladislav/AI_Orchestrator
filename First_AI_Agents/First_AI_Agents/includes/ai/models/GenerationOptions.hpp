#pragma once

#include "ai/models/PromptCachePolicy.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace ai::models {

/**
 * @brief Per-request HTTP timeout overrides shared by all HTTP providers.
 *
 * The connect timeout limits only connection establishment. The total timeout
 * covers the complete non-streaming HTTP request, including model generation
 * and response transfer. Empty values let the HTTP transport use its defaults.
 */
struct RequestTimeoutOptions {
    std::optional<std::chrono::milliseconds> connectTimeout;
    std::optional<std::chrono::milliseconds> totalTimeout;
    // Long-running interactive requests may deliberately wait for the
    // provider until the caller cancels them. Connection establishment keeps
    // its own bounded timeout even when this flag is set.
    bool disableTotalTimeout {false};
};

/**
 * @brief Common generation controls shared by all model providers.
 *
 * The first fields represent settings that most providers understand. The
 * timeout fields are provider-agnostic request controls. The metadata map is a
 * deliberate extension point for provider-specific switches such as OpenAI
 * reasoning effort or endpoint override.
 */
struct GenerationOptions {
    /// Requested model id. Providers may replace mock/default values with env/config fallbacks.
    std::string model {"mock-default"};

    /// Sampling temperature. Not every provider implementation uses it yet.
    double temperature {0.2};

    /// Upper bound for generated output tokens when supported by the provider.
    std::uint32_t maxOutputTokens {1024};

    /// Reserved for future streaming support. Current providers return full responses.
    bool stream {false};

    /// Optional per-request HTTP timeout overrides.
    RequestTimeoutOptions timeouts;

    /// Optional cooperative cancellation probe consumed by HTTP transports.
    std::function<bool()> cancellationRequested;

    /// Provider-specific key/value options without changing the public request type.
    std::unordered_map<std::string, std::string> metadata;

    // Typed single-shot prompt-cache policy. Disabled preserves the exact
    // pre-ACD-4 wire shape; provider-specific metadata must not override it.
    PromptCachePolicy promptCache;
};

} // namespace ai::models
