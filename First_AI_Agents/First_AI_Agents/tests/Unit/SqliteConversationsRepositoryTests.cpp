#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <barrier>
#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace ai::application;
using namespace ai::infrastructure;
namespace fs = std::filesystem;
using Time = std::chrono::system_clock::time_point;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

Time at(std::int64_t milliseconds)
{
    return Time(std::chrono::milliseconds(milliseconds));
}

struct Workspace {
    fs::path root = fs::temp_directory_path() / ("aio-sqlite-conversations-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path databasePath = root / "state.sqlite3";

    Workspace() { fs::create_directories(root); }
    ~Workspace() { std::error_code ignored; fs::remove_all(root, ignored); }

    std::shared_ptr<SqliteDatabase> open() const
    {
        auto result = SqliteDatabase::open(databasePath);
        require(result.success(), "Could not open test database");
        return std::move(result.value());
    }

    // Raw access for corrupting rows the public API would never write.
    void sql(const char* statement) const
    {
        sqlite3* handle = nullptr;
        const auto path = databasePath.generic_u8string();
        const auto opened = sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()),
            &handle, SQLITE_OPEN_READWRITE, nullptr);
        if (opened != SQLITE_OK) {
            if (handle) sqlite3_close(handle);
            throw std::runtime_error("Could not open test SQL connection");
        }
        const int result = sqlite3_exec(handle, statement, nullptr, nullptr, nullptr);
        sqlite3_close(handle);
        require(result == SQLITE_OK, "Test SQL statement failed");
    }

    fs::path projectRoot(const std::string& name) const { return root / "projects" / name; }
};

// Two providers with one model each and two projects: the rows a conversation may reference.
void addCatalogAndProjects(const std::shared_ptr<SqliteDatabase>& database, const Workspace& workspace)
{
    SqliteProviderRepository providers(database);
    SqliteModelRepository models(database);
    for (const char* suffix : {"1", "2"}) {
        Provider provider;
        provider.id = std::string("provider-") + suffix;
        provider.name = "Provider";
        provider.kind = "openai_compatible";
        provider.baseUrl = "https://example.invalid/v1";
        provider.enabled = true;
        provider.createdAt = at(500);
        provider.updatedAt = at(500);
        require(providers.insert(provider).success(), "Could not insert a provider for the conversation tests");
        Model model;
        model.id = std::string("model-") + suffix;
        model.providerId = provider.id;
        model.apiModelId = std::string("api-") + suffix;
        model.displayName = "Model";
        model.contextWindowTokens = 200'000;
        model.maxOutputTokens = 16'384;
        model.inputPriceMicrosPerMillionTokens = 3'000'000;
        model.outputPriceMicrosPerMillionTokens = 15'000'000;
        model.enabled = true;
        model.createdAt = at(500);
        model.updatedAt = at(500);
        require(models.insert(model).success(), "Could not insert a model for the conversation tests");
    }
    SqliteProjectRepository projects(database);
    for (const char* id : {"project-1", "project-2"}) {
        Project project;
        project.id = id;
        project.name = id;
        project.rootPath = workspace.projectRoot(id);
        project.status = ProjectStatus::Active;
        project.createdAt = at(500);
        project.updatedAt = at(500);
        require(projects.insert(project).success(), "Could not insert a project for the conversation tests");
    }
}

ChatConversation makeConversation(std::string id, std::int64_t updatedMs)
{
    ChatConversation value;
    value.id = std::move(id);
    value.projectId = std::nullopt;
    value.providerId = "provider-1";
    value.modelId = "model-1";
    value.title = "Chat \"quoted\" \xE6\xB8\xAC\xE8\xA9\xA6";
    value.preview = "Preview line\nsecond line";
    value.createdAt = at(1000);
    value.updatedAt = at(updatedMs);
    return value;
}

void requireSameConversation(const ChatConversation& actual, const ChatConversation& expected, const char* message)
{
    require(actual.id == expected.id && actual.projectId == expected.projectId
            && actual.providerId == expected.providerId && actual.modelId == expected.modelId
            && actual.title == expected.title && actual.preview == expected.preview
            && actual.pinned == expected.pinned && actual.archived == expected.archived
            && actual.inputTokens == expected.inputTokens && actual.outputTokens == expected.outputTokens
            && actual.estimatedCostMicros == expected.estimatedCostMicros
            && actual.lastLatencyMs == expected.lastLatencyMs && actual.createdAt == expected.createdAt
            && actual.updatedAt == expected.updatedAt,
        message);
}

void requireStoredConversation(const SqliteChatRepository& chats, const ChatConversation& expected, const char* message)
{
    const auto stored = chats.findConversation(expected.id);
    require(stored.success() && stored.value().has_value(), message);
    requireSameConversation(*stored.value(), expected, message);
}

