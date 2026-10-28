#include "ai/application/conversations/ChatService.hpp"

#include "ai/application/conversations/ChatProjectContext.hpp"

#include "ai/application/settings/ApplicationSettingsService.hpp"
#include "ai/application/conversations/ChatContextAssembler.hpp"
#include "ai/application/budget/CostCalculation.hpp"
#include "ai/application/IdGenerator.hpp"
#include "ai/application/agents/ModelExecutionService.hpp"
#include "ai/application/Sha256.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <limits>
#include <sstream>

namespace ai::application {
namespace {

constexpr std::string_view kChatBaseSystemPrompt =
    "You are an AI assistant working inside AI Orchestrator. Be precise and practical.";

common::Error error(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

common::Result<void> requireContextCapacity(const Model& model, std::uint64_t additionalInputTokens)
{
    const std::uint64_t outputReserve = std::min<std::uint64_t>(
        model.maxOutputTokens > 0 ? model.maxOutputTokens : 4'096, 16'384);
    const std::uint64_t contextWindow = model.contextWindowTokens > 0 ? model.contextWindowTokens : 128'000;
    if (contextWindow <= outputReserve) {
        return common::Result<void>::failure(error(
            "chat.context_window_invalid", "Model context window is not larger than its response reserve"));
    }
    const std::uint64_t inputBudget = contextWindow - outputReserve;
    const std::uint64_t baseTokens = estimateChatTokens(kChatBaseSystemPrompt);
    if (baseTokens > inputBudget || additionalInputTokens > inputBudget - baseTokens) {
        return common::Result<void>::failure(error("chat_context.exceeds_model_window",
            "The attached Project Context Snapshot does not fit the selected model's input window"));
    }
    return common::Result<void>::ok();
}

struct QuoteFigures {
    std::uint64_t costMicros {0};
    bool priceKnown {false};
};

// The additional-input estimate of a quote: the larger configured plain / long-context input rate, so the number is
// an upper bound of the added input. Zero with priceKnown=false means "no price configured", not "free".
QuoteFigures quoteFiguresFor(const Model& model, std::uint64_t estimatedInputTokens)
{
    const std::uint64_t plainRate = model.inputPriceMicrosPerMillionTokens;
    const std::uint64_t longRate = model.longContextPricing.inputPriceMicrosPerMillionTokens;
    QuoteFigures figures;
    figures.priceKnown = plainRate != 0 && (!model.longContextPricing.enabled() || longRate != 0);
    if (figures.priceKnown) {
        const long double cost = static_cast<long double>(estimatedInputTokens)
            * static_cast<long double>(std::max(plainRate, longRate)) / 1'000'000.0L;
        figures.costMicros = static_cast<std::uint64_t>(std::ceil(cost));
    }
    return figures;
}

std::string trim(std::string value)
{
    const auto text = [](unsigned char ch) { return std::isspace(ch) == 0; };
    const auto first = std::find_if(value.begin(), value.end(), text);
    const auto last = std::find_if(value.rbegin(), value.rend(), text).base();
    return first < last ? std::string(first, last) : std::string {};
}

std::string titleFrom(std::string_view content)
{
    constexpr std::size_t limit = 52;
    std::string value(content.substr(0, std::min(content.size(), limit)));
    if (content.size() > limit) value += "...";
    return value.empty() ? "New chat" : value;
}

CostBreakdown costFor(const Model& model, const models::TokenUsage& usage)
{
    // MW-P2: the cost function correctly splits any cache-write/
    // cache-read tokens out of usage.inputTokens's conservative sum instead
    // of billing them all at the plain input rate. A chat answer is ONE
    // provider response, so it is classified against the model's long-context
    // threshold by its own input_tokens (M2b).
    return computeSingleResponseCostMicros(model, usage);
}

} // namespace

ChatService::ChatService(IChatRepository& chats, IProjectRepository& projects,
    IWorkspaceStateRepository& workspace, IProviderRepository& providers,
    IModelRepository& models, IProviderClientFactory& clients,
    ModelExecutionService* modelExecutionService,
    IChatMemoryRepository* chatMemoryRepository,
    ApplicationSettingsService* settingsService, Clock clock)
    : chats_(chats), projects_(projects), workspace_(workspace), providers_(providers),
      models_(models), clients_(clients), modelExecutionService_(modelExecutionService),
      chatMemoryRepository_(chatMemoryRepository), settingsService_(settingsService), clock_(std::move(clock))
{
}

common::Result<std::vector<ChatConversation>> ChatService::listConversations() const
{
    return chats_.listConversations();
}

common::Result<std::vector<ChatMessage>> ChatService::listMessages(const ConversationId& id) const
{
    return chats_.listMessages(id);
}

common::Result<ChatConversation> ChatService::createConversation(const CreateConversationRequest& request)
{
    auto provider = providers_.findById(request.providerId);
    auto model = models_.findById(request.modelId);
    if (!provider.success()) return common::Result<ChatConversation>::failure(*provider.error());
    if (!model.success()) return common::Result<ChatConversation>::failure(*model.error());
    if (!provider.value().has_value() || !provider.value()->enabled)
        return common::Result<ChatConversation>::failure(error("chat.provider_unavailable", "Selected provider is unavailable"));
    if (!model.value().has_value() || !model.value()->enabled || model.value()->providerId != request.providerId)
        return common::Result<ChatConversation>::failure(error("chat.model_unavailable", "Selected model is unavailable"));

    std::optional<ProjectId> projectId = request.projectId;
    if (!projectId.has_value()) {
        auto active = workspace_.activeProjectId();
        if (!active.success()) return common::Result<ChatConversation>::failure(*active.error());
        projectId = active.value();
    }
    if (projectId.has_value()) {
        auto project = projects_.findById(*projectId);
        if (!project.success()) return common::Result<ChatConversation>::failure(*project.error());
        if (!project.value().has_value())
            return common::Result<ChatConversation>::failure(error("chat.project_not_found", "Project not found"));
    }

    const auto now = std::chrono::system_clock::now();
    ChatConversation value;
    value.id = generateUuid();
    value.projectId = std::move(projectId);
    value.providerId = request.providerId;
    value.modelId = request.modelId;
    value.title = trim(request.title);
    if (value.title.empty()) value.title = "New chat";
    value.createdAt = now;
    value.updatedAt = now;
    return chats_.insertConversation(std::move(value));
}

common::Result<ChatConversation> ChatService::setSelection(
    const ConversationId& id, const ProviderId& providerId, const ModelId& modelId)
{
    auto found = chats_.findConversation(id);
    auto provider = providers_.findById(providerId);
    auto model = models_.findById(modelId);
    if (!found.success()) return common::Result<ChatConversation>::failure(*found.error());
    if (!provider.success()) return common::Result<ChatConversation>::failure(*provider.error());
    if (!model.success()) return common::Result<ChatConversation>::failure(*model.error());
    if (!found.value().has_value()) return common::Result<ChatConversation>::failure(error("chat.not_found", "Conversation not found"));
    if (!provider.value().has_value() || !provider.value()->enabled)
        return common::Result<ChatConversation>::failure(error("chat.provider_unavailable", "Selected provider is unavailable"));
    if (!model.value().has_value() || !model.value()->enabled || model.value()->providerId != providerId)
        return common::Result<ChatConversation>::failure(error("chat.model_unavailable", "Selected model is unavailable"));
    auto value = *found.value();
    value.providerId = providerId;
    value.modelId = modelId;
    value.updatedAt = std::chrono::system_clock::now();
    return chats_.updateConversation(std::move(value));
}

common::Result<ChatConversation> ChatService::setProject(
    const ConversationId& id, const std::optional<ProjectId>& projectId)
{
    auto found = chats_.findConversation(id);
    if (!found.success()) return common::Result<ChatConversation>::failure(*found.error());
    if (!found.value().has_value()) return common::Result<ChatConversation>::failure(error("chat.not_found", "Conversation not found"));
    if (projectId.has_value()) {
        auto project = projects_.findById(*projectId);
        if (!project.success()) return common::Result<ChatConversation>::failure(*project.error());
        if (!project.value().has_value() || project.value()->status != ProjectStatus::Active)
            return common::Result<ChatConversation>::failure(error("chat.project_unavailable", "Selected project is unavailable"));
    }
    auto value = *found.value();
    value.projectId = projectId;
    value.updatedAt = std::chrono::system_clock::now();
    return chats_.updateConversation(std::move(value));
}

common::Result<std::vector<Project>> ChatService::listAvailableProjects() const
{
    auto result = projects_.listAll();
    if (!result.success()) return result;
    auto values = result.value();
    values.erase(std::remove_if(values.begin(), values.end(), [](const Project& value) {
        return value.status != ProjectStatus::Active;
    }), values.end());
    return common::Result<std::vector<Project>>::ok(std::move(values));
}

common::Result<void> ChatService::removeConversation(const ConversationId& id)
{
    return chats_.removeConversation(id);
}

common::Result<PreparedChatTurn> ChatService::prepareTurn(const ConversationId& id, std::string content)
{
    return prepareTurnImpl(id, std::move(content), nullptr, nullptr);
}

common::Result<PreparedChatTurn> ChatService::prepareTurnWithProjectContext(const ConversationId& id,
    std::string content, const ProjectContextSnapshot& snapshot, const std::string& quoteToken)
{
    return prepareTurnImpl(id, std::move(content), &snapshot, &quoteToken);
}

std::chrono::system_clock::time_point ChatService::now() const
{
    return clock_ ? clock_() : std::chrono::system_clock::now();
}

common::Result<void> ChatService::requireAttachmentAdmissible(
    const ChatConversation& conversation, const ChatProjectContextAttachment& attachment) const
{
    const auto valid = validateChatProjectContextAttachment(attachment);
    if (!valid.success()) return valid;
    if (!conversation.projectId.has_value() || *conversation.projectId != attachment.projectId) {
        return common::Result<void>::failure(error("chat_context.project_mismatch",
            "This response used another project's context and its conversation now selects a different project"));
    }
    const auto project = projects_.findById(attachment.projectId);
    if (!project.success()) return common::Result<void>::failure(*project.error());
    if (!project.value().has_value() || project.value()->status != ProjectStatus::Active) {
        return common::Result<void>::failure(error("chat_context.project_unavailable",
            "The project whose context this response used is no longer active"));
    }
    return common::Result<void>::ok();
}

common::Result<void> ChatService::claimQuote(const std::string& token, const ChatConversation& conversation,
    const std::string& snapshotFingerprint, const std::string& renderedContentFingerprint,
    std::uint64_t estimatedInputTokens, std::uint64_t estimatedCostMicros, bool priceKnown) const
{
    std::lock_guard lock(quoteMutex_);
    const auto it = token.empty() ? quotes_.end() : quotes_.find(token);
    if (it == quotes_.end()) {
        return common::Result<void>::failure(error("chat_context.quote_required",
            "Quote the project context and confirm the quote before sending it"));
    }
    const StoredQuote quote = std::move(it->second);
    quotes_.erase(it);   // a quote confirms ONE message, whatever the outcome of this check
    if (now() - quote.createdAt >= kProjectContextQuoteLifetime) {
        return common::Result<void>::failure(error("chat_context.quote_expired",
            "The project-context quote has expired: quote it again"));
    }
    const bool same = quote.conversationId == conversation.id
        && conversation.projectId.has_value() && quote.projectId == *conversation.projectId
        && quote.snapshotFingerprint == snapshotFingerprint
        && quote.renderedContentFingerprint == renderedContentFingerprint
        && quote.providerId == conversation.providerId && quote.modelId == conversation.modelId
        && quote.estimatedInputTokens == estimatedInputTokens
        && quote.estimatedCostMicros == estimatedCostMicros && quote.priceKnown == priceKnown;
    if (!same) {
        return common::Result<void>::failure(error("chat_context.stale_quote",
            "What was quoted no longer matches this conversation, the context bytes, the model or the price: quote it again"));
    }
    return common::Result<void>::ok();
}

common::Result<ChatProjectContextQuote> ChatService::quoteProjectContext(
    const ConversationId& id, const ProjectContextSnapshot& snapshot) const
{
    const auto conversation = chats_.findConversation(id);
    if (!conversation.success()) return common::Result<ChatProjectContextQuote>::failure(*conversation.error());
    if (!conversation.value().has_value()) return common::Result<ChatProjectContextQuote>::failure(
        error("chat.not_found", "Conversation not found"));
    if (!conversation.value()->projectId.has_value() || *conversation.value()->projectId != snapshot.projectId) {
        return common::Result<ChatProjectContextQuote>::failure(error(
            "chat_context.project_mismatch", "The previewed snapshot is not for this conversation's project"));
    }
    const auto project = projects_.findById(snapshot.projectId);
    if (!project.success()) return common::Result<ChatProjectContextQuote>::failure(*project.error());
    if (!project.value().has_value() || project.value()->status != ProjectStatus::Active) {
        return common::Result<ChatProjectContextQuote>::failure(error(
            "chat_context.project_unavailable", "The snapshot's project is no longer active"));
    }
    const auto attachment = makeChatProjectContextAttachment("quote-assistant", id, snapshot);
    if (!attachment.success()) return common::Result<ChatProjectContextQuote>::failure(*attachment.error());
    const auto model = models_.findById(conversation.value()->modelId);
    if (!model.success()) return common::Result<ChatProjectContextQuote>::failure(*model.error());
    if (!model.value().has_value()) return common::Result<ChatProjectContextQuote>::failure(
        error("chat.selection_missing", "The selected model no longer exists"));
    const auto capacity = requireContextCapacity(*model.value(), attachment.value().renderedEstimatedTokens);
    if (!capacity.success()) return common::Result<ChatProjectContextQuote>::failure(*capacity.error());

    const QuoteFigures figures = quoteFiguresFor(*model.value(), attachment.value().renderedEstimatedTokens);
    ChatProjectContextQuote quote;
    quote.projectId = snapshot.projectId;
    quote.snapshotFingerprint = snapshot.fingerprint;
    quote.providerId = conversation.value()->providerId;
    quote.modelId = conversation.value()->modelId;
    quote.estimatedInputTokens = attachment.value().renderedEstimatedTokens;
    quote.estimatedCostMicros = figures.costMicros;
    quote.priceKnown = figures.priceKnown;
    quote.quoteToken = generateUuid();
    {
        std::lock_guard lock(quoteMutex_);
        const auto current = now();
        for (auto it = quotes_.begin(); it != quotes_.end();) {
            it = current - it->second.createdAt >= kProjectContextQuoteLifetime ? quotes_.erase(it) : std::next(it);
        }
        while (quotes_.size() >= kMaxProjectContextQuotes) {
            quotes_.erase(std::min_element(quotes_.begin(), quotes_.end(), [](const auto& left, const auto& right) {
                return left.second.createdAt < right.second.createdAt;
            }));
        }
        quotes_[quote.quoteToken] = StoredQuote {id, quote.projectId, quote.snapshotFingerprint,
            attachment.value().renderedContentFingerprint, quote.providerId, quote.modelId, quote.estimatedInputTokens,
            quote.estimatedCostMicros, quote.priceKnown, current};
    }
    return common::Result<ChatProjectContextQuote>::ok(std::move(quote));
}

common::Result<PreparedChatTurn> ChatService::prepareTurnImpl(const ConversationId& id, std::string content,
    const ProjectContextSnapshot* snapshot, const std::string* quoteToken)
{
    content = trim(std::move(content));
    if (content.empty()) return common::Result<PreparedChatTurn>::failure(error("chat.empty_message", "Message cannot be empty"));
    auto found = chats_.findConversation(id);
    if (!found.success()) return common::Result<PreparedChatTurn>::failure(*found.error());
    if (!found.value().has_value()) return common::Result<PreparedChatTurn>::failure(error("chat.not_found", "Conversation not found"));

    const auto now = std::chrono::system_clock::now();
    ChatMessage user;
    user.id = generateUuid(); user.conversationId = id; user.role = ChatRole::User;
    user.status = ChatMessageStatus::Sent; user.content = content; user.createdAt = now;
    ChatMessage assistant;
    assistant.id = generateUuid(); assistant.conversationId = id; assistant.role = ChatRole::Assistant;
    assistant.status = ChatMessageStatus::Generating;
    // Strictly after the user message: listMessages orders by (created_at_ms, id),
    // and an equal timestamp lets the random UUID decide the pair order. That both
    // rendered the reply above the question and cut the new user message out of
    // the generation prompt (generate() stops history at the assistant row).
    assistant.createdAt = now + std::chrono::milliseconds {1};
    assistant.providerId = found.value()->providerId; assistant.modelId = found.value()->modelId;
    if (snapshot != nullptr) {
        if (!found.value()->projectId.has_value() || *found.value()->projectId != snapshot->projectId) {
            return common::Result<PreparedChatTurn>::failure(error("chat_context.project_mismatch",
                "The previewed Project Context Snapshot is not for this conversation's selected project"));
        }
        const auto project = projects_.findById(snapshot->projectId);
        if (!project.success()) return common::Result<PreparedChatTurn>::failure(*project.error());
        if (!project.value().has_value() || project.value()->status != ProjectStatus::Active) {
            return common::Result<PreparedChatTurn>::failure(error(
                "chat_context.project_unavailable", "The snapshot's project is no longer active"));
        }
        auto attachment = makeChatProjectContextAttachment(assistant.id, assistant.conversationId, *snapshot);
        if (!attachment.success()) return common::Result<PreparedChatTurn>::failure(*attachment.error());
        const auto model = models_.findById(found.value()->modelId);
        if (!model.success()) return common::Result<PreparedChatTurn>::failure(*model.error());
        if (!model.value().has_value()) return common::Result<PreparedChatTurn>::failure(
            error("chat.selection_missing", "The selected model no longer exists"));
        const std::uint64_t attachmentAndMessageTokens = attachment.value().renderedEstimatedTokens
            + estimateChatTokens(content);
        const auto capacity = requireContextCapacity(*model.value(), attachmentAndMessageTokens);
        if (!capacity.success()) return common::Result<PreparedChatTurn>::failure(*capacity.error());
        // The confirmed quote: this very conversation, snapshot, provider-facing bytes, model and figures, still live. It
        // is claimed (consumed) here, atomically with its check, before anything is written. A direct call that skipped
        // the quote never reaches the provider.
        const QuoteFigures figures = quoteFiguresFor(*model.value(), attachment.value().renderedEstimatedTokens);
        const auto confirmed = claimQuote(quoteToken != nullptr ? *quoteToken : std::string {}, *found.value(),
            snapshot->fingerprint, attachment.value().renderedContentFingerprint,
            attachment.value().renderedEstimatedTokens, figures.costMicros, figures.priceKnown);
        if (!confirmed.success()) return common::Result<PreparedChatTurn>::failure(*confirmed.error());
        assistant.projectContext = std::move(attachment.value());
    }

    auto conversation = *found.value();
    if (conversation.preview.empty()) conversation.title = titleFrom(content);
    conversation.preview = content;
    conversation.updatedAt = now;
    return chats_.insertPreparedTurn(std::move(conversation), {std::move(user), std::move(assistant)});
}

common::Result<ChatMessage> ChatService::generate(const ChatMessageId& assistantId, std::stop_token stopToken)
{
    auto messageResult = chats_.findMessage(assistantId);
    if (!messageResult.success()) return common::Result<ChatMessage>::failure(*messageResult.error());
    if (!messageResult.value().has_value()) return common::Result<ChatMessage>::failure(error("chat.message_not_found", "Message not found"));
    ChatMessage assistant = *messageResult.value();
    auto conversationResult = chats_.findConversation(assistant.conversationId);
    if (!conversationResult.success()) return common::Result<ChatMessage>::failure(*conversationResult.error());
    if (!conversationResult.value().has_value()) return common::Result<ChatMessage>::failure(error("chat.not_found", "Conversation not found"));
    ChatConversation conversation = *conversationResult.value();

    // From here on the Assistant row exists and is "Generating". Whatever goes wrong before the provider is called turns
    // it into a readable Error row that says why (Retry can try again); a bare early return would leave it Generating
    // for good.
    const auto failBeforeProvider = [&](const common::Error& cause) {
        assistant.role = ChatRole::Error;
        assistant.status = ChatMessageStatus::Failed;
        assistant.content = cause.message;
        const auto saved = chats_.updateMessage(assistant);
        if (!saved.success()) return common::Result<ChatMessage>::failure(*saved.error());
        return common::Result<ChatMessage>::failure(cause);
    };

    if (assistant.projectContext.has_value()) {
        const auto admissible = requireAttachmentAdmissible(conversation, *assistant.projectContext);
        if (!admissible.success()) return failBeforeProvider(*admissible.error());
    }

    const ProviderId requestProviderId = assistant.providerId.empty() ? conversation.providerId : assistant.providerId;
    const ModelId requestModelId = assistant.modelId.empty() ? conversation.modelId : assistant.modelId;
    auto providerResult = providers_.findById(requestProviderId);
    auto modelResult = models_.findById(requestModelId);
    if (!providerResult.success()) return failBeforeProvider(*providerResult.error());
    if (!modelResult.success()) return failBeforeProvider(*modelResult.error());
    if (!providerResult.value().has_value() || !modelResult.value().has_value())
        return failBeforeProvider(error("chat.selection_missing", "Provider or model no longer exists"));
    const Provider& provider = *providerResult.value();
    const Model& model = *modelResult.value();

    std::string systemPrompt(kChatBaseSystemPrompt);
    if (assistant.projectContext.has_value()) {
        const auto valid = validateChatProjectContextAttachment(*assistant.projectContext);
        if (!valid.success()) return failBeforeProvider(*valid.error());
        systemPrompt += "\n\n" + assistant.projectContext->renderedText;
    } else if (conversation.projectId.has_value()) {
        auto project = projects_.findById(*conversation.projectId);
        if (project.success() && project.value().has_value()) {
            systemPrompt += "\nProject: " + project.value()->name;
            if (!project.value()->description.empty()) systemPrompt += "\nProject description: " + project.value()->description;
        }
    }

    std::uint32_t historyLimit = 200;
    if (settingsService_ != nullptr) {
        const auto settings = settingsService_->get();
        if (!settings.success()) return failBeforeProvider(*settings.error());
        historyLimit = std::max<std::uint32_t>(1, settings.value().chatHistoryLimit);
    }

    const std::uint32_t requestMaxOutputTokens = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(model.maxOutputTokens > 0 ? model.maxOutputTokens : 4096, 16384));
    const std::uint64_t contextWindow = model.contextWindowTokens > 0
        ? model.contextWindowTokens : 128'000;
    if (contextWindow <= requestMaxOutputTokens) {
        return failBeforeProvider(error(
            "chat.context_window_invalid", "Model context window is not larger than its response reserve"));
    }
    const std::uint64_t inputBudget = contextWindow - requestMaxOutputTokens;
    auto historyResult = chats_.listMessages(conversation.id);
    if (!historyResult.success()) return failBeforeProvider(*historyResult.error());
    std::vector<ChatMemorySegment> memorySegments;
    if (chatMemoryRepository_ != nullptr) {
        const auto segmentsResult = chatMemoryRepository_->listMemorySegments(conversation.id);
        if (!segmentsResult.success()) return failBeforeProvider(*segmentsResult.error());
        memorySegments = std::move(segmentsResult.value());
    }
    ChatContextAssemblyRequest contextRequest;
    contextRequest.systemPrompt = std::move(systemPrompt);
    contextRequest.messages = std::move(historyResult.value());
    contextRequest.stopBeforeMessageId = assistant.id;
    contextRequest.memorySegments = std::move(memorySegments);
    contextRequest.historyLimit = historyLimit;
    contextRequest.inputTokenBudget = inputBudget;
    auto contextResult = assembleChatContext(std::move(contextRequest));
    if (!contextResult.success()) return failBeforeProvider(*contextResult.error());
    systemPrompt = std::move(contextResult.value().systemPrompt);
    const std::string userContent = std::move(contextResult.value().userContent);
    ProviderChatRequest request {
        provider.id,
        model.id,
        model.apiModelId,
        systemPrompt,
        userContent,
        std::chrono::milliseconds {30000},
        requestMaxOutputTokens,
        0.2,
        true};

