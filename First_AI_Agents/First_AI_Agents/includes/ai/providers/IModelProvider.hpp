#pragma once

#include "ai/models/ModelRequest.hpp"
#include "ai/models/ModelResponse.hpp"

#include <string>

namespace ai::providers {

/**
 * @brief Provider-neutral boundary between agents and concrete model backends.
 *
 * Agent and orchestration code should depend on this interface instead of
 * depending on OpenAI, Anthropic, HTTP, JSON, or any other vendor-specific
 * detail. Each provider owns the translation between the common ModelRequest /
 * ModelResponse structures and its own remote API.
 */
class IModelProvider {
public:
    virtual ~IModelProvider() = default;

    /**
     * @brief Returns a short stable provider id.
     *
     * The id is used in logs, ModelResponse::provider, and configuration files,
     * so it should remain stable even if the implementation changes.
     */
    [[nodiscard]] virtual std::string name() const = 0;

    /**
     * @brief Generates a provider-neutral response for a provider-neutral request.
     *
     * Implementations are responsible for request formatting, authentication,
     * transport, response parsing, and error normalization. Callers should not
     * need to catch provider-specific exceptions or parse vendor-specific JSON.
     */
    virtual models::ModelResponse generate(const models::ModelRequest& request) = 0;
};

} // namespace ai::providers