std::vector<std::string> conversationIds(const SqliteChatRepository& chats)
{
    const auto listed = chats.listConversations();
    require(listed.success(), "Could not list conversations");
    std::vector<std::string> result;
    for (const auto& value : listed.value()) result.push_back(value.id);
    return result;
}

ChatMessage makeMessage(std::string id, std::string conversationId, std::int64_t createdMs)
{
    ChatMessage value;
    value.id = std::move(id);
    value.conversationId = std::move(conversationId);
    value.role = ChatRole::User;
    value.status = ChatMessageStatus::Sent;
    value.content = "Message \"quoted\"\nsecond line \xE6\xB8\xAC\xE8\xA9\xA6";
    value.createdAt = at(createdMs);
    return value;
}

void requireSameMessage(const ChatMessage& actual, const ChatMessage& expected, const char* message)
{
    require(actual.id == expected.id && actual.conversationId == expected.conversationId
            && actual.role == expected.role && actual.status == expected.status
            && actual.content == expected.content && actual.inputTokens == expected.inputTokens
            && actual.outputTokens == expected.outputTokens && actual.costMicros == expected.costMicros
            && actual.latencyMs == expected.latencyMs && actual.providerId == expected.providerId
            && actual.modelId == expected.modelId && actual.createdAt == expected.createdAt
            && !actual.projectContext.has_value(),
        message);
}

void requireStoredMessage(const SqliteChatRepository& chats, const ChatMessage& expected, const char* message)
{
    const auto stored = chats.findMessage(expected.id);
    require(stored.success() && stored.value().has_value(), message);
    requireSameMessage(*stored.value(), expected, message);
}

std::vector<std::string> messageIds(const SqliteChatRepository& chats, const std::string& conversationId)
{
    const auto listed = chats.listMessages(conversationId);
    require(listed.success(), "Could not list messages");
    std::vector<std::string> result;
    for (const auto& value : listed.value()) result.push_back(value.id);
    return result;
}

