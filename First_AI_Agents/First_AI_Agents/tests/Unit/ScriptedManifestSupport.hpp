#pragma once

// Provider-free doubles for tests that drive the REAL ManifestWorkflowRunner: scripted provider
// sessions (no network, no CURL, no real adapter), a fake provider client for the conditional
// integration call, a fake verification command runner and the JSON the Planner / workers submit.
// The shapes are the ones ManifestWorkflowRunnerTests.cpp uses; this header lets a second test
// executable (the restart proofs, which need SQLite and the production recovery sequence) share
// them without that large file.
#include "ai/application/agents/AgentToolExecutionService.hpp"
#include "ai/application/execution/ExecutionManifestDraftParser.hpp"
#include "ai/application/workspace/FilesystemWorkspaceToolExecutor.hpp"
#include "ai/application/ManifestWorkflowRunner.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <vector>

namespace ai::tests::scripted {

using namespace ai::application;
using namespace ai::providers;
namespace common = ai::common;

inline void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const char* prefix = "aio-scripted-manifest-")
    {
        path = std::filesystem::temp_directory_path()
            / (std::string(prefix)
                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// The read-only workspace tools a Planner / worker may call: every call succeeds and echoes the
// requested path, exactly as the real executor's result shape does.
class AlwaysSucceedExecutor final : public IWorkspaceToolExecutor {
public:
    common::Result<AgentToolResult> execute(const AgentToolCall& call, const WorkspaceReadPolicy&) override
    {
        AgentToolResult r;
        r.callId = call.callId;
        r.toolName = call.toolName;
        r.success = true;
        nlohmann::json content;
        content["entries"] = nlohmann::json::array({"src", "README.md"});
        try {
            const auto args = nlohmann::json::parse(call.argumentsJson);
            if (args.is_object() && args.contains("path") && args.at("path").is_string()) {
                content["relativePath"] = args.at("path").get<std::string>();
            }
        } catch (const std::exception&) {
        }
        if (call.toolName == "workspace_stat_path") {
            content["exists"] = true;
            content["isFile"] = true;
            content["isDirectory"] = false;
        }
        r.contentJson = content.dump();
        r.returnedBytes = r.contentJson.size();
        return common::Result<AgentToolResult>::ok(std::move(r));
    }
};

class ScriptedProviderAgentSession final : public IProviderAgentSession {
public:
    std::queue<common::Result<ProviderAgentTurnResponse>> responses;
    // Run on the calling (worker) thread right before the next scripted response is returned.
    // The second form receives the Run's own cancellation token, so a test can hold the session
    // until a shutdown it started on another thread has requested the stop.
    std::function<void()> beforeNextResponse;
    std::function<void(std::stop_token)> beforeNextResponseWithStop;
    std::function<void(const ProviderAgentSessionRequest&)> onBegin;
    std::mutex mu;

    common::Result<ProviderAgentTurnResponse> beginSession(
        const ProviderAgentSessionRequest& request, std::stop_token token) override
    {
        if (onBegin) onBegin(request);
        return nextResponse(std::move(token));
    }
    common::Result<ProviderAgentTurnResponse> submitToolResults(std::vector<AgentToolResult>,
        std::stop_token token, std::optional<NextTurnToolPolicy> = std::nullopt) override
    {
        return nextResponse(std::move(token));
    }

private:
    common::Result<ProviderAgentTurnResponse> nextResponse(std::stop_token token)
    {
        if (beforeNextResponse) {
            auto action = std::move(beforeNextResponse);
            action();
        }
        if (beforeNextResponseWithStop) {
            auto action = std::move(beforeNextResponseWithStop);
            action(token);
        }
        std::lock_guard lock(mu);
        if (responses.empty()) {
            return common::Result<ProviderAgentTurnResponse>::failure(
                {"test.no_more_responses", "ScriptedProviderAgentSession ran out of responses"});
        }
        auto result = std::move(responses.front());
        responses.pop();
        return result;
    }
};

class ScriptedProviderAgentSessionFactory final : public IProviderAgentSessionFactory {
public:
    struct Invocation {
        ProviderId providerId;
        std::string providerKind;
        std::optional<ProviderAgentSessionRequest> request;
    };

    std::mutex mu;
    std::queue<std::unique_ptr<ScriptedProviderAgentSession>> preparedSessions;
    std::unordered_map<ProviderId, std::queue<std::unique_ptr<ScriptedProviderAgentSession>>>
        preparedSessionsByProvider;
    std::vector<Invocation> invocations;
    // Called on the creating thread, OUTSIDE the factory's lock, right after a session was handed
    // out and before it runs. A crash-state capture point: nothing of this session exists yet.
    std::function<void(const ProviderId&)> onCreateSession;

    common::Result<std::unique_ptr<IProviderAgentSession>> createSession(
        const ProviderId& providerId, const std::string& providerKind) override
    {
        std::unique_ptr<ScriptedProviderAgentSession> session;
        {
            std::lock_guard lock(mu);
            auto keyed = preparedSessionsByProvider.find(providerId);
            if (keyed != preparedSessionsByProvider.end() && !keyed->second.empty()) {
                session = std::move(keyed->second.front());
                keyed->second.pop();
            } else if (!preparedSessions.empty()) {
                session = std::move(preparedSessions.front());
                preparedSessions.pop();
            } else {
                invocations.push_back({providerId, providerKind, std::nullopt});
                return common::Result<std::unique_ptr<IProviderAgentSession>>::failure(
                    {"test.no_more_sessions", "ScriptedProviderAgentSessionFactory ran out of prepared sessions"});
            }
            const auto invocationIndex = invocations.size();
            invocations.push_back({providerId, providerKind, std::nullopt});
            session->onBegin = [this, invocationIndex](const ProviderAgentSessionRequest& request) {
                std::lock_guard callbackLock(mu);
                invocations.at(invocationIndex).request = request;
            };
        }
        if (onCreateSession) onCreateSession(providerId);
        return common::Result<std::unique_ptr<IProviderAgentSession>>::ok(std::move(session));
    }

    void prepareFor(const ProviderId& providerId, std::unique_ptr<ScriptedProviderAgentSession> session)
    {
        std::lock_guard lock(mu);
        preparedSessionsByProvider[providerId].push(std::move(session));
    }

    [[nodiscard]] std::size_t remainingSessions()
    {
        std::lock_guard lock(mu);
        std::size_t remaining = preparedSessions.size();
        for (const auto& [id, sessions] : preparedSessionsByProvider) {
            static_cast<void>(id);
            remaining += sessions.size();
        }
        return remaining;
    }

    [[nodiscard]] std::vector<Invocation> invocationSnapshot()
    {
        std::lock_guard lock(mu);
        return invocations;
    }
};

// Conditional lead integration call target (a plain provider client).
class FakeProviderClient final : public IProviderClient {
public:
    common::Result<ProviderChatResponse> send(const ProviderChatRequest& request, std::stop_token) override
    {
        std::lock_guard lock(mu_);
        requests_.push_back(request);
        ProviderChatResponse response;
        response.content = "RECONCILIATION_GUIDANCE_MARKER";
        return common::Result<ProviderChatResponse>::ok(std::move(response));
    }
    [[nodiscard]] std::size_t callCount() const
    {
        std::lock_guard lock(mu_);
        return requests_.size();
    }

private:
    mutable std::mutex mu_;
    std::vector<ProviderChatRequest> requests_;
};

class FakeProviderClientFactory final : public IProviderClientFactory {
public:
    std::shared_ptr<FakeProviderClient> client = std::make_shared<FakeProviderClient>();

    common::Result<std::shared_ptr<IProviderClient>> clientFor(const ProviderId&, const std::string&) override
    {
        return common::Result<std::shared_ptr<IProviderClient>>::ok(client);
    }
};

class FakeVerificationCommandRunner final : public IManifestVerificationCommandRunner {
public:
    std::vector<ManifestVerificationCommandOutcome> outcomes;
    std::vector<ManifestVerificationCommandRequest> requests;

    ManifestVerificationCommandOutcome run(
        const ManifestVerificationCommandRequest& request, std::stop_token) override
    {
        requests.push_back(request);
        if (requests.size() <= outcomes.size()) return outcomes[requests.size() - 1];
        ManifestVerificationCommandOutcome result;
        result.started = true;
        result.exitCode = 0;
        return result;
    }
};

inline ProviderAgentTurnResponse stopResponse(std::string text)
{
    ProviderAgentTurnResponse resp;
    resp.finishReason = ProviderAgentFinishReason::Stop;
    resp.assistantText = std::move(text);
    resp.usage.inputTokens = 10;
    resp.usage.outputTokens = 5;
    return resp;
}

inline ProviderAgentTurnResponse toolCallResponse(std::string callId, std::string toolName, std::string argumentsJson)
{
    ProviderAgentTurnResponse resp;
    resp.finishReason = ProviderAgentFinishReason::ToolUse;
    AgentToolCall call;
    call.callId = std::move(callId);
    call.toolName = std::move(toolName);
    call.argumentsJson = std::move(argumentsJson);
    resp.toolCalls = {call};
    resp.usage.inputTokens = 8;
    resp.usage.outputTokens = 4;
    return resp;
}

// A worker's final answer: one proposed new file.
inline std::string proposalSubmission(const std::string& relativePath)
{
    return "```json\n{\"schemaVersion\":\"proposal-submission.v2\","
        "\"summary\":\"Add a file.\",\"changes\":[{"
        "\"change_id\":\"create-file\",\"operation\":\"create\","
        "\"relative_path\":\"" + relativePath + "\",\"source_path\":null,"
        "\"proposed_content\":\"content\"}]}\n```";
}

struct ScriptedTask {
    std::string id;
    std::string path;
    std::string assignee;
};

// The Planner's draft for a roster Run: one independent work task per entry, each assigned to a
// member. `maxParallelism` 1 serialises the tasks in manifest order, so a hook inside a task's
// session sees exactly the tasks before it Completed and the ones after it not yet started.
inline std::string rosterDraft(const std::vector<ScriptedTask>& tasks, int maxParallelism)
{
    nlohmann::json draft;
    draft["schemaVersion"] = "execution-manifest-draft.v1";
    draft["coordinationTasks"] = nlohmann::json::array();
    draft["workTasks"] = nlohmann::json::array();
    for (const auto& task : tasks) {
        draft["workTasks"].push_back({
            {"id", task.id},
            {"objective", "Create " + task.path},
            {"role", "worker"},
            {"dependencies", nlohmann::json::array()},
            {"writePaths", nlohmann::json::array({task.path})},
            {"baselineRequiredReadPaths", nlohmann::json::array()},
            {"proposedDependencyReadPaths", nlohmann::json::array()},
            {"referencePaths", nlohmann::json::array()},
            {"excludedPaths", nlohmann::json::array()},
            {"deferredPaths", nlohmann::json::array()},
            {"acceptanceCriteria", nlohmann::json::array({"The output exists"})},
            {"acceptanceContract", {
                {"schemaVersion", "execution-acceptance.v1"},
                {"requirements", nlohmann::json::array({{
                    {"id", task.id + "-output"},
                    {"description", "The output exists"},
                    {"requiredOutputs", nlohmann::json::array({task.path})},
                    {"verificationKind", "inspect_paths"},
                    {"verificationProfileId", nullptr},
                    {"verificationBuildProfileId", nullptr},
                    {"verificationTargetId", nullptr},
                    {"verificationArguments", nlohmann::json::array()},
                    {"required", true},
                    {"verificationBootstrapPending", false},
                }})},
            }},
            {"contextReferences", nlohmann::json::array()},
            {"requestedCapability", "propose"},
            {"assigneeMemberId", task.assignee},
        });
    }
    draft["requestedMaxParallelism"] = maxParallelism;
    draft["requestedTokenBudget"] = nullptr;
    draft["requestedCostMicros"] = nullptr;
    draft["bootstrapDeclaration"] = nullptr;
    return draft.dump();
}

} // namespace ai::tests::scripted
