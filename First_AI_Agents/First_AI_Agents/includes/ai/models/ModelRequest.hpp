#pragma once

#include "ai/models/GenerationOptions.hpp"
#include "ai/models/Message.hpp"

#include <vector>

namespace ai::models {

/**
 * @brief Provider-neutral request produced by agents and consumed by providers.
 *
 * Messages capture the conversation content, while GenerationOptions carries
 * model selection and provider-specific metadata. Concrete providers translate
 * this shape into their own API payloads.
 */
struct ModelRequest {
    /// Conversation messages in chronological order.
    std::vector<Message> messages;

    /// Generation controls and optional provider metadata.
    GenerationOptions options;
};

} // namespace ai::models