void testChatConversationsRoundTripOrderingUpdateAndRemoval()
{
    Workspace workspace;
    ChatConversation pinnedOld = makeConversation("chat-pinned-old", 1000);
    pinnedOld.projectId = "project-1";
    pinnedOld.pinned = true;
    pinnedOld.archived = true;
    pinnedOld.inputTokens = 5'000'000'000ULL;
    pinnedOld.outputTokens = 4'000'000'000ULL;
    pinnedOld.estimatedCostMicros = 3'000'000'000ULL;
    pinnedOld.lastLatencyMs = 2'000'000'000ULL;
    ChatConversation pinnedNew = makeConversation("chat-pinned-new", 3000);
    pinnedNew.pinned = true;
    const ChatConversation plainB = makeConversation("chat-b", 2000);
    const ChatConversation plainA = makeConversation("chat-a", 2000);
    using Ids = std::vector<std::string>;
    const std::initializer_list<const ChatConversation*> conversationsInInsertOrder {
        &plainB, &pinnedOld, &plainA, &pinnedNew};
    {
        const auto database = workspace.open();
        addCatalogAndProjects(database, workspace);
        SqliteChatRepository chats(database);
        for (const ChatConversation* value : conversationsInInsertOrder) {
            const auto saved = chats.insertConversation(*value);
            require(saved.success(), "Could not insert a conversation");
            requireSameConversation(saved.value(), *value, "insert must return the conversation as written");
        }
        for (const ChatConversation* value : conversationsInInsertOrder) {
            requireStoredConversation(chats, *value, "A conversation must round-trip every field");
        }
        require(conversationIds(chats) == Ids {"chat-pinned-new", "chat-pinned-old", "chat-a", "chat-b"},
            "Conversations must be listed pinned first, then newest first, then by id");
        const auto missing = chats.findConversation("chat-missing");
        require(missing.success() && !missing.value().has_value(), "A missing conversation must read as absent");

        // Refused inserts leave nothing behind and the connection usable.
        const auto repeated = chats.insertConversation(makeConversation("chat-a", 9000));
        require(!repeated.success() && repeated.error()->code == "persistence.write_failed",
            "A repeated conversation id must be refused as a failed write");
        ChatConversation orphanModel = makeConversation("chat-orphan-model", 9000);
        orphanModel.modelId = "model-unknown";
        ChatConversation orphanProject = makeConversation("chat-orphan-project", 9000);
        orphanProject.projectId = "project-unknown";
        for (const ChatConversation* value : {&orphanModel, &orphanProject}) {
            const auto refused = chats.insertConversation(*value);
            require(!refused.success() && refused.error()->code == "persistence.write_failed",
                "A conversation referencing an unknown model or project must be refused as a failed write");
        }
        require(conversationIds(chats).size() == 4, "Refused inserts must not add rows");
        requireStoredConversation(chats, plainA, "A refused insert altered an existing conversation");

        // update() replaces every field, including the creation time, the project and the model.
        ChatConversation changed = pinnedOld;
        changed.projectId = std::nullopt;
        changed.providerId = "provider-2";
        changed.modelId = "model-2";
        changed.title = "Renamed";
        changed.preview = "New preview";
        changed.pinned = false;
        changed.archived = false;
        changed.inputTokens = 11;
        changed.outputTokens = 12;
        changed.estimatedCostMicros = 13;
        changed.lastLatencyMs = 14;
        changed.createdAt = at(500);
        changed.updatedAt = at(9500);
        const auto updated = chats.updateConversation(changed);
        require(updated.success(), "Could not update a conversation");
        requireSameConversation(updated.value(), changed, "update must return the conversation as written");
        requireStoredConversation(chats, changed, "An update must replace every field");
        requireStoredConversation(chats, plainA, "An update must not touch another conversation");
        // The updated conversation is no longer pinned but is the newest of the unpinned ones.
        require(conversationIds(chats) == Ids {"chat-pinned-new", "chat-pinned-old", "chat-a", "chat-b"},
            "An update must be reflected in the listing order");

        ChatConversation badUpdate = changed;
        badUpdate.title = "Must not persist";
        badUpdate.modelId = "model-unknown";
        const auto refusedUpdate = chats.updateConversation(badUpdate);
        require(!refusedUpdate.success() && refusedUpdate.error()->code == "persistence.write_failed",
            "An update to an unknown model must be refused as a failed write");
        requireStoredConversation(chats, changed, "A refused update changed the conversation");
        const auto missingUpdate = chats.updateConversation(makeConversation("chat-missing", 1000));
        require(!missingUpdate.success() && missingUpdate.error()->code == "chat.not_found",
            "Updating a missing conversation must report chat.not_found");
    }

    // Removal deletes the conversation and its messages only.
    const auto database = workspace.open();
    SqliteChatRepository chats(database);
    require(chats.insertMessage(makeMessage("message-a1", "chat-a", 1100)).success()
            && chats.insertMessage(makeMessage("message-a2", "chat-a", 1200)).success()
            && chats.insertMessage(makeMessage("message-b1", "chat-b", 1100)).success(),
        "Could not insert messages");
    require(chats.removeConversation("chat-a").success(), "Could not remove a conversation");
    const auto gone = chats.findConversation("chat-a");
    require(gone.success() && !gone.value().has_value(), "A removed conversation must disappear");
    require(messageIds(chats, "chat-a").empty(), "Removing a conversation must remove its messages");
    const auto goneMessage = chats.findMessage("message-a1");
    require(goneMessage.success() && !goneMessage.value().has_value(), "A removed conversation's message must disappear");
    require(messageIds(chats, "chat-b") == Ids {"message-b1"}, "Removal must keep other conversations' messages");
    const auto again = chats.removeConversation("chat-a");
    require(!again.success() && again.error()->code == "chat.not_found",
        "Removing a missing conversation must report chat.not_found");
    require(conversationIds(chats) == Ids {"chat-pinned-new", "chat-pinned-old", "chat-b"},
        "Only the removed conversation may disappear from the listing");
}

