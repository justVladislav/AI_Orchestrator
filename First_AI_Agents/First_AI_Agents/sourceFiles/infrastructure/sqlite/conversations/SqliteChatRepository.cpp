#include "../SqliteConnection.hpp"
#include "../SqlitePrimitives.hpp"

#include "ai/application/ChatProjectContext.hpp"
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

using application::ChatConversation;
using application::ChatMessage;
using application::ChatProjectContextAttachment;
using application::PreparedChatTurn;
using sqlite_detail::Statement;
using sqlite_detail::error;
using sqlite_detail::databaseError;
using sqlite_detail::execute;
using sqlite_detail::prepare;
using sqlite_detail::bindText;
using sqlite_detail::bindNullableText;
using sqlite_detail::bindInt64;
using sqlite_detail::bindUnsigned;
using sqlite_detail::toMilliseconds;
using sqlite_detail::fromMilliseconds;
using sqlite_detail::columnText;
using sqlite_detail::optionalText;

} // namespace

SqliteChatRepository::SqliteChatRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

namespace {
constexpr const char* chatConversationColumns =
    "id,project_id,provider_id,model_id,title,preview,pinned,archived,input_tokens,output_tokens,"
    "estimated_cost_micros,last_latency_ms,created_at_ms,updated_at_ms";
constexpr const char* chatMessageColumns =
    "m.id,m.conversation_id,m.role,m.status,m.content,m.input_tokens,m.output_tokens,m.cost_micros,m.latency_ms,"
    "m.created_at_ms,m.provider_id,m.model_id,a.schema_version,a.conversation_id,a.project_id,a.snapshot_fingerprint,"
    "a.rendered_content_fingerprint,a.snapshot_policy_id,a.workspace_fingerprint,a.selected_run_id,"
    "a.includes_derived_project_memory,a.snapshot_estimated_tokens,a.rendered_estimated_tokens,a.rendered_text";
constexpr const char* chatMessageJoin =
    " FROM chat_messages m LEFT JOIN chat_project_context_attachments a ON a.assistant_message_id=m.id ";

common::Result<ChatConversation> readChatConversation(sqlite3_stmt* s)
{
    ChatConversation value;
    value.id = columnText(s, 0); value.projectId = optionalText(s, 1);
    value.providerId = columnText(s, 2); value.modelId = columnText(s, 3);
    value.title = columnText(s, 4); value.preview = columnText(s, 5);
    value.pinned = sqlite3_column_int64(s, 6) != 0; value.archived = sqlite3_column_int64(s, 7) != 0;
    value.inputTokens = static_cast<std::uint64_t>(sqlite3_column_int64(s, 8));
    value.outputTokens = static_cast<std::uint64_t>(sqlite3_column_int64(s, 9));
    value.estimatedCostMicros = static_cast<std::uint64_t>(sqlite3_column_int64(s, 10));
    value.lastLatencyMs = static_cast<std::uint64_t>(sqlite3_column_int64(s, 11));
    value.createdAt = fromMilliseconds(sqlite3_column_int64(s, 12));
    value.updatedAt = fromMilliseconds(sqlite3_column_int64(s, 13));
    return common::Result<ChatConversation>::ok(std::move(value));
}

common::Result<ChatMessage> readChatMessage(sqlite3_stmt* s)
{
    const auto role = application::chatRoleFromString(columnText(s, 2));
    const auto status = application::chatMessageStatusFromString(columnText(s, 3));
    if (!role || !status) return common::Result<ChatMessage>::failure(error("persistence.invalid_chat_enum", "Invalid persisted chat role or status"));
    ChatMessage value;
    value.id = columnText(s, 0); value.conversationId = columnText(s, 1);
    value.role = *role; value.status = *status; value.content = columnText(s, 4);
    value.inputTokens = static_cast<std::uint64_t>(sqlite3_column_int64(s, 5));
    value.outputTokens = static_cast<std::uint64_t>(sqlite3_column_int64(s, 6));
    value.costMicros = static_cast<std::uint64_t>(sqlite3_column_int64(s, 7));
    value.latencyMs = static_cast<std::uint64_t>(sqlite3_column_int64(s, 8));
    value.createdAt = fromMilliseconds(sqlite3_column_int64(s, 9));
    value.providerId = columnText(s, 10); value.modelId = columnText(s, 11);
    if (sqlite3_column_type(s, 12) != SQLITE_NULL) {
        ChatProjectContextAttachment attachment;
        attachment.schemaVersion = columnText(s, 12);
        attachment.assistantMessageId = value.id;
        attachment.conversationId = columnText(s, 13);
        attachment.projectId = columnText(s, 14);
        attachment.snapshotFingerprint = columnText(s, 15);
        attachment.renderedContentFingerprint = columnText(s, 16);
        attachment.snapshotPolicyId = columnText(s, 17);
        attachment.workspaceFingerprint = columnText(s, 18);
        attachment.selectedRunId = optionalText(s, 19);
        attachment.includesDerivedProjectMemory = sqlite3_column_int64(s, 20) != 0;
        attachment.snapshotEstimatedTokens = static_cast<std::uint64_t>(sqlite3_column_int64(s, 21));
        attachment.renderedEstimatedTokens = static_cast<std::uint64_t>(sqlite3_column_int64(s, 22));
        attachment.renderedText = columnText(s, 23);
        // An attachment belongs to an Assistant turn. A failed generation turns that row into an Error row (Retry turns
        // it back), so both are valid carriers; a User or System row can never be.
        if (attachment.conversationId != value.conversationId
            || (value.role != application::ChatRole::Assistant && value.role != application::ChatRole::Error)) {
            return common::Result<ChatMessage>::failure(error(
                "chat_context.invalid_identity", "The stored project context does not belong to its Chat message"));
        }
        const auto valid = application::validateChatProjectContextAttachment(attachment);
        if (!valid.success()) return common::Result<ChatMessage>::failure(*valid.error());
        value.projectContext = std::move(attachment);
    }
    return common::Result<ChatMessage>::ok(std::move(value));
}

common::Result<void> bindChatMessage(sqlite3* handle, sqlite3_stmt* statement, const ChatMessage& value)
{
    const common::Result<void> bound[] = {
        bindText(handle, statement, 1, value.id), bindText(handle, statement, 2, value.conversationId),
        bindText(handle, statement, 3, std::string(application::toString(value.role))),
        bindText(handle, statement, 4, std::string(application::toString(value.status))),
        bindText(handle, statement, 5, value.content), bindUnsigned(handle, statement, 6, value.inputTokens),
        bindUnsigned(handle, statement, 7, value.outputTokens), bindUnsigned(handle, statement, 8, value.costMicros),
        bindUnsigned(handle, statement, 9, value.latencyMs),
        bindInt64(handle, statement, 10, toMilliseconds(value.createdAt)),
        bindNullableText(handle, statement, 11, value.providerId.empty()
            ? std::optional<std::string> {} : std::optional<std::string> {value.providerId}),
        bindNullableText(handle, statement, 12, value.modelId.empty()
            ? std::optional<std::string> {} : std::optional<std::string> {value.modelId})};
    for (const auto& result : bound) if (!result.success()) return result;
    return common::Result<void>::ok();
}

common::Result<void> insertChatMessage(sqlite3* handle, const ChatMessage& value)
{
    Statement statement;
    auto prepared = prepare(handle,
        "INSERT INTO chat_messages (id,conversation_id,role,status,content,input_tokens,output_tokens,cost_micros,"
        "latency_ms,created_at_ms,provider_id,model_id) VALUES (?,?,?,?,?,?,?,?,?,?,?,?);", statement);
    if (!prepared.success()) return prepared;
    auto bound = bindChatMessage(handle, statement.get(), value);
    if (!bound.success()) return bound;
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(databaseError(
            handle, "persistence.write_failed", "Could not insert chat message"));
    }
    return common::Result<void>::ok();
}

