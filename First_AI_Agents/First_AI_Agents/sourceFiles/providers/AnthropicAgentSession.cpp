#include "ai/providers/AnthropicAgentSession.hpp"
#include "ai/application/budget/CacheDurationAccounting.hpp"
#include "ai/http/HttpRequest.hpp"
#include "ai/http/HttpResponse.hpp"
#include "ai/providers/AnthropicResponseParser.hpp"
#include "ai/providers/StrictSchemaValidation.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

namespace ai::providers {

namespace {

template <typename T>
common::Result<T> httpStatusError(int statusCode, const std::string& responseBody)
{
    // Anthropic returns {"error": {"type": ..., "message": ...}, "request_id":
    // ...} for non-2xx responses; without this, every failure collapsed into
    // an undiagnosable "Provider returned HTTP 400" with no way to tell a
    // rejected schema from a bad model id from anything else. Mirrors
    // OpenAIAgentSession.cpp's httpStatusError() and AnthropicProvider.cpp's
    // httpFailureMessage() - this codebase's other Anthropic client already
    // did this; the agent session had simply never been updated to match.
    const std::string apiMessage = anthropic_detail::extractAnthropicErrorMessage(responseBody);
    auto withDetail = [&](std::string message) {
        if (!apiMessage.empty()) message += ": " + apiMessage;
        return message;
    };
    if (statusCode == 401)
        return common::Result<T>::failure(
            {"provider.auth_failed", withDetail("Authentication failed")});
    if (statusCode == 429)
        return common::Result<T>::failure(
            {"provider.rate_limited", withDetail("Rate limit exceeded")});
    if (statusCode >= 500 && statusCode < 600)
        return common::Result<T>::failure(
            {"provider.server_error",
             withDetail("Provider returned HTTP " + std::to_string(statusCode))});
    return common::Result<T>::failure(
        {"provider.request_failed",
         withDetail("Provider returned HTTP " + std::to_string(statusCode))});
}

// MW-P2: marks a JSON value as an Anthropic prompt-cache checkpoint.
// Anthropic caches the request prefix up to and including a block carrying
// this key; a later request whose own prefix (up to its own checkpoint) is
// byte-identical gets that whole prefix served from cache at a steep
// discount instead of billed as fresh input. Ephemeral is Anthropic's only
// cache_control type today (a short TTL, refreshed by reuse).
enum class CacheCheckpointDuration { FiveMinutes, OneHour };

void markCacheCheckpoint(
    nlohmann::json& block,
    CacheCheckpointDuration duration = CacheCheckpointDuration::FiveMinutes)
{
    block["cache_control"] = {{"type", "ephemeral"}};
    // Omitting ttl is deliberately the compatibility form for Anthropic's
    // default five-minute cache. Never serialize "5m": old requests must stay
    // byte-for-byte compatible at that moving breakpoint.
    if (duration == CacheCheckpointDuration::OneHour) {
        block["cache_control"]["ttl"] = "1h";
    }
}

// MW-P2: the system prompt is identical on every turn of a session and is
// often large (task/project context) - a standing cache checkpoint here
// means it is billed once (cache-write) and then re-served from cache
// (cache-read, ~10x cheaper) for every remaining turn. Array-of-blocks is
// required to attach cache_control; a plain string is wire-equivalent
// without it.
nlohmann::json buildSystemJson(
    const std::string& systemPrompt,
    bool oneHourStablePrefix)
{
    nlohmann::json block = {{"type", "text"}, {"text", systemPrompt}};
    // Anthropic hashes tools -> system -> messages. A system breakpoint thus
    // also includes the tool catalog that precedes it. Suppress the one-hour
    // marker when a per-turn policy filtered that catalog; otherwise we would
    // pay to retain a dynamic tool prefix for an hour even though the tool
    // itself carries no marker.
    if (oneHourStablePrefix) {
        markCacheCheckpoint(block, CacheCheckpointDuration::OneHour);
    }
    return nlohmann::json::array({std::move(block)});
}

// MW-P2: the tool roster is also identical every turn - one checkpoint on
// the last tool caches the whole array the same way. Anthropic allows up to
// 4 cache_control breakpoints per request; this session uses at most 3
// (system, last tool, newest message), leaving headroom.
void markLastToolAsCacheCheckpoint(nlohmann::json& tools)
{
    if (tools.is_array() && !tools.empty() && tools.back().is_object())
        markCacheCheckpoint(tools.back(), CacheCheckpointDuration::OneHour);
}

common::Result<models::TokenUsage> parseAgentSessionUsage(
    const nlohmann::json& response,
    bool oneHourCacheCheckpointUsed)
{
    models::TokenUsage parsed;
    const auto usageIt = response.find("usage");
    if (usageIt == response.end()) {
        return common::Result<models::TokenUsage>::ok(parsed);
    }
    if (!usageIt->is_object()) {
        return common::Result<models::TokenUsage>::failure(
            {"provider.invalid_response", "Anthropic usage must be an object"});
    }

    const auto counter = [&](const nlohmann::json& object, const char* name)
        -> common::Result<std::uint64_t> {
        const auto found = object.find(name);
        if (found == object.end()) {
            return common::Result<std::uint64_t>::ok(0);
        }
        if (!found->is_number_unsigned()) {
            return common::Result<std::uint64_t>::failure(
                {"provider.invalid_response",
                 std::string("Anthropic usage counter is not an unsigned integer: ") + name});
        }
        return common::Result<std::uint64_t>::ok(found->get<std::uint64_t>());
    };

    const auto fresh = counter(*usageIt, "input_tokens");
    const auto output = counter(*usageIt, "output_tokens");
    const auto creation = counter(*usageIt, "cache_creation_input_tokens");
    const auto read = counter(*usageIt, "cache_read_input_tokens");
    for (const auto* result : {&fresh, &output, &creation, &read}) {
        if (!result->success()) {
            return common::Result<models::TokenUsage>::failure(*result->error());
        }
    }

    parsed.outputTokens = output.value();
    parsed.cacheCreationInputTokens = creation.value();
    parsed.cacheReadInputTokens = read.value();
    constexpr auto maximum = ~std::uint64_t {0};
    if (creation.value() > maximum - fresh.value()
        || read.value() > maximum - fresh.value() - creation.value()) {
        return common::Result<models::TokenUsage>::failure(
            {"provider.invalid_response", "Anthropic input usage counters overflow"});
    }
    parsed.inputTokens = fresh.value() + creation.value() + read.value();

    const auto durationIt = usageIt->find("cache_creation");
    if (durationIt != usageIt->end()) {
        if (!durationIt->is_object()) {
            return common::Result<models::TokenUsage>::failure(
                {"provider.invalid_response", "Anthropic cache_creation usage must be an object"});
        }
        const auto fiveMinutes = counter(*durationIt, "ephemeral_5m_input_tokens");
        const auto oneHour = counter(*durationIt, "ephemeral_1h_input_tokens");
        if (!fiveMinutes.success()) {
            return common::Result<models::TokenUsage>::failure(*fiveMinutes.error());
        }
        if (!oneHour.success()) {
            return common::Result<models::TokenUsage>::failure(*oneHour.error());
        }
        parsed.cacheCreationDuration.known = true;
        parsed.cacheCreationDuration.fiveMinuteTokens = fiveMinutes.value();
        parsed.cacheCreationDuration.oneHourTokens = oneHour.value();
    }
    // Once this request bought a one-hour checkpoint, a positive aggregate
    // cache write without the provider's TTL split cannot safely inherit the
    // legacy five-minute price. Keep the paid usage, but make every downstream
    // cost fail honest instead of presenting a five-minute estimate as exact.
    if (oneHourCacheCheckpointUsed && creation.value() > 0
        && durationIt == usageIt->end()) {
        parsed.accountingTrusted = false;
    }
    const auto durationProblem = application::checkCacheCreationDurationUsage(
        parsed.cacheCreationInputTokens, parsed.cacheCreationDuration);
    if (durationProblem != application::CacheCreationDurationUsageProblem::None) {
        return common::Result<models::TokenUsage>::failure({
            application::cacheCreationDurationUsageProblemCode(durationProblem),
            application::cacheCreationDurationUsageProblemMessage(durationProblem)});
    }
    return common::Result<models::TokenUsage>::ok(std::move(parsed));
}

common::Result<void> validateToolResultCorrelation(
    const std::vector<application::AgentToolCall>& pendingCalls,
    const std::vector<application::AgentToolResult>& results)
{
    if (results.empty())
        return common::Result<void>::failure(
            {"agent_session.empty_tool_results", "no tool results were submitted"});

    for (const auto& result : results) {
        const auto it = std::find_if(pendingCalls.begin(), pendingCalls.end(),
            [&](const application::AgentToolCall& c) { return c.callId == result.callId; });
        if (it == pendingCalls.end())
            return common::Result<void>::failure(
                {"agent_session.unknown_tool_result",
                 "result references an unknown callId"});
        if (it->toolName != result.toolName)
            return common::Result<void>::failure(
                {"agent_session.tool_name_mismatch",
                 "result toolName does not match the call"});
    }

    for (std::size_t i = 0; i < results.size(); ++i)
        for (std::size_t j = i + 1; j < results.size(); ++j)
            if (results[i].callId == results[j].callId)
                return common::Result<void>::failure(
                    {"agent_session.duplicate_tool_result", "same callId submitted twice"});

    for (const auto& call : pendingCalls) {
        const auto it = std::find_if(results.begin(), results.end(),
            [&](const application::AgentToolResult& r) { return r.callId == call.callId; });
        if (it == results.end())
            return common::Result<void>::failure(
                {"agent_session.missing_tool_result", "a required call has no result"});
    }

    return common::Result<void>::ok();
}

} // namespace

AnthropicAgentSession::AnthropicAgentSession(
    std::string apiKey,
    std::string endpoint,
    std::shared_ptr<http::IHttpClient> httpClient)
    : apiKey_(std::move(apiKey))
    , endpoint_(std::move(endpoint))
    , httpClient_(std::move(httpClient))
{
}

// static
common::Result<std::string> AnthropicAgentSession::buildToolsJson(
    const std::vector<application::AgentToolDefinition>& tools,
    const NextTurnToolPolicy* policy)
{
    auto arr = nlohmann::json::array();
    for (const auto& def : tools) {
        if (policy && !policy->allowedToolNames.empty()) {
            const auto& allowed = policy->allowedToolNames;
            if (std::find(allowed.begin(), allowed.end(), def.name) == allowed.end())
                continue;
        }
        nlohmann::json tool = {
            {"name", def.name},
            {"description", def.description}
        };
        nlohmann::json parsedSchema;
        bool parsed = false;
        if (!def.inputSchemaJson.empty()) {
            try {
                parsedSchema = nlohmann::json::parse(def.inputSchemaJson);
                parsed = true;
            } catch (...) {
                parsed = false;
            }
        }
        if (def.strictInputSchema) {
            // Real Run "Stage 2B - Anthropic" (1865d44f-ac23-4444-8053-
            // d24f4ba030a4, 2026-09-11) failed provider.request_failed:
            // "The compiled grammar is too large, which would cause
            // performance issues. Simplify your tool schemas or reduce the
            // number of strict tools." even for just 3 strict tools sent
            // together, AFTER an earlier fix already stripped every
            // Anthropic-unsupported keyword from each schema individually.
            // Anthropic compiles ONE combined grammar over every strict
            // tool in a request, and per Anthropic's own community-
            // documented behavior, each nullable/optional-shaped field
            // roughly DOUBLES that grammar's FSM state space - this
            // codebase's strict schemas (submit_execution_manifest above
            // all: nested task/requirement arrays, several
            // ["string","null"]-typed fields per requirement) reliably
            // exceed Anthropic's undocumented size budget regardless of
            // per-keyword compatibility. Per-schema trimming can't reliably
            // stay under an unpublished, presumably-still-changing limit,
            // so Anthropic tool schemas deliberately never request
            // "strict": true at all - the documented Anthropic/community
            // workaround for schemas this large. The Backend parser/
            // validator independently re-enforces this exact schema's full
            // contract regardless of what a provider's wire declaration
            // promises - it never trusted "strict" as the actual authority
            // to begin with - so this costs nothing beyond losing
            // provider-side grammar-constrained self-correction for
            // Anthropic; the existing bounded Planner/verification repair
            // waves remain the real correction path either way. A
            // malformed schema still fails this session before any HTTP
            // call via the same structural check OpenAI's dialect must
            // already satisfy.
            if (!parsed) {
                return common::Result<std::string>::failure(
                    {"agent_session.malformed_strict_schema",
                     "Strict tool schema is not valid JSON: " + def.name});
            }
            const auto valid = validateStrictJsonSchema(parsedSchema, def.name);
            if (!valid.success()) {
                return common::Result<std::string>::failure(*valid.error());
            }
            tool["input_schema"] = std::move(parsedSchema);
        } else {
            tool["input_schema"] = parsed
                ? std::move(parsedSchema)
                : nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}};
        }
        arr.push_back(std::move(tool));
    }
    return common::Result<std::string>::ok(arr.dump());
}