void testChatMessagesRoundTripOrderingAndErrors()
{
    Workspace workspace;
    ChatMessage user = makeMessage("message-user", "chat-1", 2000);
    ChatMessage assistant = makeMessage("message-assistant", "chat-1", 2000);
    assistant.role = ChatRole::Assistant;
    assistant.status = ChatMessageStatus::Generating;
    assistant.inputTokens = 5'000'000'000ULL;
    assistant.outputTokens = 4'000'000'000ULL;
    assistant.costMicros = 3'000'000'000ULL;
    assistant.latencyMs = 2'000'000'000ULL;
    assistant.providerId = "provider-2";
    assistant.modelId = "model-2";
    ChatMessage system = makeMessage("message-system", "chat-1", 1000);
    system.role = ChatRole::System;
    system.status = ChatMessageStatus::Failed;
    ChatMessage error = makeMessage("message-error", "chat-1", 3000);
    error.role = ChatRole::Error;
    error.status = ChatMessageStatus::Stopped;
    error.content.clear();
    const ChatMessage other = makeMessage("message-other", "chat-2", 1500);
    using Ids = std::vector<std::string>;
    const std::initializer_list<const ChatMessage*> messagesInInsertOrder {&error, &user, &other, &assistant, &system};
    {
        const auto database = workspace.open();
        addCatalogAndProjects(database, workspace);
        SqliteChatRepository chats(database);
        require(chats.insertConversation(makeConversation("chat-1", 1000)).success()
                && chats.insertConversation(makeConversation("chat-2", 1000)).success(),
            "Could not insert conversations");
        for (const ChatMessage* value : messagesInInsertOrder) {
            const auto saved = chats.insertMessage(*value);
            require(saved.success(), "Could not insert a message");
            requireSameMessage(saved.value(), *value, "insert must return the message as written");
        }
        for (const ChatMessage* value : messagesInInsertOrder) {
            requireStoredMessage(chats, *value, "A message must round-trip every field");
        }
        require(messageIds(chats, "chat-1") == Ids {"message-system", "message-assistant", "message-user", "message-error"},
            "Messages must be listed by creation time and then id");
        require(messageIds(chats, "chat-2") == Ids {"message-other"}, "A listing must hold only its own conversation");
        require(messageIds(chats, "chat-unknown").empty(), "An unknown conversation must list no messages");
        const auto missing = chats.findMessage("message-missing");
        require(missing.success() && !missing.value().has_value(), "A missing message must read as absent");

        // Refused inserts leave nothing behind.
        const auto repeated = chats.insertMessage(makeMessage("message-user", "chat-2", 9000));
        require(!repeated.success() && repeated.error()->code == "persistence.write_failed",
            "A repeated message id must be refused as a failed write");
        const auto orphan = chats.insertMessage(makeMessage("message-orphan", "chat-unknown", 9000));
        require(!orphan.success() && orphan.error()->code == "persistence.write_failed",
            "A message of an unknown conversation must be refused as a failed write");
        ChatMessage withContext = makeMessage("message-context", "chat-1", 9000);
        withContext.role = ChatRole::Assistant;
        withContext.projectContext = ChatProjectContextAttachment {};
        const auto refusedContext = chats.insertMessage(withContext);
        require(!refusedContext.success() && refusedContext.error()->code == "chat.context_requires_prepared_turn",
            "A message with a project context must report chat.context_requires_prepared_turn");
        require(messageIds(chats, "chat-1").size() == 4 && messageIds(chats, "chat-2").size() == 1,
            "Refused inserts must not add rows");
        requireStoredMessage(chats, user, "A refused insert altered an existing message");

        // update() replaces every field, including the conversation and the creation time.
        ChatMessage changed = assistant;
        changed.conversationId = "chat-2";
        changed.role = ChatRole::Error;
        changed.status = ChatMessageStatus::Failed;
        changed.content = "Changed";
        changed.inputTokens = 1;
        changed.outputTokens = 2;
        changed.costMicros = 3;
        changed.latencyMs = 4;
        changed.providerId.clear();
        changed.modelId.clear();
        changed.createdAt = at(500);
        const auto updated = chats.updateMessage(changed);
        require(updated.success(), "Could not update a message");
        requireSameMessage(updated.value(), changed, "update must return the message as written");
        requireStoredMessage(chats, changed, "An update must replace every field and read empty ids back as empty");
        require(messageIds(chats, "chat-2") == Ids {"message-assistant", "message-other"},
            "A moved message must be listed under its new conversation");

        ChatMessage badUpdate = changed;
        badUpdate.content = "Must not persist";
        badUpdate.conversationId = "chat-unknown";
        const auto refusedUpdate = chats.updateMessage(badUpdate);
        require(!refusedUpdate.success() && refusedUpdate.error()->code == "persistence.write_failed",
            "An update to an unknown conversation must be refused as a failed write");
        requireStoredMessage(chats, changed, "A refused update changed the message");
        const auto missingUpdate = chats.updateMessage(makeMessage("message-missing", "chat-1", 1000));
        require(!missingUpdate.success() && missingUpdate.error()->code == "chat.message_not_found",
            "Updating a missing message must report chat.message_not_found");
    }

    // An unknown stored role or status fails closed instead of being skipped.
    workspace.sql("PRAGMA ignore_check_constraints=1; UPDATE chat_messages SET role='mystery' WHERE id='message-user';");
    workspace.sql("PRAGMA ignore_check_constraints=1; UPDATE chat_messages SET status='mystery' WHERE id='message-error';");
    const auto reopened = workspace.open();
    SqliteChatRepository chats(reopened);
    for (const char* id : {"message-user", "message-error"}) {
        const auto bad = chats.findMessage(id);
        require(!bad.success() && bad.error()->code == "persistence.invalid_chat_enum",
            "An unknown stored role or status must fail closed on findMessage");
    }
    const auto everything = chats.listMessages("chat-1");
    require(!everything.success() && everything.error()->code == "persistence.invalid_chat_enum",
        "A listing that contains an invalid message must fail rather than skip it");
    const auto onlyGood = chats.listMessages("chat-2");
    require(onlyGood.success() && onlyGood.value().size() == 2, "A listing of a healthy conversation must still work");
}

