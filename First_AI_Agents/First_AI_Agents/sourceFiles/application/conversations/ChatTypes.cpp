#include "ai/application/conversations/ChatTypes.hpp"

namespace ai::application {

std::string_view toString(ChatRole value) noexcept
{
    switch (value) {
    case ChatRole::User: return "user";
    case ChatRole::Assistant: return "assistant";
    case ChatRole::System: return "system";
    case ChatRole::Error: return "error";
    }
    return "error";
}

std::string_view toString(ChatMessageStatus value) noexcept
{
    switch (value) {
    case ChatMessageStatus::Sent: return "sent";
    case ChatMessageStatus::Generating: return "generating";
    case ChatMessageStatus::Failed: return "failed";
    case ChatMessageStatus::Stopped: return "stopped";
    }
    return "failed";
}

std::optional<ChatRole> chatRoleFromString(std::string_view value) noexcept
{
    if (value == "user") return ChatRole::User;
    if (value == "assistant") return ChatRole::Assistant;
    if (value == "system") return ChatRole::System;
    if (value == "error") return ChatRole::Error;
    return std::nullopt;
}

std::optional<ChatMessageStatus> chatMessageStatusFromString(std::string_view value) noexcept
{
    if (value == "sent") return ChatMessageStatus::Sent;
    if (value == "generating") return ChatMessageStatus::Generating;
    if (value == "failed") return ChatMessageStatus::Failed;
    if (value == "stopped") return ChatMessageStatus::Stopped;
    return std::nullopt;
}

} // namespace ai::application
