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

// Multi-turn agent session adapter for the OpenAI Responses API.
// Uses previous_response_id for stateful server-side continuation so the
// full input history is never re-sent on subsequent turns.
// Authentication: Bearer token, never exposed in error messages.
class OpenAIAgentSession final : public IProviderAgentSession {
public:
    static constexpr const char* kDefaultEndpoint = "https://api.openai.com/v1/responses";

    OpenAIAgentSession(
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

    // Returns "agent_session.malformed_strict_schema" if any strict tool's
    // inputSchemaJson fails to parse or does not satisfy the strict JSON
    // Schema subset - never silently degrades a strict tool to best-effort.
    // policy, when non-null, restricts the returned tool array to the named
    // subset; nullptr means "no restriction".
    static common::Result<std::string> buildToolsJson(
        const std::vector<application::AgentToolDefinition>& tools,
        const NextTurnToolPolicy* policy = nullptr);

    common::Result<ProviderAgentTurnResponse> doTurn(
        const std::string& requestBody,
        std::stop_token stopToken);

    State state_ {State::Idle};
    std::size_t turnIndex_ {0};
    std::vector<application::AgentToolCall> pendingCalls_;
    std::string previousResponseId_;

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
