#pragma once

#include "ai/providers/IModelProvider.hpp"
#include "ai/providers/ModelType.hpp"

#include <string>

namespace ai::providers {

namespace anthropic_detail {

// Pure request renderer used by AnthropicProvider and provider-free tests.
// Throws std::runtime_error for an impossible/unknown cache policy.
[[nodiscard]] std::string renderAnthropicRequestBody(
    const models::ModelRequest& request,
    const std::string& model);

} // namespace anthropic_detail

/**
 * @brief Model provider that talks to Anthropic through the native Messages API.
 *
 * AnthropicProvider follows the same IModelProvider boundary as OpenAIProvider:
 * callers pass a provider-neutral ModelRequest, while this class owns Anthropic
 * authentication headers, Messages API JSON, HTTP execution, and error mapping.
 */
class AnthropicProvider final : public IModelProvider {
public:
    /**
     * @brief Creates a provider using ANTHROPIC_API_KEY from the environment.
     *
     * The key is read once during construction and is never logged or written to
     * generated output. Missing credentials are reported by generate().
     */
    AnthropicProvider();

    /**
     * @brief Creates a provider using ANTHROPIC_API_KEY and an explicit model.
     *
     * @param modelType The type of Anthropic model to use.
     */
    explicit AnthropicProvider(ModelType modelType);

    /**
     * @brief Creates a provider with an explicitly supplied Anthropic API key.
     *
     * @param apiKey Secret key used for the native x-api-key header.
     * @param modelType The type of Anthropic model to use.
     */
    explicit AnthropicProvider(std::string apiKey, ModelType modelType = ModelTypeUnknown);

    [[nodiscard]] std::string name() const override;

    /**
     * @brief Sends a request to Anthropic's native Messages API.
     *
     * @param request Provider-neutral request produced by Agent.
     * @return Normalized ModelResponse. Failures are represented as
     * success=false with errorMessage populated, matching OpenAIProvider style.
     */
    models::ModelResponse generate(const models::ModelRequest& request) override;

private:
    std::string apiKey_;
    std::string endpoint_;
    std::string apiVersion_;

    // Stored constructor choice so every request resolves the same Anthropic
    // model unless ModelTypeUnknown delegates to environment/default fallback.
    ModelType modelType_;
};

} // namespace ai::providers