void testChatPreparedTurnIsAtomic()
{
    Workspace workspace;
    const auto database = workspace.open();
    addCatalogAndProjects(database, workspace);
    SqliteChatRepository chats(database);
    ChatConversation conversation = makeConversation("chat-1", 1000);
    conversation.projectId = "project-1";
    require(chats.insertConversation(conversation).success(), "Could not insert a conversation");
    require(chats.insertMessage(makeMessage("message-existing", "chat-1", 1100)).success(),
        "Could not insert a message");

    const auto makeTurn = [](const char* userId, const char* assistantId) {
        PreparedChatTurn turn;
        turn.userMessage = makeMessage(userId, "chat-1", 2000);
        turn.assistantMessage = makeMessage(assistantId, "chat-1", 2001);
        turn.assistantMessage.role = ChatRole::Assistant;
        turn.assistantMessage.status = ChatMessageStatus::Generating;
        turn.assistantMessage.content.clear();
        turn.assistantMessage.providerId = "provider-1";
        turn.assistantMessage.modelId = "model-1";
        return turn;
    };
    const auto requireUntouched = [&](const char* message) {
        requireStoredConversation(chats, conversation, message);
        require(messageIds(chats, "chat-1") == std::vector<std::string> {"message-existing"}, message);
    };
    ChatConversation next = conversation;
    next.preview = "The new turn";
    next.inputTokens = 21;
    next.updatedAt = at(2500);

    // Identity and role violations are refused before anything is written.
    PreparedChatTurn wrongConversation = makeTurn("turn-user", "turn-assistant");
    wrongConversation.userMessage.conversationId = "chat-2";
    PreparedChatTurn wrongUserRole = makeTurn("turn-user", "turn-assistant");
    wrongUserRole.userMessage.role = ChatRole::Assistant;
    PreparedChatTurn wrongAssistantRole = makeTurn("turn-user", "turn-assistant");
    wrongAssistantRole.assistantMessage.role = ChatRole::Error;
    PreparedChatTurn userWithContext = makeTurn("turn-user", "turn-assistant");
    userWithContext.userMessage.projectContext = ChatProjectContextAttachment {};
    for (const PreparedChatTurn* turn : {&wrongConversation, &wrongUserRole, &wrongAssistantRole, &userWithContext}) {
        const auto refused = chats.insertPreparedTurn(next, *turn);
        require(!refused.success() && refused.error()->code == "chat.invalid_prepared_turn",
            "A prepared turn with a wrong identity or role must report chat.invalid_prepared_turn");
        requireUntouched("A refused prepared turn changed stored state");
    }

    // A failure after the first insert rolls the whole turn back, including the conversation update.
    const auto duplicateAssistant = chats.insertPreparedTurn(next, makeTurn("turn-user", "message-existing"));
    require(!duplicateAssistant.success() && duplicateAssistant.error()->code == "persistence.write_failed",
        "A repeated assistant id must be refused as a failed write");
    requireUntouched("A failed prepared turn left its first message or a conversation change behind");
    ChatConversation orphanModel = next;
    orphanModel.modelId = "model-unknown";
    const auto failedUpdate = chats.insertPreparedTurn(orphanModel, makeTurn("turn-user", "turn-assistant"));
    require(!failedUpdate.success() && failedUpdate.error()->code == "persistence.write_failed",
        "A conversation update that violates a reference must be refused as a failed write");
    requireUntouched("A failed conversation update left the turn's messages behind");
    ChatConversation missingConversation = next;
    missingConversation.id = "chat-missing";
    PreparedChatTurn orphanTurn = makeTurn("turn-user", "turn-assistant");
    orphanTurn.userMessage.conversationId = orphanTurn.assistantMessage.conversationId = "chat-missing";
    const auto orphan = chats.insertPreparedTurn(missingConversation, orphanTurn);
    require(!orphan.success() && orphan.error()->code == "persistence.write_failed",
        "A turn of an unknown conversation must be refused as a failed write");
    requireUntouched("A turn of an unknown conversation left rows behind");

    // The accepted turn stores both messages and the conversation change together.
    const PreparedChatTurn accepted = makeTurn("turn-user", "turn-assistant");
    const auto inserted = chats.insertPreparedTurn(next, accepted);
    require(inserted.success(), "Could not insert a prepared turn");
    requireSameMessage(inserted.value().userMessage, accepted.userMessage, "insert must return the user message");
    requireSameMessage(inserted.value().assistantMessage, accepted.assistantMessage, "insert must return the assistant message");
    requireStoredConversation(chats, next, "A prepared turn must update the conversation");
    requireStoredMessage(chats, accepted.userMessage, "A prepared turn must store the user message");
    requireStoredMessage(chats, accepted.assistantMessage, "A prepared turn must store the assistant message");
    require(messageIds(chats, "chat-1") == std::vector<std::string> {"message-existing", "turn-user", "turn-assistant"},
        "The turn's messages must follow the existing ones in creation order");
}