common::Result<void> insertChatProjectContext(sqlite3* handle, const ChatProjectContextAttachment& value)
{
    const auto valid = application::validateChatProjectContextAttachment(value);
    if (!valid.success()) return valid;
    Statement statement;
    auto prepared = prepare(handle,
        "INSERT INTO chat_project_context_attachments (assistant_message_id,conversation_id,project_id,"
        "schema_version,snapshot_fingerprint,rendered_content_fingerprint,snapshot_policy_id,workspace_fingerprint,"
        "selected_run_id,includes_derived_project_memory,snapshot_estimated_tokens,rendered_estimated_tokens,"
        "rendered_text) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?);", statement);
    if (!prepared.success()) return prepared;
    const common::Result<void> bound[] = {
        bindText(handle, statement.get(), 1, value.assistantMessageId),
        bindText(handle, statement.get(), 2, value.conversationId),
        bindText(handle, statement.get(), 3, value.projectId),
        bindText(handle, statement.get(), 4, value.schemaVersion),
        bindText(handle, statement.get(), 5, value.snapshotFingerprint),
        bindText(handle, statement.get(), 6, value.renderedContentFingerprint),
        bindText(handle, statement.get(), 7, value.snapshotPolicyId),
        bindText(handle, statement.get(), 8, value.workspaceFingerprint),
        bindNullableText(handle, statement.get(), 9, value.selectedRunId),
        bindInt64(handle, statement.get(), 10, value.includesDerivedProjectMemory),
        bindUnsigned(handle, statement.get(), 11, value.snapshotEstimatedTokens),
        bindUnsigned(handle, statement.get(), 12, value.renderedEstimatedTokens),
        bindText(handle, statement.get(), 13, value.renderedText)};
    for (const auto& result : bound) if (!result.success()) return result;
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(databaseError(
            handle, "persistence.write_failed", "Could not insert Chat project context"));
    }
    return common::Result<void>::ok();
}
}

