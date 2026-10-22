// Manual smoke test for OpenAI and Anthropic agent sessions.
// Runs only when the corresponding environment variable is set.
// Not part of CTest — never calls real APIs from automated CI.
//
// Usage:
//   set OPENAI_API_KEY=sk-...
//   AgentSessionSmokeTest.exe
//
//   set ANTHROPIC_API_KEY=sk-ant-...
//   AgentSessionSmokeTest.exe
//
// When both are set both providers are tested.

#include "ai/providers/AnthropicAgentSession.hpp"
#include "ai/providers/OpenAIAgentSession.hpp"
#include "ai/http/HttpClient.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace ai::providers;
using namespace ai::application;

std::string getEnv(const char* name)
{
    const char* val = std::getenv(name);
    return val ? std::string(val) : std::string{};
}

AgentToolDefinition makeListDirTool()
{
    AgentToolDefinition def;
    def.name = "workspace_list_directory";
    def.description = "List files in a directory";
    def.inputSchemaJson = R"({
        "type": "object",
        "properties": {
            "path": {"type": "string", "description": "Directory path to list"}
        },
        "required": ["path"]
    })";
    def.readOnly = true;
    return def;
}

ProviderAgentSessionRequest makeRequest(const std::string& apiModelId)
{
    ProviderAgentSessionRequest req;
    req.providerId = "smoke-provider";
    req.modelId = apiModelId;
    req.apiModelId = apiModelId;
    req.systemPrompt = "You are a helpful assistant with access to workspace tools.";
    req.userContent =
        "Please list the contents of the current directory (path \".\") using the "
        "workspace_list_directory tool, then summarise what you found in one sentence.";
    req.tools = {makeListDirTool()};
    req.maxTurns = 4;
    req.maxTotalToolCalls = 4;
    req.maxToolCallsPerTurn = 2;
    req.maxOutputTokens = 512;
    req.temperature = 0.0;
    req.turnTimeout = std::chrono::milliseconds{30'000};
    return req;
}

AgentToolResult executeFakeTool(const AgentToolCall& call)
{
    AgentToolResult result;
    result.callId = call.callId;
    result.toolName = call.toolName;
    result.success = true;
    result.contentJson = R"(["src","README.md","CMakeLists.txt","vcpkg.json"])";
    result.returnedBytes = result.contentJson.size();
    return result;
}

bool runSession(IProviderAgentSession& session, const std::string& label,
    const std::string& apiModelId)
{
    std::cout << "\n=== " << label << " ===\n";

    ProviderAgentSessionRequest request = makeRequest(apiModelId);
    std::stop_source ss;
    auto resp = session.beginSession(request, ss.get_token());
    if (!resp.success()) {
        std::cerr << "beginSession failed: [" << resp.error()->code << "] "
                  << resp.error()->message << '\n';
        return false;
    }

    int turn = 0;
    while (resp.value().finishReason == ProviderAgentFinishReason::ToolUse) {
        std::cout << "Turn " << turn
                  << ": ToolUse (" << resp.value().toolCalls.size() << " call(s))\n";
        if (!resp.value().assistantText.empty())
            std::cout << "  Text: " << resp.value().assistantText << '\n';

        std::vector<AgentToolResult> results;
        for (const auto& call : resp.value().toolCalls) {
            std::cout << "  Call: " << call.toolName
                      << " [" << call.callId << "] args=" << call.argumentsJson << '\n';
            results.push_back(executeFakeTool(call));
        }

        resp = session.submitToolResults(std::move(results), ss.get_token());
        if (!resp.success()) {
            std::cerr << "submitToolResults failed: [" << resp.error()->code << "] "
                      << resp.error()->message << '\n';
            return false;
        }
        ++turn;
    }

    const auto& final = resp.value();
    switch (final.finishReason) {
    case ProviderAgentFinishReason::Stop:
        std::cout << "Turn " << turn << ": Stop\n"
                  << "  Text: " << final.assistantText << '\n';
        std::cout << "  Tokens: " << final.usage.inputTokens << " in, "
                  << final.usage.outputTokens << " out\n";
        return true;
    case ProviderAgentFinishReason::MaxTurns:
        std::cout << "Turn " << turn << ": MaxTurns (partial: "
                  << final.assistantText << ")\n";
        return true;
    case ProviderAgentFinishReason::MaxOutputTokens:
        std::cout << "Turn " << turn << ": MaxOutputTokens (partial: "
                  << final.assistantText << ")\n";
        return true;
    case ProviderAgentFinishReason::Error:
        std::cerr << "Turn " << turn << ": Error [" << final.errorCode << "] "
                  << final.errorMessage << '\n';
        return false;
    case ProviderAgentFinishReason::StopRequested:
        std::cout << "Turn " << turn << ": StopRequested\n";
        return true;
    default:
        std::cerr << "Unexpected finish reason\n";
        return false;
    }
}

} // namespace

int main()
{
    const auto openaiKey = getEnv("OPENAI_API_KEY");
    const auto anthropicKey = getEnv("ANTHROPIC_API_KEY");

    if (openaiKey.empty() && anthropicKey.empty()) {
        std::cout << "No API keys set. Set OPENAI_API_KEY or ANTHROPIC_API_KEY to run.\n";
        return 0;
    }

    bool allPassed = true;

    if (!openaiKey.empty()) {
        auto httpClient = std::make_shared<ai::http::HttpClient>();
        OpenAIAgentSession session(openaiKey, OpenAIAgentSession::kDefaultEndpoint,
            std::move(httpClient));
        if (!runSession(session, "OpenAI gpt-4o-mini", "gpt-4o-mini"))
            allPassed = false;
    }

    if (!anthropicKey.empty()) {
        auto httpClient = std::make_shared<ai::http::HttpClient>();
        AnthropicAgentSession session(anthropicKey,
            AnthropicAgentSession::kDefaultEndpoint, std::move(httpClient));
        if (!runSession(session, "Anthropic claude-3-5-haiku-20241022",
                "claude-3-5-haiku-20241022"))
            allPassed = false;
    }

    if (allPassed) {
        std::cout << "\nSmoke tests passed.\n";
        return 0;
    }
    std::cerr << "\nSmoke tests FAILED.\n";
    return 1;
}