ChatMemorySegment makeChatMemory(std::string id, std::string conversationId, std::int64_t createdMs)
{
    ChatMemorySegment value;
    value.id = std::move(id);
    value.conversationId = std::move(conversationId);
    value.firstMessageId = "message-first";
    value.lastMessageId = "message-last";
    value.summary = "Summary \"quoted\"\nsecond line \xE6\xB8\xAC\xE8\xA9\xA6";
    value.sourceTokenCount = 5'000'000'000ULL;
    value.summaryTokenCount = 4'000'000'000ULL;
    value.createdAt = at(createdMs);
    return value;
}

void requireSameChatMemory(const ChatMemorySegment& actual, const ChatMemorySegment& expected, const char* message)
{
    require(actual.id == expected.id && actual.conversationId == expected.conversationId
            && actual.firstMessageId == expected.firstMessageId && actual.lastMessageId == expected.lastMessageId
            && actual.summary == expected.summary && actual.sourceTokenCount == expected.sourceTokenCount
            && actual.summaryTokenCount == expected.summaryTokenCount && actual.createdAt == expected.createdAt,
        message);
}

std::vector<std::string> chatMemoryIds(const SqliteChatMemoryRepository& memory, const std::string& conversationId)
{
    const auto listed = memory.listMemorySegments(conversationId);
    require(listed.success(), "Could not list chat memory segments");
    std::vector<std::string> result;
    for (const auto& value : listed.value()) result.push_back(value.id);
    return result;
}

void testChatMemorySegmentsRoundTripOrderingRemovalAndErrors()
{
    Workspace workspace;
    ChatMemorySegment early = makeChatMemory("segment-early", "chat-1", 1000);
    early.firstMessageId = "message-1";
    early.lastMessageId = "message-9";
    const ChatMemorySegment tieB = makeChatMemory("segment-b", "chat-1", 2000);
    const ChatMemorySegment tieA = makeChatMemory("segment-a", "chat-1", 2000);
    const ChatMemorySegment other = makeChatMemory("segment-other", "chat-2", 1500);
    const std::initializer_list<const ChatMemorySegment*> inInsertOrder {&tieB, &early, &other, &tieA};
    using Ids = std::vector<std::string>;
    {
        const auto database = workspace.open();
        addCatalogAndProjects(database, workspace);
        SqliteChatRepository chats(database);
        require(chats.insertConversation(makeConversation("chat-1", 1000)).success()
                && chats.insertConversation(makeConversation("chat-2", 1000)).success(),
            "Could not insert conversations");
        SqliteChatMemoryRepository memory(database);
        for (const ChatMemorySegment* value : inInsertOrder) {
            const auto saved = memory.insertMemorySegment(*value);
            require(saved.success(), "Could not insert a memory segment");
            requireSameChatMemory(saved.value(), *value, "insert must return the segment as written");
        }
        for (const ChatMemorySegment* value : inInsertOrder) {
            const auto found = memory.findMemorySegment(value->id);
            require(found.success() && found.value().has_value(), "A stored segment must be found");
            requireSameChatMemory(*found.value(), *value, "A segment must round-trip every field");
        }
        require(chatMemoryIds(memory, "chat-1") == Ids {"segment-early", "segment-a", "segment-b"},
            "Segments must be listed by creation time and then id");
        require(chatMemoryIds(memory, "chat-2") == Ids {"segment-other"}, "A listing must hold only its own conversation");
        require(chatMemoryIds(memory, "chat-unknown").empty(), "An unknown conversation must list no segments");
        const auto missing = memory.findMemorySegment("segment-missing");
        require(missing.success() && !missing.value().has_value(), "A missing segment must read as absent");

        const auto repeated = memory.insertMemorySegment(makeChatMemory("segment-a", "chat-2", 9000));
        require(!repeated.success() && repeated.error()->code == "chat_memory.duplicate_id",
            "A repeated segment id must report chat_memory.duplicate_id");
        const auto orphan = memory.insertMemorySegment(makeChatMemory("segment-orphan", "chat-unknown", 9000));
        require(!orphan.success() && orphan.error()->code == "chat_memory.conversation_not_found",
            "A segment of an unknown conversation must report chat_memory.conversation_not_found");
        require(chatMemoryIds(memory, "chat-1").size() == 3 && chatMemoryIds(memory, "chat-2").size() == 1,
            "Refused inserts must not add rows");
        const auto stillA = memory.findMemorySegment("segment-a");
        requireSameChatMemory(*stillA.value(), tieA, "A refused insert altered an existing segment");

        // Removal is per conversation, and an empty conversation is not an error.
        require(memory.removeMemorySegments("chat-1").success(), "Could not remove a conversation's segments");
        require(chatMemoryIds(memory, "chat-1").empty(), "Removal must clear the conversation's segments");
        require(chatMemoryIds(memory, "chat-2") == Ids {"segment-other"}, "Removal must keep other conversations' segments");
        require(memory.removeMemorySegments("chat-1").success(), "Removing from an empty conversation must succeed");
        require(memory.removeMemorySegments("chat-unknown").success(), "Removing from an unknown conversation must succeed");
        require(memory.insertMemorySegment(makeChatMemory("segment-new", "chat-1", 5000)).success(),
            "The connection must be usable after refused writes");
    }

    const auto reopened = workspace.open();
    SqliteChatMemoryRepository memory(reopened);
    require(chatMemoryIds(memory, "chat-1") == Ids {"segment-new"}, "Segments must survive reopening the database");
    SqliteChatRepository chats(reopened);
    require(chats.removeConversation("chat-2").success(), "Could not remove a conversation");
    require(chatMemoryIds(memory, "chat-2").empty(), "Removing a conversation must remove its memory segments");
}