common::Result<ChatConversation> SqliteChatRepository::insertConversation(ChatConversation v)
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h = database_->impl_->handle; Statement s;
    const auto p = prepare(h, "INSERT INTO chat_conversations VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?);", s);
    if (!p.success()) return common::Result<ChatConversation>::failure(*p.error());
    const common::Result<void> b[] = {bindText(h,s.get(),1,v.id),bindNullableText(h,s.get(),2,v.projectId),bindText(h,s.get(),3,v.providerId),bindText(h,s.get(),4,v.modelId),bindText(h,s.get(),5,v.title),bindText(h,s.get(),6,v.preview),bindInt64(h,s.get(),7,v.pinned),bindInt64(h,s.get(),8,v.archived),bindUnsigned(h,s.get(),9,v.inputTokens),bindUnsigned(h,s.get(),10,v.outputTokens),bindUnsigned(h,s.get(),11,v.estimatedCostMicros),bindUnsigned(h,s.get(),12,v.lastLatencyMs),bindInt64(h,s.get(),13,toMilliseconds(v.createdAt)),bindInt64(h,s.get(),14,toMilliseconds(v.updatedAt))};
    for (const auto& x:b) if(!x.success()) return common::Result<ChatConversation>::failure(*x.error());
    if(sqlite3_step(s.get())!=SQLITE_DONE) return common::Result<ChatConversation>::failure(databaseError(h,"persistence.write_failed","Could not insert conversation"));
    return common::Result<ChatConversation>::ok(std::move(v));
}

common::Result<ChatConversation> SqliteChatRepository::updateConversation(ChatConversation v)
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h=database_->impl_->handle; Statement s;
    const auto p=prepare(h,"UPDATE chat_conversations SET project_id=?,provider_id=?,model_id=?,title=?,preview=?,pinned=?,archived=?,input_tokens=?,output_tokens=?,estimated_cost_micros=?,last_latency_ms=?,created_at_ms=?,updated_at_ms=? WHERE id=?;",s);
    if(!p.success()) return common::Result<ChatConversation>::failure(*p.error());
    const common::Result<void> b[]={bindNullableText(h,s.get(),1,v.projectId),bindText(h,s.get(),2,v.providerId),bindText(h,s.get(),3,v.modelId),bindText(h,s.get(),4,v.title),bindText(h,s.get(),5,v.preview),bindInt64(h,s.get(),6,v.pinned),bindInt64(h,s.get(),7,v.archived),bindUnsigned(h,s.get(),8,v.inputTokens),bindUnsigned(h,s.get(),9,v.outputTokens),bindUnsigned(h,s.get(),10,v.estimatedCostMicros),bindUnsigned(h,s.get(),11,v.lastLatencyMs),bindInt64(h,s.get(),12,toMilliseconds(v.createdAt)),bindInt64(h,s.get(),13,toMilliseconds(v.updatedAt)),bindText(h,s.get(),14,v.id)};
    for(const auto& x:b) if(!x.success()) return common::Result<ChatConversation>::failure(*x.error());
    if(sqlite3_step(s.get())!=SQLITE_DONE) return common::Result<ChatConversation>::failure(databaseError(h,"persistence.write_failed","Could not update conversation"));
    if(sqlite3_changes(h)==0) return common::Result<ChatConversation>::failure(error("chat.not_found","Conversation not found"));
    return common::Result<ChatConversation>::ok(std::move(v));
}