    std::optional<ModelExecutionId> executionId;
    if (modelExecutionService_ != nullptr) {
        const auto existing = modelExecutionService_->listExecutions(
            ModelExecutionSource::Chat, conversation.id);
        if (!existing.success()) return failBeforeProvider(*existing.error());

        CreateModelExecutionRequest create;
        create.source = ModelExecutionSource::Chat;
        create.sourceId = conversation.id;
        create.projectId = conversation.projectId;
        create.providerId = provider.id;
        create.modelId = model.id;
        create.role = "assistant";
        create.position = static_cast<std::uint32_t>(std::min<std::size_t>(
            existing.value().size(), std::numeric_limits<std::uint32_t>::max()));
        create.requestHash = sha256Hex(
            provider.id + "\n" + model.id + "\n" + model.apiModelId + "\n"
            + systemPrompt + "\n" + userContent);
        create.contextHash = sha256Hex(systemPrompt + "\n" + userContent);

        const auto created = modelExecutionService_->createExecution(create);
        if (!created.success()) return failBeforeProvider(*created.error());
        executionId = created.value().id;
        const auto begun = modelExecutionService_->startExecution(*executionId);
        if (!begun.success()) return failBeforeProvider(*begun.error());
    }

    auto clientResult = clients_.clientFor(provider.id, provider.kind);
    const auto started = std::chrono::steady_clock::now();
    common::Result<ProviderChatResponse> response = clientResult.success()
        ? clientResult.value()->send(request, stopToken)
        : common::Result<ProviderChatResponse>::failure(*clientResult.error());
    const auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();