std::string AnthropicAgentSession::buildMessagesJson() const
{
    auto arr = nlohmann::json::array();
    for (std::size_t i = 0; i < messages_.size(); ++i) {
        const auto& msg = messages_[i];
        nlohmann::json item;
        item["role"] = msg.role;
        // contentJson is either a JSON array string or a plain text string.
        // Always normalize to Anthropic's array-of-blocks form (a string is
        // only a shorthand for one text block): this is required to attach
        // a cache_control marker to a specific block below, and is otherwise
        // wire-equivalent.
        nlohmann::json content = (!msg.contentJson.empty() && msg.contentJson.front() == '[')
            ? nlohmann::json::parse(msg.contentJson)
            : nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", msg.contentJson}}});
        // MW-P2: mark only the newest message (the one just appended this
        // turn) as a cache checkpoint. Every later turn's request repeats
        // this exact prefix (see doc comment above markCacheCheckpoint), so
        // the checkpoint effectively "walks forward" one message per turn
        // and each turn pays full price only for its own new increment.
        if (i + 1 == messages_.size() && content.is_array() && !content.empty()
                && content.back().is_object()) {
            markCacheCheckpoint(content.back());
        }
        item["content"] = std::move(content);
        arr.push_back(std::move(item));
    }
    return arr.dump();
}