common::Result<void> SqliteChatRepository::removeConversation(const application::ConversationId& id)
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h=database_->impl_->handle; Statement s;
    auto p=prepare(h,"DELETE FROM chat_conversations WHERE id=?;",s); if(!p.success()) return p;
    auto b=bindText(h,s.get(),1,id); if(!b.success()) return b;
    if(sqlite3_step(s.get())!=SQLITE_DONE) return common::Result<void>::failure(databaseError(h,"persistence.write_failed","Could not delete conversation"));
    if(sqlite3_changes(h)==0) return common::Result<void>::failure(error("chat.not_found","Conversation not found"));
    return common::Result<void>::ok();
}

common::Result<std::optional<ChatConversation>> SqliteChatRepository::findConversation(const application::ConversationId& id) const
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h=database_->impl_->handle; Statement s; const std::string sql=std::string("SELECT ")+chatConversationColumns+" FROM chat_conversations WHERE id=?;";
    auto p=prepare(h,sql.c_str(),s); if(!p.success()) return common::Result<std::optional<ChatConversation>>::failure(*p.error()); auto b=bindText(h,s.get(),1,id); if(!b.success()) return common::Result<std::optional<ChatConversation>>::failure(*b.error());
    int step=sqlite3_step(s.get()); if(step==SQLITE_DONE) return common::Result<std::optional<ChatConversation>>::ok(std::nullopt); if(step!=SQLITE_ROW) return common::Result<std::optional<ChatConversation>>::failure(databaseError(h,"persistence.read_failed","Could not read conversation"));
    auto v=readChatConversation(s.get()); if(!v.success()) return common::Result<std::optional<ChatConversation>>::failure(*v.error()); return common::Result<std::optional<ChatConversation>>::ok(v.value());
}

common::Result<std::vector<ChatConversation>> SqliteChatRepository::listConversations() const
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h=database_->impl_->handle; Statement s; const std::string sql=std::string("SELECT ")+chatConversationColumns+" FROM chat_conversations ORDER BY pinned DESC,updated_at_ms DESC,id;";
    auto p=prepare(h,sql.c_str(),s); if(!p.success()) return common::Result<std::vector<ChatConversation>>::failure(*p.error()); std::vector<ChatConversation> out;
    for(;;){int step=sqlite3_step(s.get()); if(step==SQLITE_DONE) break; if(step!=SQLITE_ROW) return common::Result<std::vector<ChatConversation>>::failure(databaseError(h,"persistence.read_failed","Could not list conversations")); auto v=readChatConversation(s.get()); if(!v.success()) return common::Result<std::vector<ChatConversation>>::failure(*v.error()); out.push_back(v.value());}
    return common::Result<std::vector<ChatConversation>>::ok(std::move(out));
}