ProjectMemorySegment makeProjectMemory(std::string id, std::string projectId, std::int64_t createdMs)
{
    ProjectMemorySegment value;
    value.id = std::move(id);
    value.projectId = std::move(projectId);
    value.runId = "run-that-no-longer-exists";   // run_id is deliberately not a foreign key
    value.summary = "Summary \"quoted\"\nsecond line \xE6\xB8\xAC\xE8\xA9\xA6";
    value.sourceTokenCount = 5'000'000'000ULL;
    value.summaryTokenCount = 4'000'000'000ULL;
    value.createdAt = at(createdMs);
    return value;
}

void requireSameProjectMemory(const ProjectMemorySegment& actual, const ProjectMemorySegment& expected,
    const char* message)
{
    require(actual.id == expected.id && actual.projectId == expected.projectId && actual.runId == expected.runId
            && actual.summary == expected.summary && actual.sourceTokenCount == expected.sourceTokenCount
            && actual.summaryTokenCount == expected.summaryTokenCount && actual.createdAt == expected.createdAt,
        message);
}

std::vector<std::string> projectMemoryIds(const SqliteProjectMemoryRepository& memory, const std::string& projectId)
{
    const auto listed = memory.listMemorySegments(projectId);
    require(listed.success(), "Could not list project memory segments");
    std::vector<std::string> result;
    for (const auto& value : listed.value()) result.push_back(value.id);
    return result;
}

std::vector<std::string> recentProjectMemoryIds(
    const SqliteProjectMemoryRepository& memory, const std::string& projectId, std::size_t limit,
    std::uint64_t expectedTotal)
{
    const auto page = memory.listRecentMemorySegments(projectId, limit);
    require(page.success(), "Could not list recent project memory segments");
    require(page.value().totalCount == expectedTotal, "The recent page must report the project's total count");
    std::vector<std::string> result;
    for (const auto& value : page.value().newest) result.push_back(value.id);
    return result;
}

