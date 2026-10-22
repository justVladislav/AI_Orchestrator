#pragma once

#include "ai/application/ChatTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <vector>

namespace ai::application {

// Persistence port for the hybrid chat-memory layer. Segments summarize
// message ranges without ever mutating chat_messages; removing a conversation
// removes its segments (enforced by ON DELETE CASCADE in the SQLite adapter).
class IChatMemoryRepository {
public:
    virtual ~IChatMemoryRepository() = default;
    virtual common::Result<ChatMemorySegment> insertMemorySegment(ChatMemorySegment value) = 0;
    [[nodiscard]] virtual common::Result<std::vector<ChatMemorySegment>> listMemorySegments(
        const ConversationId& conversationId) const = 0;
    [[nodiscard]] virtual common::Result<std::optional<ChatMemorySegment>> findMemorySegment(
        const ChatMemorySegmentId& id) const = 0;
    virtual common::Result<void> removeMemorySegments(const ConversationId& conversationId) = 0;
};

} // namespace ai::application