    assistant.latencyMs = static_cast<std::uint64_t>(std::max<std::int64_t>(0, latency));
    ModelExecutionStatus executionStatus = ModelExecutionStatus::Completed;
    models::TokenUsage executionUsage;
    bool executionCostPriceKnown = true;
    std::string executionErrorCode;
    std::string executionErrorMessage;
    bool conversationTotalsChanged = false;
    if (stopToken.stop_requested()) {
        assistant.status = ChatMessageStatus::Stopped;
        assistant.content = "Generation stopped.";
        executionStatus = ModelExecutionStatus::Cancelled;
        executionErrorCode = "provider.cancelled";
        executionErrorMessage = "Generation stopped";
    } else if (!response.success()) {
        assistant.role = ChatRole::Error;
        assistant.status = ChatMessageStatus::Failed;
        assistant.content = response.error()->message;
        executionStatus = ModelExecutionStatus::Failed;
        executionErrorCode = response.error()->code;
        executionErrorMessage = response.error()->message;
    } else {
        assistant.status = ChatMessageStatus::Sent;
        assistant.content = response.value().content;
        assistant.inputTokens = response.value().usage.inputTokens;
        assistant.outputTokens = response.value().usage.outputTokens;
        const auto costBreakdown = costFor(model, response.value().usage);
        assistant.costMicros = costBreakdown.costMicros;
        executionCostPriceKnown = costBreakdown.priceKnown;
        executionUsage = response.value().usage;
        conversation.inputTokens += assistant.inputTokens;
        conversation.outputTokens += assistant.outputTokens;
        conversation.estimatedCostMicros += assistant.costMicros;
        conversation.lastLatencyMs = assistant.latencyMs;
        conversation.preview = assistant.content;
        conversation.updatedAt = std::chrono::system_clock::now();
        conversationTotalsChanged = true;   // written AFTER the answer itself is stored, below
    }