void testProjectMemorySegmentsRoundTripOrderingRecentPageAndErrors()
{
    Workspace workspace;
    const ProjectMemorySegment early = makeProjectMemory("segment-early", "project-1", 1000);
    const ProjectMemorySegment tieB = makeProjectMemory("segment-b", "project-1", 2000);
    const ProjectMemorySegment tieA = makeProjectMemory("segment-a", "project-1", 2000);
    const ProjectMemorySegment other = makeProjectMemory("segment-other", "project-2", 1500);
    const std::initializer_list<const ProjectMemorySegment*> inInsertOrder {&tieB, &early, &other, &tieA};
    using Ids = std::vector<std::string>;
    {
        const auto database = workspace.open();
        addCatalogAndProjects(database, workspace);
        SqliteProjectMemoryRepository memory(database);
        for (const ProjectMemorySegment* value : inInsertOrder) {
            const auto saved = memory.insertMemorySegment(*value);
            require(saved.success(), "Could not insert a memory segment");
            requireSameProjectMemory(saved.value(), *value, "insert must return the segment as written");
        }
        for (const ProjectMemorySegment* value : inInsertOrder) {
            const auto found = memory.findMemorySegment(value->id);
            require(found.success() && found.value().has_value(), "A stored segment must be found");
            requireSameProjectMemory(*found.value(), *value, "A segment must round-trip every field");
        }
        require(projectMemoryIds(memory, "project-1") == Ids {"segment-early", "segment-a", "segment-b"},
            "Segments must be listed oldest first, with the id as tie-break");
        require(projectMemoryIds(memory, "project-2") == Ids {"segment-other"}, "A listing must hold only its own project");
        require(projectMemoryIds(memory, "project-unknown").empty(), "An unknown project must list no segments");
        const auto missing = memory.findMemorySegment("segment-missing");
        require(missing.success() && !missing.value().has_value(), "A missing segment must read as absent");

        // The bounded read is newest first with the id descending as tie-break, plus the total count.
        require(recentProjectMemoryIds(memory, "project-1", 2, 3) == Ids {"segment-b", "segment-a"},
            "The recent page must hold the newest segments, newest first");
        require(recentProjectMemoryIds(memory, "project-1", 10, 3) == Ids {"segment-b", "segment-a", "segment-early"},
            "A limit above the count must return every segment, newest first");
        require(recentProjectMemoryIds(memory, "project-1", 3, 3) == Ids {"segment-b", "segment-a", "segment-early"},
            "A limit equal to the count must return every segment");
        require(recentProjectMemoryIds(memory, "project-1", 0, 3).empty(),
            "A zero limit must return no segments but still report the total");
        require(recentProjectMemoryIds(memory, "project-2", 5, 1) == Ids {"segment-other"},
            "The recent page must hold only its own project");
        require(recentProjectMemoryIds(memory, "project-unknown", 5, 0).empty(),
            "An unknown project must have an empty recent page");
        const auto huge = memory.listRecentMemorySegments("project-1", std::numeric_limits<std::size_t>::max());
        require(!huge.success() && huge.error()->code == "persistence.value_out_of_range",
            "A limit beyond the signed 64-bit range is currently refused");

        const auto repeated = memory.insertMemorySegment(makeProjectMemory("segment-a", "project-2", 9000));
        require(!repeated.success() && repeated.error()->code == "project_memory.duplicate_id",
            "A repeated segment id must report project_memory.duplicate_id");
        const auto orphan = memory.insertMemorySegment(makeProjectMemory("segment-orphan", "project-unknown", 9000));
        require(!orphan.success() && orphan.error()->code == "project_memory.project_not_found",
            "A segment of an unknown project must report project_memory.project_not_found");
        require(projectMemoryIds(memory, "project-1").size() == 3 && projectMemoryIds(memory, "project-2").size() == 1,
            "Refused inserts must not add rows");
        const auto stillA = memory.findMemorySegment("segment-a");
        requireSameProjectMemory(*stillA.value(), tieA, "A refused insert altered an existing segment");

        // Removal is per project, and an empty project is not an error.
        require(memory.removeMemorySegments("project-1").success(), "Could not remove a project's segments");
        require(projectMemoryIds(memory, "project-1").empty(), "Removal must clear the project's segments");
        require(recentProjectMemoryIds(memory, "project-1", 5, 0).empty(), "Removal must clear the recent page too");
        require(projectMemoryIds(memory, "project-2") == Ids {"segment-other"}, "Removal must keep other projects' segments");
        require(memory.removeMemorySegments("project-1").success(), "Removing from an empty project must succeed");
        require(memory.removeMemorySegments("project-unknown").success(), "Removing from an unknown project must succeed");
        require(memory.insertMemorySegment(makeProjectMemory("segment-new", "project-1", 5000)).success(),
            "The connection must be usable after refused writes");
    }

    const auto reopened = workspace.open();
    SqliteProjectMemoryRepository memory(reopened);
    require(projectMemoryIds(memory, "project-1") == Ids {"segment-new"}, "Segments must survive reopening the database");
}
} // namespace

int main()
{
    try {
        testChatConversationsRoundTripOrderingUpdateAndRemoval();
        testChatMessagesRoundTripOrderingAndErrors();
        testChatPreparedTurnIsAtomic();
        testChatMemorySegmentsRoundTripOrderingRemovalAndErrors();
        testProjectMemorySegmentsRoundTripOrderingRecentPageAndErrors();
        std::cout << "SQLite Conversations contracts: 5 passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SQLite Conversations contracts failed: " << error.what() << '\n';
        return 1;
    }
}