common::Result<ProviderAgentTurnResponse> AnthropicAgentSession::beginSession(
    const ProviderAgentSessionRequest& request,
    std::stop_token stopToken)
{
    if (state_ != State::Idle)
        return common::Result<ProviderAgentTurnResponse>::failure(
            {"agent_session.invalid_state",
             "beginSession called after session was already started"});

    if (auto v = validateSessionRequest(request); !v.success())
        return common::Result<ProviderAgentTurnResponse>::failure(*v.error());

    model_ = request.apiModelId;
    systemPrompt_ = request.systemPrompt;
    tools_ = request.tools;
    auto toolsJsonResult = buildToolsJson(tools_);
    if (!toolsJsonResult.success())
        return common::Result<ProviderAgentTurnResponse>::failure(*toolsJsonResult.error());
    toolsJson_ = std::move(toolsJsonResult.value());
    maxOutputTokens_ = request.maxOutputTokens;
    maxToolCallsPerTurn_ = request.maxToolCallsPerTurn;
    turnTimeout_ = request.turnTimeout;
    disallowParallelToolCalls_ = request.disallowParallelToolCalls;

    // An optional first-turn policy is validated against the advertised
    // tools before any state changes, exactly like a NextTurnToolPolicy.
    if (request.firstTurnToolPolicy.has_value()) {
        for (const auto& name : request.firstTurnToolPolicy->allowedToolNames) {
            const bool known = std::any_of(request.tools.begin(), request.tools.end(),
                [&](const application::AgentToolDefinition& def) { return def.name == name; });
            if (!known)
                return common::Result<ProviderAgentTurnResponse>::failure(
                    {"agent_session.invalid_first_turn_policy",
                     "firstTurnToolPolicy names a tool never advertised to beginSession"});
        }
    }

    // Seed conversation history with the first user message
    messages_.push_back({"user", nlohmann::json(request.userContent).dump()});

    nlohmann::json body;
    body["model"] = model_;
    body["messages"] = nlohmann::json::parse(buildMessagesJson());
    bool oneHourStablePrefix = request.tools.empty();
    bool oneHourCacheCheckpointUsed = false;
    if (!request.tools.empty()) {
        // This session contract never permits a plain-text final answer: every
        // caller advertises only read-only inspect tools plus one or more
        // terminal declarative tools and requires exactly one terminal call.
        // Without this, the model is free to stop with prose instead of
        // calling a tool, which surfaced as a real-Run planner.missing_submission
        // failure even after two independent inspection/repair turns.
        nlohmann::json toolChoice = {{"type", "any"}};
        bool disableParallel = disallowParallelToolCalls_;
        const NextTurnToolPolicy* firstTurnPolicy = request.firstTurnToolPolicy.has_value()
            ? &*request.firstTurnToolPolicy : nullptr;
        std::string turnToolsJson = toolsJson_;
        if (firstTurnPolicy != nullptr) {
            auto filtered = buildToolsJson(tools_, firstTurnPolicy);
            if (!filtered.success())
                return common::Result<ProviderAgentTurnResponse>::failure(*filtered.error());
            if (!filtered.value().empty() && filtered.value() != "[]") {
                turnToolsJson = std::move(filtered.value());
                // Same wire contract as submitToolResults(): exactly one
                // allowed tool is forced by name, several use type=any
                // within the filtered catalog.
                if (firstTurnPolicy->allowedToolNames.size() == 1) {
                    toolChoice = {{"type", "tool"},
                        {"name", firstTurnPolicy->allowedToolNames.front()}};
                }
                if (firstTurnPolicy->disallowParallelToolCalls) disableParallel = true;
            }
        }
        body["tools"] = nlohmann::json::parse(turnToolsJson);
        // A filtered first-turn catalog is not stable across the session. It
        // must not receive the paid one-hour write, and giving it a five-minute
        // marker would put a shorter TTL before the one-hour system prefix.
        if (turnToolsJson == toolsJson_) {
            markLastToolAsCacheCheckpoint(body["tools"]);
            oneHourStablePrefix = true;
            oneHourCacheCheckpointUsed = true;
        }
        if (disableParallel) {
            toolChoice["disable_parallel_tool_use"] = true;
        }
        body["tool_choice"] = std::move(toolChoice);
    }
    if (!systemPrompt_.empty()) {
        body["system"] = buildSystemJson(systemPrompt_, oneHourStablePrefix);
        oneHourCacheCheckpointUsed = oneHourCacheCheckpointUsed || oneHourStablePrefix;
    }
    body["max_tokens"] = maxOutputTokens_;

    return doTurn(body.dump(), std::move(stopToken), oneHourCacheCheckpointUsed);
}

