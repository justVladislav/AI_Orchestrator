#pragma once

#include "ai/providers/IProviderAgentSession.hpp"
#include "ai/http/IHttpClient.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ai::providers {

// Multi-turn agent session adapter for the Anthropic Messages API.
// Maintains the full messages[] history in the session object so each turn
// sends the complete conversation context (Anthropic has no server-side
// stateful continuation primitive).
// Authentication: x-api-key header, never exposed in error messages.
class AnthropicAgentSession final : public IProviderAgentSession {
public:
    static constexpr const char* kDefaultEndpoint = "https://api.anthropic.com/v1/messages";
    static constexpr const char* kApiVersion = "2023-06-01";

    AnthropicAgentSession(
        std::string apiKey,
        std::string endpoint,
        std::shared_ptr<http::IHttpClient> httpClient);

    common::Result<ProviderAgentTurnResponse> beginSession(
        const ProviderAgentSessionRequest& request,
        std::stop_token stopToken) override;

    common::Result<ProviderAgentTurnResponse> submitToolResults(
        std::vector<application::AgentToolResult> results,
        std::stop_token stopToken,
        std::optional<NextTurnToolPolicy> nextTurnPolicy = std::nullopt) override;

private:
    enum class State { Idle, WaitingForToolResults, Terminal };

    // One accumulated conversation message: role + JSON-serialized content.
    // Storing contentJson as a string keeps nlohmann::json out of the header.
    struct StoredMessage {
        std::string role;
        std::string contentJson;  // JSON array or plain string
    };

    // Returns "agent_session.malformed_strict_schema" if any strict tool's
    // inputSchemaJson fails to parse or does not satisfy the strict JSON
    // Schema subset - never silently degrades a strict tool to best-effort.
    // policy, when non-null, restricts the returned tool array to the named
    // subset; nullptr means "no restriction".
    static common::Result<std::string> buildToolsJson(
        const std::vector<application::AgentToolDefinition>& tools,
        const NextTurnToolPolicy* policy = nullptr);

    std::string buildMessagesJson() const;

    common::Result<ProviderAgentTurnResponse> doTurn(
        const std::string& requestBody,
        std::stop_token stopToken,
        bool oneHourCacheCheckpointUsed);

    State state_ {State::Idle};
    std::size_t turnIndex_ {0};
    std::vector<application::AgentToolCall> pendingCalls_;
    std::vector<StoredMessage> messages_;

    // Saved at beginSession for use in subsequent turns
    std::string model_;
    std::string systemPrompt_;
    std::vector<application::AgentToolDefinition> tools_;
    std::string toolsJson_;
    std::uint32_t maxOutputTokens_ {4096};
    std::size_t maxToolCallsPerTurn_ {8};
    std::chrono::milliseconds turnTimeout_ {60'000};
    bool disallowParallelToolCalls_ {false};

    std::string apiKey_;
    std::string endpoint_;
    std::shared_ptr<http::IHttpClient> httpClient_;
};

} // namespace ai::providers