common::Result<PreparedChatTurn> SqliteChatRepository::insertPreparedTurn(
    ChatConversation conversation, PreparedChatTurn turn)
{
    if (turn.userMessage.conversationId != conversation.id
        || turn.assistantMessage.conversationId != conversation.id
        || turn.userMessage.role != application::ChatRole::User
        || turn.assistantMessage.role != application::ChatRole::Assistant
        || turn.userMessage.projectContext.has_value()) {
        return common::Result<PreparedChatTurn>::failure(error(
            "chat.invalid_prepared_turn", "The prepared Chat turn identity or roles are invalid"));
    }
    if (turn.assistantMessage.projectContext.has_value()) {
        const auto& attachment = *turn.assistantMessage.projectContext;
        const auto valid = application::validateChatProjectContextAttachment(attachment);
        if (!valid.success()) return common::Result<PreparedChatTurn>::failure(*valid.error());
        if (attachment.assistantMessageId != turn.assistantMessage.id
            || attachment.conversationId != conversation.id
            || !conversation.projectId.has_value() || attachment.projectId != *conversation.projectId) {
            return common::Result<PreparedChatTurn>::failure(error(
                "chat_context.invalid_identity", "The context attachment does not belong to this prepared turn"));
        }
    }

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    auto begun = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!begun.success()) return common::Result<PreparedChatTurn>::failure(*begun.error());
    const auto rollback = [&] { static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed")); };
    const auto user = insertChatMessage(handle, turn.userMessage);
    if (!user.success()) { rollback(); return common::Result<PreparedChatTurn>::failure(*user.error()); }
    const auto assistant = insertChatMessage(handle, turn.assistantMessage);
    if (!assistant.success()) { rollback(); return common::Result<PreparedChatTurn>::failure(*assistant.error()); }
    if (turn.assistantMessage.projectContext.has_value()) {
        const auto attached = insertChatProjectContext(handle, *turn.assistantMessage.projectContext);
        if (!attached.success()) { rollback(); return common::Result<PreparedChatTurn>::failure(*attached.error()); }
    }
    Statement statement;
    auto prepared = prepare(handle,
        "UPDATE chat_conversations SET project_id=?,provider_id=?,model_id=?,title=?,preview=?,pinned=?,archived=?,"
        "input_tokens=?,output_tokens=?,estimated_cost_micros=?,last_latency_ms=?,created_at_ms=?,updated_at_ms=? "
        "WHERE id=?;", statement);
    if (!prepared.success()) { rollback(); return common::Result<PreparedChatTurn>::failure(*prepared.error()); }
    const common::Result<void> bound[] = {
        bindNullableText(handle,statement.get(),1,conversation.projectId),bindText(handle,statement.get(),2,conversation.providerId),
        bindText(handle,statement.get(),3,conversation.modelId),bindText(handle,statement.get(),4,conversation.title),
        bindText(handle,statement.get(),5,conversation.preview),bindInt64(handle,statement.get(),6,conversation.pinned),
        bindInt64(handle,statement.get(),7,conversation.archived),bindUnsigned(handle,statement.get(),8,conversation.inputTokens),
        bindUnsigned(handle,statement.get(),9,conversation.outputTokens),bindUnsigned(handle,statement.get(),10,conversation.estimatedCostMicros),
        bindUnsigned(handle,statement.get(),11,conversation.lastLatencyMs),bindInt64(handle,statement.get(),12,toMilliseconds(conversation.createdAt)),
        bindInt64(handle,statement.get(),13,toMilliseconds(conversation.updatedAt)),bindText(handle,statement.get(),14,conversation.id)};
    for (const auto& result : bound) {
        if (!result.success()) { rollback(); return common::Result<PreparedChatTurn>::failure(*result.error()); }
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE || sqlite3_changes(handle) == 0) {
        const auto cause = databaseError(handle, "persistence.write_failed", "Could not update prepared Chat conversation");
        rollback();
        return common::Result<PreparedChatTurn>::failure(cause);
    }
    const auto committed = execute(handle, "COMMIT;", "persistence.transaction_failed");
    if (!committed.success()) { rollback(); return common::Result<PreparedChatTurn>::failure(*committed.error()); }
    return common::Result<PreparedChatTurn>::ok(std::move(turn));
}

common::Result<ChatMessage> SqliteChatRepository::insertMessage(ChatMessage v)
{
    if (v.projectContext.has_value()) return common::Result<ChatMessage>::failure(error(
        "chat.context_requires_prepared_turn", "Project context must be inserted atomically with its Chat turn"));
    std::lock_guard lock(database_->impl_->mutex);
    auto inserted = insertChatMessage(database_->impl_->handle, v);
    if (!inserted.success()) return common::Result<ChatMessage>::failure(*inserted.error());
    return common::Result<ChatMessage>::ok(std::move(v));
}

