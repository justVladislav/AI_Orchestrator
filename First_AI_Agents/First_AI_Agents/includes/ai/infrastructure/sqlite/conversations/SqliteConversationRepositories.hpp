#pragma once

#include "ai/application/conversations/IChatMemoryRepository.hpp"
#include "ai/application/conversations/IChatRepository.hpp"
#include "ai/application/conversations/IProjectMemoryCompactionRepository.hpp"
#include "ai/application/conversations/IProjectMemoryRepository.hpp"
#include "ai/common/Result.hpp"
#include "ai/infrastructure/sqlite/SqliteDatabase.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace ai::infrastructure {

class SqliteChatRepository final : public application::IChatRepository {
public:
    explicit SqliteChatRepository(std::shared_ptr<SqliteDatabase> database);
    common::Result<application::ChatConversation> insertConversation(application::ChatConversation) override;
    common::Result<application::ChatConversation> updateConversation(application::ChatConversation) override;
    common::Result<void> removeConversation(const application::ConversationId&) override;
    [[nodiscard]] common::Result<std::optional<application::ChatConversation>> findConversation(const application::ConversationId&) const override;
    [[nodiscard]] common::Result<std::vector<application::ChatConversation>> listConversations() const override;
    common::Result<application::PreparedChatTurn> insertPreparedTurn(
        application::ChatConversation, application::PreparedChatTurn) override;
    common::Result<application::ChatMessage> insertMessage(application::ChatMessage) override;
    common::Result<application::ChatMessage> updateMessage(application::ChatMessage) override;
    [[nodiscard]] common::Result<std::optional<application::ChatMessage>> findMessage(const application::ChatMessageId&) const override;
    [[nodiscard]] common::Result<std::vector<application::ChatMessage>> listMessages(const application::ConversationId&) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

class SqliteChatMemoryRepository final : public application::IChatMemoryRepository {
public:
    explicit SqliteChatMemoryRepository(std::shared_ptr<SqliteDatabase> database);
    common::Result<application::ChatMemorySegment> insertMemorySegment(application::ChatMemorySegment) override;
    [[nodiscard]] common::Result<std::vector<application::ChatMemorySegment>> listMemorySegments(
        const application::ConversationId&) const override;
    [[nodiscard]] common::Result<std::optional<application::ChatMemorySegment>> findMemorySegment(
        const application::ChatMemorySegmentId&) const override;
    common::Result<void> removeMemorySegments(const application::ConversationId&) override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

class SqliteProjectMemoryRepository final : public application::IProjectMemoryRepository {
public:
    explicit SqliteProjectMemoryRepository(std::shared_ptr<SqliteDatabase> database);
    common::Result<application::ProjectMemorySegment> insertMemorySegment(application::ProjectMemorySegment) override;
    [[nodiscard]] common::Result<std::vector<application::ProjectMemorySegment>> listMemorySegments(
        const application::ProjectId&) const override;
    [[nodiscard]] common::Result<application::ProjectMemoryRecentPage> listRecentMemorySegments(
        const application::ProjectId&, std::size_t limit) const override;
    [[nodiscard]] common::Result<std::optional<application::ProjectMemorySegment>> findMemorySegment(
        const application::ProjectMemorySegmentId&) const override;
    common::Result<void> removeMemorySegments(const application::ProjectId&) override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

/** SQLite implementation of the AG-11 compaction provenance port. */
class SqliteProjectMemoryCompactionRepository final
    : public application::IProjectMemoryCompactionRepository {
public:
    explicit SqliteProjectMemoryCompactionRepository(std::shared_ptr<SqliteDatabase> database);

    common::Result<void> insertCompactionRecord(
        const application::ProjectMemoryCompactionRecord& record) override;
    common::Result<void> commitSuccessfulCompaction(
        const application::ProjectMemoryCompactionRecord& record,
        application::ProjectMemorySegment resultSegment) override;

    [[nodiscard]] common::Result<std::vector<application::ProjectMemoryCompactionRecord>>
    listCompactionRecords(const application::ProjectId& projectId) const override;

private:
    std::shared_ptr<SqliteDatabase> database_;
};

} // namespace ai::infrastructure