common::Result<ProviderAgentTurnResponse> AnthropicAgentSession::submitToolResults(
    std::vector<application::AgentToolResult> results,
    std::stop_token stopToken,
    std::optional<NextTurnToolPolicy> nextTurnPolicy)
{
    if (state_ == State::Idle)
        return common::Result<ProviderAgentTurnResponse>::failure(
            {"agent_session.not_started", "beginSession was never called"});
    if (state_ == State::Terminal)
        return common::Result<ProviderAgentTurnResponse>::failure(
            {"agent_session.already_terminal", "session has already finished"});

    if (auto v = validateToolResultCorrelation(pendingCalls_, results); !v.success())
        return common::Result<ProviderAgentTurnResponse>::failure(*v.error());

    if (nextTurnPolicy.has_value()) {
        for (const auto& name : nextTurnPolicy->allowedToolNames) {
            const bool known = std::any_of(tools_.begin(), tools_.end(),
                [&](const application::AgentToolDefinition& def) { return def.name == name; });
            if (!known)
                return common::Result<ProviderAgentTurnResponse>::failure(
                    {"agent_session.invalid_next_turn_policy",
                     "nextTurnPolicy names a tool never advertised to beginSession"});
        }
    }

    // Build tool_result content blocks
    auto resultBlocks = nlohmann::json::array();
    for (const auto& result : results) {
        nlohmann::json block = {
            {"type", "tool_result"},
            {"tool_use_id", result.callId}
        };
        if (!result.success) {
            block["is_error"] = true;
            std::string detail = result.errorCode.empty()
                ? std::string("error: tool execution failed")
                : "error: " + result.errorCode;
            // A Backend-authored explanation of the failure (for example the allowed paths of a
            // denied read) travels with the code; a failure without one keeps the bare code.
            if (!result.contentJson.empty()) detail += "\n" + result.contentJson;
            block["content"] = std::move(detail);
        } else {
            block["content"] = result.contentJson.empty() ? "{}" : result.contentJson;
        }
        resultBlocks.push_back(std::move(block));
    }

    // Append tool result message to history
    messages_.push_back({"user", resultBlocks.dump()});

    nlohmann::json body;
    body["model"] = model_;
    body["messages"] = nlohmann::json::parse(buildMessagesJson());
    bool oneHourStablePrefix = toolsJson_.empty() && !nextTurnPolicy.has_value();
    bool oneHourCacheCheckpointUsed = false;
    if (nextTurnPolicy.has_value()) {
        auto turnToolsJson = buildToolsJson(tools_, &(*nextTurnPolicy));
        if (!turnToolsJson.success())
            return common::Result<ProviderAgentTurnResponse>::failure(*turnToolsJson.error());
        if (!turnToolsJson.value().empty() && turnToolsJson.value() != "[]") {
            body["tools"] = nlohmann::json::parse(turnToolsJson.value());
            if (turnToolsJson.value() == toolsJson_) {
                markLastToolAsCacheCheckpoint(body["tools"]);
                oneHourStablePrefix = true;
                oneHourCacheCheckpointUsed = true;
            }
            // A one-tool Backend policy is stronger than "choose any of the
            // advertised tools": name that exact tool on the wire. Real Run
            // 8048a3e5-699b-43d4-9294-840f3ee17d94 showed Claude continuing
            // its prior inspection pattern on the first forced-terminal
            // turn even though the request carried a filtered one-tool
            // catalog with type=any. Anthropic's Messages contract provides
            // type=tool specifically for this case. Multi-tool policies keep
            // type=any because the Backend intentionally allows the model to
            // choose among several genuinely-applicable terminal outcomes.
            nlohmann::json toolChoice = nextTurnPolicy->allowedToolNames.size() == 1
                ? nlohmann::json {
                    {"type", "tool"},
                    {"name", nextTurnPolicy->allowedToolNames.front()}}
                : nlohmann::json {{"type", "any"}};
            if (disallowParallelToolCalls_ || nextTurnPolicy->disallowParallelToolCalls) {
                toolChoice["disable_parallel_tool_use"] = true;
            }
            body["tool_choice"] = std::move(toolChoice);
        }
    } else if (!toolsJson_.empty()) {
        body["tools"] = nlohmann::json::parse(toolsJson_);
        markLastToolAsCacheCheckpoint(body["tools"]);
        oneHourStablePrefix = true;
        oneHourCacheCheckpointUsed = true;
        nlohmann::json toolChoice = {{"type", "any"}};
        if (disallowParallelToolCalls_) {
            toolChoice["disable_parallel_tool_use"] = true;
        }
        body["tool_choice"] = std::move(toolChoice);
    }
    if (!systemPrompt_.empty()) {
        body["system"] = buildSystemJson(systemPrompt_, oneHourStablePrefix);
        oneHourCacheCheckpointUsed = oneHourCacheCheckpointUsed || oneHourStablePrefix;
    }
    body["max_tokens"] = maxOutputTokens_;

    return doTurn(body.dump(), std::move(stopToken), oneHourCacheCheckpointUsed);
}

