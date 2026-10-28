#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/catalog/ProviderCatalogTypes.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ai::application {

using ConversationId = std::string;
using ChatMessageId = std::string;

enum class ChatRole { User, Assistant, System, Error };
enum class ChatMessageStatus { Sent, Generating, Failed, Stopped };

inline constexpr std::string_view kChatProjectContextAttachmentSchemaVersion =
    "chat-project-context-attachment.v1";

// The exact Project Context text supplied to one Assistant generation. This is immutable provenance, not a
// pointer to a mutable Project: retrying the response reuses `renderedText` byte-for-byte. The snapshot
// fingerprint identifies the canonical PC-1 snapshot; renderedContentFingerprint protects the provider-facing
// wrapper as stored. The attachment is written atomically with its user/assistant turn.
struct ChatProjectContextAttachment {
    std::string schemaVersion {std::string(kChatProjectContextAttachmentSchemaVersion)};
    ChatMessageId assistantMessageId;
    ConversationId conversationId;
    ProjectId projectId;
    std::string snapshotFingerprint;
    std::string renderedContentFingerprint;
    std::string snapshotPolicyId;
    std::string workspaceFingerprint;
    std::optional<RunId> selectedRunId;
    bool includesDerivedProjectMemory {false};
    std::uint64_t snapshotEstimatedTokens {0};
    std::uint64_t renderedEstimatedTokens {0};
    std::string renderedText;

    [[nodiscard]] bool operator==(const ChatProjectContextAttachment&) const noexcept = default;
};

struct ChatConversation {
    ConversationId id;
    std::optional<ProjectId> projectId;
    ProviderId providerId;
    ModelId modelId;
    std::string title;
    std::string preview;
    bool pinned {false};
    bool archived {false};
    std::uint64_t inputTokens {0};
    std::uint64_t outputTokens {0};
    std::uint64_t estimatedCostMicros {0};
    std::uint64_t lastLatencyMs {0};
    std::chrono::system_clock::time_point createdAt;
    std::chrono::system_clock::time_point updatedAt;
};

struct ChatMessage {
    ChatMessageId id;
    ConversationId conversationId;
    ChatRole role {ChatRole::User};
    ChatMessageStatus status {ChatMessageStatus::Sent};
    std::string content;
    std::uint64_t inputTokens {0};
    std::uint64_t outputTokens {0};
    std::uint64_t costMicros {0};
    std::uint64_t latencyMs {0};
    // Snapshot the selection used for this response. User/system messages
    // leave both empty. This keeps per-model accounting truthful when a
    // conversation switches models between turns.
    ProviderId providerId;
    ModelId modelId;
    std::chrono::system_clock::time_point createdAt;
    // Read-only projection. Repositories accept a value here only through insertPreparedTurn(); updateMessage()
    // never changes the persisted attachment.
    std::optional<ChatProjectContextAttachment> projectContext;
};

// A summarized slice of conversation history. Segments are an additional
// memory layer on top of chat_messages — the source rows they cover are
// never modified or deleted by this feature.
using ChatMemorySegmentId = std::string;

struct ChatMemorySegment {
    ChatMemorySegmentId id;
    ConversationId conversationId;
    ChatMessageId firstMessageId;
    ChatMessageId lastMessageId;
    std::string summary;
    std::uint64_t sourceTokenCount {0};
    std::uint64_t summaryTokenCount {0};
    std::chrono::system_clock::time_point createdAt;
};

struct CreateConversationRequest {
    std::optional<ProjectId> projectId;
    ProviderId providerId;
    ModelId modelId;
    std::string title;
};

struct PreparedChatTurn {
    ChatMessage userMessage;
    ChatMessage assistantMessage;
};

struct ChatProjectContextQuote {
    ProjectId projectId;
    std::string snapshotFingerprint;
    // Opaque, single-use and short-lived: the only thing that lets a Chat turn carry this snapshot. It is bound to the
    // conversation, the snapshot, the selected model and the figures below; when any of them changes it goes stale.
    std::string quoteToken;
    ProviderId providerId;
    ModelId modelId;
    std::uint64_t estimatedInputTokens {0};
    // Conservative additional-input estimate: the larger configured plain/long-context input rate. Zero with
    // priceKnown=false means pricing was not configured, not that the request is free.
    std::uint64_t estimatedCostMicros {0};
    bool priceKnown {false};
};

[[nodiscard]] std::string_view toString(ChatRole role) noexcept;
[[nodiscard]] std::string_view toString(ChatMessageStatus status) noexcept;
[[nodiscard]] std::optional<ChatRole> chatRoleFromString(std::string_view value) noexcept;
[[nodiscard]] std::optional<ChatMessageStatus> chatMessageStatusFromString(std::string_view value) noexcept;

} // namespace ai::application
