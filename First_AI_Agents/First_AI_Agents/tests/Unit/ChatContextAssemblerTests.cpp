#include "ai/application/ChatContextAssembler.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <string>

using namespace ai::application;

namespace {

ChatMessage message(std::string id, ChatRole role, ChatMessageStatus status, std::string content)
{
    ChatMessage value;
    value.id = std::move(id);
    value.conversationId = "conversation";
    value.role = role;
    value.status = status;
    value.content = std::move(content);
    return value;
}

std::size_t occurrences(const std::string& text, const std::string& needle)
{
    std::size_t count = 0;
    for (std::size_t position = 0;
         (position = text.find(needle, position)) != std::string::npos;
         position += needle.size()) {
        ++count;
    }
    return count;
}

void testRecentWindowFiltersInvalidRowsAndKeepsCurrentOnce()
{
    ChatContextAssemblyRequest request;
    request.systemPrompt = "system";
    request.inputTokenBudget = 10'000;
    request.historyLimit = 2;
    request.stopBeforeMessageId = "placeholder";
    request.messages = {
        message("old-user", ChatRole::User, ChatMessageStatus::Sent, "OLD_USER_TEXT"),
        message("old-answer", ChatRole::Assistant, ChatMessageStatus::Sent, "OLD_ANSWER_TEXT"),
        message("failed", ChatRole::Assistant, ChatMessageStatus::Failed, "FAILED_TEXT"),
        message("current", ChatRole::User, ChatMessageStatus::Sent, "CURRENT_TEXT"),
        message("placeholder", ChatRole::Assistant, ChatMessageStatus::Generating, "")};

    const auto result = assembleChatContext(std::move(request));
    assert(result.success());
    assert(result.value().includedMessageIds.size() == 2);
    assert(result.value().includedMessageIds[0] == "old-answer");
    assert(result.value().includedMessageIds[1] == "current");
    assert(result.value().userContent.find("OLD_USER_TEXT") == std::string::npos);
    assert(result.value().userContent.find("FAILED_TEXT") == std::string::npos);
    assert(occurrences(result.value().userContent, "CURRENT_TEXT") == 1);
}

void testOlderSummaryIsIncludedWithoutOverlappingRecentMessages()
{
    ChatContextAssemblyRequest request;
    request.systemPrompt = "system";
    request.inputTokenBudget = 10'000;
    request.historyLimit = 1;
    request.stopBeforeMessageId = "placeholder";
    request.messages = {
        message("old-user", ChatRole::User, ChatMessageStatus::Sent, "old question"),
        message("old-answer", ChatRole::Assistant, ChatMessageStatus::Sent, "old answer"),
        message("current", ChatRole::User, ChatMessageStatus::Sent, "current question"),
        message("placeholder", ChatRole::Assistant, ChatMessageStatus::Generating, "")};

    ChatMemorySegment older;
    older.id = "older-summary";
    older.conversationId = "conversation";
    older.firstMessageId = "old-user";
    older.lastMessageId = "old-answer";
    older.summary = "Important earlier decision";
    ChatMemorySegment overlap = older;
    overlap.id = "overlap-summary";
    overlap.lastMessageId = "current";
    overlap.summary = "Must not duplicate current turn";
    request.memorySegments = {older, overlap};

    const auto result = assembleChatContext(std::move(request));
    assert(result.success());
    assert(result.value().includedMemorySegmentIds.size() == 1);
    assert(result.value().includedMemorySegmentIds.front() == "older-summary");
    assert(result.value().systemPrompt.find("Important earlier decision") != std::string::npos);
    assert(result.value().systemPrompt.find("Must not duplicate") == std::string::npos);
    assert(result.value().userContent.find("old question") == std::string::npos);
    assert(result.value().userContent.find("current question") != std::string::npos);
}

void testTokenBudgetDropsOldRowsButNeverCurrentMessage()
{
    const std::string current = "CURRENT_REQUIRED";
    ChatContextAssemblyRequest request;
    request.systemPrompt = "system";
    request.historyLimit = 20;
    request.stopBeforeMessageId = "placeholder";
    request.messages = {
        message("large-old", ChatRole::Assistant, ChatMessageStatus::Sent, std::string(900, 'x')),
        message("current", ChatRole::User, ChatMessageStatus::Sent, current),
        message("placeholder", ChatRole::Assistant, ChatMessageStatus::Generating, "")};
    request.inputTokenBudget = estimateChatTokens(request.systemPrompt)
        + estimateChatTokens(current) + 20;

    const auto result = assembleChatContext(request);
    assert(result.success());
    assert(result.value().includedMessageIds.size() == 1);
    assert(result.value().includedMessageIds.front() == "current");

    request.inputTokenBudget = estimateChatTokens(request.systemPrompt) + 1;
    const auto tooLarge = assembleChatContext(std::move(request));
    assert(!tooLarge.success());
    assert(tooLarge.error()->code == "chat.message_too_large");
}

} // namespace

int runChatContextAssemblerTests()
{
    testRecentWindowFiltersInvalidRowsAndKeepsCurrentOnce();
    testOlderSummaryIsIncludedWithoutOverlappingRecentMessages();
    testTokenBudgetDropsOldRowsButNeverCurrentMessage();
    return 0;
}
