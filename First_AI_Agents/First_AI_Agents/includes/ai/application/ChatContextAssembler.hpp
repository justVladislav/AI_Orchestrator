#pragma once

#include "ai/application/ChatTypes.hpp"
#include "ai/common/Result.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ai::application {

struct ChatContextAssemblyRequest {
    std::string systemPrompt;
    std::vector<ChatMessage> messages;
    ChatMessageId stopBeforeMessageId;
    std::vector<ChatMemorySegment> memorySegments;
    std::uint32_t historyLimit {200};
    std::uint64_t inputTokenBudget {0};
};

struct ChatContextAssembly {
    std::string systemPrompt;
    std::string userContent;
    std::vector<ChatMessageId> includedMessageIds;
    std::vector<ChatMemorySegmentId> includedMemorySegmentIds;
    std::uint64_t estimatedInputTokens {0};
};

// Conservative provider-neutral estimate. Provider-reported usage remains
// authoritative for billing; this estimate exists only to keep a request
// safely inside the selected model's context window.
[[nodiscard]] std::uint64_t estimateChatTokens(std::string_view text);

[[nodiscard]] common::Result<ChatContextAssembly> assembleChatContext(
    ChatContextAssemblyRequest request);

} // namespace ai::application
