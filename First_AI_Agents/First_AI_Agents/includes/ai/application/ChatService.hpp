#pragma once

#include "ai/application/IChatRepository.hpp"
#include "ai/application/IChatMemoryRepository.hpp"
#include "ai/application/catalog/IModelRepository.hpp"
#include "ai/application/projects/IProjectRepository.hpp"
#include "ai/application/IProviderClient.hpp"
#include "ai/application/catalog/IProviderRepository.hpp"
#include "ai/application/IWorkspaceStateRepository.hpp"
#include "ai/application/ProjectContextSnapshot.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <stop_token>

namespace ai::application {

class ModelExecutionService;
class ApplicationSettingsService;

class ChatService final {
public:
    using Clock = std::function<std::chrono::system_clock::time_point()>;
    // A project-context quote expires after this long, at most this many are kept (the oldest goes first).
    static constexpr std::chrono::minutes kProjectContextQuoteLifetime {15};
    static constexpr std::size_t kMaxProjectContextQuotes = 16;

    ChatService(IChatRepository&, IProjectRepository&, IWorkspaceStateRepository&,
                IProviderRepository&, IModelRepository&, IProviderClientFactory&,
                ModelExecutionService* modelExecutionService = nullptr,
                IChatMemoryRepository* chatMemoryRepository = nullptr,
                ApplicationSettingsService* settingsService = nullptr,
                Clock clock = {});

    [[nodiscard]] common::Result<std::vector<ChatConversation>> listConversations() const;
    [[nodiscard]] common::Result<std::vector<ChatMessage>> listMessages(const ConversationId&) const;
    common::Result<ChatConversation> createConversation(const CreateConversationRequest&);
    common::Result<ChatConversation> setSelection(const ConversationId&, const ProviderId&, const ModelId&);
    common::Result<ChatConversation> setProject(const ConversationId&, const std::optional<ProjectId>&);
    [[nodiscard]] common::Result<std::vector<Project>> listAvailableProjects() const;
    common::Result<void> removeConversation(const ConversationId&);
    common::Result<PreparedChatTurn> prepareTurn(const ConversationId&, std::string content);
    // Explicit opt-in path: quote -> confirm -> send. quoteProjectContext is provider-free and writes nothing but a
    // short-lived record of what it quoted; it returns an opaque single-use token. prepareTurnWithProjectContext
    // refuses (chat_context.quote_required / quote_expired / stale_quote) unless it is given the live token of a quote
    // made for this very conversation, snapshot, provider-facing bytes, model and figures, so a paid send with context
    // cannot be reached by a direct call that skipped the quote. The token is claimed (checked and spent in one step)
    // before anything is written, so two concurrent sends cannot share one confirmation. The already-previewed snapshot is sealed with the Assistant row; it is never
    // rebuilt during generation or retry. The snapshot project must still be the conversation's selected active project.
    common::Result<PreparedChatTurn> prepareTurnWithProjectContext(const ConversationId&, std::string content,
        const ProjectContextSnapshot&, const std::string& quoteToken);
    [[nodiscard]] common::Result<ChatProjectContextQuote> quoteProjectContext(
        const ConversationId&, const ProjectContextSnapshot&) const;
    common::Result<ChatMessage> generate(const ChatMessageId& assistantMessageId, std::stop_token);
    common::Result<ChatMessage> retry(const ChatMessageId& assistantMessageId, std::stop_token);
    common::Result<ChatMessage> editUserMessage(const ChatMessageId&, std::string content);
    common::Result<ChatConversation> clearUsage(const ConversationId&);

private:
    struct StoredQuote {
        ConversationId conversationId;
        ProjectId projectId;
        std::string snapshotFingerprint;
        std::string renderedContentFingerprint;   // the exact provider-facing bytes that were quoted
        ProviderId providerId;
        ModelId modelId;
        std::uint64_t estimatedInputTokens {0};
        std::uint64_t estimatedCostMicros {0};
        bool priceKnown {false};
        std::chrono::system_clock::time_point createdAt;
    };

    common::Result<PreparedChatTurn> prepareTurnImpl(
        const ConversationId&, std::string content, const ProjectContextSnapshot*, const std::string* quoteToken);
    [[nodiscard]] std::chrono::system_clock::time_point now() const;
    // Fail-closed: a stored attachment is sent again (Retry, first generation) only while it is still valid for the
    // conversation as it is NOW - the conversation selects the attachment's project and that project is Active.
    [[nodiscard]] common::Result<void> requireAttachmentAdmissible(
        const ChatConversation&, const ChatProjectContextAttachment&) const;
    // Validates AND consumes a quote in one step under the lock: whoever finds a live quote takes it, so two concurrent
    // sends can never share one confirmation, and a quote that is refused (expired / stale) is gone too. The quote is
    // spent before anything is written; if the write then fails the user simply quotes again.
    [[nodiscard]] common::Result<void> claimQuote(const std::string& token, const ChatConversation&,
        const std::string& snapshotFingerprint, const std::string& renderedContentFingerprint,
        std::uint64_t estimatedInputTokens, std::uint64_t estimatedCostMicros, bool priceKnown) const;
    IChatRepository& chats_;
    IProjectRepository& projects_;
    IWorkspaceStateRepository& workspace_;
    IProviderRepository& providers_;
    IModelRepository& models_;
    IProviderClientFactory& clients_;
    ModelExecutionService* modelExecutionService_;
    IChatMemoryRepository* chatMemoryRepository_;
    ApplicationSettingsService* settingsService_;
    Clock clock_;
    mutable std::mutex quoteMutex_;
    mutable std::map<std::string, StoredQuote> quotes_;
};

} // namespace ai::application