common::Result<ProviderAgentTurnResponse> AnthropicAgentSession::doTurn(
    const std::string& requestBody,
    std::stop_token stopToken,
    bool oneHourCacheCheckpointUsed)
{
    auto makeStopResp = [this]() -> common::Result<ProviderAgentTurnResponse> {
        ProviderAgentTurnResponse resp;
        resp.turnIndex = turnIndex_;
        resp.finishReason = ProviderAgentFinishReason::StopRequested;
        if (auto v = validateTurnResponse(resp, turnIndex_, maxToolCallsPerTurn_);
                !v.success())
            return common::Result<ProviderAgentTurnResponse>::failure(*v.error());
        state_ = State::Terminal;
        ++turnIndex_;
        return common::Result<ProviderAgentTurnResponse>::ok(std::move(resp));
    };

    if (stopToken.stop_requested())
        return makeStopResp();

    http::HttpRequest req;
    req.url = endpoint_;
    req.method = "POST";
    req.headers["x-api-key"] = apiKey_;
    req.headers["anthropic-version"] = kApiVersion;
    req.headers["Content-Type"] = "application/json";
    req.body = requestBody;
    req.totalTimeout = turnTimeout_;
    req.cancellationRequested = [st = stopToken] { return st.stop_requested(); };

    const http::HttpResponse httpResp = httpClient_->send(req);

    if (httpResp.hasTransportError()) {
        if (stopToken.stop_requested())
            return makeStopResp();
        return common::Result<ProviderAgentTurnResponse>::failure(
            {"provider.network_error",
             "Transport error during provider request: " + httpResp.errorMessage});
    }

    if (!httpResp.isSuccess())
        return httpStatusError<ProviderAgentTurnResponse>(httpResp.statusCode, httpResp.body);

    nlohmann::json json;
    try {
        json = nlohmann::json::parse(httpResp.body);
    } catch (const std::exception&) {
        return common::Result<ProviderAgentTurnResponse>::failure(
            {"provider.invalid_response", "Failed to parse provider JSON response"});
    }

    // Top-level error from Anthropic (type == "error") - rare (errors are
    // normally reported via a non-2xx status), but defended the same way.
    if (json.value("type", "") == "error") {
        const std::string apiMessage =
            anthropic_detail::extractAnthropicErrorMessage(httpResp.body);
        std::string message = "Provider returned error response";
        if (!apiMessage.empty()) message += ": " + apiMessage;
        return common::Result<ProviderAgentTurnResponse>::failure(
            {"provider.request_failed", std::move(message)});
    }

    if (!json.contains("content") || !json["content"].is_array())
        return common::Result<ProviderAgentTurnResponse>::failure(
            {"provider.invalid_response", "Response missing content array"});

    const auto parsedUsage = parseAgentSessionUsage(json, oneHourCacheCheckpointUsed);
    if (!parsedUsage.success()) {
        return common::Result<ProviderAgentTurnResponse>::failure(*parsedUsage.error());
    }

    const std::string stopReason = json.value("stop_reason", std::string{});

    // Parse content blocks, preserving order
    std::string assistantText;
    std::vector<application::AgentToolCall> toolCalls;
    auto assistantContentBlocks = nlohmann::json::array();
    std::size_t callOrdinal = 0;

    for (const auto& block : json["content"]) {
        if (!block.is_object()) continue;
        assistantContentBlocks.push_back(block);

        const auto type = block.value("type", std::string{});
        if (type == "text") {
            if (block.contains("text") && block["text"].is_string())
                assistantText += block["text"].get<std::string>();
        } else if (type == "tool_use") {
            // Anthropic uses `id` as the callId, not a separate call_id field
            if (!block.contains("id") || !block["id"].is_string())
                return common::Result<ProviderAgentTurnResponse>::failure(
                    {"provider.invalid_response",
                     "tool_use block missing id field"});
            if (!block.contains("name") || !block["name"].is_string())
                return common::Result<ProviderAgentTurnResponse>::failure(
                    {"provider.invalid_response",
                     "tool_use block missing name field"});

            application::AgentToolCall call;
            call.callId = block["id"].get<std::string>();
            call.toolName = block["name"].get<std::string>();
            // input is already an object — serialize to JSON string
            call.argumentsJson = block.contains("input") && block["input"].is_object()
                ? block["input"].dump()
                : std::string("{}");
            call.turnIndex = turnIndex_;
            call.ordinal = callOrdinal++;
            toolCalls.push_back(std::move(call));
        }
        // Other block types (e.g. "thinking") are carried through to history
        // but not surfaced to the coordinator
    }

    // Append assistant turn to conversation history for subsequent turns
    messages_.push_back({"assistant", assistantContentBlocks.dump()});

    ProviderAgentTurnResponse resp;
    resp.turnIndex = turnIndex_;
    resp.assistantText = std::move(assistantText);

    if (stopReason == "tool_use") {
        resp.finishReason = ProviderAgentFinishReason::ToolUse;
        resp.toolCalls = std::move(toolCalls);
    } else if (stopReason == "end_turn" || stopReason == "stop_sequence") {
        resp.finishReason = ProviderAgentFinishReason::Stop;
    } else if (stopReason == "max_tokens") {
        resp.finishReason = ProviderAgentFinishReason::MaxOutputTokens;
    } else {
        // Unknown stop reason — treat as error
        resp.finishReason = ProviderAgentFinishReason::Error;
        resp.errorCode = "provider.invalid_response";
        resp.errorMessage = "Unknown or missing stop_reason";
    }

    resp.usage = parsedUsage.value();

    if (auto v = validateTurnResponse(resp, turnIndex_, maxToolCallsPerTurn_); !v.success())
        return common::Result<ProviderAgentTurnResponse>::failure(*v.error());

    ++turnIndex_;

    if (resp.finishReason == ProviderAgentFinishReason::ToolUse) {
        pendingCalls_ = resp.toolCalls;
        state_ = State::WaitingForToolResults;
    } else {
        pendingCalls_.clear();
        state_ = State::Terminal;
    }

    return common::Result<ProviderAgentTurnResponse>::ok(std::move(resp));
}

} // namespace ai::providers