    // The answer comes first: it is what the provider was paid for. Saving the terminal row before anything secondary
    // means a later failure can never leave the row "Generating" or lose the answer and its attachment.
    const auto savedAssistant = chats_.updateMessage(assistant);
    if (!savedAssistant.success()) return savedAssistant;

    // Secondary writes (the conversation totals, the execution log): a failure is still reported, but the stored answer
    // stays readable and the execution log is settled anyway. If both fail, the first error is the one returned.
    std::optional<common::Error> secondaryFailure;
    if (conversationTotalsChanged) {
        const auto savedConversation = chats_.updateConversation(conversation);
        if (!savedConversation.success()) secondaryFailure = *savedConversation.error();
    }

    if (executionId.has_value()) {
        FinishModelExecutionRequest finish;
        finish.status = executionStatus;
        finish.outputContent = executionStatus == ModelExecutionStatus::Completed
            ? assistant.content
            : std::string {};
        finish.outputPreview = executionStatus == ModelExecutionStatus::Completed
            ? assistant.content.substr(0, 1000)
            : std::string {};
        finish.inputTokens = executionUsage.inputTokens;
        finish.outputTokens = executionUsage.outputTokens;
        finish.cacheCreationInputTokens = executionUsage.cacheCreationInputTokens;
        finish.cacheReadInputTokens = executionUsage.cacheReadInputTokens;
        finish.cacheCreationDuration = executionUsage.cacheCreationDuration;
        finish.costMicros = assistant.costMicros;
        finish.costPriceKnown = executionCostPriceKnown;
        finish.latencyMs = assistant.latencyMs;
        finish.errorCode = std::move(executionErrorCode);
        finish.errorMessage = std::move(executionErrorMessage);
        const auto logged = modelExecutionService_->finishExecution(*executionId, finish);
        if (!logged.success() && !secondaryFailure.has_value()) secondaryFailure = *logged.error();
    }
    if (secondaryFailure.has_value()) return common::Result<ChatMessage>::failure(*secondaryFailure);
    return savedAssistant;
}

common::Result<ChatMessage> ChatService::retry(const ChatMessageId& id, std::stop_token stopToken)
{
    auto found = chats_.findMessage(id);
    if (!found.success()) return common::Result<ChatMessage>::failure(*found.error());
    if (!found.value().has_value()) return common::Result<ChatMessage>::failure(error("chat.message_not_found", "Message not found"));
    auto value = *found.value();
    if (value.role != ChatRole::Assistant && value.role != ChatRole::Error)
        return common::Result<ChatMessage>::failure(error("chat.retry_invalid_role", "Only assistant responses can be retried"));
    if (value.projectContext.has_value()) {
        // The exact stored bytes are reused only while they are admissible for the conversation as it is now. A
        // refusal leaves the row exactly as it was.
        const auto conversation = chats_.findConversation(value.conversationId);
        if (!conversation.success()) return common::Result<ChatMessage>::failure(*conversation.error());
        if (!conversation.value().has_value()) return common::Result<ChatMessage>::failure(
            error("chat.not_found", "Conversation not found"));
        const auto admissible = requireAttachmentAdmissible(*conversation.value(), *value.projectContext);
        if (!admissible.success()) return common::Result<ChatMessage>::failure(*admissible.error());
    }
    value.role = ChatRole::Assistant;
    value.status = ChatMessageStatus::Generating;
    value.content.clear();
    value.inputTokens = value.outputTokens = value.costMicros = value.latencyMs = 0;
    auto saved = chats_.updateMessage(std::move(value));
    if (!saved.success()) return saved;
    return generate(id, stopToken);
}

common::Result<ChatMessage> ChatService::editUserMessage(const ChatMessageId& id, std::string content)
{
    auto found = chats_.findMessage(id);
    if (!found.success()) return common::Result<ChatMessage>::failure(*found.error());
    if (!found.value().has_value()) return common::Result<ChatMessage>::failure(error("chat.message_not_found", "Message not found"));
    auto value = *found.value();
    if (value.role != ChatRole::User) return common::Result<ChatMessage>::failure(error("chat.edit_invalid_role", "Only user messages can be edited"));
    value.content = trim(std::move(content));
    if (value.content.empty()) return common::Result<ChatMessage>::failure(error("chat.empty_message", "Message cannot be empty"));
    return chats_.updateMessage(std::move(value));
}

common::Result<ChatConversation> ChatService::clearUsage(const ConversationId& id)
{
    auto found = chats_.findConversation(id);
    if (!found.success()) return common::Result<ChatConversation>::failure(*found.error());
    if (!found.value().has_value()) return common::Result<ChatConversation>::failure(error("chat.not_found", "Conversation not found"));
    auto value = *found.value();
    value.inputTokens = value.outputTokens = value.estimatedCostMicros = value.lastLatencyMs = 0;
    return chats_.updateConversation(std::move(value));
}

} // namespace ai::application
