#include "../SqliteConnection.hpp"
#include "../SqlitePrimitives.hpp"

#include "ai/application/ChatTypes.hpp"

#include <sqlite3.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ai::infrastructure {
namespace {

using application::ChatMemorySegment;
using sqlite_detail::Statement;
using sqlite_detail::error;
using sqlite_detail::databaseError;
using sqlite_detail::prepare;
using sqlite_detail::bindText;
using sqlite_detail::bindInt64;
using sqlite_detail::bindUnsigned;
using sqlite_detail::toMilliseconds;
using sqlite_detail::fromMilliseconds;
using sqlite_detail::columnText;

} // namespace

namespace {
constexpr const char* chatMemorySegmentColumns =
    "id,conversation_id,first_message_id,last_message_id,summary,"
    "source_token_count,summary_token_count,created_at_ms";

common::Result<ChatMemorySegment> readChatMemorySegment(sqlite3_stmt* s)
{
    ChatMemorySegment value;
    value.id = columnText(s, 0);
    value.conversationId = columnText(s, 1);
    value.firstMessageId = columnText(s, 2);
    value.lastMessageId = columnText(s, 3);
    value.summary = columnText(s, 4);
    value.sourceTokenCount = static_cast<std::uint64_t>(sqlite3_column_int64(s, 5));
    value.summaryTokenCount = static_cast<std::uint64_t>(sqlite3_column_int64(s, 6));
    value.createdAt = fromMilliseconds(sqlite3_column_int64(s, 7));
    return common::Result<ChatMemorySegment>::ok(std::move(value));
}
}

SqliteChatMemoryRepository::SqliteChatMemoryRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<ChatMemorySegment> SqliteChatMemoryRepository::insertMemorySegment(ChatMemorySegment v)
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h = database_->impl_->handle; Statement s;
    const auto p = prepare(h,
        "INSERT INTO chat_memory_segments (id,conversation_id,first_message_id,last_message_id,"
        "summary,source_token_count,summary_token_count,created_at_ms) VALUES (?,?,?,?,?,?,?,?);", s);
    if (!p.success()) return common::Result<ChatMemorySegment>::failure(*p.error());
    const common::Result<void> b[] = {bindText(h,s.get(),1,v.id),bindText(h,s.get(),2,v.conversationId),
        bindText(h,s.get(),3,v.firstMessageId),bindText(h,s.get(),4,v.lastMessageId),
        bindText(h,s.get(),5,v.summary),bindUnsigned(h,s.get(),6,v.sourceTokenCount),
        bindUnsigned(h,s.get(),7,v.summaryTokenCount),bindInt64(h,s.get(),8,toMilliseconds(v.createdAt))};
    for (const auto& x : b) if (!x.success()) return common::Result<ChatMemorySegment>::failure(*x.error());
    if (sqlite3_step(s.get()) != SQLITE_DONE) {
        const int code = sqlite3_extended_errcode(h);
        if (code == SQLITE_CONSTRAINT_PRIMARYKEY)
            return common::Result<ChatMemorySegment>::failure(
                error("chat_memory.duplicate_id", "Memory segment id already exists"));
        if (code == SQLITE_CONSTRAINT_FOREIGNKEY)
            return common::Result<ChatMemorySegment>::failure(
                error("chat_memory.conversation_not_found", "Conversation does not exist"));
        return common::Result<ChatMemorySegment>::failure(
            databaseError(h, "persistence.write_failed", "Could not insert chat memory segment"));
    }
    return common::Result<ChatMemorySegment>::ok(std::move(v));
}

common::Result<std::vector<ChatMemorySegment>> SqliteChatMemoryRepository::listMemorySegments(
    const application::ConversationId& conversationId) const
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h = database_->impl_->handle; Statement s;
    // Stable chronological order: created_at_ms first, id as deterministic tiebreak.
    const std::string sql = std::string("SELECT ") + chatMemorySegmentColumns
        + " FROM chat_memory_segments WHERE conversation_id=? ORDER BY created_at_ms,id;";
    const auto p = prepare(h, sql.c_str(), s);
    if (!p.success()) return common::Result<std::vector<ChatMemorySegment>>::failure(*p.error());
    const auto b = bindText(h, s.get(), 1, conversationId);
    if (!b.success()) return common::Result<std::vector<ChatMemorySegment>>::failure(*b.error());
    std::vector<ChatMemorySegment> out;
    for (;;) {
        const int step = sqlite3_step(s.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW)
            return common::Result<std::vector<ChatMemorySegment>>::failure(
                databaseError(h, "persistence.read_failed", "Could not list chat memory segments"));
        auto v = readChatMemorySegment(s.get());
        if (!v.success()) return common::Result<std::vector<ChatMemorySegment>>::failure(*v.error());
        out.push_back(std::move(v.value()));
    }
    return common::Result<std::vector<ChatMemorySegment>>::ok(std::move(out));
}

common::Result<std::optional<ChatMemorySegment>> SqliteChatMemoryRepository::findMemorySegment(
    const application::ChatMemorySegmentId& id) const
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h = database_->impl_->handle; Statement s;
    const std::string sql = std::string("SELECT ") + chatMemorySegmentColumns
        + " FROM chat_memory_segments WHERE id=?;";
    const auto p = prepare(h, sql.c_str(), s);
    if (!p.success()) return common::Result<std::optional<ChatMemorySegment>>::failure(*p.error());
    const auto b = bindText(h, s.get(), 1, id);
    if (!b.success()) return common::Result<std::optional<ChatMemorySegment>>::failure(*b.error());
    const int step = sqlite3_step(s.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<ChatMemorySegment>>::ok(std::nullopt);
    if (step != SQLITE_ROW)
        return common::Result<std::optional<ChatMemorySegment>>::failure(
            databaseError(h, "persistence.read_failed", "Could not read chat memory segment"));
    auto v = readChatMemorySegment(s.get());
    if (!v.success()) return common::Result<std::optional<ChatMemorySegment>>::failure(*v.error());
    return common::Result<std::optional<ChatMemorySegment>>::ok(std::move(v.value()));
}

common::Result<void> SqliteChatMemoryRepository::removeMemorySegments(
    const application::ConversationId& conversationId)
{
    // Deleting zero rows is success: "no memory for this conversation" is a
    // valid state, unlike removeConversation's not-found contract.
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h = database_->impl_->handle; Statement s;
    const auto p = prepare(h, "DELETE FROM chat_memory_segments WHERE conversation_id=?;", s);
    if (!p.success()) return p;
    const auto b = bindText(h, s.get(), 1, conversationId);
    if (!b.success()) return b;
    if (sqlite3_step(s.get()) != SQLITE_DONE)
        return common::Result<void>::failure(
            databaseError(h, "persistence.write_failed", "Could not remove chat memory segments"));
    return common::Result<void>::ok();
}

} // namespace ai::infrastructure
