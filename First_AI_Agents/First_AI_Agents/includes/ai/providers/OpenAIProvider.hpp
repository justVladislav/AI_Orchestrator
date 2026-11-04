#pragma once

#include "ai/providers/IModelProvider.hpp"
#include "ai/providers/ModelType.hpp"

#include <string>

namespace ai::http {
class IHttpClient;
}

namespace ai::providers {

/**
 * @brief Model provider that talks to OpenAI through the Responses API.
 *
 * OpenAIProvider is the only class that knows how the generic ModelRequest maps
 * to OpenAI's HTTP endpoint, headers, request JSON, and response JSON. Agents and
 * the orchestrator only depend on IModelProvider, so replacing this provider or
 * adding another vendor does not require changing agent execution code.
 */
class OpenAIProvider final : public IModelProvider {
public:
    /**
     * @brief Creates a provider using OPENAI_API_KEY from the process environment.
     *
     * The key is read once during construction and is never logged or written to
     * config files. Keeping secret handling inside the provider prevents higher
     * orchestration layers from accidentally exposing credentials.
     */
    OpenAIProvider();

    /**
     * @brief Creates a provider using OPENAI_API_KEY and an explicit model.
     *
     * @param modelType The OpenAI model choice to use for generated requests.
     */
    explicit OpenAIProvider(const ModelType& modelType);

    /**
     * @brief Creates a provider with an explicitly supplied API key.
     *
     * @param apiKey Secret bearer token to use for OpenAI requests. The
     * constructor is explicit to avoid accidentally converting arbitrary strings
     * into a network-capable provider.
     */
    explicit OpenAIProvider(std::string apiKey, const ModelType& modelType = ModelTypeUnknown);

    /**
     * @brief Returns the stable provider id used by logs and ModelResponse.
     *
     * @return The literal provider name "openai".
     */
    [[nodiscard]] std::string name() const override;

    /**
     * @brief Sends a request to the OpenAI Responses API.
     *
     * @param request Provider-neutral request produced by Agent.
     * @return Normalized ModelResponse. Failures are represented as
     * success=false with errorMessage populated, so callers can handle provider
     * errors without catching vendor-specific exceptions.
     */
    models::ModelResponse generate(const models::ModelRequest& request) override;

    /**
     * @brief The response of one request together with the Backend's STRUCTURED failure code.
     *
     * `errorCode` is empty on success and for every failure that has no received HTTP error response (missing key,
     * transport error, an unusable success body, an exception): those stay on the caller's explicit text fallback.
     * For ANY received non-2xx response it is the stable code the shared classifier chose from the HTTP status (and,
     * for a 429, the structured quota fields) - never derived from the human message.
     */
    struct DetailedResult {
        models::ModelResponse response;
        std::string errorCode;
    };

    /// Performs the single request over the real transport. generate() is a thin wrapper over this.
    [[nodiscard]] DetailedResult generateDetailed(const models::ModelRequest& request);

    /// The same, over an explicit transport (the seam for deterministic tests; production passes the real client).
    [[nodiscard]] DetailedResult generateDetailed(
        const models::ModelRequest& request, http::IHttpClient& transport);

private:
    // Stored once so every request can add Authorization without re-reading
    // environment/configuration and without exposing the secret to higher layers.
    std::string apiKey_;

    // Defaults to the Responses API endpoint, but stays as state so a future
    // configuration loader can set it without changing the public provider API.
    std::string endpoint_;

    // Stored constructor choice so requests can resolve a stable provider model.
    ModelType modelType_;
};

} // namespace ai::providers