common::Result<ChatMessage> SqliteChatRepository::updateMessage(ChatMessage v)
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h=database_->impl_->handle; Statement s; auto p=prepare(h,"UPDATE chat_messages SET conversation_id=?,role=?,status=?,content=?,input_tokens=?,output_tokens=?,cost_micros=?,latency_ms=?,created_at_ms=?,provider_id=?,model_id=? WHERE id=?;",s); if(!p.success()) return common::Result<ChatMessage>::failure(*p.error());
    const common::Result<void> b[]={bindText(h,s.get(),1,v.conversationId),bindText(h,s.get(),2,std::string(application::toString(v.role))),bindText(h,s.get(),3,std::string(application::toString(v.status))),bindText(h,s.get(),4,v.content),bindUnsigned(h,s.get(),5,v.inputTokens),bindUnsigned(h,s.get(),6,v.outputTokens),bindUnsigned(h,s.get(),7,v.costMicros),bindUnsigned(h,s.get(),8,v.latencyMs),bindInt64(h,s.get(),9,toMilliseconds(v.createdAt)),bindNullableText(h,s.get(),10,v.providerId.empty()?std::optional<std::string>{}:std::optional<std::string>{v.providerId}),bindNullableText(h,s.get(),11,v.modelId.empty()?std::optional<std::string>{}:std::optional<std::string>{v.modelId}),bindText(h,s.get(),12,v.id)}; for(const auto& x:b) if(!x.success()) return common::Result<ChatMessage>::failure(*x.error());
    if(sqlite3_step(s.get())!=SQLITE_DONE) return common::Result<ChatMessage>::failure(databaseError(h,"persistence.write_failed","Could not update chat message")); if(sqlite3_changes(h)==0) return common::Result<ChatMessage>::failure(error("chat.message_not_found","Message not found")); return common::Result<ChatMessage>::ok(std::move(v));
}

common::Result<std::optional<ChatMessage>> SqliteChatRepository::findMessage(const application::ChatMessageId& id) const
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h=database_->impl_->handle; Statement s; const std::string sql=std::string("SELECT ")+chatMessageColumns+chatMessageJoin+"WHERE m.id=?;"; auto p=prepare(h,sql.c_str(),s); if(!p.success()) return common::Result<std::optional<ChatMessage>>::failure(*p.error()); auto b=bindText(h,s.get(),1,id); if(!b.success()) return common::Result<std::optional<ChatMessage>>::failure(*b.error()); int step=sqlite3_step(s.get()); if(step==SQLITE_DONE) return common::Result<std::optional<ChatMessage>>::ok(std::nullopt); if(step!=SQLITE_ROW) return common::Result<std::optional<ChatMessage>>::failure(databaseError(h,"persistence.read_failed","Could not read chat message")); auto v=readChatMessage(s.get()); if(!v.success()) return common::Result<std::optional<ChatMessage>>::failure(*v.error()); return common::Result<std::optional<ChatMessage>>::ok(v.value());
}

common::Result<std::vector<ChatMessage>> SqliteChatRepository::listMessages(const application::ConversationId& id) const
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h=database_->impl_->handle; Statement s; const std::string sql=std::string("SELECT ")+chatMessageColumns+chatMessageJoin+"WHERE m.conversation_id=? ORDER BY m.created_at_ms,m.id;"; auto p=prepare(h,sql.c_str(),s); if(!p.success()) return common::Result<std::vector<ChatMessage>>::failure(*p.error()); auto b=bindText(h,s.get(),1,id); if(!b.success()) return common::Result<std::vector<ChatMessage>>::failure(*b.error()); std::vector<ChatMessage> out; for(;;){int step=sqlite3_step(s.get()); if(step==SQLITE_DONE) break; if(step!=SQLITE_ROW) return common::Result<std::vector<ChatMessage>>::failure(databaseError(h,"persistence.read_failed","Could not list chat messages")); auto v=readChatMessage(s.get()); if(!v.success()) return common::Result<std::vector<ChatMessage>>::failure(*v.error()); out.push_back(v.value());} return common::Result<std::vector<ChatMessage>>::ok(std::move(out));
}

} // namespace ai::infrastructure
