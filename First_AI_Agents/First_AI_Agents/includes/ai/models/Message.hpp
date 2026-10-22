#pragma once

#include <string>

namespace ai::models {

/**
 * @brief Conversation role understood by provider adapters.
 */
enum class MessageRole {
    System,
    User,
    Assistant,
    Tool
};

/**
 * @brief Converts a MessageRole into the stable lowercase configuration label.
 */
inline std::string toString(MessageRole role)
{
    switch (role) {
    case MessageRole::System:
        return "system";
    case MessageRole::User:
        return "user";
    case MessageRole::Assistant:
        return "assistant";
    case MessageRole::Tool:
        return "tool";
    }

    return "unknown";
}

/**
 * @brief Single provider-neutral chat message.
 *
 * Providers decide how to map roles to their own API. For example,
 * OpenAIProvider moves system messages into Responses API instructions.
 */
struct Message {
    /// Logical role of the message in the conversation.
    MessageRole role {MessageRole::User};

    /// Plain text content for the current project stage.
    std::string content;
};

} // namespace ai::models
