#pragma once

#include "ai/application/conversations/ChatTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <vector>

namespace ai::application {

class IChatRepository {
public:
    virtual ~IChatRepository() = default;
    virtual common::Result<ChatConversation> insertConversation(ChatConversation value) = 0;
    virtual common::Result<ChatConversation> updateConversation(ChatConversation value) = 0;
    virtual common::Result<void> removeConversation(const ConversationId& id) = 0;
    [[nodiscard]] virtual common::Result<std::optional<ChatConversation>> findConversation(const ConversationId& id) const = 0;
    [[nodiscard]] virtual common::Result<std::vector<ChatConversation>> listConversations() const = 0;
    // Inserts both messages, the optional immutable Assistant context attachment and the updated conversation in
    // one transaction. This is the only write path for a message carrying projectContext.
    virtual common::Result<PreparedChatTurn> insertPreparedTurn(
        ChatConversation conversation, PreparedChatTurn turn) = 0;
    virtual common::Result<ChatMessage> insertMessage(ChatMessage value) = 0;
    virtual common::Result<ChatMessage> updateMessage(ChatMessage value) = 0;
    [[nodiscard]] virtual common::Result<std::optional<ChatMessage>> findMessage(const ChatMessageId& id) const = 0;
    [[nodiscard]] virtual common::Result<std::vector<ChatMessage>> listMessages(const ConversationId& conversationId) const = 0;
};

} // namespace ai::application
