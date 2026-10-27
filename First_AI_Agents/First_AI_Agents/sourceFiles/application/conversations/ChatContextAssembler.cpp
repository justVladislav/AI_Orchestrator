#include "ai/application/conversations/ChatContextAssembler.hpp"

#include <algorithm>
#include <sstream>
#include <unordered_map>

namespace ai::application {
namespace {

common::Error contextError(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

} // namespace

std::uint64_t estimateChatTokens(std::string_view text)
{
    constexpr std::uint64_t framingTokens = 8;
    return framingTokens + std::max<std::uint64_t>(1, (text.size() + 2) / 3);
}

common::Result<ChatContextAssembly> assembleChatContext(ChatContextAssemblyRequest request)
{
    if (request.systemPrompt.empty() || request.inputTokenBudget == 0) {
        return common::Result<ChatContextAssembly>::failure(contextError(
            "chat.context_invalid", "Chat context input is incomplete"));
    }

    ChatContextAssembly output;
    output.systemPrompt = std::move(request.systemPrompt);
    output.estimatedInputTokens = estimateChatTokens(output.systemPrompt);
    if (output.estimatedInputTokens >= request.inputTokenBudget) {
        return common::Result<ChatContextAssembly>::failure(contextError(
            "chat.context_too_large", "System and project context exceed the selected model context window"));
    }

    std::vector<const ChatMessage*> validHistory;
    std::unordered_map<ChatMessageId, std::size_t> messagePositions;
    for (std::size_t index = 0; index < request.messages.size(); ++index) {
        const auto& item = request.messages[index];
        messagePositions.emplace(item.id, index);
        if (item.id == request.stopBeforeMessageId) break;
        if ((item.role == ChatRole::User && item.status == ChatMessageStatus::Sent)
            || (item.role == ChatRole::Assistant && item.status == ChatMessageStatus::Sent)) {
            validHistory.push_back(&item);
        }
    }
    if (validHistory.empty()) {
        return common::Result<ChatContextAssembly>::failure(contextError(
            "chat.current_message_missing", "Current user message is missing from conversation history"));
    }

    const std::size_t effectiveLimit = std::max<std::size_t>(1, request.historyLimit);
    const std::size_t limitedStart = validHistory.size() > effectiveLimit
        ? validHistory.size() - effectiveLimit : 0;
    std::vector<const ChatMessage*> includedMessages;
    for (std::size_t cursor = validHistory.size(); cursor > limitedStart; --cursor) {
        const ChatMessage* item = validHistory[cursor - 1];
        const std::uint64_t itemTokens = estimateChatTokens(item->content)
            + (item->role == ChatRole::User ? 2 : 4);
        if (output.estimatedInputTokens + itemTokens > request.inputTokenBudget) {
            // The newest valid row is the current user message persisted by
            // prepareTurn(). It must never be silently omitted or truncated.
            if (includedMessages.empty()) {
                return common::Result<ChatContextAssembly>::failure(contextError(
                    "chat.message_too_large", "Current message exceeds the selected model context window"));
            }
            continue;
        }
        includedMessages.insert(includedMessages.begin(), item);
        output.estimatedInputTokens += itemTokens;
    }

    std::vector<ChatMemorySegment> includedSegments;
    const auto firstIncludedPosition = messagePositions.find(includedMessages.front()->id);
    if (firstIncludedPosition != messagePositions.end()) {
        std::size_t earliestCoveredPosition = firstIncludedPosition->second;
        for (auto cursor = request.memorySegments.rbegin();
             cursor != request.memorySegments.rend(); ++cursor) {
            const auto firstPosition = messagePositions.find(cursor->firstMessageId);
            const auto lastPosition = messagePositions.find(cursor->lastMessageId);
            if (firstPosition == messagePositions.end()
                || lastPosition == messagePositions.end()
                || firstPosition->second > lastPosition->second
                || lastPosition->second >= earliestCoveredPosition) {
                continue; // Never duplicate recent rows or another selected segment.
            }
            const std::uint64_t measuredSegmentTokens = estimateChatTokens("\n- " + cursor->summary);
            const std::uint64_t segmentTokens = std::max(
                cursor->summaryTokenCount, measuredSegmentTokens);
            const std::uint64_t headingTokens = includedSegments.empty()
                ? estimateChatTokens("\nConversation memory from older completed turns:") : 0;
            if (output.estimatedInputTokens + headingTokens + segmentTokens
                > request.inputTokenBudget) continue;
            includedSegments.insert(includedSegments.begin(), *cursor);
            output.estimatedInputTokens += headingTokens + segmentTokens;
            earliestCoveredPosition = firstPosition->second;
        }
    }

    if (!includedSegments.empty()) {
        output.systemPrompt += "\nConversation memory from older completed turns:";
        for (const auto& segment : includedSegments) {
            output.systemPrompt += "\n- " + segment.summary;
            output.includedMemorySegmentIds.push_back(segment.id);
        }
    }

    std::ostringstream prompt;
    for (const ChatMessage* item : includedMessages) {
        prompt << (item->role == ChatRole::User ? "User: " : "Assistant: ")
               << item->content << "\n";
        output.includedMessageIds.push_back(item->id);
    }
    output.userContent = prompt.str();
    return common::Result<ChatContextAssembly>::ok(std::move(output));
}

} // namespace ai::application
