#include "ai/application/execution/ExecutionManifestPlanner.hpp"
#include "ai/application/execution/ExecutionManifestDraftParser.hpp"
#include "ai/application/execution/ExecutionManifestExecutionCapacity.hpp"
#include "ai/application/execution/ExecutionManifestPlannerTurnBudget.hpp"
#include "ai/application/execution/ExecutionManifestRepairControls.hpp"
#include "ai/application/inmemory/InMemoryExecutionManifestRepository.hpp"
#include "ai/application/inmemory/InMemoryRepositories.hpp"
#include "ai/application/inmemory/InMemoryRunWorkspaceScopeRepository.hpp"
#include "ai/application/verification/ManifestMandatoryVerificationPolicy.hpp"
#include "ai/application/execution/ManifestRouting.hpp"
#include "ai/application/execution/RunWorkspaceScopeService.hpp"
#include "ai/application/workspace/WorkspaceReadPolicy.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace common = ai::common;
using namespace ai::application;
using namespace ai::providers;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct SessionLog {
    std::vector<ProviderAgentSessionRequest> requests;
    std::vector<std::vector<AgentToolResult>> submittedResults;
    std::vector<std::optional<NextTurnToolPolicy>> nextTurnPolicies;
};

class ScriptedSession final : public IProviderAgentSession {
public:
    SessionLog* log {nullptr};
    std::queue<common::Result<ProviderAgentTurnResponse>> responses;

    common::Result<ProviderAgentTurnResponse> beginSession(
        const ProviderAgentSessionRequest& request,
        std::stop_token) override
    {
        log->requests.push_back(request);
        return next();
    }

    common::Result<ProviderAgentTurnResponse> submitToolResults(
        std::vector<AgentToolResult> results,
        std::stop_token,
        std::optional<NextTurnToolPolicy> nextTurnPolicy = std::nullopt) override
    {
        log->submittedResults.push_back(std::move(results));
        log->nextTurnPolicies.push_back(std::move(nextTurnPolicy));
        return next();
    }

private:
    common::Result<ProviderAgentTurnResponse> next()
    {
        if (responses.empty()) {
            return common::Result<ProviderAgentTurnResponse>::failure(
                {"test.responses_exhausted", "Scripted planner session has no response"});
        }
        auto result = std::move(responses.front());
        responses.pop();
        return result;
    }
};

class ScriptedSessionFactory final : public IProviderAgentSessionFactory {
public:
    SessionLog log;
    std::queue<std::unique_ptr<ScriptedSession>> sessions;
    std::size_t createCalls {0};

    common::Result<std::unique_ptr<IProviderAgentSession>> createSession(
        const ProviderId&,
        const std::string&) override
    {
        ++createCalls;
        if (sessions.empty()) {
            return common::Result<std::unique_ptr<IProviderAgentSession>>::failure(
                {"test.sessions_exhausted", "No scripted planner session remains"});
        }
        auto session = std::move(sessions.front());
        sessions.pop();
        session->log = &log;
        return common::Result<std::unique_ptr<IProviderAgentSession>>::ok(
            std::move(session));
    }
};

class PolicyAwareExecutor final : public IWorkspaceToolExecutor {
public:
    common::Result<AgentToolResult> execute(
        const AgentToolCall& call,
        const WorkspaceReadPolicy& policy) override
    {
        ++callCount;
        AgentToolResult result;
        result.callId = call.callId;
        result.toolName = call.toolName;
        const auto path = call.argumentsJson.find(".git/config") != std::string::npos
            ? std::filesystem::path(".git/config")
            : std::filesystem::path("README.md");
        const auto allowed = WorkspaceReadPolicyValidator::validateRelativePath(path, policy);
        result.success = allowed.success();
        if (result.success) {
            result.contentJson = R"({"content":"repository context"})";
            result.returnedBytes = result.contentJson.size();
        } else {
            result.errorCode = allowed.error()->code;
        }
        return common::Result<AgentToolResult>::ok(std::move(result));
    }

    std::size_t callCount {0};
};

ToolRegistry defaultRegistry()
{
    auto registry = ToolRegistry::createDefaultReadOnly();
    if (!registry.success()) throw std::runtime_error("Could not create tool registry");
    return std::move(registry.value());
}

struct Fixture {
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifestRepository;
    InMemoryAgentToolSessionRepository auditSessions;
    InMemoryAgentToolAuditRepository audits;
    PolicyAwareExecutor executor;
    ScriptedSessionFactory sessionFactory;
    AgentToolExecutionService toolExecution;
    ExecutionManifestService manifests;
    ExecutionManifestPlanner planner;

    Fixture()
        : toolExecution(defaultRegistry(), executor, auditSessions, audits, true)
        , manifests(manifestRepository, runs)
        , planner(sessionFactory, toolExecution, manifests)
    {
        Run run;
        run.id = "planner-run";
        run.projectId = "project-1";
        run.title = "Phase 3";
        run.task = "Plan the execution graph";
        run.workflowId = "manifest-v1";
        run.createdAt = std::chrono::system_clock::now();
        require(runs.insertRun(std::move(run)).success(), "Could not seed planner Run");
    }

    void addSession(std::vector<ProviderAgentTurnResponse> turns)
    {
        auto session = std::make_unique<ScriptedSession>();
        for (auto& turn : turns) {
            session->responses.push(
                common::Result<ProviderAgentTurnResponse>::ok(std::move(turn)));
        }
        sessionFactory.sessions.push(std::move(session));
    }
};

ProviderAgentTurnResponse toolTurn(
    std::size_t turnIndex,
    std::string callId,
    std::string toolName,
    std::string arguments)
{
    ProviderAgentTurnResponse response;
    response.turnIndex = turnIndex;
    response.finishReason = ProviderAgentFinishReason::ToolUse;
    response.usage.inputTokens = 7;
    response.usage.outputTokens = 3;
    AgentToolCall call;
    call.callId = std::move(callId);
    call.toolName = std::move(toolName);
    call.argumentsJson = std::move(arguments);
    call.turnIndex = turnIndex;
    response.toolCalls.push_back(std::move(call));
    return response;
}

ProviderAgentTurnResponse submitTurn(std::size_t turnIndex, std::string manifest)
{
    return toolTurn(
        turnIndex, "submit-" + std::to_string(turnIndex),
        kSubmitExecutionManifestDraftToolName, std::move(manifest));
}

ProviderAgentTurnResponse noChangesTurn(std::size_t turnIndex, std::string submission)
{
    return toolTurn(
        turnIndex, "no-changes-" + std::to_string(turnIndex),
        kSubmitNoChangesNeededToolName, std::move(submission));
}

ProviderAgentTurnResponse stopTurn(std::size_t turnIndex)
{
    ProviderAgentTurnResponse response;
    response.turnIndex = turnIndex;
    response.finishReason = ProviderAgentFinishReason::Stop;
    response.usage.inputTokens = 5;
    response.usage.outputTokens = 2;
    return response;
}

// path defaults to "README.md" because every fixture inspect call in this
// file reads "README.md" (see toolTurn(..., "workspace_read_file",
// R"({"path":"README.md"})")) - the evidence path must now match a path this
// same attempt session actually inspected, so the default here must agree
// with the default inspect path used across this file's scripted turns.
std::string noChangesJson(
    const std::string& summary = "All Stage 1 acceptance criteria are already satisfied.",
    const std::string& criterion = "Stage 1 files exist",
    const std::string& path = "README.md",
    const std::string& observation = "File already present with the expected content.",
    const std::optional<std::string>& recommendedNextStep = std::nullopt)
{
    std::string json = "{\"schemaVersion\":\"execution-manifest-no-changes.v1\","
        "\"summary\":\"" + summary + "\","
        "\"evidence\":[{\"criterion\":\"" + criterion + "\",\"path\":\"" + path
        + "\",\"observation\":\"" + observation + "\"}]";
    if (recommendedNextStep.has_value()) {
        json += ",\"recommendedNextStep\":\"" + *recommendedNextStep + "\"";
    }
    json += "}";
    return json;
}

std::string noChangesJsonWithoutEvidence(const std::string& summary = "Nothing to do")
{
    return "{\"schemaVersion\":\"execution-manifest-no-changes.v1\","
        "\"summary\":\"" + summary + "\",\"evidence\":[]}";
}

std::string taskJson(
    const std::string& id,
    const std::string& dependencies,
    const std::string& ownedPath,
    const std::string& capability)
{
    return "{\"id\":\"" + id
        + "\",\"objective\":\"Complete " + id
        + "\",\"role\":\"worker\",\"dependencies\":" + dependencies
        + ",\"writePaths\":[\"" + ownedPath
        + "\"],\"baselineRequiredReadPaths\":[],\"proposedDependencyReadPaths\":[],"
          "\"referencePaths\":[],\"excludedPaths\":[],\"deferredPaths\":[],"
          "\"acceptanceCriteria\":[\"Tests pass\"],"
          "\"acceptanceContract\":{\"schemaVersion\":\"execution-acceptance.v1\","
          "\"requirements\":[{\"id\":\"verify-output\",\"description\":\"Verify output\","
          "\"requiredOutputs\":[\"" + ownedPath + "\"],\"verificationKind\":\"inspect_paths\","
          "\"verificationProfileId\":null,\"verificationBuildProfileId\":null,"
          "\"verificationTargetId\":null,"
          "\"verificationArguments\":[],\"required\":true,"
          "\"verificationBootstrapPending\":false}]},"
          "\"contextReferences\":[\"CODEX_CONTEXT.md\"],"
          "\"requestedCapability\":\"" + capability + "\"}";
}

std::string coordinationTaskJson(const std::string& id, const std::string& dependencies)
{
    return "{\"id\":\"" + id + "\",\"objective\":\"Coordinate " + id
        + "\",\"role\":\"coordinator\",\"dependencies\":" + dependencies + "}";
}

// Real-Run incident (Run 2f26ecc5-2723-4f9c-98fd-9ff317f13387, 2026-09-11):
// an inspect-capability "verify-task" whose single requirement uses
// `verificationKind` ("cmake_build" reproduces the incident; "inspect_paths"
// is the repaired shape a Planner repair must resubmit). No baselineSnapshot
// is set by these tests' request(), so the inspect_paths variant's
// requiredReadPaths/requiredOutputs deliberately name a context file no
// other task owns, staying clear of the unrelated
// manifest.missing_scope_dependency check.
// MW-P3A package 2: the read-role distinction (baseline vs proposed-
// dependency) is irrelevant to what this helper actually exercises
// (verificationKind shape rejection for an inspect-capability task), so
// both read lists and requiredOutputs stay empty for both variants -
// requiredOutputs is legitimately allowed to be empty (see
// ExecutionAcceptanceContract's own field comment), which avoids needing
// an unrelated baseline/producer fixture just to keep this helper's output
// well-formed.
std::string inspectVerifyTaskJson(const std::string& verificationKind)
{
    const bool isInspectPaths = verificationKind == "inspect_paths";
    const std::string profileFields = isInspectPaths
        ? "\"verificationProfileId\":null,\"verificationBuildProfileId\":null,"
          "\"verificationTargetId\":null,"
        : "\"verificationProfileId\":\"local-qt-debug\","
          "\"verificationBuildProfileId\":\"local-qt-debug\",\"verificationTargetId\":null,";
    return "{\"id\":\"verify-task\","
        "\"objective\":\"Verify build and lint\",\"role\":\"verification\",\"dependencies\":[],"
        "\"writePaths\":[],"
        "\"baselineRequiredReadPaths\":[],\"proposedDependencyReadPaths\":[],"
        "\"referencePaths\":[],\"excludedPaths\":[],\"deferredPaths\":[],"
        "\"acceptanceCriteria\":[\"Build and lint verified\"],"
        "\"acceptanceContract\":{\"schemaVersion\":\"execution-acceptance.v1\","
        "\"requirements\":[{\"id\":\"req-build\",\"description\":\"Build succeeds\","
        "\"requiredOutputs\":[],\"verificationKind\":\""
        + verificationKind + "\"," + profileFields
        + "\"verificationArguments\":[],\"required\":true,"
          "\"verificationBootstrapPending\":false}]},"
        "\"contextReferences\":[\"CODEX_CONTEXT.md\"],"
        "\"requestedCapability\":\"inspect\"}";
}

// Real-Run incident (Runs fcd484b8-.../cebcd3cf-.../c5f7d3da-...,
// 2026-09-11): unlike inspectVerifyTaskJson (always dependencies:[]), this
// lets a test independently control "verify-task"'s own declared
// dependencies - needed to reproduce a primary submission that carries BOTH
// an inspect+command-verification shape defect AND a genuine dependency
// cycle at the same time.
std::string inspectVerifyTaskJsonWithDependencies(
    const std::string& verificationKind, const std::string& dependencies)
{
    const bool isInspectPaths = verificationKind == "inspect_paths";
    const std::string profileFields = isInspectPaths
        ? "\"verificationProfileId\":null,\"verificationBuildProfileId\":null,"
          "\"verificationTargetId\":null,"
        : "\"verificationProfileId\":\"local-qt-debug\","
          "\"verificationBuildProfileId\":\"local-qt-debug\",\"verificationTargetId\":null,";
    return "{\"id\":\"verify-task\","
        "\"objective\":\"Verify build and lint\",\"role\":\"verification\",\"dependencies\":"
        + dependencies + ","
        "\"writePaths\":[],"
        "\"baselineRequiredReadPaths\":[],\"proposedDependencyReadPaths\":[],"
        "\"referencePaths\":[],\"excludedPaths\":[],\"deferredPaths\":[],"
        "\"acceptanceCriteria\":[\"Build and lint verified\"],"
        "\"acceptanceContract\":{\"schemaVersion\":\"execution-acceptance.v1\","
        "\"requirements\":[{\"id\":\"req-build\",\"description\":\"Build succeeds\","
        "\"requiredOutputs\":[],\"verificationKind\":\""
        + verificationKind + "\"," + profileFields
        + "\"verificationArguments\":[],\"required\":true,"
          "\"verificationBootstrapPending\":false}]},"
        "\"contextReferences\":[\"CODEX_CONTEXT.md\"],"
        "\"requestedCapability\":\"inspect\"}";
}

// Planner recovery hardening for manifest.missing_scope_dependency: unlike
// taskJson (a single fixed ownedPaths entry), this lets a test independently
// control ownedPaths/plannedCreatePaths/requiredReadPaths (each "" meaning an
// empty array) - needed to build a genuine consumer/producer scope-
// dependency scenario.
// MW-P3A package 2: ownedPath/plannedCreatePath both collapse into one
// writePaths list - the compiler alone classifies existing-vs-new from the
// baseline, so which parameter a path came from no longer changes any
// caller-visible behavior these tests assert on (deriveManifestDependencyGraph
// and the path-role validator already treated the two identically as "this
// task's write claim"). requiredReadPath maps to proposedDependencyReadPaths:
// every use of this helper in this file sets up a GENUINE producer/consumer
// relationship (the read path is always some other task's ownedPath in the
// same submission), which is exactly what proposedDependencyReadPaths means -
// the newer baseline-only false-ownership tests use their own hand-built
// JSON instead of this helper (see the tests near
// testSharedPreexistingReferenceFileOwnedByNeitherTaskHasNoCycle).
std::string taskJsonWithRoles(
    const std::string& id,
    const std::string& dependencies,
    const std::string& ownedPath,
    const std::string& plannedCreatePath,
    const std::string& requiredReadPath,
    const std::string& capability)
{
    const auto arrayOrEmpty = [](const std::string& value) {
        return value.empty() ? std::string("[]") : "[\"" + value + "\"]";
    };
    const std::string writePathsArray = (!ownedPath.empty() && !plannedCreatePath.empty())
        ? "[\"" + ownedPath + "\",\"" + plannedCreatePath + "\"]"
        : arrayOrEmpty(!ownedPath.empty() ? ownedPath : plannedCreatePath);
    const std::string requiredOutput = !ownedPath.empty() ? ownedPath
        : !plannedCreatePath.empty() ? plannedCreatePath : requiredReadPath;
    return "{\"id\":\"" + id
        + "\",\"objective\":\"Complete " + id
        + "\",\"role\":\"worker\",\"dependencies\":" + dependencies
        + ",\"writePaths\":" + writePathsArray
        + ",\"baselineRequiredReadPaths\":[]"
        + ",\"proposedDependencyReadPaths\":" + arrayOrEmpty(requiredReadPath)
        + ",\"referencePaths\":[],\"excludedPaths\":[],\"deferredPaths\":[],"
          "\"acceptanceCriteria\":[\"Tests pass\"],"
          "\"acceptanceContract\":{\"schemaVersion\":\"execution-acceptance.v1\","
          "\"requirements\":[{\"id\":\"verify-output\",\"description\":\"Verify output\","
          "\"requiredOutputs\":[\"" + requiredOutput + "\"],\"verificationKind\":\"inspect_paths\","
          "\"verificationProfileId\":null,\"verificationBuildProfileId\":null,"
          "\"verificationTargetId\":null,"
          "\"verificationArguments\":[],\"required\":true,"
          "\"verificationBootstrapPending\":false}]},"
          "\"contextReferences\":[\"CODEX_CONTEXT.md\"],"
          "\"requestedCapability\":\"" + capability + "\"}";
}

std::string manifestJson(
    std::vector<std::string> workTasks, std::vector<std::string> coordinationTasks = {})
{
    std::string result =
        "{\"schemaVersion\":\"execution-manifest-draft.v1\",\"coordinationTasks\":[";
    for (std::size_t index = 0; index < coordinationTasks.size(); ++index) {
        if (index != 0) result += ',';
        result += coordinationTasks[index];
    }
    result += "],\"workTasks\":[";
    for (std::size_t index = 0; index < workTasks.size(); ++index) {
        if (index != 0) result += ',';
        result += workTasks[index];
    }
    result += "],\"requestedMaxParallelism\":1,\"requestedTokenBudget\":null,"
        "\"requestedCostMicros\":null,\"bootstrapDeclaration\":null}";
    return result;
}

std::string validManifest(const std::string& capability = "propose")
{
    return manifestJson({taskJson("inspect", "[]", "src/inspect", capability)});
}

// AG-18.2: a provider that still submits a raw environment argument (even
// a "target-only" one) for cmake_build is a forbidden override, not merely
// an incomplete argument set - verificationArguments must be empty for
// this kind, with the real selection made via verificationProfileId/
// verificationBuildProfileId instead.
std::string targetOnlyCMakeBuildManifest()
{
    auto result = validManifest("propose");
    const std::string from =
        "\"verificationKind\":\"inspect_paths\","
        "\"verificationProfileId\":null,\"verificationBuildProfileId\":null,"
        "\"verificationTargetId\":null,"
        "\"verificationArguments\":[]";
    const std::string to =
        "\"verificationKind\":\"cmake_build\","
        "\"verificationProfileId\":\"local-qt-debug\","
        "\"verificationBuildProfileId\":\"local-qt-debug\","
        "\"verificationTargetId\":null,"
        "\"verificationArguments\":[{"
        "\"name\":\"target\",\"value\":\"all_qmllint\"}]";
    const auto position = result.find(from);
    if (position == std::string::npos) {
        throw std::runtime_error("Planner test Manifest shape changed");
    }
    result.replace(position, from.size(), to);
    return result;
}

// Manifest verification reliability hardening: there is no Backend-owned
// trusted catalog of admissible definition/registration/target triples for
// cmake_target_consistency, so a fresh submission using this kind must
// always be rejected fail-closed, before freeze/dispatch, exactly like any
// other malformed/untrusted submission - this fixture is otherwise a
// perfectly well-formed manifest.
std::string cmakeTargetConsistencyManifest()
{
    auto result = validManifest("propose");
    const std::string from =
        "\"verificationKind\":\"inspect_paths\","
        "\"verificationProfileId\":null,\"verificationBuildProfileId\":null,"
        "\"verificationTargetId\":null,"
        "\"verificationArguments\":[]";
    const std::string to =
        "\"verificationKind\":\"cmake_target_consistency\","
        "\"verificationProfileId\":null,\"verificationBuildProfileId\":null,"
        "\"verificationTargetId\":\"Dashboard_Content\","
        "\"verificationArguments\":[{"
        "\"name\":\"definitionFile\",\"value\":\"CMakeLists.txt\"},{"
        "\"name\":\"registrationFile\",\"value\":\"qml/CMakeLists.txt\"}]";
    const auto position = result.find(from);
    if (position == std::string::npos) {
        throw std::runtime_error("Planner test Manifest shape changed");
    }
    result.replace(position, from.size(), to);
    return result;
}

ExecutionManifestPolicy policy(WorkspaceCapability maximum = WorkspaceCapability::Propose)
{
    ExecutionManifestPolicy value;
    value.maxTasks = 8;
    value.maxDepth = 4;
    value.maxParallelism = 3;
    value.maximumCapability = maximum;
    return value;
}

ExecutionManifestPlannerRequest request(
    WorkspaceCapability maximum = WorkspaceCapability::Propose)
{
    ExecutionManifestPlannerRequest value;
    value.runId = "planner-run";
    value.providerKind = "fake";
    value.sessionRequest.providerId = "provider-1";
    value.sessionRequest.modelId = "model-1";
    value.sessionRequest.apiModelId = "test-model";
    value.sessionRequest.systemPrompt = "Plan only the requested work.";
    value.sessionRequest.userContent = "Split the implementation safely.";
    value.sessionRequest.maxTurns = 12;
    value.sessionRequest.maxTotalToolCalls = 48;
    value.sessionRequest.maxToolCallsPerTurn = 16;
    value.sessionRequest.maxOutputTokens = 4096;
    value.inspectPolicy = AgentToolExecutionService::buildPolicy(
        std::filesystem::temp_directory_path() / "planner-workspace");
    value.trustedManifestPolicy = policy(maximum);
    value.repositoryProjection = RepositoryInspectionProjection {
        "revision-42", {"src/current.cpp"}, "one source file changed"};
    return value;
}

ExecutionManifestPlannerRequest noChangesRequest()
{
    auto value = request();
    value.allowEvidenceBackedNoChanges = true;
    return value;
}

bool requestAdvertises(
    const ProviderAgentSessionRequest& request,
    const std::string& toolName)
{
    return std::any_of(request.tools.begin(), request.tools.end(),
        [&](const AgentToolDefinition& tool) { return tool.name == toolName; });
}

void testValidPlanningUsesInspectOnlyToolsAndFreezesManifest()
{
    Fixture fixture;
    fixture.addSession({
        toolTurn(0, "read-1", "workspace_read_file", R"({"path":"README.md"})"),
        submitTurn(1, validManifest())});
    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success && outcome.manifest.has_value(), "Valid planning failed");
    require(outcome.manifest->runId == "planner-run", "Trusted runId was not stamped");
    require(outcome.manifest->policy.maxTasks == 8, "Trusted policy was not frozen");
    require(outcome.totalTurns == 2 && outcome.totalToolCalls == 2,
        "Planner evidence counters are wrong");
    require(outcome.successfulInspectionCalls == 1 && fixture.executor.callCount == 1,
        "Inspection call was not executed exactly once");
    require(fixture.sessionFactory.log.requests.size() == 1,
        "Valid plan unexpectedly opened a repair session");
    const auto& sent = fixture.sessionFactory.log.requests.front();
    require(requestAdvertises(sent, kSubmitExecutionManifestDraftToolName),
        "Manifest terminal tool was not advertised");
    require(!requestAdvertises(sent, "propose_change_set"),
        "Planner advertised the forbidden proposal terminal");
    require(sent.userContent.find("revision-42") != std::string::npos,
        "Typed repository projection was not supplied");
    require(sent.systemPrompt.find("completion preflight") != std::string::npos
            && sent.systemPrompt.find("repository audit") != std::string::npos
            && sent.systemPrompt.find("ancestor/descendant") != std::string::npos
            && sent.systemPrompt.find("exactly these thirteen fields")
                != std::string::npos
            && sent.systemPrompt.find("baselineRequiredReadPaths") != std::string::npos
            && sent.systemPrompt.find("proposedDependencyReadPaths") != std::string::npos
            && sent.systemPrompt.find("referencePaths") != std::string::npos
            && sent.systemPrompt.find("excludedPaths") != std::string::npos
            && sent.systemPrompt.find("deferredPaths") != std::string::npos
            && sent.systemPrompt.find("never repeat a path across roles")
                != std::string::npos
            && sent.systemPrompt.find("already allowed to read") != std::string::npos
            && sent.systemPrompt.find("Every requiredOutputs path")
                != std::string::npos
            && sent.systemPrompt.find("execution-acceptance.v1")
                != std::string::npos,
        "Planner prompt omitted completion or AG-15 path-role guidance");
    const auto manifestTool = std::find_if(sent.tools.begin(), sent.tools.end(),
        [](const AgentToolDefinition& tool) {
            return tool.name == kSubmitExecutionManifestDraftToolName;
        });
    require(manifestTool != sent.tools.end(), "Manifest tool definition was not sent");
    require(manifestTool->inputSchemaJson.find("\"minItems\":1") != std::string::npos
            && manifestTool->inputSchemaJson.find("\"maximum\":32")
                != std::string::npos
            && manifestTool->inputSchemaJson.find("report-only filler tasks")
                != std::string::npos
            && manifestTool->inputSchemaJson.find("execution-acceptance.v1")
                != std::string::npos
            && manifestTool->inputSchemaJson.find("never report labels")
                != std::string::npos
            && manifestTool->inputSchemaJson.find("\"enum\":[\"inspect_paths\",\"cmake_build\",\"qml_lint\"]")
                != std::string::npos,
        "Manifest tool schema omitted its bounded non-empty contract");
    // Manifest verification reliability hardening: there is no Backend-owned
    // trusted catalog of admissible definition/registration/target triples
    // for cmake_target_consistency yet, so a fresh submission must never be
    // able to freely create one - the Planner tool schema no longer
    // advertises this kind at all.
    require(manifestTool->inputSchemaJson.find("cmake_target_consistency")
            == std::string::npos,
        "Manifest tool schema still advertises the untrusted cmake_target_consistency kind");

    const auto sessions = fixture.auditSessions.listForRun("planner-run");
    require(sessions.success() && sessions.value().size() == 1,
        "Planner did not persist one logical audit session");
    require(sessions.value().front().state == AgentToolSessionState::Completed,
        "Successful planner audit session did not complete");
    require(sessions.value().front().totalCalls == 1,
        "Terminal submission was incorrectly counted as filesystem execution");
}

void testPlannerPreservesCacheUsageAcrossAllTurns()
{
    Fixture fixture;
    auto inspected = toolTurn(
        0, "read-cache", "workspace_read_file", R"({"path":"README.md"})");
    inspected.usage.cacheCreationInputTokens = 2;
    inspected.usage.cacheReadInputTokens = 3;
    auto submitted = submitTurn(1, validManifest());
    submitted.usage.cacheCreationInputTokens = 1;
    submitted.usage.cacheReadInputTokens = 4;
    fixture.addSession({std::move(inspected), std::move(submitted)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success, "Cache-usage planner fixture did not complete");
    require(outcome.usage.inputTokens == 14 && outcome.usage.outputTokens == 6,
        "Planner changed the conservative aggregate token totals");
    require(outcome.usage.cacheCreationInputTokens == 3
            && outcome.usage.cacheReadInputTokens == 7,
        "Planner discarded the Anthropic cache-usage breakdown");
}

void testMalformedSubmissionGetsExactlyOneRepair()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0, "{not-json")});
    fixture.addSession({submitTurn(0, validManifest())});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success, "Malformed first submission was not repaired");
    require(outcome.repairAttempts == 1 && fixture.sessionFactory.createCalls == 2,
        "Malformed submission did not use exactly one repair session");
    require(fixture.sessionFactory.log.requests[1].userContent.find("manifest.parse_error")
            != std::string::npos,
        "Repair prompt omitted the typed parse rejection");
    require(fixture.sessionFactory.log.requests[1].userContent.find(
                "exactly these thirteen fields") != std::string::npos
            && fixture.sessionFactory.log.requests[1].userContent.find(
                "baselineRequiredReadPaths") != std::string::npos
            && fixture.sessionFactory.log.requests[1].userContent.find(
                "deferredPaths") != std::string::npos,
        "Repair prompt omitted the complete AG-15 task field contract");
}

void testRequireManifestRejectsAuditOnlyGraphBeforeFreeze()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0, validManifest("inspect"))});
    fixture.addSession({submitTurn(0, validManifest("propose"))});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success && outcome.manifest.has_value()
            && outcome.manifest->tasks.front().requestedCapability
                == WorkspaceCapability::Propose,
        "RequireManifest did not repair an audit-only graph into executable work");
    require(outcome.primaryRejectionCode == "planner.non_executable_manifest"
            && outcome.repairAttempts == 1,
        "Audit-only implementation graph did not use one typed repair");
    require(fixture.sessionFactory.log.requests[1].userContent.find(
                "do not apply automatically") != std::string::npos,
        "Executable-work repair did not disambiguate proposal from Apply");
}

void testAcceptanceOutputOutsideTaskScopeIsRepairedBeforeFreeze()
{
    Fixture fixture;
    auto impossible = validManifest("propose");
    const std::string validOutput = "\"requiredOutputs\":[\"src/inspect\"]";
    const auto position = impossible.find(validOutput);
    require(position != std::string::npos,
        "Acceptance-output fixture shape changed");
    impossible.replace(position, validOutput.size(),
        "\"requiredOutputs\":[\"inspection_notes\"]");
    fixture.addSession({submitTurn(0, impossible)});
    fixture.addSession({submitTurn(0, validManifest("propose"))});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success && outcome.manifest.has_value(),
        "Out-of-scope acceptance output was not repaired");
    require(outcome.primaryRejectionCode
                == "manifest.acceptance_output_outside_task_scope"
            && outcome.repairAttempts == 1,
        "Out-of-scope acceptance output did not use one typed repair");
    require(fixture.sessionFactory.log.requests[1].userContent.find(
                "report label") != std::string::npos,
        "Acceptance-output repair omitted the concrete output-scope rule");
}

// MW-P3A package 3: a task-local finding gives the bounded repair authority
// to correct that task, not to re-plan an unrelated task that was already
// valid. The repaired draft is otherwise valid, so only the Backend's
// monotonicity guard can reject this rewrite.
void testRepairCannotRewriteTaskUnrelatedToPrimaryFinding()
{
    Fixture fixture;
    auto primary = manifestJson({
        taskJson("broken", "[]", "src/broken.cpp", "propose"),
        taskJson("stable", "[]", "src/stable.cpp", "propose")});
    const std::string validOutput = "\"requiredOutputs\":[\"src/broken.cpp\"]";
    const auto primaryOutput = primary.find(validOutput);
    require(primaryOutput != std::string::npos,
        "Monotonic-repair primary fixture shape changed");
    primary.replace(primaryOutput, validOutput.size(),
        "\"requiredOutputs\":[\"outside.txt\"]");

    auto repair = primary;
    const std::string invalidOutput = "\"requiredOutputs\":[\"outside.txt\"]";
    const auto repairOutput = repair.find(invalidOutput);
    const std::string stableObjective = "\"objective\":\"Complete stable\"";
    require(repairOutput != std::string::npos,
        "Monotonic-repair repaired fixture shape changed");
    repair.replace(repairOutput, invalidOutput.size(), validOutput);
    const auto stablePosition = repair.find(stableObjective);
    require(stablePosition != std::string::npos,
        "Monotonic-repair stable-task fixture shape changed");
    repair.replace(stablePosition, stableObjective.size(),
        "\"objective\":\"Unrelated rewrite\"");

    fixture.addSession({submitTurn(0, primary)});
    fixture.addSession({submitTurn(0, repair)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(!outcome.success && outcome.errorCode == "planner.repair_exhausted",
        "A repair rewriting an unrelated task was accepted");
    require(outcome.primaryRejectionCode
                == "manifest.acceptance_output_outside_task_scope"
            && outcome.lastRejectionCode == "planner.repair_regressed",
        "Monotonic repair did not preserve primary/regression diagnostics");
    require(!outcome.lastFindings.empty()
            && outcome.lastFindings.front().code == "planner.repair_regressed",
        "Monotonic repair did not expose its stable rejection finding");
    require(fixture.sessionFactory.log.requests[1].userContent.find(
                "This repair is monotonic") != std::string::npos
            && fixture.sessionFactory.log.requests[1].userContent.find(
                "preserve every unrelated task") != std::string::npos,
        "Repair prompt did not disclose the Backend monotonicity contract");
}

// Run de6d81fb: a repair rejected as planner.repair_regressed named no field, so it could not be
// told whether the model changed a number, a bootstrap declaration or the number of tasks. The
// message must now name the first changed control with Backend-parsed values only.
std::string replaceOnce(std::string text, const std::string& from, const std::string& to)
{
    const auto position = text.find(from);
    require(position != std::string::npos, "Planner test Manifest shape changed");
    text.replace(position, from.size(), to);
    return text;
}

void testRegressedRepairNamesTheChangedTopLevelControl()
{
    struct Case {
        std::string name;
        std::string expected;
        std::function<std::string(const std::string&)> change;
    };
    const std::vector<Case> cases = {
        // schemaVersion is provider-authored text: the message names the field and never quotes
        // the value (the exact-equality check below also fails if "v2" ever leaks into it).
        {"schema version", "schemaVersion",
            [](const std::string& repair) {
                return replaceOnce(repair, "\"schemaVersion\":\"execution-manifest-draft.v1\"",
                    "\"schemaVersion\":\"execution-manifest-draft.v2\"");
            }},
        // The three numeric controls are restored from the primary draft instead of rejecting the
        // repair (ExecutionManifestRepairControls.hpp); see the restoration tests below.
        {"bootstrap", "bootstrapDeclaration: absent -> present",
            [](const std::string& repair) {
                return replaceOnce(repair, "\"bootstrapDeclaration\":null",
                    "\"bootstrapDeclaration\":{\"bootstrapTaskId\":\"broken\","
                    "\"setupPaths\":[\"CMakeUserPresets.json\"]}");
            }},
        {"coordination count", "coordinationTasks count: 0 -> 1",
            [](const std::string& repair) {
                return replaceOnce(repair, "\"coordinationTasks\":[]",
                    "\"coordinationTasks\":[{\"id\":\"anchor\",\"objective\":\"Anchor\","
                    "\"role\":\"coordinator\",\"dependencies\":[]}]");
            }},
        {"work count", "workTasks count: 2 -> 3",
            [](const std::string&) {
                return manifestJson({
                    taskJson("broken", "[]", "src/broken.cpp", "propose"),
                    taskJson("stable", "[]", "src/stable.cpp", "propose"),
                    taskJson("extra", "[]", "src/extra.cpp", "propose")});
            }},
    };
    for (const auto& testCase : cases) {
        Fixture fixture;
        const auto repaired = manifestJson({
            taskJson("broken", "[]", "src/broken.cpp", "propose"),
            taskJson("stable", "[]", "src/stable.cpp", "propose")});
        const auto primary = replaceOnce(repaired,
            "\"requiredOutputs\":[\"src/broken.cpp\"]", "\"requiredOutputs\":[\"outside.txt\"]");
        fixture.addSession({submitTurn(0, primary)});
        fixture.addSession({submitTurn(0, testCase.change(repaired))});

        const auto outcome = fixture.planner.plan(request(), {});
        const std::string acceptedMessage =
            "A repair changing a top-level control was accepted: " + testCase.name;
        require(!outcome.success && outcome.lastRejectionCode == "planner.repair_regressed"
                && !outcome.lastFindings.empty()
                && outcome.lastFindings.front().code == "planner.repair_regressed",
            acceptedMessage.c_str());
        std::string unnamedMessage =
            "The regression message did not name the changed control: " + testCase.name;
        for (const auto& finding : outcome.lastFindings)
            unnamedMessage += " | [" + finding.code + "] " + finding.message;
        require(outcome.lastFindings.front().message
                == "Planner repair changed " + testCase.expected,
            unnamedMessage.c_str());
    }
}

// Run de6d81fb follow-up: requestedMaxParallelism / requestedTokenBudget / requestedCostMicros are
// the model's requests, so a task-scoped repair gets the primary draft's values back instead of
// being rejected for nudging one of them.
ExecutionManifestDraft controlsDraft(
    std::uint32_t parallelism,
    std::optional<std::uint64_t> tokens,
    std::optional<std::uint64_t> cost)
{
    ExecutionManifestDraft draft;
    draft.requestedMaxParallelism = parallelism;
    draft.requestedTokenBudget = tokens;
    draft.requestedCostMicros = cost;
    return draft;
}

std::vector<ExecutionManifestFinding> taskFinding(const std::string& code)
{
    return {{code, "task-local finding", std::string("broken")}};
}

void testRepairControlsAreRestoredFromThePrimaryDraft()
{
    const auto primary = controlsDraft(3, 100000, 7000);
    auto repair = controlsDraft(2, 200000, 9000);
    const auto audit = restoreNonAuthoritativeRepairControls(
        primary, repair, taskFinding("manifest.acceptance_output_outside_task_scope"));
    require(repair.requestedMaxParallelism == 3 && repair.requestedTokenBudget == 100000
            && repair.requestedCostMicros == 7000,
        "All three numeric controls must be restored from the primary draft");
    require(audit.size() == 3
            && audit[0] == "Planner repair changed requestedMaxParallelism from 3 (primary) to 2; "
                           "restored the primary value 3"
            && audit[1] == "Planner repair changed requestedTokenBudget from 100000 (primary) to "
                           "200000; restored the primary value 100000"
            && audit[2] == "Planner repair changed requestedCostMicros from 7000 (primary) to "
                           "9000; restored the primary value 7000",
        "The normalizations must state each field with the repair and primary values");

    auto unchanged = controlsDraft(3, 100000, 7000);
    require(restoreNonAuthoritativeRepairControls(
                primary, unchanged, taskFinding("manifest.empty_objective"))
                .empty(),
        "Equal controls must produce no normalization");
}

void testRepairControlsRestoreNullAndValueInBothDirections()
{
    const auto expectBoth = [](const ExecutionManifestDraft& primary,
                               ExecutionManifestDraft repair, const std::string& tokenLine,
                               const std::string& costLine, const char* message) {
        const auto audit = restoreNonAuthoritativeRepairControls(
            primary, repair, taskFinding("manifest.empty_objective"));
        require(repair.requestedTokenBudget == primary.requestedTokenBudget
                && repair.requestedCostMicros == primary.requestedCostMicros
                && audit.size() == 2 && audit[0] == tokenLine && audit[1] == costLine,
            message);
    };
    expectBoth(controlsDraft(1, std::nullopt, std::nullopt), controlsDraft(1, 200000, 5000),
        "Planner repair changed requestedTokenBudget from null (primary) to 200000; restored the "
        "primary value null",
        "Planner repair changed requestedCostMicros from null (primary) to 5000; restored the "
        "primary value null",
        "A value added by the repair must be restored to null");
    expectBoth(controlsDraft(1, 200000, 5000), controlsDraft(1, std::nullopt, std::nullopt),
        "Planner repair changed requestedTokenBudget from 200000 (primary) to null; restored the "
        "primary value 200000",
        "Planner repair changed requestedCostMicros from 5000 (primary) to null; restored the "
        "primary value 5000",
        "A value dropped by the repair must be restored");
}

void testGraphWideOrEmptyFindingsRestoreNothing()
{
    const auto primary = controlsDraft(3, 100000, 7000);
    const std::vector<std::pair<std::string, std::vector<ExecutionManifestFinding>>> cases = {
        {"no findings", {}},
        {"finding without a task id", {{"manifest.empty_tasks", "graph", std::nullopt}}},
        {"task-local plus task-less",
            {{"manifest.empty_objective", "a", std::string("broken")},
                {"manifest.empty_tasks", "b", std::nullopt}}},
        {"dependency cycle", {{"manifest.dependency_cycle", "c", std::string("broken")}}},
        {"duplicate task id", {{"manifest.duplicate_task_id", "d", std::string("broken")}}},
        {"ownership overlap", {{"manifest.ownership_overlap", "e", std::string("broken")}}},
    };
    for (const auto& [name, findings] : cases) {
        auto repair = controlsDraft(2, 200000, 9000);
        const auto audit = restoreNonAuthoritativeRepairControls(primary, repair, findings);
        const std::string message = "A graph-wide or empty finding set must restore nothing: " + name;
        require(audit.empty() && repair.requestedMaxParallelism == 2
                && repair.requestedTokenBudget == 200000 && repair.requestedCostMicros == 9000,
            message.c_str());
    }
}

void testAFindingOnOneControlExcludesOnlyThatField()
{
    // The validator emits these three codes without a task id (so they are graph-wide today); the
    // per-field exclusion is the guard for the day one of them is attached to a task.
    const std::vector<std::pair<std::string, int>> cases = {
        {"manifest.parallelism_exceeded", 0},
        {"manifest.token_budget_exceeded", 1},
        {"manifest.cost_budget_exceeded", 2},
    };
    for (const auto& [code, keptIndex] : cases) {
        const auto primary = controlsDraft(3, 100000, 7000);
        auto repair = controlsDraft(2, 200000, 9000);
        const auto audit = restoreNonAuthoritativeRepairControls(primary, repair, taskFinding(code));
        const bool parallelismKept = repair.requestedMaxParallelism == 2;
        const bool tokensKept = repair.requestedTokenBudget == 200000;
        const bool costKept = repair.requestedCostMicros == 9000;
        const std::string message = "Only the field named by " + code + " may stay as submitted";
        require(audit.size() == 2 && parallelismKept == (keptIndex == 0)
                && tokensKept == (keptIndex == 1) && costKept == (keptIndex == 2),
            message.c_str());
    }
}

void testRestoringTheControlsTwiceIsIdempotent()
{
    const auto primary = controlsDraft(3, 100000, std::nullopt);
    auto repair = controlsDraft(2, std::nullopt, 9000);
    const auto findings = taskFinding("manifest.empty_objective");
    const auto first = restoreNonAuthoritativeRepairControls(primary, repair, findings);
    const auto afterFirst = repair;
    const auto second = restoreNonAuthoritativeRepairControls(primary, repair, findings);
    require(first.size() == 3 && second.empty()
            && repair.requestedMaxParallelism == afterFirst.requestedMaxParallelism
            && repair.requestedTokenBudget == afterFirst.requestedTokenBudget
            && repair.requestedCostMicros == afterFirst.requestedCostMicros,
        "A second restoration must change and report nothing");
}

void testRestorationLeavesTheHardControlsAlone()
{
    ExecutionManifestDraft primary = controlsDraft(3, 100000, 7000);
    ExecutionManifestDraft repair = controlsDraft(2, 200000, 9000);
    repair.schemaVersion = "other";
    repair.bootstrapDeclaration = ManifestBootstrapDeclaration {"broken", {}};
    repair.coordinationTasks.push_back({"anchor", "Anchor", "coordinator", {}});
    repair.workTasks.emplace_back();
    (void)restoreNonAuthoritativeRepairControls(
        primary, repair, taskFinding("manifest.empty_objective"));
    require(repair.schemaVersion == "other" && repair.bootstrapDeclaration.has_value()
            && repair.coordinationTasks.size() == 1 && repair.workTasks.size() == 1,
        "schemaVersion, bootstrap and the task lists must stay exactly as the repair submitted them");
}

// End to end through plan(): a repair that fixes the named defect but also nudges the numbers is
// accepted with the primary values and an exact audit; a cost the policy would reject is simply
// restored; a structural change next to a numeric one is still rejected, and it is the structural
// change that gets named.
std::string primaryWithOutsideOutput(const std::string& repaired)
{
    return replaceOnce(repaired,
        "\"requiredOutputs\":[\"src/broken.cpp\"]", "\"requiredOutputs\":[\"outside.txt\"]");
}

std::string twoTaskRepairedManifest()
{
    return manifestJson({
        taskJson("broken", "[]", "src/broken.cpp", "propose"),
        taskJson("stable", "[]", "src/stable.cpp", "propose")});
}

void testRepairNudgingNumericControlsIsAcceptedWithPrimaryValues()
{
    Fixture fixture;
    const auto repaired = twoTaskRepairedManifest();
    // The primary asks for parallelism 2; the repair drifts to 3 and also requests a cost the
    // trusted policy would reject with manifest.cost_budget_exceeded.
    const auto primary = replaceOnce(primaryWithOutsideOutput(repaired),
        "\"requestedMaxParallelism\":1", "\"requestedMaxParallelism\":2");
    auto repair = replaceOnce(repaired,
        "\"requestedMaxParallelism\":1", "\"requestedMaxParallelism\":3");
    repair = replaceOnce(repair, "\"requestedCostMicros\":null", "\"requestedCostMicros\":5000");
    fixture.addSession({submitTurn(0, primary)});
    fixture.addSession({submitTurn(0, repair)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success && outcome.manifest.has_value() && outcome.repairAttempts == 1,
        "A task-scoped repair that only nudged numeric controls must be accepted");
    require(outcome.manifest->requestedMaxParallelism == 2
            && !outcome.manifest->requestedCostMicros.has_value(),
        "The frozen Manifest must carry the primary controls, not the repair's");
    std::string auditMessage = "The restoration must be reported in the normalization audit:";
    for (const auto& line : outcome.normalizations) auditMessage += " | " + line;
    // Both tasks reuse the fixture's requirement id, so the cross-task rename is audited as well;
    // the restoration lines come first, in field order.
    require(outcome.normalizations.size() >= 2
            && outcome.normalizations[0]
                == "Planner repair changed requestedMaxParallelism from 2 (primary) to 3; "
                   "restored the primary value 2"
            && outcome.normalizations[1]
                == "Planner repair changed requestedCostMicros from null (primary) to 5000; "
                   "restored the primary value null",
        auditMessage.c_str());
}

void testStructuralChangeNextToANumericOneIsStillRejected()
{
    Fixture fixture;
    const auto repaired = twoTaskRepairedManifest();
    const auto primary = primaryWithOutsideOutput(repaired);
    auto repair = manifestJson({
        taskJson("broken", "[]", "src/broken.cpp", "propose"),
        taskJson("stable", "[]", "src/stable.cpp", "propose"),
        taskJson("extra", "[]", "src/extra.cpp", "propose")});
    repair = replaceOnce(repair, "\"requestedMaxParallelism\":1", "\"requestedMaxParallelism\":2");
    fixture.addSession({submitTurn(0, primary)});
    fixture.addSession({submitTurn(0, repair)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(!outcome.success && outcome.lastRejectionCode == "planner.repair_regressed"
            && !outcome.lastFindings.empty()
            && outcome.lastFindings.front().message
                == "Planner repair changed workTasks count: 2 -> 3",
        "Restoring the numbers must not hide a structural regression, and the message must name "
        "the structural change");
}

void testGraphWidePrimaryFindingKeepsTheRepairsControls()
{
    Fixture fixture;
    // Two tasks claiming overlapping paths is a graph-wide primary finding: the repair may
    // reshape the allocation, so its own parallelism request must be kept.
    const auto primary = manifestJson({
        taskJson("one", "[]", "src", "propose"),
        taskJson("two", "[]", "src/detail", "propose")});
    const auto repair = replaceOnce(validManifest("propose"),
        "\"requestedMaxParallelism\":1", "\"requestedMaxParallelism\":2");
    fixture.addSession({submitTurn(0, primary)});
    fixture.addSession({submitTurn(0, repair)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success && outcome.manifest.has_value()
            && outcome.manifest->requestedMaxParallelism == 2 && outcome.normalizations.empty(),
        "A graph-wide repair must keep its own numeric controls and report no restoration");
}

// Run de6d81fb (1A): a path a task still reads loses the SAME task's excludedPaths note. Only
// excluded vs required_read / reference_only inside one task; write, deferred and cross-task
// conflicts are left to the validator.
ExecutionManifestTask scopeTask(const std::string& id)
{
    ExecutionManifestTask task;
    task.id = id;
    return task;
}

ManifestExcludedOrDeferredPath scopeNote(const std::string& path)
{
    return {std::filesystem::path(path), "not needed"};
}

std::string scopeLine(const std::string& taskId, const std::string& path, const std::string& role)
{
    return "task '" + taskId + "': '" + path + "' is read as " + role
        + ", so its excludedPaths entry was removed";
}

void testRequiredReadRemovesTheSameTasksExcludedEntry()
{
    ExecutionManifest manifest;
    auto task = scopeTask("t1");
    task.requiredReadPaths = {"docs/a.md"};
    task.excludedPaths = {scopeNote("docs/a.md"), scopeNote("docs/keep.md")};
    manifest.tasks.push_back(task);
    const auto audit = canonicalizeExcludedReadConflicts(manifest);
    require(audit == std::vector<std::string> {scopeLine("t1", "docs/a.md", "required_read")},
        "A required read must drop its excluded entry with one exact audit line");
    require(manifest.tasks[0].excludedPaths.size() == 1
            && manifest.tasks[0].excludedPaths[0].path == std::filesystem::path("docs/keep.md")
            && manifest.tasks[0].requiredReadPaths.size() == 1,
        "Only the conflicting excluded entry may go; the read and unrelated notes stay");
}

void testReferenceOnlyRemovesTheSameTasksExcludedEntry()
{
    ExecutionManifest manifest;
    auto task = scopeTask("t1");
    task.referencePaths = {"docs/r.md"};
    task.excludedPaths = {scopeNote("docs/r.md")};
    manifest.tasks.push_back(task);
    const auto audit = canonicalizeExcludedReadConflicts(manifest);
    require(audit == std::vector<std::string> {scopeLine("t1", "docs/r.md", "reference_only")}
            && manifest.tasks[0].excludedPaths.empty() && manifest.tasks[0].referencePaths.size() == 1,
        "A reference-only read must drop its excluded entry");

    // required_read > reference_only names the stronger role when a caller passes both.
    ExecutionManifest both;
    auto bothTask = scopeTask("t1");
    bothTask.requiredReadPaths = {"docs/r.md"};
    bothTask.referencePaths = {"docs/r.md"};
    bothTask.excludedPaths = {scopeNote("docs/r.md")};
    both.tasks.push_back(bothTask);
    require(canonicalizeExcludedReadConflicts(both)
                == std::vector<std::string> {scopeLine("t1", "docs/r.md", "required_read")},
        "required_read must win over reference_only when naming the role");
}

void testEquivalentNormalizedPathsConflict()
{
    ExecutionManifest manifest;
    auto task = scopeTask("t1");
    task.requiredReadPaths = {"docs/a.md", "Docs/Sub/File.qml"};
    task.excludedPaths = {scopeNote("DOCS/A.MD"), scopeNote("docs\\sub\\file.qml")};
    manifest.tasks.push_back(task);
    const auto audit = canonicalizeExcludedReadConflicts(manifest);
    require(audit.size() == 2 && audit[0] == scopeLine("t1", "docs/a.md", "required_read")
            && audit[1] == scopeLine("t1", "Docs/Sub/File.qml", "required_read")
            && manifest.tasks[0].excludedPaths.empty(),
        "Case and separator variants of the same path must be treated as the same path");
}

void testSeveralExcludedDuplicatesGiveOneNormalization()
{
    ExecutionManifest manifest;
    auto task = scopeTask("t1");
    task.requiredReadPaths = {"docs/a.md"};
    task.excludedPaths = {scopeNote("docs/a.md"), scopeNote("DOCS/A.MD"), scopeNote("docs/a.md")};
    manifest.tasks.push_back(task);
    const auto audit = canonicalizeExcludedReadConflicts(manifest);
    require(audit == std::vector<std::string> {scopeLine("t1", "docs/a.md", "required_read")}
            && manifest.tasks[0].excludedPaths.empty(),
        "Every excluded duplicate must go, with a single audit line for the path");
}

void testWriteAndDeferredConflictsAreNotResolved()
{
    ExecutionManifest manifest;
    auto owned = scopeTask("owned");
    owned.ownedPaths = {"src/a.cpp"};
    owned.requiredReadPaths = {"src/a.cpp"}; // defensive: even if a caller skipped the active pass
    owned.excludedPaths = {scopeNote("src/a.cpp")};
    auto created = scopeTask("created");
    created.plannedCreatePaths = {"src/new.cpp"};
    created.referencePaths = {"src/new.cpp"};
    created.excludedPaths = {scopeNote("src/new.cpp")};
    auto deferred = scopeTask("deferred");
    deferred.referencePaths = {"docs/r.md"};
    deferred.deferredPaths = {scopeNote("docs/r.md")};
    manifest.tasks = {owned, created, deferred};
    const auto audit = canonicalizeExcludedReadConflicts(manifest);
    require(audit.empty() && manifest.tasks[0].excludedPaths.size() == 1
            && manifest.tasks[1].excludedPaths.size() == 1
            && manifest.tasks[2].deferredPaths.size() == 1
            && manifest.tasks[2].referencePaths.size() == 1,
        "owned_write / planned_create + excluded and read + deferred must stay untouched");
}

void testTheSamePathInAnotherTaskIsNotCanonicalized()
{
    ExecutionManifest manifest;
    auto excluder = scopeTask("excluder");
    excluder.excludedPaths = {scopeNote("docs/a.md")};
    auto reader = scopeTask("reader");
    reader.requiredReadPaths = {"docs/a.md"};
    manifest.tasks = {excluder, reader};
    require(canonicalizeExcludedReadConflicts(manifest).empty()
            && manifest.tasks[0].excludedPaths.size() == 1
            && manifest.tasks[1].requiredReadPaths.size() == 1,
        "An excluded entry of one task must not be removed because another task reads the path");
}

void testExcludedReadCanonicalizationIsIdempotentAndOrdered()
{
    ExecutionManifest manifest;
    auto first = scopeTask("t1");
    first.requiredReadPaths = {"docs/a.md", "docs/b.md"};
    first.referencePaths = {"docs/c.md"};
    first.excludedPaths = {scopeNote("docs/b.md"), scopeNote("docs/c.md"), scopeNote("docs/a.md")};
    auto second = scopeTask("t2");
    second.referencePaths = {"docs/d.md"};
    second.excludedPaths = {scopeNote("docs/d.md")};
    manifest.tasks = {first, second};
    const auto audit = canonicalizeExcludedReadConflicts(manifest);
    require(audit
                == std::vector<std::string> {
                    scopeLine("t1", "docs/b.md", "required_read"),
                    scopeLine("t1", "docs/c.md", "reference_only"),
                    scopeLine("t1", "docs/a.md", "required_read"),
                    scopeLine("t2", "docs/d.md", "reference_only")},
        "The audit must list tasks in Manifest order and excluded entries in submitted order");
    const auto afterFirst = manifest;
    require(canonicalizeExcludedReadConflicts(manifest).empty()
            && manifest.tasks[0].excludedPaths.empty() && manifest.tasks[1].excludedPaths.empty()
            && manifest.tasks[0].requiredReadPaths == afterFirst.tasks[0].requiredReadPaths,
        "A second canonicalization must change and report nothing");
}

std::string scopeArray(const std::vector<std::string>& paths)
{
    std::string result = "[";
    for (std::size_t index = 0; index < paths.size(); ++index) {
        if (index != 0) result += ',';
        result += "\"" + paths[index] + "\"";
    }
    return result + "]";
}

std::string scopeNoteArray(const std::vector<std::string>& paths)
{
    std::string result = "[";
    for (std::size_t index = 0; index < paths.size(); ++index) {
        if (index != 0) result += ',';
        result += "{\"path\":\"" + paths[index] + "\",\"reason\":\"not needed\"}";
    }
    return result + "]";
}

// taskJson() with its four read/exclusion lists replaced.
std::string taskJsonWithScope(
    const std::string& id,
    const std::string& writePath,
    const std::vector<std::string>& baselineReads,
    const std::vector<std::string>& references,
    const std::vector<std::string>& excluded,
    const std::vector<std::string>& deferred)
{
    return replaceOnce(taskJson(id, "[]", writePath, "propose"),
        "\"baselineRequiredReadPaths\":[],\"proposedDependencyReadPaths\":[],"
        "\"referencePaths\":[],\"excludedPaths\":[],\"deferredPaths\":[]",
        "\"baselineRequiredReadPaths\":" + scopeArray(baselineReads)
            + ",\"proposedDependencyReadPaths\":[],\"referencePaths\":" + scopeArray(references)
            + ",\"excludedPaths\":" + scopeNoteArray(excluded)
            + ",\"deferredPaths\":" + scopeNoteArray(deferred));
}

ExecutionManifestPlannerRequest requestWithBaselineFiles(const std::vector<std::string>& files)
{
    auto plannerRequest = request();
    WorkspaceBaselineSnapshot baseline;
    for (const auto& file : files) {
        WorkspaceBaselineEntry entry;
        entry.relativePath = file;
        entry.byteSize = 10;
        entry.sha256 = "hash-" + file;
        baseline.files.push_back(entry);
    }
    plannerRequest.baselineSnapshot = baseline;
    return plannerRequest;
}

// The validator keeps rejecting what the canonicalization deliberately leaves alone.
void testWriteAndDeferredExcludedConflictsStillFailThroughPlan()
{
    struct Case {
        std::string name;
        std::string task;
        std::vector<std::string> baseline;
    };
    const std::vector<Case> cases = {
        {"owned_write + required_read + excluded",
            taskJsonWithScope("t1", "src/app.cpp", {"src/app.cpp"}, {}, {"src/app.cpp"}, {}),
            {"src/app.cpp"}},
        {"planned_create + reference + excluded",
            taskJsonWithScope("t1", "src/app.cpp", {}, {"src/app.cpp"}, {"src/app.cpp"}, {}),
            {}},
        {"reference + deferred",
            taskJsonWithScope("t1", "src/app.cpp", {}, {"docs/r.md"}, {}, {"docs/r.md"}),
            {"docs/r.md"}},
    };
    for (const auto& testCase : cases) {
        Fixture fixture;
        const auto bad = manifestJson({testCase.task});
        fixture.addSession({submitTurn(0, bad)});
        fixture.addSession({submitTurn(0, bad)});
        const auto outcome = fixture.planner.plan(requestWithBaselineFiles(testCase.baseline), {});
        const std::string message = "The validator must still reject: " + testCase.name;
        require(!outcome.success && outcome.primaryRejectionCode == "manifest.conflicting_path_role",
            message.c_str());
    }
}

// The Run de6d81fb shape: three required reads that were also excluded, across the machine and
// alert tasks. The primary submission now passes without a paid repair session.
void testMachinesAndAlertsScenarioPassesWithoutRepair()
{
    Fixture fixture;
    const std::string metric = "qml/components/machines/MachineMetricPanel.qml";
    const std::string trend = "qml/components/machines/MachineTrendChart.qml";
    const std::string badge = "qml/components/alerts/AlertSeverityBadge.qml";
    const std::string history = "qml/views/HistoryView.qml";
    const auto manifest = manifestJson({
        taskJsonWithScope("machine-details-responsive", "qml/views/MachineDetailsView.qml",
            {metric, trend}, {}, {metric, trend, history}, {}),
        taskJsonWithScope("alerts-responsive", "qml/views/AlertsView.qml",
            {badge}, {}, {badge}, {})});
    fixture.addSession({submitTurn(0, manifest)});

    const auto outcome = fixture.planner.plan(requestWithBaselineFiles({
        "qml/views/MachineDetailsView.qml", "qml/views/AlertsView.qml", metric, trend, badge,
        history}), {});
    require(outcome.success && outcome.manifest.has_value(),
        "The machines-and-alerts primary submission must be accepted");
    require(outcome.repairAttempts == 0 && fixture.sessionFactory.createCalls == 1
            && outcome.primaryRejectionCode.empty() && outcome.primaryFindings.empty(),
        "No repair session may be spent on the excluded/required-read conflict");

    std::vector<std::string> removed;
    for (const auto& line : outcome.normalizations) {
        if (line.find("excludedPaths entry was removed") != std::string::npos) removed.push_back(line);
    }
    require(removed
                == std::vector<std::string> {
                    scopeLine("machine-details-responsive", metric, "required_read"),
                    scopeLine("machine-details-responsive", trend, "required_read"),
                    scopeLine("alerts-responsive", badge, "required_read")},
        "The three removed excluded entries must be audited in order");
    const auto& machine = outcome.manifest->tasks[0];
    require(machine.excludedPaths.size() == 1
            && machine.excludedPaths[0].path == std::filesystem::path(history)
            && machine.requiredReadPaths.size() == 2
            && outcome.manifest->tasks[1].excludedPaths.empty(),
        "Only the conflicting notes may be removed; reads and unrelated notes stay");
}

void testTaskLocalRepairPreservesUnrelatedTaskAndSucceeds()
{
    Fixture fixture;
    const auto repaired = manifestJson({
        taskJson("broken", "[]", "src/broken.cpp", "propose"),
        taskJson("stable", "[]", "src/stable.cpp", "propose")});
    auto primary = repaired;
    const std::string validOutput = "\"requiredOutputs\":[\"src/broken.cpp\"]";
    const auto outputPosition = primary.find(validOutput);
    require(outputPosition != std::string::npos,
        "Positive monotonic-repair fixture shape changed");
    primary.replace(outputPosition, validOutput.size(),
        "\"requiredOutputs\":[\"outside.txt\"]");

    fixture.addSession({submitTurn(0, primary)});
    fixture.addSession({submitTurn(0, repaired)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success && outcome.manifest.has_value()
            && outcome.manifest->tasks.size() == 2,
        "A task-local repair preserving its unrelated task was rejected");
    require(outcome.primaryRejectionCode
                == "manifest.acceptance_output_outside_task_scope"
            && outcome.repairAttempts == 1,
        "Successful monotonic repair lost its primary diagnostics");
}

// Fixing the named defect while introducing a different defect on that same
// mutable task is still regression, not progress. This catches the exact
// primary-A/final-B failure pattern from the paid Runs without comparing
// provider prose or raw JSON.
void testRepairCannotTradePrimaryFindingForNewFinding()
{
    Fixture fixture;
    auto primary = validManifest("propose");
    const std::string validOutput = "\"requiredOutputs\":[\"src/inspect\"]";
    const auto primaryOutput = primary.find(validOutput);
    require(primaryOutput != std::string::npos,
        "Finding-regression primary fixture shape changed");
    primary.replace(primaryOutput, validOutput.size(),
        "\"requiredOutputs\":[\"outside.txt\"]");

    auto repair = primary;
    const std::string invalidOutput = "\"requiredOutputs\":[\"outside.txt\"]";
    const auto repairOutput = repair.find(invalidOutput);
    const std::string validObjective = "\"objective\":\"Complete inspect\"";
    require(repairOutput != std::string::npos,
        "Finding-regression repaired fixture shape changed");
    repair.replace(repairOutput, invalidOutput.size(), validOutput);
    const auto objectivePosition = repair.find(validObjective);
    require(objectivePosition != std::string::npos,
        "Finding-regression objective fixture shape changed");
    repair.replace(objectivePosition, validObjective.size(), "\"objective\":\"\"");

    fixture.addSession({submitTurn(0, primary)});
    fixture.addSession({submitTurn(0, repair)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(!outcome.success && outcome.lastRejectionCode == "planner.repair_regressed",
        "A repair trading its primary finding for a new finding was accepted");
    require(outcome.lastFindings.size() >= 2
            && outcome.lastFindings.front().code == "planner.repair_regressed"
            && outcome.lastFindings[1].code == "manifest.empty_objective",
        "Finding regression did not retain the safe final Backend finding");
}

void testStringExcludedPathIsRepairedToPathReasonObject()
{
    Fixture fixture;
    auto invalid = validManifest();
    auto repaired = invalid;
    const std::string emptyExcluded = "\"excludedPaths\":[]";
    const auto invalidPosition = invalid.find(emptyExcluded);
    const auto repairedPosition = repaired.find(emptyExcluded);
    require(invalidPosition != std::string::npos && repairedPosition != std::string::npos,
        "Planner excluded-path fixture shape changed");
    invalid.replace(invalidPosition, emptyExcluded.size(),
        "\"excludedPaths\":[\"vendor/generated.qml\"]");
    repaired.replace(repairedPosition, emptyExcluded.size(),
        "\"excludedPaths\":[{\"path\":\"vendor/generated.qml\","
        "\"reason\":\"Generated vendor content is outside this task\"}]");
    fixture.addSession({submitTurn(0, invalid)});
    fixture.addSession({submitTurn(0, repaired)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success && outcome.manifest.has_value(),
        "String-form excluded path was not repaired to a path/reason object");
    require(outcome.primaryRejectionCode == "manifest.parse_error"
            && outcome.repairAttempts == 1,
        "Excluded-path incident did not use exactly one typed validation repair");
    require(outcome.manifest->tasks.front().excludedPaths.size() == 1
            && outcome.manifest->tasks.front().excludedPaths.front().reason
                == "Generated vendor content is outside this task",
        "Repaired excluded-path audit reason was not preserved");
    const auto& repairPrompt = fixture.sessionFactory.log.requests[1].userContent;
    require(repairPrompt.find("excludedPaths and deferredPaths contain only objects")
            != std::string::npos
            && repairPrompt.find("path and reason") != std::string::npos,
        "Repair prompt omitted the excluded/deferred path/reason object contract");
}

void testRedundantOwnedAndRequiredReadRoleIsCanonicalizedWithoutRepair()
{
    Fixture fixture;
    auto conflicting = validManifest();
    const std::string emptyProposedDependency = "\"proposedDependencyReadPaths\":[]";
    const auto position = conflicting.find(emptyProposedDependency);
    require(position != std::string::npos,
        "Planner conflicting path-role fixture shape changed");
    conflicting.replace(position, emptyProposedDependency.size(),
        "\"proposedDependencyReadPaths\":[\"src/inspect\"]");
    fixture.addSession({submitTurn(0, conflicting)});

    // "src/inspect" must be a real baseline file so the compiler classifies
    // it as ownedPaths (an existing file being modified) rather than
    // plannedCreatePaths - this test is specifically about the owned/
    // required-read redundancy, not about write-path classification.
    auto plannerRequest = request();
    WorkspaceBaselineSnapshot baseline;
    WorkspaceBaselineEntry inspectEntry;
    inspectEntry.relativePath = "src/inspect";
    inspectEntry.byteSize = 10;
    inspectEntry.sha256 = "hash-inspect";
    baseline.files.push_back(inspectEntry);
    plannerRequest.baselineSnapshot = baseline;

    const auto outcome = fixture.planner.plan(std::move(plannerRequest), {});
    require(outcome.success && outcome.manifest.has_value(),
        "Redundant owned/required-read roles were not canonicalized");
    require(outcome.repairAttempts == 0 && fixture.sessionFactory.createCalls == 1,
        "Safe active-role canonicalization unnecessarily spent a repair session");
    require(outcome.manifest->tasks.front().ownedPaths
                == std::vector<std::filesystem::path> {"src/inspect"}
            && outcome.manifest->tasks.front().requiredReadPaths.empty(),
        "Canonicalization did not retain the modified file exclusively as owned");
}

void testAllRedundantActiveScopeRolesUseStableNonExpandingPrecedence()
{
    // MW-P3A package 2: writePaths is one list now, classified by the
    // Backend compiler against the baseline rather than declared by the
    // provider - "src/inspect" is put in the baseline (compiles to
    // ownedPaths) and "qml/New.qml" is left absent (compiles to
    // plannedCreatePaths), so this still exercises both write-classification
    // branches alongside the dedup/precedence canonicalization pass.
    Fixture fixture;
    auto redundant = validManifest();
    const std::string oldRoles =
        "\"writePaths\":[\"src/inspect\"],"
        "\"baselineRequiredReadPaths\":[],\"proposedDependencyReadPaths\":[],"
        "\"referencePaths\":[]";
    const std::string newRoles =
        "\"writePaths\":[\"src/inspect\",\"SRC/INSPECT\","
        "\"qml/New.qml\",\"QML/NEW.QML\"],"
        "\"baselineRequiredReadPaths\":[\"src/inspect\",\"qml/New.qml\","
        "\"README.md\",\"readme.md\"],\"proposedDependencyReadPaths\":[],"
        "\"referencePaths\":[\"src/inspect\",\"qml/New.qml\","
        "\"README.md\",\"docs/guide.md\",\"DOCS/GUIDE.MD\"]";
    const auto position = redundant.find(oldRoles);
    require(position != std::string::npos,
        "Planner redundant-active-role fixture shape changed");
    redundant.replace(position, oldRoles.size(), newRoles);
    fixture.addSession({submitTurn(0, redundant)});

    auto plannerRequest = request();
    WorkspaceBaselineSnapshot baseline;
    WorkspaceBaselineEntry inspectEntry;
    inspectEntry.relativePath = "src/inspect";
    inspectEntry.byteSize = 10;
    inspectEntry.sha256 = "hash-inspect";
    baseline.files.push_back(inspectEntry);
    WorkspaceBaselineEntry readmeEntry;
    readmeEntry.relativePath = "README.md";
    readmeEntry.byteSize = 10;
    readmeEntry.sha256 = "hash-readme";
    baseline.files.push_back(readmeEntry);
    // "docs/guide.md" survives canonicalization as a referencePaths entry -
    // RunWorkspaceScopeService::validateManifestAgainstBaseline independently
    // requires every active reference_only path to be either a real baseline
    // file or another task's output, so it must also be a real baseline
    // entry here, exactly like README.md above.
    WorkspaceBaselineEntry guideEntry;
    guideEntry.relativePath = "docs/guide.md";
    guideEntry.byteSize = 10;
    guideEntry.sha256 = "hash-guide";
    baseline.files.push_back(guideEntry);
    plannerRequest.baselineSnapshot = baseline;

    const auto outcome = fixture.planner.plan(std::move(plannerRequest), {});
    require(outcome.success && outcome.manifest.has_value(),
        "Redundant active roles were not canonicalized");
    require(outcome.repairAttempts == 0,
        "Redundant active roles unexpectedly consumed the repair budget");
    const auto& task = outcome.manifest->tasks.front();
    require(task.ownedPaths == std::vector<std::filesystem::path> {"src/inspect"}
            && task.plannedCreatePaths
                == std::vector<std::filesystem::path> {"qml/New.qml"}
            && task.requiredReadPaths
                == std::vector<std::filesystem::path> {"README.md"}
            && task.referencePaths
                == std::vector<std::filesystem::path> {"docs/guide.md"},
        "Active-role precedence expanded authority or lost stable first-seen order");
}

void testContradictoryActiveAndExcludedRolesRemainFailClosed()
{
    Fixture fixture;
    auto contradictory = validManifest();
    const std::string emptyExcluded = "\"excludedPaths\":[]";
    const auto position = contradictory.find(emptyExcluded);
    require(position != std::string::npos,
        "Planner contradictory-role fixture shape changed");
    contradictory.replace(position, emptyExcluded.size(),
        "\"excludedPaths\":[{\"path\":\"src/inspect\","
        "\"reason\":\"This file must remain inactive\"}]");
    fixture.addSession({submitTurn(0, contradictory)});
    fixture.addSession({submitTurn(0, contradictory)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(!outcome.success && outcome.errorCode == "planner.repair_exhausted",
        "Active/excluded contradiction must remain fail-closed");
    require(outcome.primaryRejectionCode == "manifest.conflicting_path_role"
            && outcome.lastRejectionCode == "manifest.conflicting_path_role",
        "Contradictory role rejection was hidden by active-role canonicalization");
    const auto stored = fixture.manifests.findForRun("planner-run");
    require(stored.success() && !stored.value().has_value(),
        "Contradictory role submission unexpectedly froze a Manifest");
}

// MW-P3A package 2: the pre-package-2 version of this test proved a
// provider mis-declaring an absent file as ownedPaths got a typed repair
// finding (workspace_scope.owned_write_path_missing_from_baseline) and had
// to resubmit with the role moved to plannedCreatePaths. That whole defect
// class is now structurally unrepresentable - a work task declares only
// writePaths, and the Backend compiler alone classifies existing-vs-new
// from the frozen baseline (see Run 3ffb52df-98ca-472f-9b1c-537ab134cf18).
// This is now a positive test: the exact same absent-file submission
// compiles correctly and freezes on the FIRST attempt, no repair needed.
void testAbsentWritePathClassifiesAsPlannedCreateWithoutAnyRepair()
{
    Fixture fixture;
    const auto submission = manifestJson({
        taskJson("create-ui", "[]", "qml/NewPanel.qml", "propose")});
    fixture.addSession({submitTurn(0, submission)});

    auto plannerRequest = request();
    plannerRequest.baselineSnapshot = WorkspaceBaselineSnapshot {};
    const auto outcome = fixture.planner.plan(std::move(plannerRequest), {});
    require(outcome.success && outcome.manifest.has_value(),
        "An absent writePaths entry must compile and freeze cleanly");
    require(outcome.repairAttempts == 0 && fixture.sessionFactory.createCalls == 1,
        "The Backend compiler must classify existing-vs-new correctly on the first "
        "attempt - no repair should ever be needed for this");
    require(outcome.manifest->tasks.front().ownedPaths.empty()
            && outcome.manifest->tasks.front().plannedCreatePaths
                == std::vector<std::filesystem::path> {"qml/NewPanel.qml"},
        "An absent baseline path must compile to plannedCreatePaths, never ownedPaths");
}

void testInvalidVerificationProfileUsesExactlyOneRepairBeforeFreeze()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0, targetOnlyCMakeBuildManifest())});
    fixture.addSession({submitTurn(0, validManifest("propose"))});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success && outcome.manifest.has_value(),
        "Invalid verification profile was not repaired");
    require(outcome.repairAttempts == 1 && fixture.sessionFactory.createCalls == 2,
        "Invalid verification profile did not use exactly one repair session");
    require(outcome.primaryRejectionCode
            == "acceptance.provider_owned_verification_argument",
        "Planner lost the primary verification-profile rejection code");
    const auto& repairPrompt = fixture.sessionFactory.log.requests[1].userContent;
    require(repairPrompt.find("acceptance.provider_owned_verification_argument")
                != std::string::npos
            && repairPrompt.find("verificationProfileId") != std::string::npos
            && repairPrompt.find("verificationBuildProfileId") != std::string::npos
            && repairPrompt.find("qml_lint") != std::string::npos,
        "Repair prompt omitted the exact verification-profile contract");
}

// P1 #2/#3: a provider submission using cmake_target_consistency is
// rejected before freeze/dispatch with the dedicated stable code, the
// existing bounded repair recovers it by resubmitting an ordinary manifest,
// and the frozen result never contains this kind - so no worker session can
// ever be dispatched against an untrusted consistency requirement (P1 #5).
void testCmakeTargetConsistencySubmissionIsRejectedThenRepairedToBuild()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0, cmakeTargetConsistencyManifest())});
    fixture.addSession({submitTurn(0, validManifest("propose"))});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success && outcome.manifest.has_value(),
        "Untrusted cmake_target_consistency submission was not repaired");
    require(outcome.repairAttempts == 1 && fixture.sessionFactory.createCalls == 2,
        "cmake_target_consistency submission did not use exactly one repair session");
    require(outcome.primaryRejectionCode
            == "acceptance.untrusted_cmake_consistency_selection",
        "Planner lost the untrusted cmake_target_consistency rejection code");
    const auto& repairPrompt = fixture.sessionFactory.log.requests[1].userContent;
    require(repairPrompt.find("acceptance.untrusted_cmake_consistency_selection")
                != std::string::npos
            && repairPrompt.find("cmake_build") != std::string::npos,
        "Repair prompt omitted the untrusted cmake_target_consistency explanation");
    for (const auto& task : outcome.manifest->tasks) {
        if (!task.acceptanceContract.has_value()) continue;
        for (const auto& requirementItem : task.acceptanceContract->requirements) {
            require(requirementItem.verificationKind
                    != ExecutionVerificationKind::CMakeTargetConsistency,
                "A frozen Manifest must never contain an untrusted "
                "cmake_target_consistency requirement");
        }
    }
}

// P1 #4: a repeated invalid submission exhausts the ordinary bounded repair
// exactly like any other rejection - no special-cased extra leniency for
// this kind.
void testCmakeTargetConsistencyRepeatedSubmissionExhaustsRepair()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0, cmakeTargetConsistencyManifest())});
    fixture.addSession({submitTurn(0, cmakeTargetConsistencyManifest())});

    const auto outcome = fixture.planner.plan(request(), {});
    require(!outcome.success && outcome.errorCode == "planner.repair_exhausted",
        "A repeated untrusted cmake_target_consistency submission did not exhaust repair");
    require(outcome.lastRejectionCode
            == "acceptance.untrusted_cmake_consistency_selection",
        "Exhausted outcome lost the untrusted cmake_target_consistency rejection code");
    const auto stored = fixture.manifests.findForRun("planner-run");
    require(stored.success() && !stored.value().has_value(),
        "Exhausted repair left a partial manifest snapshot");
}

void testOwnershipOverlapIsRejectedThenRepaired()
{
    Fixture fixture;
    const auto overlap = manifestJson({
        taskJson("one", "[]", "src", "propose"),
        taskJson("two", "[]", "src/detail", "propose")});
    fixture.addSession({submitTurn(0, overlap)});
    fixture.addSession({submitTurn(0, validManifest("propose"))});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success, "Ownership overlap was not repairable");
    require(fixture.sessionFactory.log.requests[1].userContent.find(
                "manifest.ownership_overlap") != std::string::npos,
        "Overlap finding was not returned to the repair session");
}

// Stage 9B Run cbe83247: the Planner is not told the Run's parallelism ceiling and guessed above it in
// 4 of 6 roster Runs; each rejection cost a paid repair session and one repair regressed. The
// request is a hint, so the Backend lowers it to the ceiling (the test policy's ceiling is 3).
std::string withRequestedParallelism(std::string manifest, const std::string& value)
{
    const std::string from = "\"requestedMaxParallelism\":1,";
    const auto position = manifest.find(from);
    require(position != std::string::npos, "Planner test Manifest shape changed");
    manifest.replace(position, from.size(), "\"requestedMaxParallelism\":" + value + ",");
    return manifest;
}

void testRequestedParallelismAboveTheCeilingIsLoweredNotRejected()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0, withRequestedParallelism(validManifest("propose"), "5"))});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success && outcome.manifest.has_value(),
        "A parallelism request above the ceiling must not fail planning");
    require(outcome.repairAttempts == 0 && fixture.sessionFactory.createCalls == 1
            && outcome.primaryRejectionCode.empty() && outcome.primaryFindings.empty(),
        "A parallelism request above the ceiling must not spend a repair session");
    require(outcome.manifest->requestedMaxParallelism == 3,
        "The frozen Manifest must carry the ceiling, never the larger request");
    require(outcome.normalizations.size() == 1
            && outcome.normalizations.front().find("5") != std::string::npos
            && outcome.normalizations.front().find("lowered to 3") != std::string::npos,
        "The lowered request must be reported in the normalization audit");
}

void testRequestedParallelismWithinTheCeilingIsKeptAsSubmitted()
{
    for (const char* value : {"1", "2", "3"}) {
        Fixture fixture;
        fixture.addSession({submitTurn(0, withRequestedParallelism(validManifest("propose"), value))});
        const auto outcome = fixture.planner.plan(request(), {});
        require(outcome.success && outcome.manifest.has_value()
                && outcome.manifest->requestedMaxParallelism == std::stoul(value)
                && outcome.normalizations.empty(),
            "A parallelism request at or below the ceiling must be frozen unchanged");
    }
}

void testZeroOrOversizedParallelismRequestIsStillRejected()
{
    for (const char* value : {"0", "33"}) {
        Fixture fixture;
        fixture.addSession({submitTurn(0, withRequestedParallelism(validManifest("propose"), value))});
        fixture.addSession({submitTurn(0, validManifest("propose"))});
        const auto outcome = fixture.planner.plan(request(), {});
        require(outcome.success && outcome.repairAttempts == 1
                && !outcome.primaryRejectionCode.empty() && outcome.normalizations.empty(),
            "A zero or beyond-schema parallelism request must stay a fail-closed rejection, "
            "never be silently lowered");
    }
}

void testNormalizationAuditDescribesOnlyTheAcceptedSubmission()
{
    Fixture fixture;
    // The primary submission asks for 5 and is rejected for an unrelated defect; its repair asks for
    // 2, which needs no lowering. The audit must not keep the primary's stale note.
    const auto overlap = withRequestedParallelism(manifestJson({
        taskJson("one", "[]", "src", "propose"),
        taskJson("two", "[]", "src/detail", "propose")}), "5");
    fixture.addSession({submitTurn(0, overlap)});
    fixture.addSession({submitTurn(0, withRequestedParallelism(validManifest("propose"), "2"))});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success && outcome.repairAttempts == 1
            && outcome.primaryRejectionCode == "manifest.ownership_overlap",
        "The unrelated defect must still be rejected and repaired");
    require(outcome.normalizations.empty() && outcome.manifest->requestedMaxParallelism == 2,
        "The audit must describe the accepted submission, not the rejected primary one");
}

// Stage 10A-Repair Run 8e22e38a: two independent tasks both declared build-local-debug and
// lint-dashboard-content. Planning accepted it, both workers were paid, then the proposal-set
// verifier failed with verification.invalid_criterion because it keys its criteria by requirement id
// across the whole Manifest. taskJson() gives every task the requirement id "verify-output".
std::string withRequirementId(std::string task, const std::string& id)
{
    const std::string from = "{\"id\":\"verify-output\"";
    const auto position = task.find(from);
    require(position != std::string::npos, "Planner test task shape changed");
    task.replace(position, from.size(), "{\"id\":\"" + id + "\"");
    return task;
}

std::vector<std::string> frozenRequirementIds(const ExecutionManifest& manifest)
{
    std::vector<std::string> ids;
    for (const auto& task : manifest.tasks) {
        if (!task.acceptanceContract.has_value()) continue;
        for (const auto& requirement : task.acceptanceContract->requirements)
            ids.push_back(requirement.id);
    }
    return ids;
}

void testRequirementIdRepeatedByAnotherTaskIsRenamedNotRejected()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0, manifestJson({
        taskJson("one", "[]", "src/one", "propose"),
        taskJson("two", "[]", "src/two", "propose")}))});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success && outcome.manifest.has_value(),
        "A requirement id repeated by another task must not fail planning");
    require(outcome.repairAttempts == 0 && fixture.sessionFactory.createCalls == 1
            && outcome.primaryRejectionCode.empty() && outcome.primaryFindings.empty(),
        "A requirement id repeated by another task must not spend a repair session");
    const auto ids = frozenRequirementIds(*outcome.manifest);
    require(ids.size() == 2 && ids[0] == "verify-output" && ids[1] == "verify-output-two",
        "The first task must keep its requirement id and the later one must get the task suffix");
    require(outcome.normalizations.size() == 1
            && outcome.normalizations.front().find("'verify-output' of task 'two'")
                != std::string::npos
            && outcome.normalizations.front().find("task 'one'") != std::string::npos
            && outcome.normalizations.front().find("renamed to 'verify-output-two'")
                != std::string::npos,
        "The renamed requirement id must be reported in the normalization audit");
}

void testRequirementIdRenameNeverCollidesWithAnotherDeclaredId()
{
    Fixture fixture;
    // "two" would become verify-output-two, which task "three" already declares: the counter moves
    // it to verify-output-two-2 and "three" keeps its own id.
    fixture.addSession({submitTurn(0, manifestJson({
        taskJson("one", "[]", "src/one", "propose"),
        taskJson("two", "[]", "src/two", "propose"),
        withRequirementId(taskJson("three", "[]", "src/three", "propose"), "verify-output-two")}))});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success && outcome.manifest.has_value() && outcome.repairAttempts == 0,
        "A rename colliding with a declared id must still be planned without a repair");
    const auto ids = frozenRequirementIds(*outcome.manifest);
    require(ids.size() == 3 && ids[0] == "verify-output" && ids[1] == "verify-output-two-2"
            && ids[2] == "verify-output-two",
        "The rename must skip an id another task declares and leave that task untouched");
    require(std::set<std::string>(ids.begin(), ids.end()).size() == 3,
        "Frozen requirement ids must be unique across the Manifest");
    require(outcome.normalizations.size() == 1,
        "Only the genuinely repeated id may be renamed and reported");
}

void testDistinctRequirementIdsAreKeptAsSubmitted()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0, manifestJson({
        withRequirementId(taskJson("one", "[]", "src/one", "propose"), "req-a-output"),
        withRequirementId(taskJson("two", "[]", "src/two", "propose"), "req-b-output")}))});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success && outcome.manifest.has_value()
            && outcome.normalizations.empty(),
        "Distinct requirement ids must not be reported as normalized");
    const auto ids = frozenRequirementIds(*outcome.manifest);
    require(ids.size() == 2 && ids[0] == "req-a-output" && ids[1] == "req-b-output",
        "Distinct requirement ids must be frozen exactly as submitted");
}

void testRequirementIdRepeatedInsideOneTaskIsStillRejected()
{
    Fixture fixture;
    // Two requirements with one id INSIDE a single task stay the validator's fail-closed rejection.
    auto task = taskJson("one", "[]", "src/one", "propose");
    const std::string open = "\"requirements\":[";
    const std::string close = "]},\"contextReferences\"";
    const auto begin = task.find(open);
    const auto end = task.find(close);
    require(begin != std::string::npos && end != std::string::npos && begin < end,
        "Planner test task shape changed");
    const auto body = task.substr(begin + open.size(), end - (begin + open.size()));
    task.insert(end, "," + body);
    fixture.addSession({submitTurn(0, manifestJson({task}))});
    fixture.addSession({submitTurn(0, validManifest("propose"))});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success && outcome.repairAttempts == 1
            && outcome.primaryRejectionCode == "acceptance.duplicate_requirement_id",
        "A requirement id repeated inside one task must stay a rejection");
    require(outcome.normalizations.empty(),
        "A repeat inside one task must never be renamed by the Backend");
}

void testDependencyCycleIsRejectedThenRepaired()
{
    Fixture fixture;
    const auto cycle = manifestJson({
        taskJson("one", "[\"two\"]", "src/one", "propose"),
        taskJson("two", "[\"one\"]", "src/two", "propose")});
    fixture.addSession({submitTurn(0, cycle)});
    fixture.addSession({submitTurn(0, validManifest("propose"))});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success, "Dependency cycle was not repairable");
    require(fixture.sessionFactory.log.requests[1].userContent.find(
                "manifest.dependency_cycle") != std::string::npos,
        "Cycle finding was not returned to the repair session");
}

void testDeniedCapabilityCannotSelfAuthorize()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0, validManifest("propose"))});
    fixture.addSession({submitTurn(0, validManifest("inspect"))});

    const auto outcome = fixture.planner.plan(
        request(WorkspaceCapability::Inspect), {});
    require(outcome.success, "Capability escalation was not repaired downward");
    require(outcome.manifest->policy.maximumCapability == WorkspaceCapability::Inspect,
        "Planner replaced the trusted capability ceiling");
    require(fixture.sessionFactory.log.requests[1].userContent.find(
                "manifest.capability_exceeded") != std::string::npos,
        "Capability finding was not returned to the repair session");
}

void testExhaustedRepairFailsWithoutSnapshot()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0, "not-json")});
    fixture.addSession({submitTurn(0, "still-not-json")});

    const auto outcome = fixture.planner.plan(request(), {});
    require(!outcome.success && outcome.errorCode == "planner.repair_exhausted",
        "Second rejected submission did not exhaust repair");
    require(outcome.lastRejectionCode == "manifest.parse_error",
        "Exhausted outcome lost the last typed rejection");
    const auto stored = fixture.manifests.findForRun("planner-run");
    require(stored.success() && !stored.value().has_value(),
        "Exhausted repair left a partial manifest snapshot");
}

void testRawGitReadStaysDeniedAndNeverBecomesProposalPath()
{
    Fixture fixture;
    fixture.addSession({
        toolTurn(0, "git-1", "workspace_read_file", R"({"path":".git/config"})"),
        submitTurn(1, validManifest())});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success, "Denied raw git read prevented later valid submission");
    require(fixture.sessionFactory.log.submittedResults.size() == 1,
        "Denied read result was not returned to the provider");
    const auto& denied = fixture.sessionFactory.log.submittedResults.front().front();
    require(!denied.success && denied.errorCode == "workspace_path.denied",
        "Raw .git access was not denied by inspect policy");
    require(!requestAdvertises(fixture.sessionFactory.log.requests.front(),
                "propose_change_set"),
        "Proposal path leaked into the planner catalog");
}

void testUnsafeTypedProjectionFailsBeforeProviderOrAudit()
{
    Fixture fixture;
    auto invalidRequest = request();
    invalidRequest.repositoryProjection->changedPaths = {".git/config"};

    const auto outcome = fixture.planner.plan(std::move(invalidRequest), {});
    require(!outcome.success
            && outcome.errorCode == "planner.invalid_repository_projection",
        "Unsafe typed projection path was not rejected");
    require(fixture.sessionFactory.createCalls == 0,
        "Unsafe projection spent a provider call");
    const auto sessions = fixture.auditSessions.listForRun("planner-run");
    require(sessions.success() && sessions.value().empty(),
        "Unsafe projection opened an audit session");
}

void testEvidenceBackedNoChangesNeededSucceedsWithoutManifestSnapshot()
{
    Fixture fixture;
    fixture.addSession({
        toolTurn(0, "read-1", "workspace_read_file", R"({"path":"README.md"})"),
        noChangesTurn(1, noChangesJson())});

    const auto outcome = fixture.planner.plan(noChangesRequest(), {});
    require(outcome.success, "Evidence-backed no-op should succeed");
    require(outcome.disposition == PlanningDisposition::NoChangesNeeded,
        "Disposition should be NoChangesNeeded");
    require(!outcome.manifest.has_value(), "No-op outcome must not carry a manifest");
    require(outcome.noChangesNeeded.has_value(),
        "No-op outcome must carry its typed result");
    require(outcome.noChangesNeeded->evidence.size() == 1
            && outcome.noChangesNeeded->evidence.front().criterion == "Stage 1 files exist"
            && outcome.noChangesNeeded->evidence.front().path == "README.md",
        "No-op evidence item was not parsed correctly, or no longer matches the "
        "actually inspected path");
    require(!outcome.noChangesNeeded->summary.empty(),
        "No-op summary must be non-empty");
    require(fixture.sessionFactory.log.requests.size() == 1,
        "Evidence-backed no-op unexpectedly opened a repair session");

    const auto stored = fixture.manifests.findForRun("planner-run");
    require(stored.success() && !stored.value().has_value(),
        "No-op must not persist a manifest snapshot");
}

void testRequireManifestRejectsNoChangesAndRepairsToManifest()
{
    Fixture fixture;
    fixture.addSession({
        toolTurn(0, "read-1", "workspace_read_file", R"({"path":"README.md"})"),
        noChangesTurn(1, noChangesJson())});
    fixture.addSession({submitTurn(0, validManifest())});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success
            && outcome.disposition == PlanningDisposition::ManifestReady
            && outcome.manifest.has_value(),
        "RequireManifest did not repair a disallowed no-op into a Manifest");
    require(outcome.primaryRejectionCode == "planner.no_changes_disallowed"
            && outcome.repairAttempts == 1,
        "RequireManifest lost the typed no-op rejection or repair count");
    require(fixture.sessionFactory.log.requests.size() == 2,
        "RequireManifest did not use exactly one bounded validation repair");
    for (const auto& sent : fixture.sessionFactory.log.requests) {
        require(requestAdvertises(sent, kSubmitExecutionManifestDraftToolName),
            "RequireManifest request omitted the Manifest terminal tool");
        require(!requestAdvertises(sent, kSubmitNoChangesNeededToolName),
            "RequireManifest advertised the disallowed no-changes terminal tool");
    }
    require(fixture.sessionFactory.log.requests[1].userContent.find(
                "submit_no_changes_needed is not accepted") != std::string::npos,
        "RequireManifest repair prompt did not restate the trusted policy");

    const auto stored = fixture.manifests.findForRun("planner-run");
    require(stored.success() && stored.value().has_value(),
        "RequireManifest repair did not freeze the accepted Manifest");
}

void testNoChangesNeededWithoutEvidenceIsRejectedThenRepaired()
{
    Fixture fixture;
    fixture.addSession({
        toolTurn(0, "read-1", "workspace_read_file", R"({"path":"README.md"})"),
        noChangesTurn(1, noChangesJsonWithoutEvidence())});
    fixture.addSession({
        toolTurn(0, "read-2", "workspace_read_file", R"({"path":"README.md"})"),
        noChangesTurn(1, noChangesJson())});

    const auto outcome = fixture.planner.plan(noChangesRequest(), {});
    require(outcome.success && outcome.disposition == PlanningDisposition::NoChangesNeeded,
        "Empty-evidence no-op was not repaired into a valid one");
    require(outcome.repairAttempts == 1 && fixture.sessionFactory.createCalls == 2,
        "Empty-evidence no-op did not use exactly one repair session");
    require(outcome.primaryRejectionCode == "planner.no_changes_empty_evidence",
        "Empty-evidence rejection code was not preserved as primary");
    require(fixture.sessionFactory.log.requests[1].userContent.find(
                "planner.no_changes_empty_evidence") != std::string::npos,
        "Repair prompt omitted the typed empty-evidence rejection");

    const auto stored = fixture.manifests.findForRun("planner-run");
    require(stored.success() && !stored.value().has_value(),
        "No-op must not persist a manifest snapshot even after repair");
}

void testNoChangesNeededWithoutSuccessfulInspectionIsRejected()
{
    Fixture fixture;
    fixture.addSession({noChangesTurn(0, noChangesJson())});
    fixture.addSession({noChangesTurn(0, noChangesJson())});

    const auto outcome = fixture.planner.plan(noChangesRequest(), {});
    require(!outcome.success && outcome.errorCode == "planner.repair_exhausted",
        "No-op without any successful inspection call should exhaust repair");
    require(outcome.primaryRejectionCode == "planner.no_changes_missing_evidence",
        "Primary rejection code lost the missing-inspection-evidence reason");
    require(outcome.lastRejectionCode == "planner.no_changes_missing_evidence",
        "Final rejection code lost the missing-inspection-evidence reason");
    require(outcome.successfulInspectionCalls == 0,
        "Test fixture unexpectedly performed a successful inspection call");

    const auto stored = fixture.manifests.findForRun("planner-run");
    require(stored.success() && !stored.value().has_value(),
        "Rejected no-op must not persist a manifest snapshot");
}

void testEmptyManifestTasksStillRejectedAsEmptyTasks()
{
    Fixture fixture;
    const auto empty = manifestJson({});
    fixture.addSession({submitTurn(0, empty)});
    fixture.addSession({submitTurn(0, empty)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(!outcome.success && outcome.errorCode == "planner.repair_exhausted",
        "Empty-tasks manifest must remain an error, never silently accepted");
    require(outcome.primaryRejectionCode == "manifest.empty_tasks",
        "Empty-tasks rejection code was not preserved as primary");
    require(outcome.lastRejectionCode == "manifest.empty_tasks",
        "Empty-tasks rejection code was not preserved as final");
    require(outcome.primaryFindings.size() == 1
            && outcome.primaryFindings.front().code == "manifest.empty_tasks",
        "Primary manifest findings were not preserved");
    require(outcome.lastFindings.size() == 1
            && outcome.lastFindings.front().code == "manifest.empty_tasks",
        "Final manifest findings were not preserved");
    require(!outcome.manifest.has_value() && !outcome.noChangesNeeded.has_value(),
        "Exhausted empty-tasks submission must not carry a manifest or no-op result");
}

void testMissingSubmissionRepairsIntoValidNoChangesNeeded()
{
    Fixture fixture;
    fixture.addSession({stopTurn(0)});
    fixture.addSession({
        toolTurn(0, "read-1", "workspace_read_file", R"({"path":"README.md"})"),
        noChangesTurn(1, noChangesJson())});

    const auto outcome = fixture.planner.plan(noChangesRequest(), {});
    require(outcome.success && outcome.disposition == PlanningDisposition::NoChangesNeeded,
        "Missing submission was not repairable into a valid no-op");
    require(outcome.primaryRejectionCode == "planner.missing_submission",
        "Missing-submission primary rejection code was lost");
    require(outcome.repairAttempts == 1 && fixture.sessionFactory.createCalls == 2,
        "Missing submission should use exactly one terminalization recovery");
    require(fixture.sessionFactory.log.requests[1].userContent.find(
                "# One bounded terminalization recovery") != std::string::npos
            && fixture.sessionFactory.log.requests[1].userContent.find(
                "planner.missing_submission") != std::string::npos,
        "Terminalization recovery prompt omitted its mode or rejection");
    require(!outcome.manifest.has_value(), "Repaired no-op must not carry a manifest");

    const auto stored = fixture.manifests.findForRun("planner-run");
    require(stored.success() && !stored.value().has_value(),
        "Repaired no-op must not persist a manifest snapshot");
}

// MW-P3A package 2: manifest.missing_acceptance_contract - a runtime
// business-rule rejection AFTER parsing - can no longer occur via this
// pipeline (see Run 3ffb52df-98ca-472f-9b1c-537ab134cf18's closure note in
// ARCHITECTURE_GAPS.md). acceptanceContract is now a required key at the
// PARSER boundary for every work task, so an omitted key fails to parse at
// all (manifest.missing_field) - this is now a positive test that the
// structural fix itself works and still recovers within the one bounded
// repair, never that the old runtime rule fires.
void testWorkTaskMissingAcceptanceContractKeyFailsToParseNotARuntimeRule()
{
    Fixture fixture;
    const auto missingContract = manifestJson({
        "{\"id\":\"inspect\",\"objective\":\"Inspect the requested output\","
        "\"role\":\"worker\",\"dependencies\":[],\"writePaths\":[\"src/inspect\"],"
        "\"baselineRequiredReadPaths\":[],\"proposedDependencyReadPaths\":[],"
        "\"referencePaths\":[],\"excludedPaths\":[],\"deferredPaths\":[],"
        "\"acceptanceCriteria\":[\"Tests pass\"],\"contextReferences\":[],"
        "\"requestedCapability\":\"inspect\"}"});
    fixture.addSession({submitTurn(0, missingContract)});
    fixture.addSession({submitTurn(0, validManifest())});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success && outcome.disposition == PlanningDisposition::ManifestReady,
        "A manifest missing its acceptanceContract key was not repairable");
    require(outcome.primaryRejectionCode == "manifest.missing_field",
        "A missing acceptanceContract key must fail to parse, never the old "
        "manifest.missing_acceptance_contract runtime rule");
    require(outcome.repairAttempts == 1,
        "A missing acceptanceContract key must use exactly one validation repair");
}

void testSuccessfulRepairPreservesPrimaryManifestFindings()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0, manifestJson({}))});
    fixture.addSession({
        toolTurn(0, "read-1", "workspace_read_file", R"({"path":"README.md"})"),
        noChangesTurn(1, noChangesJson())});

    const auto outcome = fixture.planner.plan(noChangesRequest(), {});
    require(outcome.success && outcome.disposition == PlanningDisposition::NoChangesNeeded,
        "Empty manifest was not repaired into a valid no-op");
    require(outcome.primaryRejectionCode == "manifest.empty_tasks",
        "Successful repair lost its primary rejection code");
    require(outcome.primaryFindings.size() == 1
            && outcome.primaryFindings.front().code == "manifest.empty_tasks",
        "Successful repair lost its primary validation finding");
    require(outcome.lastRejectionCode == "manifest.empty_tasks"
            && outcome.lastFindings.size() == 1,
        "The most recent rejected-attempt diagnostics should remain the primary rejection");
}

void testPrimaryAndFinalRejectionCodesBothSurvive()
{
    Fixture fixture;
    fixture.addSession({stopTurn(0)});
    fixture.addSession({submitTurn(0, "not-json")});
    fixture.addSession({submitTurn(0, "still-not-json")});

    const auto outcome = fixture.planner.plan(request(), {});
    require(!outcome.success && outcome.errorCode == "planner.repair_exhausted",
        "Distinct primary/final rejections should still exhaust repair");
    require(outcome.primaryRejectionCode == "planner.missing_submission",
        "Primary rejection code was overwritten or lost");
    require(outcome.lastRejectionCode == "manifest.parse_error",
        "Final rejection code was lost or conflated with the primary one");
    require(outcome.repairAttempts == 2 && fixture.sessionFactory.createCalls == 3,
        "Terminalization followed by validation failure must use both bounded budgets");
    require(outcome.primaryRejectionCode != outcome.lastRejectionCode,
        "Primary and final rejection codes must be independently observable");
}

void testIncidentChainRecoversAfterTerminalizationAndValidationRepair()
{
    Fixture fixture;
    const auto invalidManifest = manifestJson({
        taskJson("t1-repo-audit", "[]", ".", "inspect"),
        taskJson("t3-cmake-registration", "[]", "src", "propose"),
        taskJson("t4-verify-and-report", "[]", "src/app", "inspect")});
    fixture.addSession({stopTurn(0)});
    fixture.addSession({submitTurn(0, invalidManifest)});
    fixture.addSession({
        toolTurn(0, "read-repair", "workspace_read_file", R"({"path":"README.md"})"),
        noChangesTurn(1, noChangesJson())});

    const auto outcome = fixture.planner.plan(noChangesRequest(), {});
    require(outcome.success && outcome.disposition == PlanningDisposition::NoChangesNeeded,
        "Incident-shaped failure did not recover into a validated terminal outcome");
    require(outcome.repairAttempts == 2 && fixture.sessionFactory.createCalls == 3,
        "Incident-shaped recovery did not use exactly two fresh bounded sessions");
    require(outcome.primaryRejectionCode == "planner.missing_submission",
        "Incident recovery lost the primary missing-submission code");
    require(outcome.lastRejectionCode == "manifest.invalid_write_path",
        "Incident recovery lost the rejected Manifest's first validation code");
    require(outcome.lastFindings.size() == 1
            && outcome.lastFindings.front().code == "manifest.invalid_write_path",
        "Incident recovery did not preserve the raw-path parser finding");
    require(fixture.sessionFactory.log.requests[1].userContent.find(
                "# One bounded terminalization recovery") != std::string::npos,
        "Second session was not identified as terminalization recovery");
    const auto& validationPrompt = fixture.sessionFactory.log.requests[2].userContent;
    require(validationPrompt.find("# One bounded validation repair") != std::string::npos
            && validationPrompt.find("manifest.invalid_write_path") != std::string::npos
            && validationPrompt.find("canonical workspace-relative")
                != std::string::npos,
        "Validation repair prompt omitted the incident's safe raw-path finding");

    const auto stored = fixture.manifests.findForRun("planner-run");
    require(stored.success() && !stored.value().has_value(),
        "Recovered no-op must not freeze the rejected Manifest");
}

void testRepeatedMissingSubmissionCannotLoop()
{
    Fixture fixture;
    fixture.addSession({stopTurn(0)});
    fixture.addSession({stopTurn(0)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(!outcome.success && outcome.errorCode == "planner.repair_exhausted",
        "Repeated missing terminal submissions must exhaust recovery");
    require(outcome.repairAttempts == 1 && fixture.sessionFactory.createCalls == 2,
        "Repeated Stop must not open a third or unbounded session");
    require(outcome.primaryRejectionCode == "planner.missing_submission"
            && outcome.lastRejectionCode == "planner.missing_submission",
        "Repeated missing-submission diagnostics were not preserved");
}

void testPartialCompletionStillReturnsNonEmptyManifest()
{
    Fixture fixture;
    const auto partial = manifestJson({
        taskJson("only-remaining", "[]", "src/remaining", "propose")});
    fixture.addSession({
        toolTurn(0, "read-1", "workspace_read_file", R"({"path":"README.md"})"),
        submitTurn(1, partial)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success && outcome.disposition == PlanningDisposition::ManifestReady,
        "Partial-completion manifest submission should still succeed as ManifestReady");
    require(outcome.manifest.has_value() && outcome.manifest->tasks.size() == 1,
        "Partial completion must still produce a non-empty manifest");
    require(!outcome.noChangesNeeded.has_value(),
        "ManifestReady outcome must not carry a no-op payload");

    const auto stored = fixture.manifests.findForRun("planner-run");
    require(stored.success() && stored.value().has_value()
            && !stored.value()->tasks.empty(),
        "Partial-completion manifest was not persisted");
}

void testTerminalSubmissionMustBeAloneInTurnEvenForNoChanges()
{
    Fixture fixture;
    ProviderAgentTurnResponse mixedTurn;
    mixedTurn.turnIndex = 0;
    mixedTurn.finishReason = ProviderAgentFinishReason::ToolUse;
    mixedTurn.usage.inputTokens = 7;
    mixedTurn.usage.outputTokens = 3;

    AgentToolCall inspectCall;
    inspectCall.callId = "read-1";
    inspectCall.toolName = "workspace_read_file";
    inspectCall.argumentsJson = R"({"path":"README.md"})";
    inspectCall.turnIndex = 0;
    mixedTurn.toolCalls.push_back(inspectCall);

    AgentToolCall submitCall;
    submitCall.callId = "no-changes-1";
    submitCall.toolName = kSubmitNoChangesNeededToolName;
    submitCall.argumentsJson = noChangesJson();
    submitCall.turnIndex = 0;
    mixedTurn.toolCalls.push_back(submitCall);

    fixture.addSession({mixedTurn});
    fixture.addSession({
        toolTurn(0, "read-2", "workspace_read_file", R"({"path":"README.md"})"),
        noChangesTurn(1, noChangesJson())});

    const auto outcome = fixture.planner.plan(noChangesRequest(), {});
    require(outcome.success && outcome.disposition == PlanningDisposition::NoChangesNeeded,
        "Invalid combined turn should still repair into a valid standalone no-op");
    require(outcome.primaryRejectionCode == "planner.invalid_submission_turn",
        "Combined terminal+inspect turn was not rejected as an invalid submission turn");
    require(fixture.executor.callCount == 1,
        "The combined turn's inspect call must not have executed before rejection");
}

void testEvidencePathNotBackedByThisAttemptsInspectionIsRejected()
{
    // "read README.md, evidence points to src/existing.cpp -> rejection":
    // both primary and repair inspect README.md but claim evidence for a
    // completely different, never-inspected path - both attempts must be
    // rejected with planner.no_changes_uninspected_evidence_path.
    Fixture fixture;
    const auto mismatched = noChangesJson(
        "Nothing left to do", "Stage 1 files exist",
        "src/existing.cpp", "File already present");
    fixture.addSession({
        toolTurn(0, "read-1", "workspace_read_file", R"({"path":"README.md"})"),
        noChangesTurn(1, mismatched)});
    fixture.addSession({
        toolTurn(0, "read-2", "workspace_read_file", R"({"path":"README.md"})"),
        noChangesTurn(1, mismatched)});

    const auto outcome = fixture.planner.plan(noChangesRequest(), {});
    require(!outcome.success && outcome.errorCode == "planner.repair_exhausted",
        "Evidence path never actually inspected this attempt must remain rejected");
    require(outcome.primaryRejectionCode == "planner.no_changes_uninspected_evidence_path",
        "Uninspected-evidence-path rejection code was lost as primary");
    require(outcome.lastRejectionCode == "planner.no_changes_uninspected_evidence_path",
        "Uninspected-evidence-path rejection code was lost as final");

    const auto stored = fixture.manifests.findForRun("planner-run");
    require(stored.success() && !stored.value().has_value(),
        "Rejected no-op must not persist a manifest snapshot");
}

void testRepairCannotUsePrimarysInspectionForNoChangesNeeded()
{
    // Primary inspects README.md but is rejected for an unrelated reason
    // (an empty-tasks manifest submission). The repair session immediately
    // submits a no-op without performing any inspection of its own - it must
    // not be able to borrow the primary attempt's successful inspection.
    Fixture fixture;
    const auto empty = manifestJson({});
    fixture.addSession({
        toolTurn(0, "read-1", "workspace_read_file", R"({"path":"README.md"})"),
        submitTurn(1, empty)});
    fixture.addSession({noChangesTurn(0, noChangesJson())});

    const auto outcome = fixture.planner.plan(noChangesRequest(), {});
    require(!outcome.success && outcome.errorCode == "planner.repair_exhausted",
        "Repair no-op without its own inspection must not borrow the primary's evidence");
    require(outcome.primaryRejectionCode == "manifest.empty_tasks",
        "Primary rejection code was lost");
    require(outcome.lastRejectionCode == "planner.repair_regressed"
            && !outcome.lastFindings.empty()
            && outcome.lastFindings.front().code == "planner.repair_regressed",
        "Repair trading an empty-Manifest finding for missing no-op evidence was not "
        "classified as a monotonic regression");

    const auto stored = fixture.manifests.findForRun("planner-run");
    require(stored.success() && !stored.value().has_value(),
        "Rejected repair no-op must not persist a manifest snapshot");
}

void testRepairOwnMatchingInspectionRecoversNoChangesNeeded()
{
    // Primary claims evidence for a path it never inspected and is rejected;
    // the repair session independently inspects that exact path and its
    // matching no-op succeeds.
    Fixture fixture;
    const auto mismatched = noChangesJson(
        "Nothing left to do", "Stage 1 files exist",
        "src/existing.cpp", "File already present");
    const auto matched = mismatched;
    fixture.addSession({
        toolTurn(0, "read-1", "workspace_read_file", R"({"path":"README.md"})"),
        noChangesTurn(1, mismatched)});
    fixture.addSession({
        toolTurn(0, "read-2", "workspace_read_file", R"({"path":"src/existing.cpp"})"),
        noChangesTurn(1, matched)});

    const auto outcome = fixture.planner.plan(noChangesRequest(), {});
    require(outcome.success && outcome.disposition == PlanningDisposition::NoChangesNeeded,
        "Repair inspecting its own matching evidence path should recover the no-op");
    require(outcome.primaryRejectionCode == "planner.no_changes_uninspected_evidence_path",
        "Primary rejection code was lost");
    require(outcome.noChangesNeeded.has_value()
            && outcome.noChangesNeeded->evidence.front().path == "src/existing.cpp",
        "Recovered no-op lost its evidence path");

    const auto stored = fixture.manifests.findForRun("planner-run");
    require(stored.success() && !stored.value().has_value(),
        "Recovered no-op must not persist a manifest snapshot");
}

// ---------------------------------------------------------------------
// AG-18.3 hardening (blocker 2): the SetupRequired decision matrix.
// ---------------------------------------------------------------------

ExecutionManifestPlannerRequest setupRequiredRequest()
{
    auto value = request();
    VerificationEnvironmentSnapshot snapshot;
    snapshot.state = VerificationEnvironmentState::SetupRequired;
    value.verificationEnvironment = std::move(snapshot);
    return value;
}

ExecutionManifestPlannerRequest existingProfileRequest()
{
    auto value = request();
    VerificationEnvironmentSnapshot snapshot;
    snapshot.state = VerificationEnvironmentState::ExistingProfile;
    value.verificationEnvironment = std::move(snapshot);
    return value;
}

std::string actionRequiredJson(
    const std::string& summary = "No usable CMake preset exists and no safe same-Run bootstrap "
                                  "is possible.",
    const std::string& reason = "verification_environment.setup_required")
{
    return "{\"schemaVersion\":\"verification-action-required.v1\","
        "\"summary\":\"" + summary + "\",\"reason\":\"" + reason + "\","
        "\"recommendedNextStep\":null}";
}

void testExplicitActionRequiredAcceptedWhenSetupRequired()
{
    Fixture fixture;
    fixture.addSession({
        toolTurn(0, "action-required-1", kSubmitVerificationActionRequiredToolName,
            actionRequiredJson())});
    const auto outcome = fixture.planner.plan(setupRequiredRequest(), {});
    require(outcome.success
            && outcome.disposition == PlanningDisposition::VerificationActionRequired,
        "An explicit submit_verification_action_required call was rejected while genuinely "
        "SetupRequired");
    require(outcome.verificationActionRequired.has_value()
            && !outcome.verificationActionRequired->summary.empty(),
        "ActionRequired outcome lost its typed summary");
    require(!outcome.manifest.has_value(),
        "ActionRequired must never also freeze a Manifest snapshot");
    const auto stored = fixture.manifests.findForRun("planner-run");
    require(stored.success() && !stored.value().has_value(),
        "ActionRequired incorrectly froze a manifest snapshot");
}

void testExplicitActionRequiredRejectedWhenExistingProfile()
{
    Fixture fixture;
    fixture.addSession({
        toolTurn(0, "action-required-1", kSubmitVerificationActionRequiredToolName,
            actionRequiredJson())});
    fixture.addSession({submitTurn(0, "still-not-json")});
    const auto outcome = fixture.planner.plan(existingProfileRequest(), {});
    require(!outcome.success,
        "submit_verification_action_required was accepted while a usable profile exists");
    require(outcome.primaryRejectionCode == "planner.verification_action_required_not_applicable",
        "Expected the not-applicable rejection code when a profile already exists");
}

void testProviderFailurePassesThroughWithoutBecomingActionRequired()
{
    Fixture fixture;
    auto session = std::make_unique<ScriptedSession>();
    session->responses.push(common::Result<ProviderAgentTurnResponse>::failure(
        {"provider.request_failed", "The provider request could not be completed"}));
    fixture.sessionFactory.sessions.push(std::move(session));

    const auto outcome = fixture.planner.plan(setupRequiredRequest(), {});
    require(!outcome.success && outcome.errorCode == "provider.request_failed",
        "A raw provider failure was not propagated as-is, even while SetupRequired");
    require(outcome.disposition != PlanningDisposition::VerificationActionRequired,
        "A provider-level failure must never be coerced into ActionRequired");
    require(!outcome.verificationActionRequired.has_value(),
        "A provider-level failure must never synthesize an ActionRequired payload");
}

void testMalformedSubmissionNeverBecomesActionRequiredWhenSetupRequired()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0, "not-json")});
    fixture.addSession({submitTurn(0, "still-not-json")});

    const auto outcome = fixture.planner.plan(setupRequiredRequest(), {});
    require(!outcome.success && outcome.errorCode == "planner.repair_exhausted",
        "A malformed submission did not exhaust repair even while SetupRequired");
    require(outcome.lastRejectionCode == "manifest.parse_error",
        "A malformed submission's own parse error must survive unchanged, never disguised as "
        "verification_setup_required");
    require(outcome.disposition != PlanningDisposition::VerificationActionRequired,
        "A malformed submission must never be coerced into ActionRequired");
}

void testNoChangesCannotDisguiseSetupRequired()
{
    Fixture fixture;
    fixture.addSession({
        toolTurn(0, "read-1", "workspace_read_file", R"({"path":"README.md"})"),
        noChangesTurn(1, noChangesJson())});
    fixture.addSession({
        toolTurn(0, "read-2", "workspace_read_file", R"({"path":"README.md"})"),
        noChangesTurn(1, noChangesJson())});

    auto setupRequired = setupRequiredRequest();
    setupRequired.allowEvidenceBackedNoChanges = true;
    const auto outcome = fixture.planner.plan(std::move(setupRequired), {});
    require(!outcome.success,
        "An evidence-backed no-changes submission was accepted while genuinely SetupRequired");
    require(outcome.primaryRejectionCode == "planner.verification_setup_required",
        "Expected verification_setup_required, not a generic no-changes rejection");
    require(outcome.disposition != PlanningDisposition::NoChangesNeeded,
        "NoChangesNeeded must never disguise missing verification setup");
}

// Mirrors ManifestWorkflowRunnerTests.cpp's bootstrapManifestSubmission():
// a "bootstrap" task plannedCreates the repository-root
// CMakeUserPresets.json, and a dependent "build-task" carries a
// cmake_build requirement deferred pending that bootstrap.
std::string bootstrapDeclaredManifestJson()
{
    return "{\"schemaVersion\":\"execution-manifest-draft.v1\",\"coordinationTasks\":[],"
        "\"workTasks\":[{"
        "\"id\":\"bootstrap\",\"objective\":\"Create the CMake user preset\","
        "\"role\":\"worker\",\"dependencies\":[],"
        "\"writePaths\":[\"CMakeUserPresets.json\"],"
        "\"baselineRequiredReadPaths\":[],\"proposedDependencyReadPaths\":[],"
        "\"referencePaths\":[],\"excludedPaths\":[],"
        "\"deferredPaths\":[],"
        "\"acceptanceCriteria\":[\"A usable CMake preset exists\"],"
        "\"acceptanceContract\":{\"schemaVersion\":\"execution-acceptance.v1\","
        "\"requirements\":[{\"id\":\"preset-exists\","
        "\"description\":\"The CMake user preset file exists\","
        "\"requiredOutputs\":[\"CMakeUserPresets.json\"],\"verificationKind\":\"inspect_paths\","
        "\"verificationProfileId\":null,\"verificationBuildProfileId\":null,"
        "\"verificationTargetId\":null,\"verificationArguments\":[],"
        "\"required\":true,\"verificationBootstrapPending\":false}]},"
        "\"contextReferences\":[],\"requestedCapability\":\"propose\"},"
        "{\"id\":\"build-task\",\"objective\":\"Build the project\","
        "\"role\":\"worker\",\"dependencies\":[\"bootstrap\"],"
        "\"writePaths\":[\"src/app.cpp\"],"
        "\"baselineRequiredReadPaths\":[],\"proposedDependencyReadPaths\":[],"
        "\"referencePaths\":[],\"excludedPaths\":[],"
        "\"deferredPaths\":[],"
        "\"acceptanceCriteria\":[\"The project builds\"],"
        "\"acceptanceContract\":{\"schemaVersion\":\"execution-acceptance.v1\","
        "\"requirements\":[{\"id\":\"build\",\"description\":\"The project builds\","
        "\"requiredOutputs\":[],\"verificationKind\":\"cmake_build\","
        "\"verificationProfileId\":null,\"verificationBuildProfileId\":null,"
        "\"verificationTargetId\":null,\"verificationArguments\":[],"
        "\"required\":true,\"verificationBootstrapPending\":true}]},"
        "\"contextReferences\":[],\"requestedCapability\":\"propose\"}"
        "],\"requestedMaxParallelism\":1,"
        "\"requestedTokenBudget\":null,\"requestedCostMicros\":null,"
        "\"bootstrapDeclaration\":{\"bootstrapTaskId\":\"bootstrap\","
        "\"setupPaths\":[\"CMakeUserPresets.json\"]}}";
}

void testValidBootstrapManifestRemainsAllowedWhenSetupRequired()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0, bootstrapDeclaredManifestJson())});

    const auto outcome = fixture.planner.plan(setupRequiredRequest(), {});
    require(outcome.success && outcome.manifest.has_value()
            && outcome.manifest->bootstrapDeclaration.has_value(),
        "A validly-declared same-Run bootstrap Manifest was rejected while SetupRequired");
    require(outcome.disposition == PlanningDisposition::ManifestReady,
        "A valid bootstrap Manifest must reach the ordinary ManifestReady disposition");
}

// Cross-task required-read edges are mechanically implied by the frozen path
// allocation. The Planner boundary now completes that one safe relation
// without spending a provider repair session, then freezes the canonical
// graph normally.
void testMissingScopeDependencyIsCanonicalizedBeforeManifestFreeze()
{
    Fixture fixture;
    const auto missingDependency = manifestJson({
        taskJsonWithRoles("define-shared", "[]", "src/shared.hpp", "", "", "propose"),
        taskJsonWithRoles(
            "implement", "[]", "", "src/consumer.cpp", "src/shared.hpp", "propose")});
    fixture.addSession({submitTurn(0, missingDependency)});

    auto plannerRequest = request();
    WorkspaceBaselineSnapshot baseline;
    WorkspaceBaselineEntry sharedEntry;
    sharedEntry.relativePath = "src/shared.hpp";
    sharedEntry.byteSize = 10;
    sharedEntry.sha256 = "hash-shared";
    baseline.files.push_back(sharedEntry);
    plannerRequest.baselineSnapshot = baseline;

    const auto outcome = fixture.planner.plan(std::move(plannerRequest), {});
    require(outcome.success && outcome.manifest.has_value(),
        "Planner did not canonicalize the implied scope dependency");
    require(outcome.repairAttempts == 0 && outcome.primaryRejectionCode.empty()
            && fixture.sessionFactory.createCalls == 1,
        "Canonical dependency completion unnecessarily spent a provider repair");
    require(outcome.manifest->tasks.back().dependencies
                == std::vector<std::string> {"define-shared"},
        "Canonical manifest must declare the identified producer dependency");

    InMemoryRunWorkspaceScopeRepository scopeRepository;
    RunWorkspaceScopeService scopeService(scopeRepository);
    const auto frozen =
        scopeService.freezeForManifest("planner-run", *outcome.manifest, baseline);
    require(frozen.success(), "The repaired Manifest must freeze successfully");
}

// Multiple reads from the same producer still imply exactly one stable edge;
// existing dependencies keep their order and are never duplicated.
void testCanonicalScopeDependencyIsAddedExactlyOnce()
{
    Fixture fixture;
    const auto missingDependency = manifestJson({
        taskJsonWithRoles("define-shared", "[]", "src/shared.hpp", "", "", "propose"),
        taskJsonWithRoles(
            "implement", "[]", "", "src/consumer.cpp", "src/shared.hpp", "propose")});
    fixture.addSession({submitTurn(0, missingDependency)});

    auto plannerRequest = request();
    WorkspaceBaselineSnapshot baseline;
    WorkspaceBaselineEntry sharedEntry;
    sharedEntry.relativePath = "src/shared.hpp";
    sharedEntry.byteSize = 10;
    sharedEntry.sha256 = "hash-shared";
    baseline.files.push_back(sharedEntry);
    plannerRequest.baselineSnapshot = baseline;

    const auto outcome = fixture.planner.plan(std::move(plannerRequest), {});
    require(outcome.success && outcome.manifest.has_value(),
        "Planner did not complete the implied dependency");
    require(outcome.manifest->tasks.back().dependencies
                == std::vector<std::string> {"define-shared"},
        "Implied dependency was missing or duplicated");
}

// A repair session can still submit a WRONG fix - e.g. it adds the producer
// dependency but also, by mistake, makes the producer depend back on the
// consumer, forming a cycle. The existing DAG cycle check remains
// authoritative: this is rejected, and (since exactly one bounded repair is
// ever used) the Planner exhausts its repair budget rather than looping.
void testMissingScopeDependencyRepairThatCreatesCycleStaysRejected()
{
    Fixture fixture;
    const auto cyclicAfterCanonicalization = manifestJson({
        taskJsonWithRoles(
            "define-shared", "[\"implement\"]", "src/shared.hpp", "", "", "propose"),
        taskJsonWithRoles(
            "implement", "[]", "", "src/consumer.cpp", "src/shared.hpp", "propose")});
    fixture.addSession({submitTurn(0, cyclicAfterCanonicalization)});
    fixture.addSession({submitTurn(0, cyclicAfterCanonicalization)});

    auto plannerRequest = request();
    WorkspaceBaselineSnapshot baseline;
    WorkspaceBaselineEntry sharedEntry;
    sharedEntry.relativePath = "src/shared.hpp";
    sharedEntry.byteSize = 10;
    sharedEntry.sha256 = "hash-shared";
    baseline.files.push_back(sharedEntry);
    plannerRequest.baselineSnapshot = baseline;

    const auto outcome = fixture.planner.plan(std::move(plannerRequest), {});
    require(!outcome.success && outcome.errorCode == "planner.repair_exhausted",
        "A repair introducing a dependency cycle must not be accepted");
    require(outcome.primaryRejectionCode == "manifest.dependency_cycle",
        "Canonical dependency cycle was not rejected before freeze");
    require(outcome.lastRejectionCode == "manifest.dependency_cycle",
        "The existing DAG cycle check must still reject the malformed repair");
    const auto stored = fixture.manifests.findForRun("planner-run");
    require(stored.success() && !stored.value().has_value(),
        "Exhausted repair left a partial manifest snapshot");
}

// Verification-repair reliability hardening item #6 (Planner guidance
// hardening) - #34: the system prompt sent to the Planner must explicitly
// distinguish excludedPaths from requiredReadPaths for a file whose
// declarations owned files consume - the real incident this closes was
// caused by a Theme singleton being excluded merely because it must not be
// edited, when requiredReadPaths was the correct role all along.
void testPlannerPromptDistinguishesExcludedFromRequiredReadOnlyPaths()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0, validManifest("propose"))});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success, "Baseline valid-manifest planning must still succeed");
    const auto& systemPrompt = fixture.sessionFactory.log.requests.front().systemPrompt;
    require(systemPrompt.find("excludedPaths is not a synonym for") != std::string::npos,
        "Planner system prompt must explicitly say excludedPaths is not a synonym for read-only");
    require(systemPrompt.find("must never be excluded merely because it") != std::string::npos,
        "Planner system prompt must explicitly forbid excluding a file merely because it must "
        "not be modified");
    require(systemPrompt.find("theme/design-token singletons") != std::string::npos
            && systemPrompt.find("build or module-registration files") != std::string::npos,
        "Planner system prompt must name the concrete contract-file categories "
        "(theme/singleton/module-registration) that belong in baselineRequiredReadPaths");
}

// #35: a valid Manifest that places a consumed contract file (owned by a
// different task) in the consumer's requiredReadPaths - exactly the
// Planner-guidance-hardened pattern above - is accepted and freezes
// cleanly, with no special-casing needed anywhere in the schema/validator.
void testValidManifestPlacesConsumedContractFileInRequiredReadPaths()
{
    Fixture fixture;
    const auto manifestWithContractDependency = manifestJson({
        taskJsonWithRoles("define-theme", "[]", "qml/app/Theme.qml", "", "", "propose"),
        taskJsonWithRoles("sidebar", "[\"define-theme\"]", "",
            "qml/components/navigation/AppSidebar.qml", "qml/app/Theme.qml", "propose")});
    fixture.addSession({submitTurn(0, manifestWithContractDependency)});

    auto plannerRequest = request();
    WorkspaceBaselineSnapshot baseline;
    WorkspaceBaselineEntry themeEntry;
    themeEntry.relativePath = "qml/app/Theme.qml";
    themeEntry.byteSize = 10;
    themeEntry.sha256 = "hash-theme";
    baseline.files.push_back(themeEntry);
    plannerRequest.baselineSnapshot = baseline;

    const auto outcome = fixture.planner.plan(std::move(plannerRequest), {});
    require(outcome.success && outcome.manifest.has_value() && outcome.repairAttempts == 0,
        "A correctly declared contract-file requiredReadPaths dependency must be accepted "
        "without needing any repair");
    const auto& sidebar = outcome.manifest->tasks.back();
    require(sidebar.id == "sidebar"
            && sidebar.requiredReadPaths
                == std::vector<std::filesystem::path> {"qml/app/Theme.qml"}
            && sidebar.dependencies == std::vector<std::string> {"define-theme"},
        "The frozen manifest must retain the contract file in requiredReadPaths, never move it "
        "into ownedPaths or drop the declared producer dependency");
}

// Real-Run incident (Run 2f26ecc5-2723-4f9c-98fd-9ff317f13387, 2026-09-11): a
// primary submission with an inspect-capability task carrying a command-
// backed (cmake_build) requirement is structurally unrepairable by that
// worker session - it must be repaired in exactly one bounded attempt by
// changing the requirement's shape, not by re-dispatching the same worker.
void testInspectTaskCommandVerificationIsRepairedToInspectPaths()
{
    Fixture fixture;
    const auto badSubmission = manifestJson({
        taskJson("build-task", "[]", "src/app.cpp", "propose"),
        inspectVerifyTaskJson("cmake_build")});
    const auto repairedSubmission = manifestJson({
        taskJson("build-task", "[]", "src/app.cpp", "propose"),
        inspectVerifyTaskJson("inspect_paths")});
    fixture.addSession({submitTurn(0, badSubmission)});
    fixture.addSession({submitTurn(0, repairedSubmission)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success && outcome.manifest.has_value(),
        "The inspect+command-backed shape must be repairable in exactly one bounded attempt");
    require(outcome.repairAttempts == 1
            && outcome.primaryRejectionCode == "manifest.inspect_task_command_verification",
        "Missing or wrong primary rejection code / repair count for this shape");
    require(outcome.manifest->tasks.back().acceptanceContract.has_value()
            && outcome.manifest->tasks.back().acceptanceContract->requirements.front()
                    .verificationKind == ExecutionVerificationKind::InspectPaths,
        "The repaired manifest must no longer carry a command-backed requirement on the "
        "inspect-capability task");
}

// The repair prompt must give the model something concrete to act on
// (both allowed corrections named explicitly), not just the bare code -
// exactly the same discipline established for manifest.missing_scope_dependency.
void testInspectTaskCommandVerificationRepairPromptNamesBothCorrectionOptions()
{
    Fixture fixture;
    const auto badSubmission = manifestJson({
        taskJson("build-task", "[]", "src/app.cpp", "propose"),
        inspectVerifyTaskJson("cmake_build")});
    const auto repairedSubmission = manifestJson({
        taskJson("build-task", "[]", "src/app.cpp", "propose"),
        inspectVerifyTaskJson("inspect_paths")});
    fixture.addSession({submitTurn(0, badSubmission)});
    fixture.addSession({submitTurn(0, repairedSubmission)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success, "Setup: the repair must succeed for this test to be meaningful");
    const auto& repairPrompt = fixture.sessionFactory.log.requests[1].userContent;
    require(repairPrompt.find("manifest.inspect_task_command_verification") != std::string::npos,
        "Repair prompt omitted the rejection code");
    require(repairPrompt.find("move that requirement onto a propose-capability")
                != std::string::npos
            && repairPrompt.find(
                   "replace the offending requirement's verificationKind with inspect_paths")
                != std::string::npos,
        "Repair prompt omitted one of the two explicit correction options");
    require(repairPrompt.find("never runs cmake_build, qml_lint") != std::string::npos,
        "Repair prompt omitted the explicit statement that an inspect worker cannot run "
        "build/lint itself");
}

// ---------------------------------------------------------------------
// Real-Run incident (Runs fcd484b8-9.../cebcd3cf-.../c5f7d3da-...,
// 2026-09-11): planner.repair_exhausted (primary:
// manifest.inspect_task_command_verification, final:
// manifest.dependency_cycle) across three Stage 2A-canary-9 attempts. The
// bounded repair fixed the first-sorted finding but had no specific
// instruction for a cycle that was present in the SAME primary submission,
// and once the cycle became the ONLY finding on later attempts, the repair
// prompt gave no concrete guidance on which edge to fix or how.
// ---------------------------------------------------------------------

// #1: a primary submission carrying BOTH an inspect+command-verification
// shape defect AND a genuine dependency cycle at once is still repairable
// in exactly one bounded attempt - every applicable correction must be
// given, not just the one for whichever finding happened to sort first.
void testInspectCommandVerificationAndDependencyCycleBothRepairedInOneAttempt()
{
    Fixture fixture;
    const auto badSubmission = manifestJson({
        taskJsonWithRoles("build-task", "[\"verify-task\"]", "src/app.cpp", "", "", "propose"),
        inspectVerifyTaskJsonWithDependencies("cmake_build", "[\"build-task\"]")});
    const auto repairedSubmission = manifestJson({
        taskJsonWithRoles("build-task", "[]", "src/app.cpp", "", "", "propose"),
        inspectVerifyTaskJsonWithDependencies("inspect_paths", "[\"build-task\"]")});
    fixture.addSession({submitTurn(0, badSubmission)});
    fixture.addSession({submitTurn(0, repairedSubmission)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success && outcome.manifest.has_value(),
        "A primary submission with BOTH an inspect+command-verification shape defect and a "
        "dependency cycle must still be repairable in exactly one bounded attempt");
    require(outcome.repairAttempts == 1, "Expected exactly one bounded repair attempt");
    const auto& repairPrompt = fixture.sessionFactory.log.requests[1].userContent;
    require(repairPrompt.find("manifest.inspect_task_command_verification") != std::string::npos,
        "Repair prompt omitted the inspect+command-verification correction, even though it was "
        "not the first-sorted finding this time");
    require(repairPrompt.find("manifest.dependency_cycle") != std::string::npos
            && repairPrompt.find("build-task -> verify-task -> build-task") != std::string::npos,
        "Repair prompt omitted the dependency-cycle correction and/or the exact cycle chain");
}

// #2: a pure dependency cycle (no other defect) is repairable by fixing the
// wrong edge direction - the repair prompt must name the exact chain and
// the structural correction rule.
void testDependencyCycleRepairedByFixingEdgeDirection()
{
    Fixture fixture;
    const auto cyclic = manifestJson({
        taskJson("task-a", "[\"task-b\"]", "src/a", "propose"),
        taskJson("task-b", "[\"task-a\"]", "src/b", "propose")});
    const auto fixed = manifestJson({
        taskJson("task-a", "[\"task-b\"]", "src/a", "propose"),
        taskJson("task-b", "[]", "src/b", "propose")});
    fixture.addSession({submitTurn(0, cyclic)});
    fixture.addSession({submitTurn(0, fixed)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success && outcome.manifest.has_value(),
        "A dependency cycle fixed by correcting the edge direction must be repairable in one "
        "bounded attempt");
    require(outcome.primaryRejectionCode == "manifest.dependency_cycle",
        "Wrong primary rejection code for a pure dependency-cycle submission");
    const auto& repairPrompt = fixture.sessionFactory.log.requests[1].userContent;
    require(repairPrompt.find("task-a -> task-b -> task-a") != std::string::npos,
        "Repair prompt omitted the exact cycle chain");
    require(repairPrompt.find("merge them into") != std::string::npos,
        "Repair prompt omitted the structural merge-or-fix-direction guidance");
}

// #3: a pure dependency cycle repaired by merging the two mutually-dependent
// tasks into one - the second explicit correction option the repair prompt
// offers.
void testDependencyCycleRepairedByMergingMutuallyDependentTasks()
{
    Fixture fixture;
    const auto cyclic = manifestJson({
        taskJson("components", "[\"integration\"]", "src/components.qml", "propose"),
        taskJson("integration", "[\"components\"]", "src/integration.qml", "propose")});
    const auto merged = manifestJson({
        taskJson("components-and-integration", "[]", "src/components.qml", "propose")});
    fixture.addSession({submitTurn(0, cyclic)});
    fixture.addSession({submitTurn(0, merged)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success && outcome.manifest.has_value()
            && outcome.manifest->tasks.size() == 1,
        "A dependency cycle fixed by merging the two mutually-dependent tasks into one must be "
        "repairable in one bounded attempt");
}

// MW-P1 (real-Run incident, three OpenAI "Stage 2B" Runs, 2026-09-13): the
// ACTUAL shape that repeatedly exhausted the bounded repair - unlike #2/#3
// above, NEITHER task declares a `dependencies` edge at all; each simply
// requires a path the other task produces. The previous declared-only
// cycle check was blind to this shape entirely: it could pass the
// Planner's one bounded repair chance undetected and only surface later in
// RunWorkspaceScopeService, after the manifest was already immutably
// frozen and no repair chance remained. This proves the real incident
// shape is now caught at the earliest possible point and is repairable in
// one bounded attempt, with the repair prompt naming the exact derived
// path that created each edge.
void testDependencyCycleFromMutualRequiredReadPathsIsRepairedByMerging()
{
    Fixture fixture;
    const auto cyclic = manifestJson({
        taskJsonWithRoles("task-a-pagehost", "[]", "qml/pagehost.qml", "",
            "qml/cmakelists.txt", "propose"),
        taskJsonWithRoles("task-b-integration", "[]", "qml/cmakelists.txt", "",
            "qml/pagehost.qml", "propose")});
    const auto merged = manifestJson({
        taskJsonWithRoles("pagehost-and-integration", "[]", "qml/pagehost.qml", "", "",
            "propose")});
    fixture.addSession({submitTurn(0, cyclic)});
    fixture.addSession({submitTurn(0, merged)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.primaryRejectionCode == "manifest.dependency_cycle",
        "A cycle formed purely by mutual requiredReadPaths (no declared dependency at all) "
        "was not caught");
    require(outcome.success && outcome.manifest.has_value(),
        "The real Stage 2B shape (a cycle from mutual requiredReadPaths alone) must be "
        "repairable in one bounded attempt");
    const auto& repairPrompt = fixture.sessionFactory.log.requests[1].userContent;
    require(repairPrompt.find("backend_derived: qml/cmakelists.txt") != std::string::npos
            && repairPrompt.find("backend_derived: qml/pagehost.qml") != std::string::npos,
        "Repair prompt did not name the exact derived path that created each edge");
}

// MW-P3A package 2: this test's whole premise - a work task submitted with
// requestedCapability "none" - is no longer representable at all. The
// draft schema's work-task requestedCapability enum only accepts
// "inspect"/"propose" (manifest.invalid_capability rejects "none"
// immediately), and a pure coordination node is now its own structurally
// paths/acceptance-free shape (coordinationTasks), so
// manifest.none_task_not_empty can never fire via this pipeline any more -
// removed rather than adapted, per the same structural-fix-supersedes-
// regression-test precedent as the analogous fix in
// ARCHITECTURE_GAPS.md's MW-P3A package 1 closure note.

// Real Stage 2B incident (Run 207df296-dbb6-4d02-8a94-882358459e78, 2026-09-14,
// real OpenAI Run): same failure shape as above, but the primary rejection
// was manifest.conflicting_path_role instead.
void testConflictingPathRoleRepairPromptWarnsAboutCycleRisk()
{
    Fixture fixture;
    const std::string taskA = taskJsonWithRoles(
        "task-a-pagehost", "[]", "qml/pagehost.qml", "", "", "propose");
    // A hand-built task (taskJsonWithRoles has no deferredPaths parameter):
    // qml/pagehost.qml is declared in BOTH requiredReadPaths and
    // deferredPaths. ExecutionManifestPlanner's own pre-validation
    // canonicalization pass (canonicalizeManifestPathRoles, see
    // ExecutionManifestPlanner.cpp) silently removes a requiredReadPaths/
    // referencePaths entry that duplicates ownedPaths/plannedCreatePaths or
    // each other BEFORE validation ever runs, so those three combinations
    // can never actually reach manifest.conflicting_path_role through this
    // real flow - only excludedPaths/deferredPaths are left untouched by
    // that pass, matching how the real Run 207df296-... conflict actually
    // reached validation.
    const std::string taskBConflicting =
        "{\"id\":\"task-b-integration\",\"objective\":\"Complete task-b-integration\","
        "\"role\":\"worker\",\"dependencies\":[\"task-a-pagehost\"],"
        "\"writePaths\":[\"qml/project_context.md\"],"
        "\"baselineRequiredReadPaths\":[],"
        "\"proposedDependencyReadPaths\":[\"qml/pagehost.qml\"],"
        "\"referencePaths\":[],"
        "\"excludedPaths\":[],"
        "\"deferredPaths\":[{\"path\":\"qml/pagehost.qml\",\"reason\":\"Not needed this attempt\"}],"
        "\"acceptanceCriteria\":[\"Tests pass\"],"
        "\"acceptanceContract\":{\"schemaVersion\":\"execution-acceptance.v1\","
        "\"requirements\":[{\"id\":\"verify-output\",\"description\":\"Verify output\","
        "\"requiredOutputs\":[\"qml/project_context.md\"],\"verificationKind\":\"inspect_paths\","
        "\"verificationProfileId\":null,\"verificationBuildProfileId\":null,"
        "\"verificationTargetId\":null,"
        "\"verificationArguments\":[],\"required\":true,"
        "\"verificationBootstrapPending\":false}]},"
        "\"contextReferences\":[\"CODEX_CONTEXT.md\"],"
        "\"requestedCapability\":\"propose\"}";
    const auto primary = manifestJson({taskA, taskBConflicting});
    // The repair reproduces the real Run's own mistake: task-a now also
    // requires task-b's output, forming a cycle with task-b's existing
    // requirement on task-a's output.
    const auto repaired = manifestJson({
        taskJsonWithRoles("task-a-pagehost", "[]", "qml/pagehost.qml", "",
            "qml/project_context.md", "propose"),
        taskJsonWithRoles("task-b-integration", "[\"task-a-pagehost\"]",
            "qml/project_context.md", "", "qml/pagehost.qml", "propose")});
    fixture.addSession({submitTurn(0, primary)});
    fixture.addSession({submitTurn(0, repaired)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.primaryRejectionCode == "manifest.conflicting_path_role",
        "Expected the real Stage 2B primary rejection shape (Run 207df296-...)");
    const auto& repairPrompt = fixture.sessionFactory.log.requests[1].userContent;
    require(repairPrompt.find("manifest.conflicting_path_role") != std::string::npos
            && repairPrompt.find("NEW dependency cycle") != std::string::npos,
        "Repair prompt for conflicting_path_role must now also warn that reassigning a "
        "path's role can create a new cycle");
}

// AG-18.5 hardening (Run ed9b84fc-f0fd-4796-b7d2-743715247be1, 2026-09-27,
// real Anthropic canary "MW-P3 AG-18.5 bounded Anthropic canary: one-file
// verified change"): builds a single-writePaths work task with caller-
// controlled referencePaths/excludedPaths arrays (each a ready-made JSON
// array literal), to reproduce the real canary's referencePaths/excludedPaths
// conflict shape. taskJsonWithRoles has no excludedPaths parameter.
// Since Run de6d81fb (canonicalizeExcludedReadConflicts) a referencePaths entry
// that also appears in the SAME task's excludedPaths is resolved by the Backend
// and never reaches manifest.conflicting_path_role, so these fixtures now
// put the conflicting notes in deferredPaths instead: reference_only vs
// deferred is the active-role/inactive-note combination that is deliberately
// still a hard conflict. The parameter keeps its historical name; its array
// is written to deferredPaths.
std::string referenceExcludedConflictTaskJson(
    const std::string& id,
    const std::string& writePath,
    const std::string& referencePathsArray,
    const std::string& excludedPathsArray)
{
    return "{\"id\":\"" + id
        + "\",\"objective\":\"Complete " + id
        + "\",\"role\":\"worker\",\"dependencies\":[],"
          "\"writePaths\":[\"" + writePath + "\"],"
          "\"baselineRequiredReadPaths\":[],\"proposedDependencyReadPaths\":[],"
          "\"referencePaths\":" + referencePathsArray + ","
          "\"excludedPaths\":[],"
          "\"deferredPaths\":" + excludedPathsArray + ","
          "\"acceptanceCriteria\":[\"Tests pass\"],"
          "\"acceptanceContract\":{\"schemaVersion\":\"execution-acceptance.v1\","
          "\"requirements\":[{\"id\":\"verify-output\",\"description\":\"Verify output\","
          "\"requiredOutputs\":[\"" + writePath + "\"],\"verificationKind\":\"inspect_paths\","
          "\"verificationProfileId\":null,\"verificationBuildProfileId\":null,"
          "\"verificationTargetId\":null,"
          "\"verificationArguments\":[],\"required\":true,"
          "\"verificationBootstrapPending\":false}]},"
          "\"contextReferences\":[\"CODEX_CONTEXT.md\"],"
          "\"requestedCapability\":\"propose\"}";
}

std::string excludedEntry(
    const std::string& path,
    const std::string& reason = "Not part of this task's scope")
{
    return "{\"path\":\"" + path + "\",\"reason\":\"" + reason + "\"}";
}

// Exact production regression: the real bounded repair did not remove the
// one reported referencePaths/excludedPaths conflict on CMakeLists.txt - it
// left that conflict in place AND reproduced it on every other untouched
// baseline file (CMakeUserPresets.json, qml/CMakeLists.txt, main.cpp,
// importedcontent/README.md, qml/Main.qml), growing 1 conflicting path into
// 6. Before this hardening, introducesUnrelatedFinding compared findings only
// by (code, taskId), so all 6 repair findings shared the same (code, taskId)
// as the 1 primary finding and this real Run was reported as
// planner.repair_exhausted with manifest.conflicting_path_role as both the
// primary AND final rejection code, hiding that the repair made things worse.
void testConflictingPathRoleRepairThatMultipliesConflictsIsRegression()
{
    Fixture fixture;
    const auto primary = manifestJson({referenceExcludedConflictTaskJson(
        "task-canarypanel-label-text-edit", "qml/CanaryPanel.qml",
        "[\"CMakeLists.txt\"]", "[" + excludedEntry("CMakeLists.txt") + "]")});
    const auto repaired = manifestJson({referenceExcludedConflictTaskJson(
        "task-canarypanel-label-text-edit", "qml/CanaryPanel.qml",
        "[\"CMakeLists.txt\",\"CMakeUserPresets.json\",\"qml/CMakeLists.txt\","
        "\"main.cpp\",\"importedcontent/README.md\",\"qml/Main.qml\"]",
        "[" + excludedEntry("CMakeLists.txt") + "," + excludedEntry("CMakeUserPresets.json")
            + "," + excludedEntry("qml/CMakeLists.txt") + "," + excludedEntry("main.cpp") + ","
            + excludedEntry("importedcontent/README.md") + "," + excludedEntry("qml/Main.qml")
            + "]")});
    fixture.addSession({submitTurn(0, primary)});
    fixture.addSession({submitTurn(0, repaired)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.primaryRejectionCode == "manifest.conflicting_path_role",
        "Expected the real canary's primary rejection shape (Run ed9b84fc-...)");
    require(!outcome.success && outcome.errorCode == "planner.repair_exhausted",
        "Top-level budget-exhausted code must stay the same umbrella code as every "
        "other repair-regression outcome");
    require(outcome.lastRejectionCode == "planner.repair_regressed",
        "A repair that turned 1 conflicting path into 6 must be reported as a "
        "regression, not silently re-reported as the same primary code");
    require(!outcome.lastFindings.empty()
            && outcome.lastFindings.front().code == "planner.repair_regressed",
        "Regression finding must lead the final findings list");
    require(outcome.lastFindings.size() == 7,
        "Regression reporting must retain all six Backend validation findings after "
        "prepending planner.repair_regressed");
    const std::vector<std::string_view> normalizedConflictingPaths {
        "cmakelists.txt",
        "cmakeuserpresets.json",
        "qml/cmakelists.txt",
        "main.cpp",
        "importedcontent/readme.md",
        "qml/main.qml"};
    for (const auto path : normalizedConflictingPaths) {
        std::string expectedMessageFragment = "Path '";
        expectedMessageFragment += path;
        expectedMessageFragment += "'";
        require(std::any_of(outcome.lastFindings.begin(), outcome.lastFindings.end(),
                    [&](const auto& finding) {
                        return finding.code == "manifest.conflicting_path_role"
                            && finding.message.find(expectedMessageFragment) != std::string::npos
                            && finding.message.find("'reference_only'") != std::string::npos
                            && finding.message.find("'deferred'") != std::string::npos;
                    }),
            "Regression reporting dropped a deterministic Backend path/role finding");
    }
    const auto stored = fixture.manifests.findForRun("planner-run");
    require(stored.success() && !stored.value().has_value(),
        "A regressed repair unexpectedly froze a Manifest");
}

// Same (code, taskId) pair on both attempts, but a DIFFERENT conflicting
// path: the repair fixes the originally-reported conflict yet introduces one
// on a path never mentioned in the primary findings. This is the minimal
// case the old (code, taskId)-only identity could never distinguish from "no
// progress" - it must now register as a regression too.
void testConflictingPathRoleRepairWithDifferentPathIsRegression()
{
    Fixture fixture;
    const auto primary = manifestJson({referenceExcludedConflictTaskJson(
        "task-edit", "qml/Edit.qml", "[\"A.txt\"]", "[" + excludedEntry("A.txt") + "]")});
    const auto repaired = manifestJson({referenceExcludedConflictTaskJson(
        "task-edit", "qml/Edit.qml", "[\"B.txt\"]", "[" + excludedEntry("B.txt") + "]")});
    fixture.addSession({submitTurn(0, primary)});
    fixture.addSession({submitTurn(0, repaired)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.primaryRejectionCode == "manifest.conflicting_path_role",
        "Different-path regression fixture did not reach the expected primary code");
    require(outcome.lastRejectionCode == "planner.repair_regressed"
            && !outcome.lastFindings.empty()
            && outcome.lastFindings.front().code == "planner.repair_regressed",
        "Fixing the reported path while conflicting a different path in the same "
        "task must still be a regression, not a repeat of the same defect");
}

// Positive control: a repair that actually removes the referencePaths/
// excludedPaths conflict - by dropping the path from excludedPaths and
// keeping it only as referencePaths, exactly as the hardened repair prompt
// now instructs - must still succeed. Hardening the regression guard must
// never block a genuine fix.
void testConflictingPathRoleSuccessfulRepairRemovesConflict()
{
    Fixture fixture;
    const auto primary = manifestJson({referenceExcludedConflictTaskJson(
        "task-canarypanel-label-text-edit", "qml/CanaryPanel.qml",
        "[\"CMakeLists.txt\"]", "[" + excludedEntry("CMakeLists.txt") + "]")});
    const auto repaired = manifestJson({referenceExcludedConflictTaskJson(
        "task-canarypanel-label-text-edit", "qml/CanaryPanel.qml",
        "[\"CMakeLists.txt\"]", "[]")});
    fixture.addSession({submitTurn(0, primary)});
    fixture.addSession({submitTurn(0, repaired)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success && outcome.manifest.has_value(),
        "A genuine referencePaths/excludedPaths fix was rejected by the regression guard");
    require(outcome.primaryRejectionCode == "manifest.conflicting_path_role"
            && outcome.repairAttempts == 1,
        "Successful repair lost its primary diagnostics");
    const auto& task = outcome.manifest->tasks.front();
    require(task.referencePaths == std::vector<std::filesystem::path> {"CMakeLists.txt"}
            && task.excludedPaths.empty() && task.deferredPaths.empty(),
        "Successful repair's resulting manifest lost the expected reference-only path");
}

// Repair-prompt hardening: for a referencePaths/excludedPaths conflict, the
// generic six-list guidance was not enough for the real canary - the prompt
// must now also carry the dedicated rule set (allowed read-only context vs.
// deliberately-inactive path, never both; untouched baseline files need no
// entry at all; never "fix" one conflict by copying the rest of the baseline
// into both lists).
void testConflictingPathRoleRepairPromptWarnsAboutReferenceExcludedConfusion()
{
    Fixture fixture;
    const auto primary = manifestJson({referenceExcludedConflictTaskJson(
        "task-canarypanel-label-text-edit", "qml/CanaryPanel.qml",
        "[\"CMakeLists.txt\"]", "[" + excludedEntry("CMakeLists.txt") + "]")});
    const auto repaired = manifestJson({referenceExcludedConflictTaskJson(
        "task-canarypanel-label-text-edit", "qml/CanaryPanel.qml",
        "[\"CMakeLists.txt\"]", "[]")});
    fixture.addSession({submitTurn(0, primary)});
    fixture.addSession({submitTurn(0, repaired)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success, "Repair-prompt fixture unexpectedly failed to repair");
    const auto& repairPrompt = fixture.sessionFactory.log.requests[1].userContent;
    require(repairPrompt.find(
                "referencePaths means allowed, read-only context") != std::string::npos
            && repairPrompt.find(
                "excludedPaths means a path deliberately kept OUT of every active role")
                != std::string::npos,
        "Repair prompt must explicitly define referencePaths vs excludedPaths semantics");
    require(repairPrompt.find("belongs in NEITHER list at all") != std::string::npos,
        "Repair prompt must forbid auto-excluding untouched baseline files");
    require(repairPrompt.find(
                "NEVER an acceptable fix to resolve one reported referencePaths/excludedPaths "
                "conflict by adding the rest of the baseline into both") != std::string::npos,
        "Repair prompt must forbid copying the rest of the baseline into both lists as a "
        "'fix'");
}

// Real Anthropic canary efc7736a-6f6e-4897-9195-c16469fc1943
// (2026-09-28): the primary submission omitted requestedCapability and
// therefore failed at the parser boundary before a typed draft existed. Its
// one bounded repair added that field but introduced three unrelated
// referencePaths/excludedPaths conflicts. The finding-regression guard used
// to be accidentally gated on monotonicBaseDraft.has_value(), so this exact
// parser-failure -> validator-failure trade escaped as an ordinary exhausted
// repair instead of planner.repair_regressed.
void testMissingFieldRepairThatIntroducesPathConflictsIsRegression()
{
    Fixture fixture;
    auto primary = validManifest("propose");
    const std::string requestedCapability = ",\"requestedCapability\":\"propose\"";
    const auto requestedCapabilityPosition = primary.find(requestedCapability);
    require(requestedCapabilityPosition != std::string::npos,
        "Missing-field production fixture no longer matches the draft shape");
    primary.erase(requestedCapabilityPosition, requestedCapability.size());

    const auto repaired = manifestJson({referenceExcludedConflictTaskJson(
        "inspect", "src/inspect",
        "[\"CMakeLists.txt\",\"qml/CMakeLists.txt\",\"qml/Main.qml\"]",
        "[" + excludedEntry("CMakeLists.txt") + ","
            + excludedEntry("qml/CMakeLists.txt") + ","
            + excludedEntry("qml/Main.qml") + "]")});
    fixture.addSession({submitTurn(0, primary)});
    fixture.addSession({submitTurn(0, repaired)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(!outcome.success && outcome.errorCode == "planner.repair_exhausted",
        "A parser failure repaired into unrelated path conflicts was accepted");
    require(outcome.primaryRejectionCode == "manifest.missing_field",
        "Production regression fixture did not reach its parser-level primary rejection");
    require(outcome.lastRejectionCode == "planner.repair_regressed"
            && !outcome.lastFindings.empty()
            && outcome.lastFindings.front().code == "planner.repair_regressed",
        "Parser-level primary rejection did not participate in finding regression review");
    require(outcome.lastFindings.size() == 4,
        "Regression reporting must retain all three final path-conflict findings");
    require(std::count_if(outcome.lastFindings.begin(), outcome.lastFindings.end(),
                [](const auto& finding) {
                    return finding.code == "manifest.conflicting_path_role";
                }) == 3,
        "Regression reporting lost one or more deterministic path-conflict findings");
    const auto stored = fixture.manifests.findForRun("planner-run");
    require(stored.success() && !stored.value().has_value(),
        "A parser failure repaired into unrelated findings unexpectedly froze a Manifest");
}

// Both attempts fail with manifest.missing_field, but on different required
// fields. Because the parser's message is Backend-authored solely from the
// structural field location, that location is part of finding identity: a
// repair may fix the reported omission or repeat it, but may not trade it for
// a different missing field while appearing to make no new regression.
void testMissingFieldRepairWithDifferentMissingFieldIsRegression()
{
    Fixture fixture;
    auto primary = validManifest("propose");
    const std::string requestedCapability = ",\"requestedCapability\":\"propose\"";
    const auto requestedCapabilityPosition = primary.find(requestedCapability);
    require(requestedCapabilityPosition != std::string::npos,
        "Primary missing-field fixture no longer matches the draft shape");
    primary.erase(requestedCapabilityPosition, requestedCapability.size());

    auto repaired = validManifest("propose");
    const std::string role = "\"role\":\"worker\",";
    const auto rolePosition = repaired.find(role);
    require(rolePosition != std::string::npos,
        "Repair missing-field fixture no longer matches the draft shape");
    repaired.erase(rolePosition, role.size());
    fixture.addSession({submitTurn(0, primary)});
    fixture.addSession({submitTurn(0, repaired)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.primaryRejectionCode == "manifest.missing_field",
        "Primary missing-field fixture did not reach the parser boundary");
    require(outcome.lastRejectionCode == "planner.repair_regressed"
            && outcome.lastFindings.size() == 2
            && outcome.lastFindings.front().code == "planner.repair_regressed"
            && outcome.lastFindings[1].code == "manifest.missing_field"
            && outcome.lastFindings[1].message.find("workTasks[0].role")
                != std::string::npos,
        "A repair that exchanged one deterministic missing field for another was not "
        "classified as a regression");
}

// Real-Run incident (Runs 37640091-.../207df296-.../e8275bde-.../397c0ffb-.../
// 2d2cf273-..., 2026-09-14, all real Stage 2B on OpenAI, same real task): the
// dependency_cycle repair guidance's "merge the tasks" instruction, already
// present since 2026-09-11, was not enough - all three of these Runs kept
// resubmitting the same or a near-identical cycle even with that guidance
// available. In every one, one of the two cycle-forming edges was a
// pre-existing project-context file (QML_PROJECT_CONTEXT.md) neither task's
// real work modifies, falsely claimed as owned by one task purely to give
// the other task's read "something to depend on" - merging was never the
// right fix. This proves the system prompt now warns against inventing that
// ownership in the first place, and the repair prompt now tells the model to
// check for exactly this shape before considering a merge.
void testSystemPromptWarnsAgainstFalseSharedReferenceOwnership()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0,
        manifestJson({taskJsonWithRoles("build-task", "[]", "src/app.cpp", "", "", "propose")}))});
    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success, "A trivial single-task submission should succeed unrelated to this check");
    const auto& systemPrompt = fixture.sessionFactory.log.requests[0].systemPrompt;
    require(systemPrompt.find("QML_PROJECT_CONTEXT.md") != std::string::npos
            && systemPrompt.find("single most common real cause") != std::string::npos,
        "System prompt must warn against ever claiming ownership of a pre-existing shared "
        "reference file merely to justify another task's read of it");
}

void testDependencyCycleRepairPromptChecksFalseOwnershipBeforeMerging()
{
    Fixture fixture;
    const auto cyclic = manifestJson({
        taskJson("task-a", "[\"task-b\"]", "src/a", "propose"),
        taskJson("task-b", "[\"task-a\"]", "src/b", "propose")});
    const auto stillCyclic = manifestJson({
        taskJson("task-a", "[\"task-b\"]", "src/a", "propose"),
        taskJson("task-b", "[\"task-a\"]", "src/b", "propose")});
    fixture.addSession({submitTurn(0, cyclic)});
    fixture.addSession({submitTurn(0, stillCyclic)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.primaryRejectionCode == "manifest.dependency_cycle",
        "This fixture's declared-dependency cycle must be caught as the primary rejection");
    const auto& repairPrompt = fixture.sessionFactory.log.requests[1].userContent;
    require(repairPrompt.find("First check whether either edge-forming") != std::string::npos
            && repairPrompt.find("do NOT merge the tasks in this case") != std::string::npos,
        "manifest.dependency_cycle repair guidance must check for false shared-reference "
        "ownership before suggesting a merge");
}

// The manifest shape MW-P1's own diagnostics proved was the real Run's
// actual problem: neither task owns QML_PROJECT_CONTEXT.md (it is a
// pre-existing file this Run does not modify), both read it directly, and
// task-b separately, legitimately depends on task-a's new file. This must
// validate cleanly with no cycle at all - proving the Backend already fully
// supports the CORRECT shape the guidance above now steers the Planner
// toward; the real failure was purely a prompting gap, never a validator
// limitation.
void testSharedPreexistingReferenceFileOwnedByNeitherTaskHasNoCycle()
{
    Fixture fixture;
    const std::string taskA =
        "{\"id\":\"task-a-pagehost\",\"objective\":\"Complete task-a-pagehost\","
        "\"role\":\"worker\",\"dependencies\":[],"
        "\"writePaths\":[\"qml/components/navigation/pagehost.qml\"],"
        "\"baselineRequiredReadPaths\":[\"QML_PROJECT_CONTEXT.md\"],"
        "\"proposedDependencyReadPaths\":[],"
        "\"referencePaths\":[],\"excludedPaths\":[],\"deferredPaths\":[],"
        "\"acceptanceCriteria\":[\"Tests pass\"],"
        "\"acceptanceContract\":{\"schemaVersion\":\"execution-acceptance.v1\","
        "\"requirements\":[{\"id\":\"verify-output\",\"description\":\"Verify output\","
        "\"requiredOutputs\":[\"qml/components/navigation/pagehost.qml\"],"
        "\"verificationKind\":\"inspect_paths\","
        "\"verificationProfileId\":null,\"verificationBuildProfileId\":null,"
        "\"verificationTargetId\":null,"
        "\"verificationArguments\":[],\"required\":true,"
        "\"verificationBootstrapPending\":false}]},"
        "\"contextReferences\":[\"CODEX_CONTEXT.md\"],"
        "\"requestedCapability\":\"propose\"}";
    const std::string taskB =
        "{\"id\":\"task-b-integration\",\"objective\":\"Complete task-b-integration\","
        "\"role\":\"worker\",\"dependencies\":[\"task-a-pagehost\"],"
        "\"writePaths\":[\"qml/navigation/registry.qml\"],"
        "\"baselineRequiredReadPaths\":[\"QML_PROJECT_CONTEXT.md\"],"
        "\"proposedDependencyReadPaths\":[\"qml/components/navigation/pagehost.qml\"],"
        "\"referencePaths\":[],\"excludedPaths\":[],\"deferredPaths\":[],"
        "\"acceptanceCriteria\":[\"Tests pass\"],"
        "\"acceptanceContract\":{\"schemaVersion\":\"execution-acceptance.v1\","
        "\"requirements\":[{\"id\":\"verify-output\",\"description\":\"Verify output\","
        "\"requiredOutputs\":[\"qml/navigation/registry.qml\"],"
        "\"verificationKind\":\"inspect_paths\","
        "\"verificationProfileId\":null,\"verificationBuildProfileId\":null,"
        "\"verificationTargetId\":null,"
        "\"verificationArguments\":[],\"required\":true,"
        "\"verificationBootstrapPending\":false}]},"
        "\"contextReferences\":[\"CODEX_CONTEXT.md\"],"
        "\"requestedCapability\":\"propose\"}";
    const auto correct = manifestJson({taskA, taskB});
    fixture.addSession({submitTurn(0, correct)});

    // MW-P3A package 2: QML_PROJECT_CONTEXT.md must be a real baseline file
    // for the Backend compiler to accept it as a genuine
    // baselineRequiredReadPaths entry (manifest.baseline_required_read_path_not_in_baseline
    // otherwise) - this is itself part of what this test now proves: the
    // Backend actively verifies the "pre-existing" claim rather than trusting
    // it blindly.
    auto plannerRequest = request();
    WorkspaceBaselineSnapshot baseline;
    WorkspaceBaselineEntry contextEntry;
    contextEntry.relativePath = "QML_PROJECT_CONTEXT.md";
    contextEntry.byteSize = 10;
    contextEntry.sha256 = "hash-context";
    baseline.files.push_back(contextEntry);
    plannerRequest.baselineSnapshot = baseline;

    const auto outcome = fixture.planner.plan(std::move(plannerRequest), {});
    require(outcome.success && outcome.manifest.has_value(),
        "A manifest where the pre-existing shared reference file is owned by neither task "
        "must be accepted on the first submission, with no cycle");
    require(fixture.sessionFactory.log.requests.size() == 1,
        "No repair should ever be needed for this already-correct shape");
}

// #4/#5: a cycle still present after the one bounded repair must exhaust
// honestly (planner.repair_exhausted), never loop further - and, since no
// Manifest is ever frozen in that case, no worker session could ever be
// dispatched for this Run.
void testRepeatedDependencyCycleExhaustsRepairHonestly()
{
    Fixture fixture;
    const auto cyclic = manifestJson({
        taskJson("task-a", "[\"task-b\"]", "src/a", "propose"),
        taskJson("task-b", "[\"task-a\"]", "src/b", "propose")});
    fixture.addSession({submitTurn(0, cyclic)});
    fixture.addSession({submitTurn(0, cyclic)});

    const auto outcome = fixture.planner.plan(request(), {});
    require(!outcome.success && outcome.errorCode == "planner.repair_exhausted",
        "A cycle repeated after the one bounded repair must exhaust honestly, never loop "
        "further");
    require(outcome.primaryRejectionCode == "manifest.dependency_cycle"
            && outcome.lastRejectionCode == "manifest.dependency_cycle",
        "Primary/final rejection codes lost across the exhausted repair");
    require(!outcome.manifest.has_value(),
        "An exhausted repair must never produce a manifest - no worker dispatch is ever "
        "possible from this Run");
    const auto stored = fixture.manifests.findForRun("planner-run");
    require(stored.success() && !stored.value().has_value(),
        "Exhausted repair left a partial manifest snapshot");
}

void testTurnBudgetPartitionForcesTerminalBoundaryBeforeRunWideLimit()
{
    // Real-Run incidents ade2ca50-0c15-4762-a2b8-3ffb0e6f0b3e and its Edit &
    // rerun retry 7385ad56-1720-4835-b717-123bcff298f9 (2026-09-28): a
    // Planner that keeps requesting inspection must never be allowed to
    // spend the entire frozen Run-wide turn budget without ever reaching a
    // terminal submission. With maxTurns=24 (the frozen production
    // ceiling), partitionPlannerTurnBudget() reserves inspectionBoundaryTurn
    // = 24 - 1 (terminal) - 4 (recovery) - 4 (worker) = 15, so turn 15 must
    // be Backend-forced to a terminal-only NextTurnToolPolicy long before
    // the shared guard's own 24-turn ceiling could ever be reached.
    Fixture fixture;
    std::vector<ProviderAgentTurnResponse> turns;
    for (std::size_t i = 0; i < 24; ++i) {
        turns.push_back(toolTurn(
            i, "inspect-" + std::to_string(i), "workspace_read_file",
            R"({"path":"README.md"})"));
    }
    fixture.addSession(std::move(turns));

    ProviderExecutionBudgetPolicy policy;
    policy.maxTurns = 24;
    auto guard = std::make_shared<ProviderSessionBudgetGuard>(policy);
    auto plannerRequest = request();
    plannerRequest.providerBudgetGuard = guard;

    const auto outcome = fixture.planner.plan(plannerRequest, {});
    require(!outcome.success && outcome.errorCode == "planner.forced_terminal_turn_violated",
        "An infinitely-inspecting scripted Planner must be stopped by the Backend-forced "
        "terminal turn, never by exhausting the shared Run-wide guard");
    // Turns 0..14 (15 inspect turns) are free; the submitToolResults call
    // that produces turn 15 is the first Backend-forced terminal-only turn -
    // this scripted session ignores the restriction and requests another
    // inspect tool, which must be rejected without ever being executed.
    require(guard->usage().turnsStarted == 16,
        "The forced-terminal boundary did not fire at the expected turn index");
    require(fixture.executor.callCount == 15,
        "An inspect tool call executed on or after the Backend-forced terminal turn");
    require(fixture.sessionFactory.log.nextTurnPolicies.size() == 15,
        "One submitToolResults call is expected per turn after beginSession up to and "
        "including the forced turn");
    const auto& lastPolicy = fixture.sessionFactory.log.nextTurnPolicies.back();
    require(lastPolicy.has_value(),
        "The submitToolResults call for the boundary turn must carry the forced policy");
    require(lastPolicy->allowedToolNames.size() == 1
            && lastPolicy->allowedToolNames.front() == kSubmitExecutionManifestDraftToolName,
        "The default Run only makes submit_execution_manifest_draft applicable on a forced "
        "terminal turn");
    require(lastPolicy->disallowParallelToolCalls,
        "A Backend-forced terminal turn must disallow parallel tool calls");
    for (std::size_t i = 0; i + 1 < fixture.sessionFactory.log.nextTurnPolicies.size(); ++i) {
        require(!fixture.sessionFactory.log.nextTurnPolicies[i].has_value(),
            "Every submitToolResults call before the inspection boundary must stay "
            "unrestricted");
    }
}

void testWorkerTurnReserveRemainsAfterSuccessfulForcedTerminalSubmission()
{
    // The Planner is pushed all the way to the Backend-forced boundary, then
    // complies and submits a valid Manifest on the forced turn itself - the
    // worker reserve carved out by partitionPlannerTurnBudget() must still
    // be intact in the SAME shared guard afterward, since
    // ExecutionManifestPlannerRequest::providerBudgetGuard is reused
    // unchanged for every worker session on this Run.
    Fixture fixture;
    std::vector<ProviderAgentTurnResponse> turns;
    for (std::size_t i = 0; i < 15; ++i) {
        turns.push_back(toolTurn(
            i, "inspect-" + std::to_string(i), "workspace_read_file",
            R"({"path":"README.md"})"));
    }
    turns.push_back(submitTurn(
        15, manifestJson({taskJson("task-boundary", "[]", "src/boundary", "propose")})));
    fixture.addSession(std::move(turns));

    ProviderExecutionBudgetPolicy policy;
    policy.maxTurns = 24;
    auto guard = std::make_shared<ProviderSessionBudgetGuard>(policy);
    auto plannerRequest = request();
    plannerRequest.providerBudgetGuard = guard;

    const auto outcome = fixture.planner.plan(plannerRequest, {});
    require(outcome.success && outcome.manifest.has_value(),
        "A valid Manifest submitted on the forced terminal turn must still be accepted");
    require(guard->usage().turnsStarted == 16,
        "Planning at the exact boundary must consume exactly inspectionBoundaryTurn + 1 turns");
    require(24 - guard->usage().turnsStarted >= kPlannerWorkerTurnReserve,
        "A nonzero, fully-reserved worker turn budget must remain after successful planning");
}

void testInsufficientRunWideTurnBudgetFailsBeforeProviderDispatch()
{
    // A frozen Run-wide budget too small to fit the fixed reserves plus one
    // inspection turn must refuse before ANY provider session is created -
    // no session is even queued below, so a provider dispatch would fail
    // with "test.sessions_exhausted" instead of this stable typed code.
    Fixture fixture;
    ProviderExecutionBudgetPolicy policy;
    policy.maxTurns = kPlannerMinimumRunWideTurnsForPartition - 1;
    auto guard = std::make_shared<ProviderSessionBudgetGuard>(policy);
    auto plannerRequest = request();
    plannerRequest.providerBudgetGuard = guard;

    const auto outcome = fixture.planner.plan(plannerRequest, {});
    require(!outcome.success
            && outcome.errorCode == "planner_turn_budget.insufficient_run_wide_turns",
        "An insufficient Run-wide turn budget must fail closed with a stable typed code");
    require(fixture.sessionFactory.createCalls == 0,
        "An insufficient turn budget must refuse before any provider session is created");
    require(guard->usage().turnsStarted == 0,
        "No provider turn may ever be started when the budget partition itself is refused");
}

void testInvalidSubmissionOnForcedTerminalTurnStillFailsClosedWithinRecoveryReserve()
{
    // The forced terminal turn itself can still be answered with an invalid
    // typed submission; the existing bounded validation-repair mechanism
    // must still apply, and it must stay inside the Backend-owned recovery
    // reserve rather than reopening the shared Run-wide budget.
    Fixture fixture;
    std::vector<ProviderAgentTurnResponse> primaryTurns;
    for (std::size_t i = 0; i < 15; ++i) {
        primaryTurns.push_back(toolTurn(
            i, "inspect-" + std::to_string(i), "workspace_read_file",
            R"({"path":"README.md"})"));
    }
    primaryTurns.push_back(submitTurn(15, R"({"schemaVersion":"execution-manifest.v1"})"));
    fixture.addSession(std::move(primaryTurns));
    fixture.addSession({submitTurn(0, R"({"schemaVersion":"execution-manifest.v1"})")});

    ProviderExecutionBudgetPolicy policy;
    policy.maxTurns = 24;
    auto guard = std::make_shared<ProviderSessionBudgetGuard>(policy);
    auto plannerRequest = request();
    plannerRequest.providerBudgetGuard = guard;

    const auto outcome = fixture.planner.plan(plannerRequest, {});
    require(!outcome.success && outcome.errorCode == "planner.repair_exhausted",
        "An invalid submission on the forced terminal turn must still exhaust the one bounded "
        "validation repair and fail closed");
    require(fixture.sessionFactory.createCalls == 2,
        "Exactly one repair session must be created after the forced-terminal rejection");
    require(guard->usage().turnsStarted <= 24 - kPlannerWorkerTurnReserve,
        "The bounded repair after the forced terminal turn must stay inside the recovery "
        "reserve, never spending the worker reserve");
    require(!outcome.manifest.has_value(),
        "An exhausted repair must never freeze a Manifest");
}

void testTurnBudgetPartitionV3CapsInspectionAndDirectsSurplusToWorkers()
{
    const auto original = partitionPlannerTurnBudget(24);
    require(original.success(), "The established 24-turn partition must remain valid");
    require(original.value().policyId == "planner_turn_budget_partition_v3",
        "The fingerprinted partition identity must advance to v3");
    require(original.value().inspectionTurns == 15
            && original.value().workerReserveTurns == 4,
        "v3 must preserve the established 24-turn 15/4 partition");

    const auto expanded = partitionPlannerTurnBudget(40);
    require(expanded.success(), "The 40-turn Stage 4B partition must be valid");
    require(expanded.value().inspectionTurns == 15
            && expanded.value().inspectionBoundaryTurn == 15,
        "Increasing maxTurns must never increase Planner inspection above 15");
    require(expanded.value().workerReserveTurns == 20,
        "Every turn added above 24 must increase the guaranteed worker reserve");

    const auto maximum = partitionPlannerTurnBudget(kProviderBudgetHardMaxTurns);
    require(maximum.success() && maximum.value().inspectionTurns == 15
            && maximum.value().workerReserveTurns == 44,
        "The inspection cap and monotonic worker allocation must hold at the hard ceiling");
}

void testBothRecoveryKindsConsumeExactReserveAndLeaveWorkerFloor()
{
    // Exercise the worst permitted Planner path behind the 1+4+4 partition,
    // rather than proving each reserve only in isolation:
    //   15 primary inspection turns + forced Stop,
    //   one unrestricted terminalization-recovery inspection + forced
    //   invalid submission,
    //   one unrestricted validation-repair inspection + forced valid
    //   submission.
    // The Planner must finish on turn 20 and leave exactly the four frozen
    // worker turns. A future change that accidentally reopens unrestricted
    // inspection in either fresh recovery session makes this test fail.
    Fixture fixture;
    std::vector<ProviderAgentTurnResponse> primaryTurns;
    for (std::size_t i = 0; i < 15; ++i) {
        primaryTurns.push_back(toolTurn(
            i, "primary-inspect-" + std::to_string(i), "workspace_read_file",
            R"({"path":"README.md"})"));
    }
    primaryTurns.push_back(stopTurn(15));
    fixture.addSession(std::move(primaryTurns));

    fixture.addSession({
        toolTurn(0, "terminalization-inspect", "workspace_read_file",
            R"({"path":"README.md"})"),
        submitTurn(1, R"({"schemaVersion":"execution-manifest.v1"})")});

    fixture.addSession({
        toolTurn(0, "validation-inspect", "workspace_read_file",
            R"({"path":"README.md"})"),
        submitTurn(1,
            manifestJson({taskJson("task-after-both-recoveries", "[]",
                "src/recovered", "propose")}))});

    ProviderExecutionBudgetPolicy policy;
    policy.maxTurns = 24;
    auto guard = std::make_shared<ProviderSessionBudgetGuard>(policy);
    auto plannerRequest = request();
    plannerRequest.providerBudgetGuard = guard;

    const auto outcome = fixture.planner.plan(plannerRequest, {});
    require(outcome.success && outcome.manifest.has_value(),
        "The bounded terminalization and validation recoveries must still accept a valid "
        "final Manifest");
    require(outcome.repairAttempts == 2,
        "The worst-case reserve fixture must exercise both independent recovery kinds");
    require(fixture.sessionFactory.createCalls == 3,
        "Primary, terminalization recovery, and validation repair must use exactly three "
        "provider sessions");
    require(guard->usage().turnsStarted == 20
            && guard->usage().turnsCompleted == 20,
        "Both bounded recovery kinds must consume exactly the four reserved recovery turns");
    require(policy.maxTurns - guard->usage().turnsStarted == kPlannerWorkerTurnReserve,
        "The complete worst-case Planner path must leave the exact Backend-owned worker floor");
    require(fixture.executor.callCount == 17,
        "Only the fifteen primary and one-per-recovery inspection calls may execute");
    const auto forcedPolicyCount = std::count_if(
        fixture.sessionFactory.log.nextTurnPolicies.begin(),
        fixture.sessionFactory.log.nextTurnPolicies.end(),
        [](const auto& value) { return value.has_value(); });
    require(forcedPolicyCount == 3,
        "The primary boundary and both fresh recovery sessions must each force their terminal "
        "submission turn");
}

void testForcedTerminalPolicyIncludesNoChangesOnlyWhenApplicable()
{
    // allowEvidenceBackedNoChanges=true widens the forced policy to include
    // submit_no_changes_needed; this Run's verification environment is not
    // SetupRequired, so submit_verification_action_required stays excluded
    // even though the full session always advertises it (see
    // ExecutionManifestPlanner::plan()'s tool list).
    Fixture fixture;
    std::vector<ProviderAgentTurnResponse> turns;
    for (std::size_t i = 0; i < 1; ++i) {
        turns.push_back(toolTurn(
            i, "inspect-" + std::to_string(i), "workspace_read_file",
            R"({"path":"README.md"})"));
    }
    turns.push_back(toolTurn(
        1, "still-inspecting", "workspace_read_file", R"({"path":"README.md"})"));
    fixture.addSession(std::move(turns));

    ProviderExecutionBudgetPolicy policy;
    policy.maxTurns = kPlannerMinimumRunWideTurnsForPartition; // inspectionBoundaryTurn == 1
    auto guard = std::make_shared<ProviderSessionBudgetGuard>(policy);
    auto plannerRequest = noChangesRequest();
    plannerRequest.providerBudgetGuard = guard;

    const auto outcome = fixture.planner.plan(plannerRequest, {});
    require(!outcome.success, "Scripted continued inspection must still be stopped");
    const auto& policies = fixture.sessionFactory.log.nextTurnPolicies;
    require(!policies.empty() && policies.front().has_value(),
        "The first submitToolResults call must already be forced given inspectionBoundaryTurn "
        "== 1");
    const auto& allowed = policies.front()->allowedToolNames;
    require(allowed.size() == 2, "Exactly two terminal tools are applicable to this Run");
    require(std::find(allowed.begin(), allowed.end(),
                std::string(kSubmitExecutionManifestDraftToolName)) != allowed.end(),
        "submit_execution_manifest_draft is always applicable");
    require(std::find(allowed.begin(), allowed.end(),
                std::string(kSubmitNoChangesNeededToolName)) != allowed.end(),
        "allowEvidenceBackedNoChanges=true must widen the forced terminal policy to include "
        "submit_no_changes_needed");
    require(std::find(allowed.begin(), allowed.end(),
                std::string(kSubmitVerificationActionRequiredToolName)) == allowed.end(),
        "submit_verification_action_required must stay excluded when verification is not "
        "SetupRequired");
}

void testForcedTerminalPolicyIncludesVerificationActionRequiredOnlyWhenSetupRequired()
{
    Fixture fixture;
    fixture.addSession({
        toolTurn(0, "inspect-0", "workspace_read_file", R"({"path":"README.md"})"),
        toolTurn(1, "still-inspecting", "workspace_read_file", R"({"path":"README.md"})")});

    ProviderExecutionBudgetPolicy policy;
    policy.maxTurns = kPlannerMinimumRunWideTurnsForPartition; // inspectionBoundaryTurn == 1
    auto guard = std::make_shared<ProviderSessionBudgetGuard>(policy);
    auto plannerRequest = setupRequiredRequest();
    plannerRequest.providerBudgetGuard = guard;

    const auto outcome = fixture.planner.plan(plannerRequest, {});
    require(!outcome.success, "Scripted continued inspection must still be stopped");
    const auto& policies = fixture.sessionFactory.log.nextTurnPolicies;
    require(!policies.empty() && policies.front().has_value(),
        "The first submitToolResults call must already be forced given inspectionBoundaryTurn "
        "== 1");
    const auto& allowed = policies.front()->allowedToolNames;
    require(allowed.size() == 2, "Exactly two terminal tools are applicable to this SetupRequired "
        "Run");
    require(std::find(allowed.begin(), allowed.end(),
                std::string(kSubmitVerificationActionRequiredToolName)) != allowed.end(),
        "submit_verification_action_required must be applicable while genuinely SetupRequired");
    require(std::find(allowed.begin(), allowed.end(),
                std::string(kSubmitNoChangesNeededToolName)) == allowed.end(),
        "submit_no_changes_needed must stay excluded when this Run disallows it");
}

// ---------------------------------------------------------------------
// AG-17.0: Backend-owned mandatory verification policy at the Planner seam.
// ---------------------------------------------------------------------

VerificationEnvironmentSnapshot mandatorySnapshot(
    const std::vector<std::string>& targets, bool complete = true)
{
    VerificationEnvironmentSnapshot snapshot;
    snapshot.runId = "planner-run";
    snapshot.projectId = "project-1";
    snapshot.baselineFingerprint = "baseline-1";
    snapshot.state = VerificationEnvironmentState::ExistingProfile;
    VerificationProfile profile;
    profile.profileId = "local-qt-debug";
    profile.configurePresetName = "local-qt-debug";
    profile.resolvedBinaryDir = "build/debug";
    profile.origin = VerificationProfileOrigin::Discovered;
    profile.buildPresetsConfirmed = true;
    VerificationBuildPresetDescriptor buildPreset;
    buildPreset.name = "local-qt-debug";
    buildPreset.source = VerificationPresetFileSource::UserPresets;
    buildPreset.declaredConfigurePreset = "local-qt-debug";
    buildPreset.allowedTargetsConfirmed = true;
    buildPreset.allowedTargetsComplete = complete;
    buildPreset.allowedTargets = targets;
    profile.buildPresets.push_back(std::move(buildPreset));
    snapshot.profiles.push_back(std::move(profile));
    return snapshot;
}

ExecutionManifestPlannerRequest mandatoryRequest(const VerificationEnvironmentSnapshot& snapshot)
{
    auto value = request();
    value.verificationEnvironment = snapshot;
    return value;
}

// A Manifest that writes QML and, like the real Stage 4B.1/4B.2 Manifests,
// carries no qml_lint (or any command-backed) criterion of its own.
std::string qmlOnlyManifest()
{
    return manifestJson({taskJson("qml-task", "[]", "qml/Page.qml", "propose")});
}

const ExecutionAcceptanceRequirement* findFrozenRequirement(
    const ExecutionManifest& manifest, const std::string& id)
{
    for (const auto& task : manifest.tasks) {
        if (!task.acceptanceContract.has_value()) continue;
        for (const auto& requirement : task.acceptanceContract->requirements) {
            if (requirement.id == id) return &requirement;
        }
    }
    return nullptr;
}

void testMandatoryPolicyInjectsOmittedLintWithoutAnyRepair()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0, qmlOnlyManifest())});
    const auto outcome = fixture.planner.plan(
        mandatoryRequest(mandatorySnapshot({"all_qmllint", "Dashboard_Content_qmllint", "all"})), {});
    require(outcome.success && outcome.manifest.has_value(),
        "A Manifest that omitted qml_lint must freeze with the Backend's requirement added");
    require(outcome.repairAttempts == 0 && fixture.sessionFactory.createCalls == 1,
        "Injecting a mandatory requirement must never cost a provider repair turn");

    const auto* lint = findFrozenRequirement(*outcome.manifest, "backend-mandatory-qml-lint");
    const auto* build = findFrozenRequirement(*outcome.manifest, "backend-mandatory-cmake-build");
    require(lint != nullptr && build != nullptr, "The mandatory lint/build requirements are missing");
    require(lint->origin == ExecutionRequirementOrigin::BackendMandatory
            && build->origin == ExecutionRequirementOrigin::BackendMandatory,
        "Backend-added requirements must carry origin=backend_mandatory");
    require(lint->required && build->required
            && lint->verificationKind == ExecutionVerificationKind::QmlLint
            && build->verificationKind == ExecutionVerificationKind::CMakeBuild,
        "The injected requirements have the wrong kind or are not required");
    require(!lint->verificationArguments.empty() && lint->qmlLintDiagnosticPolicyId.has_value()
            && !build->verificationArguments.empty(),
        "The injected requirements must be resolved by the one existing binder before freeze");
    require(lint->verificationTargetId == std::optional<std::string>("all_qmllint"),
        "The aggregate lint target was not selected");
    // The planner's own requirement is untouched and stays planner_declared.
    const auto* own = findFrozenRequirement(*outcome.manifest, "verify-output");
    require(own != nullptr && own->origin == ExecutionRequirementOrigin::PlannerDeclared,
        "The Planner's own requirement must stay planner_declared");

    require(outcome.manifest->mandatoryVerification.has_value()
            && outcome.manifest->mandatoryVerification->policyId
                == kManifestMandatoryVerificationPolicyId,
        "The frozen Manifest must carry the mandatory verification decision record");
    const auto& decisions = outcome.manifest->mandatoryVerification->decisions;
    require(decisions.size() == 2
            && decisions[0].status == MandatoryVerificationDecisionStatus::Injected
            && decisions[0].requirementId == "backend-mandatory-qml-lint"
            && decisions[0].taskId == "qml-task",
        "The record must name the injected lint requirement and its anchor task");
}

void testMandatoryPolicyFailureEndsPlanningWithoutRepairOrFreeze()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0, qmlOnlyManifest())});
    // A second session is queued so that a (wrong) repair would consume it.
    fixture.addSession({submitTurn(0, qmlOnlyManifest())});
    const auto outcome = fixture.planner.plan(
        mandatoryRequest(mandatorySnapshot({"A_qmllint", "B_qmllint", "all"})), {});
    require(!outcome.success && !outcome.manifest.has_value(),
        "An ambiguous lint target must not freeze a Manifest");
    require(outcome.errorCode == kVerificationPolicyLintTargetAmbiguous,
        "The typed policy code must be the planning error, not a repair outcome");
    require(outcome.repairAttempts == 0 && fixture.sessionFactory.createCalls == 1
            && fixture.sessionFactory.log.requests.size() == 1,
        "A Backend policy failure must call the provider exactly once and never repair");
    require(outcome.primaryRejectionCode == kVerificationPolicyLintTargetAmbiguous
            && outcome.lastRejectionCode == kVerificationPolicyLintTargetAmbiguous
            && !outcome.primaryFindings.empty(),
        "The policy rejection must be recorded for the planning failure signature");
    require(outcome.errorCode != "planner.repair_exhausted",
        "A policy failure must not masquerade as an exhausted repair");
    require(isNonRepairableSubmissionFailure(outcome.errorCode),
        "The classifier must recognise the policy code");
}

void testMandatoryPolicyFailureAfterARepairKeepsTheTypedCode()
{
    Fixture fixture;
    // The primary submission is an ordinary (repairable) rejection; the one
    // repair then reaches the policy and is rejected THERE.
    fixture.addSession({submitTurn(0, targetOnlyCMakeBuildManifest())});
    fixture.addSession({submitTurn(0, qmlOnlyManifest())});
    const auto outcome = fixture.planner.plan(
        mandatoryRequest(mandatorySnapshot({"A_qmllint", "B_qmllint"})), {});
    require(!outcome.success && !outcome.manifest.has_value(),
        "A repaired Manifest rejected by the policy must not freeze");
    require(outcome.errorCode == kVerificationPolicyLintTargetAmbiguous,
        "A policy rejection of the repaired submission must keep its typed code, not become "
        "planner.repair_regressed or planner.repair_exhausted");
    require(fixture.sessionFactory.createCalls == 2,
        "Exactly the one ordinary repair may have been spent before the policy rejected");
    require(outcome.primaryRejectionCode == "acceptance.provider_owned_verification_argument"
            && outcome.lastRejectionCode == kVerificationPolicyLintTargetAmbiguous,
        "The primary and final rejection codes must both survive");
}

void testNoVerificationEnvironmentMeansThePolicyIsNotEvaluated()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0, qmlOnlyManifest())});
    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success && outcome.manifest.has_value(),
        "A Run without a verification environment must keep planning as before");
    require(!outcome.manifest->mandatoryVerification.has_value()
            && findFrozenRequirement(*outcome.manifest, "backend-mandatory-qml-lint") == nullptr,
        "Without a snapshot there is nothing to derive from: no record, no injection");
}

void testProviderCanNeverSubmitARequirementOrigin()
{
    auto json = qmlOnlyManifest();
    const std::string from = "\"verificationBootstrapPending\":false}";
    const auto position = json.find(from);
    require(position != std::string::npos, "Planner test Manifest shape changed");
    json.replace(position, from.size(),
        "\"verificationBootstrapPending\":false,\"origin\":\"backend_mandatory\"}");
    require(!ExecutionManifestDraftParser::parse(json).success(),
        "A provider submission carrying an origin field must be rejected as an unexpected field");
}

// ---------------------------------------------------------------------
// AG-17.1: the Backend-owned routing record at the Planner seam.
// ---------------------------------------------------------------------

ManifestRoutingRecord singleAssignmentRouting()
{
    StageModelAssignment architect;
    architect.stageType = "architect";
    architect.providerId = "provider-1";
    architect.modelId = "model-1";
    architect.role = "lead";
    architect.instructions = "Plan carefully.";
    architect.position = 0;
    architect.timeout = std::chrono::milliseconds(60000);
    return ManifestRouting::fromAssignments({architect});
}

void testTrustedRoutingIsStampedOnTheFrozenManifest()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0, validManifest())});
    auto plannerRequest = request();
    plannerRequest.trustedRouting = singleAssignmentRouting();
    const auto outcome = fixture.planner.plan(plannerRequest, {});
    require(outcome.success && outcome.manifest.has_value(),
        "Planning with a trusted routing record failed");
    require(outcome.repairAttempts == 0 && fixture.sessionFactory.createCalls == 1,
        "Stamping the routing record must not change the provider call count");
    require(outcome.manifest->routing.has_value()
            && outcome.manifest->routing->fingerprint == plannerRequest.trustedRouting->fingerprint
            && outcome.manifest->routing->mode == ManifestRoutingMode::SingleAssignment
            && outcome.manifest->routing->members.size() == 1
            && outcome.manifest->routing->members.front().memberId == "architect",
        "The frozen Manifest must carry the trusted routing record unchanged");
    for (const auto& task : outcome.manifest->tasks) {
        require(!task.assigneeMemberId.has_value(),
            "A single-assignment Manifest's tasks must stay without an assignee");
    }
}

void testInvalidTrustedRoutingFailsBeforeAnyProviderSession()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0, validManifest())});
    auto plannerRequest = request();
    plannerRequest.trustedRouting = singleAssignmentRouting();
    plannerRequest.trustedRouting->members.front().role = "tampered"; // fingerprint now stale
    const auto outcome = fixture.planner.plan(plannerRequest, {});
    require(!outcome.success && !outcome.manifest.has_value(),
        "A defective Backend routing record must not freeze a Manifest");
    require(outcome.errorCode == "planner.routing_invalid",
        "A defective routing record must fail with the typed Backend code");
    require(fixture.sessionFactory.createCalls == 0 && outcome.repairAttempts == 0,
        "A Backend defect must be rejected before any provider session, never sent to a repair");
}

void testWithoutTrustedRoutingTheManifestCarriesNone()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0, validManifest())});
    const auto outcome = fixture.planner.plan(request(), {});
    require(outcome.success && outcome.manifest.has_value()
            && !outcome.manifest->routing.has_value(),
        "Without a trusted record (pre-AG-17.1 callers) the Manifest must carry no routing");
}

void testProviderCanNeverSubmitARoutingRecord()
{
    auto json = qmlOnlyManifest();
    const std::string closing = "\"bootstrapDeclaration\":null}";
    const auto position = json.find(closing);
    require(position != std::string::npos, "Planner test Manifest shape changed");
    json.replace(position, closing.size(),
        "\"bootstrapDeclaration\":null,\"routing\":{\"mode\":\"roster\"}}");
    require(!ExecutionManifestDraftParser::parse(json).success(),
        "A provider submission carrying a routing record must be rejected as an unexpected field");
}

// ---------------------------------------------------------------------
// AG-17.4: roster-constrained Planner (catalog, assignee schema, repair).
// ---------------------------------------------------------------------

std::vector<StageModelAssignment> rosterAssignments()
{
    const auto member = [](const char* id, const char* role, bool lead, bool workspace,
                           const char* instructions, std::uint32_t position) {
        StageModelAssignment value;
        value.stageType = id;
        value.providerId = "provider-1";
        value.modelId = "model-1";
        value.role = role;
        value.leadArchitect = lead;
        value.workspaceAccess = workspace;
        value.instructions = instructions;
        value.position = position;
        value.timeout = std::chrono::milliseconds(60000);
        return value;
    };
    return {member("architect", "lead", true, false, "Plan carefully.", 0),
            member("qml-worker", "qml", false, true, "Write QML only.", 1),
            member("cpp-worker", "cpp", false, true, "Write C++ only.", 2),
            member("doc-writer", "docs", false, false, "Write docs.", 3)};
}

std::vector<ManifestRosterMemberInfo> rosterCatalogOf(
    const std::vector<StageModelAssignment>& assignments)
{
    std::vector<ManifestRosterMemberInfo> catalog;
    for (const auto& item : assignments)
        catalog.push_back({item.stageType, "model of " + item.stageType, item.instructions});
    return catalog;
}

ExecutionManifestPlannerRequest rosterRequest()
{
    auto value = request();
    const auto assignments = rosterAssignments();
    value.trustedRouting = ManifestRouting::fromAssignments(assignments);
    value.rosterCatalog = rosterCatalogOf(assignments);
    return value;
}

std::string withAssignee(std::string task, const std::string& memberId)
{
    task.pop_back(); // the closing brace
    return task + ",\"assigneeMemberId\":\"" + memberId + "\"}";
}

std::string rosterManifest(const std::string& uiAssignee, const std::string& coreAssignee)
{
    return manifestJson({
        withAssignee(taskJson("ui", "[]", "src/ui.qml", "propose"), uiAssignee),
        withAssignee(taskJson("core", "[]", "src/core.cpp", "propose"), coreAssignee)});
}

const AgentToolDefinition* findTool(
    const ProviderAgentSessionRequest& sent, const std::string& name)
{
    for (const auto& tool : sent.tools) {
        if (tool.name == name) return &tool;
    }
    return nullptr;
}

void testRosterPlanningShowsTheCatalogAndRequiresAnAssignee()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0, rosterManifest("qml-worker", "cpp-worker"))});
    const auto outcome = fixture.planner.plan(rosterRequest(), {});
    require(outcome.success && outcome.manifest.has_value(),
        "A roster Manifest with valid assignees must be planned");
    require(outcome.repairAttempts == 0 && fixture.sessionFactory.createCalls == 1,
        "A valid roster submission must not cost a repair");
    require(outcome.manifest->routing.has_value()
            && outcome.manifest->routing->mode == ManifestRoutingMode::Roster
            && outcome.manifest->routing->members.size() == 4,
        "The frozen Manifest must carry the Backend's roster routing record");
    const auto assigneeOf = [&](const std::string& taskId) -> std::optional<std::string> {
        for (const auto& task : outcome.manifest->tasks) {
            if (task.id == taskId) return task.assigneeMemberId;
        }
        return std::nullopt;
    };
    require(assigneeOf("ui") == "qml-worker" && assigneeOf("core") == "cpp-worker",
        "Each task must keep the assignee the provider named");

    const auto& sent = fixture.sessionFactory.log.requests.front();
    require(sent.systemPrompt.find("# Trusted roster") != std::string::npos
            && sent.systemPrompt.find("memberId: \"qml-worker\"") != std::string::npos
            && sent.systemPrompt.find("Write C++ only.") != std::string::npos
            && sent.systemPrompt.find("model: model of cpp-worker") != std::string::npos,
        "The Planner must be shown the trusted roster with each worker's role and instructions");
    require(sent.systemPrompt.find("doc-writer") == std::string::npos
            && sent.systemPrompt.find("memberId: \"architect\"") == std::string::npos,
        "Only assignable members may be offered: not the lead, not a member without workspace "
        "access");
    require(sent.systemPrompt.find("exactly these thirteen fields") != std::string::npos
            && sent.systemPrompt.find("fourteen") != std::string::npos,
        "The roster section must reconcile the thirteen-field task shape with the extra field");

    const auto* tool = findTool(sent, kSubmitExecutionManifestDraftToolName);
    require(tool != nullptr, "The draft terminal tool must stay advertised");
    const auto schema = nlohmann::json::parse(tool->inputSchemaJson);
    const auto& assignee = schema["properties"]["workTasks"]["items"]["properties"]["assigneeMemberId"];
    require(assignee["enum"] == nlohmann::json::array({"qml-worker", "cpp-worker"}),
        "The tool schema must restrict the assignee to exactly the assignable members");
    require(tool->inputSchemaJson == submitExecutionManifestDraftToolDefinition(
                {"qml-worker", "cpp-worker"}).inputSchemaJson,
        "The Planner must advertise the roster variant built from the Backend's assignable ids");
}

void testSingleAssignmentPlanningIsUnchangedByTheRosterContract()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0, validManifest())});
    auto plannerRequest = request();
    plannerRequest.trustedRouting = singleAssignmentRouting();
    const auto outcome = fixture.planner.plan(plannerRequest, {});
    require(outcome.success && outcome.manifest.has_value(), "Single-assignment planning failed");

    const auto& sent = fixture.sessionFactory.log.requests.front();
    require(sent.systemPrompt.find("Trusted roster") == std::string::npos,
        "A single-assignment prompt must carry no roster section");
    const auto* tool = findTool(sent, kSubmitExecutionManifestDraftToolName);
    require(tool != nullptr
            && tool->inputSchemaJson == submitExecutionManifestDraftToolDefinition().inputSchemaJson
            && tool->inputSchemaJson.find("assigneeMemberId") == std::string::npos,
        "A single-assignment Run must be offered the unchanged tool schema");
}

void testRosterTaskWithoutAnAssigneeIsRepairedOnce()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0, manifestJson({
        withAssignee(taskJson("ui", "[]", "src/ui.qml", "propose"), "qml-worker"),
        taskJson("core", "[]", "src/core.cpp", "propose")}))});
    fixture.addSession({submitTurn(0, rosterManifest("qml-worker", "cpp-worker"))});

    const auto outcome = fixture.planner.plan(rosterRequest(), {});
    require(outcome.success && outcome.manifest.has_value() && outcome.repairAttempts == 1,
        "A roster task missing its assignee must be fixed by the one bounded repair");
    require(outcome.primaryRejectionCode == "manifest.routing_assignee_missing",
        "The primary rejection must be the typed routing finding");

    require(fixture.sessionFactory.log.requests.size() == 2,
        "Exactly one repair session must follow");
    const auto& repair = fixture.sessionFactory.log.requests[1];
    require(repair.userContent.find("manifest.routing_assignee_missing") != std::string::npos
            && repair.userContent.find("fourteen fields in total") != std::string::npos
            && repair.userContent.find("\"qml-worker\", \"cpp-worker\"") != std::string::npos,
        "The repair prompt must restate the roster field and the assignable ids, because its "
        "thirteen-field restatement would otherwise drop the assignee");
    const auto* tool = findTool(repair, kSubmitExecutionManifestDraftToolName);
    require(tool != nullptr
            && tool->inputSchemaJson.find("\"enum\":[\"qml-worker\",\"cpp-worker\"]")
                != std::string::npos,
        "The repair session must keep the roster tool schema");
    require(repair.systemPrompt.find("# Trusted roster") != std::string::npos,
        "The repair session must keep the trusted roster");
}

void testRosterRejectsLeadUnknownAndNonWorkspaceAssignees()
{
    const std::pair<const char*, const char*> cases[] = {
        {"architect", "manifest.routing_assignee_is_lead"},
        {"ghost", "manifest.routing_assignee_unknown"},
        {"doc-writer", "manifest.routing_assignee_capability_mismatch"}};
    for (const auto& [assignee, code] : cases) {
        Fixture fixture;
        fixture.addSession({submitTurn(0, rosterManifest(assignee, "cpp-worker"))});
        fixture.addSession({submitTurn(0, rosterManifest("qml-worker", "cpp-worker"))});
        const auto outcome = fixture.planner.plan(rosterRequest(), {});
        require(outcome.primaryRejectionCode == code,
            "A defective assignee must be rejected with its typed routing code");
        require(outcome.success && outcome.repairAttempts == 1,
            "The one bounded repair must be able to fix a defective assignee");
    }

    Fixture stuck;
    stuck.addSession({submitTurn(0, rosterManifest("architect", "cpp-worker"))});
    stuck.addSession({submitTurn(0, rosterManifest("architect", "cpp-worker"))});
    const auto exhausted = stuck.planner.plan(rosterRequest(), {});
    require(!exhausted.success && !exhausted.manifest.has_value()
            && exhausted.errorCode == "planner.repair_exhausted",
        "A repair that keeps assigning the lead must never freeze a Manifest");
}

void testRepairChangingAnUnrelatedTasksAssigneeIsARegression()
{
    Fixture fixture;
    // Primary: only "core" is defective (no assignee). The repair fixes it but also
    // moves the unrelated, valid "ui" task to another member.
    fixture.addSession({submitTurn(0, manifestJson({
        withAssignee(taskJson("ui", "[]", "src/ui.qml", "propose"), "qml-worker"),
        taskJson("core", "[]", "src/core.cpp", "propose")}))});
    fixture.addSession({submitTurn(0, rosterManifest("cpp-worker", "cpp-worker"))});
    const auto outcome = fixture.planner.plan(rosterRequest(), {});
    require(!outcome.success && !outcome.manifest.has_value()
            && outcome.errorCode == "planner.repair_exhausted"
            && outcome.lastRejectionCode == "planner.repair_regressed",
        "Reassigning a task the findings never named must be rejected as a regression");
}

void testRosterCatalogDefectsFailBeforeAnyProviderSession()
{
    const auto expectInvalid = [](ExecutionManifestPlannerRequest plannerRequest,
                                  const char* code, const char* why) {
        Fixture fixture;
        fixture.addSession({submitTurn(0, rosterManifest("qml-worker", "cpp-worker"))});
        const auto outcome = fixture.planner.plan(plannerRequest, {});
        require(!outcome.success && !outcome.manifest.has_value() && outcome.errorCode == code, why);
        require(fixture.sessionFactory.createCalls == 0 && outcome.repairAttempts == 0,
            "A Backend defect must be rejected before any provider session, never repaired");
    };

    auto drifted = rosterRequest();
    drifted.rosterCatalog[1].instructions += " (edited after freeze)";
    expectInvalid(drifted, "planner.roster_catalog_invalid",
        "A catalog that drifted from the frozen record must fail");

    auto missing = rosterRequest();
    missing.rosterCatalog.clear();
    expectInvalid(missing, "planner.roster_catalog_invalid",
        "A roster without a catalog must fail rather than plan blind");

    auto orphan = request();
    orphan.rosterCatalog = rosterCatalogOf(rosterAssignments());
    expectInvalid(orphan, "planner.roster_catalog_invalid",
        "A catalog without the routing record it describes must fail");

    auto single = request();
    single.trustedRouting = singleAssignmentRouting();
    single.rosterCatalog = rosterCatalogOf(rosterAssignments());
    expectInvalid(single, "planner.roster_catalog_invalid",
        "A single-assignment Run must not receive a roster catalog");

    auto noWorker = rosterAssignments();
    noWorker[1].workspaceAccess = false;
    noWorker[2].workspaceAccess = false;
    auto lonely = request();
    lonely.trustedRouting = ManifestRouting::fromAssignments(noWorker);
    lonely.rosterCatalog = rosterCatalogOf(noWorker);
    expectInvalid(lonely, "planner.roster_no_assignable_member",
        "A roster with no workspace worker has nobody to assign and must fail closed");
}

void testSingleAssignmentAcceptsOnlyItsOwnMemberAsExplicitAssignee()
{
    Fixture named;
    named.addSession({submitTurn(0, manifestJson({
        withAssignee(taskJson("ui", "[]", "src/ui.qml", "propose"), "architect")}))});
    auto namedRequest = request();
    namedRequest.trustedRouting = singleAssignmentRouting();
    const auto accepted = named.planner.plan(namedRequest, {});
    require(accepted.success && accepted.manifest.has_value()
            && accepted.manifest->tasks.front().assigneeMemberId == "architect",
        "A single-assignment task may name its sole member explicitly");

    Fixture stranger;
    stranger.addSession({submitTurn(0, manifestJson({
        withAssignee(taskJson("ui", "[]", "src/ui.qml", "propose"), "ghost")}))});
    stranger.addSession({submitTurn(0, validManifest())});
    auto strangerRequest = request();
    strangerRequest.trustedRouting = singleAssignmentRouting();
    const auto repaired = stranger.planner.plan(strangerRequest, {});
    require(repaired.primaryRejectionCode == "manifest.routing_assignee_unknown",
        "A single-assignment Manifest cannot name a member outside its record");
}

void testNonRepairableClassifierCoversOnlyPolicyCodes()
{
    require(isNonRepairableSubmissionFailure("verification_policy.lint_target_ambiguous")
            && isNonRepairableSubmissionFailure("verification_policy.anything_else"),
        "Every verification_policy.* code must be non-repairable");
    require(!isNonRepairableSubmissionFailure("manifest.verification_target_unknown")
            && !isNonRepairableSubmissionFailure("planner.missing_submission")
            && !isNonRepairableSubmissionFailure("acceptance.invalid_verification_selection"),
        "An ordinary repairable rejection must stay repairable");
}

} // namespace

// ---------------------------------------------------------------------------
// worker_capacity_reservation_v2: the Run's turn capacity is judged BEFORE the Planner's
// answer is accepted and frozen
// ---------------------------------------------------------------------------

// A propose task whose `readCount` required reads are baseline files named docs/read-<i>.md
// (its serial-safe floor is readCount + 1 turns).
std::string readHeavyTaskJson(
    const std::string& id, const std::string& ownedPath, std::size_t readCount,
    const std::string& dependencies = "[]")
{
    std::string reads = "[";
    for (std::size_t index = 0; index < readCount; ++index) {
        if (index != 0) reads += ',';
        reads += "\"docs/read-" + std::to_string(index) + ".md\"";
    }
    reads += "]";
    return "{\"id\":\"" + id + "\",\"objective\":\"Complete " + id
        + "\",\"role\":\"worker\",\"dependencies\":" + dependencies
        + ",\"writePaths\":[\"" + ownedPath + "\"],\"baselineRequiredReadPaths\":" + reads
        + ",\"proposedDependencyReadPaths\":[],\"referencePaths\":[],\"excludedPaths\":[],"
          "\"deferredPaths\":[],\"acceptanceCriteria\":[\"Tests pass\"],"
          "\"acceptanceContract\":{\"schemaVersion\":\"execution-acceptance.v1\","
          "\"requirements\":[{\"id\":\"verify-output\",\"description\":\"Verify output\","
          "\"requiredOutputs\":[\"" + ownedPath + "\"],\"verificationKind\":\"inspect_paths\","
          "\"verificationProfileId\":null,\"verificationBuildProfileId\":null,"
          "\"verificationTargetId\":null,\"verificationArguments\":[],\"required\":true,"
          "\"verificationBootstrapPending\":false}]},"
          "\"contextReferences\":[\"CODEX_CONTEXT.md\"],\"requestedCapability\":\"propose\"}";
}

ExecutionManifestPlannerRequest capacityRequest(
    const std::shared_ptr<ProviderSessionBudgetGuard>& guard, std::size_t baselineFiles = 80)
{
    auto plannerRequest = request();
    WorkspaceBaselineSnapshot baseline;
    for (std::size_t index = 0; index < baselineFiles; ++index) {
        WorkspaceBaselineEntry entry;
        entry.relativePath = "docs/read-" + std::to_string(index) + ".md";
        entry.byteSize = 10;
        entry.sha256 = "hash-" + std::to_string(index);
        baseline.files.push_back(entry);
    }
    plannerRequest.baselineSnapshot = baseline;
    plannerRequest.providerBudgetGuard = guard;
    return plannerRequest;
}

std::shared_ptr<ProviderSessionBudgetGuard> capacityGuard(std::uint64_t maxTurns, bool automatic)
{
    ProviderExecutionBudgetPolicy policy;
    policy.maxTurns = maxTurns;
    policy.autoExtendTurns = automatic;
    return std::make_shared<ProviderSessionBudgetGuard>(policy);
}

bool findingsContain(
    const std::vector<ExecutionManifestFinding>& findings, std::string_view code)
{
    return std::any_of(findings.begin(), findings.end(),
        [&](const ExecutionManifestFinding& finding) { return finding.code == code; });
}

// A pinned Run limit the answer does not fit: rejected as repairable feedback, the one
// bounded repair re-shapes the work, and only the repaired answer is frozen.
void testCapacityRefusalIsRepairedBeforeTheManifestIsFrozen()
{
    Fixture fixture;
    // 2 tasks x 5 reads = floors 6 + 6 = 12, each reserved twice (first attempt + protected retry
    // reservation, worker_capacity_reservation_v3) = 24; 1 turn used; the Run is pinned at 20 turns.
    fixture.addSession({submitTurn(0, manifestJson({
        readHeavyTaskJson("task-a", "src/a.qml", 5),
        readHeavyTaskJson("task-b", "src/b.qml", 5)}))});
    // The repair: 2 reads each (floors 3 + 3 = 6, twice = 12; 2 used) fits.
    fixture.addSession({submitTurn(1, manifestJson({
        readHeavyTaskJson("task-a", "src/a.qml", 2),
        readHeavyTaskJson("task-b", "src/b.qml", 2)}))});

    const auto guard = capacityGuard(20, /*automatic=*/false);
    const auto outcome = fixture.planner.plan(capacityRequest(guard), {});
    require(outcome.success && outcome.manifest.has_value() && outcome.repairAttempts == 1,
        "A capacity refusal must be repaired in the one bounded repair, not frozen or failed");
    require(outcome.primaryRejectionCode == "planner.capacity_run_wide"
            && findingsContain(outcome.primaryFindings, "planner.capacity_run_wide"),
        "The primary rejection must be the Run-wide capacity finding");
    require(outcome.manifest->tasks.size() == 2
            && outcome.manifest->tasks[0].requiredReadPaths.size() == 2
            && outcome.manifest->tasks[1].requiredReadPaths.size() == 2,
        "Only the repaired Manifest may be frozen");
    require(fixture.sessionFactory.log.requests.size() == 2,
        "The repair must be exactly one more provider session");
    const auto& prompt = fixture.sessionFactory.log.requests[1].userContent;
    require(prompt.find("planner.capacity_run_wide") != std::string::npos
            && prompt.find("planner.capacity_*") != std::string::npos
            && prompt.find("24 provider turn(s) in total") != std::string::npos
            && prompt.find("protected retry reservation") != std::string::npos
            && prompt.find("explicit turn limit") != std::string::npos
            && prompt.find("referencePaths") != std::string::npos
            && prompt.find("reduce the total by at least") != std::string::npos,
        "The repair prompt must carry the numbers, the unit and what to change");
    const auto stored = fixture.manifests.findForRun("planner-run");
    require(stored.success() && stored.value().has_value()
            && stored.value()->tasks[0].requiredReadPaths.size() == 2,
        "The stored Manifest must be the repaired one");
    // The post-freeze admission agrees with what the Planner accepted.
    const auto admitted = admitManifestWorkerReservations(
        *outcome.manifest, {}, guard->policy(), guard->usage());
    require(admitted.success(), "A Manifest the Planner accepted must be admitted afterwards");
}

void testCapacityRefusalThatTheRepairDoesNotFixNeverFreezes()
{
    Fixture fixture;
    const auto oversized = manifestJson({
        readHeavyTaskJson("task-a", "src/a.qml", 5),
        readHeavyTaskJson("task-b", "src/b.qml", 5)});
    fixture.addSession({submitTurn(0, oversized)});
    fixture.addSession({submitTurn(1, oversized)});

    const auto outcome = fixture.planner.plan(
        capacityRequest(capacityGuard(12, /*automatic=*/false)), {});
    require(!outcome.success && !outcome.manifest.has_value()
            && outcome.errorCode == "planner.repair_exhausted",
        "A repair that leaves the capacity problem must exhaust, not freeze");
    require(outcome.lastRejectionCode == "planner.capacity_run_wide"
            && findingsContain(outcome.lastFindings, "planner.capacity_run_wide"),
        "The final findings must still name the capacity refusal");
    const auto stored = fixture.manifests.findForRun("planner-run");
    require(stored.success() && !stored.value().has_value(),
        "A refused Manifest must leave no frozen snapshot");
}

// The same answer is judged by the limit the policy permits: an automatic turn limit may
// still be raised up to the Backend ceiling, a pinned one may not.
void testAutomaticLimitAcceptsWhatPinnedLimitRefuses()
{
    // 2 tasks x 11 reads = floors 12 x 2 = 24, each reserved twice (first attempt + protected
    // retry reservation) = 48; 1 turn used -> 49 turns.
    const auto answer = manifestJson({
        readHeavyTaskJson("task-a", "src/a.qml", 11),
        readHeavyTaskJson("task-b", "src/b.qml", 11)});
    {
        Fixture fixture;
        fixture.addSession({submitTurn(0, answer)});
        const auto outcome = fixture.planner.plan(
            capacityRequest(capacityGuard(24, /*automatic=*/true)), {});
        require(outcome.success && outcome.repairAttempts == 0 && outcome.manifest.has_value(),
            "49 turns fit the Backend ceiling, so an automatic limit needs no repair");
    }
    {
        Fixture fixture;
        fixture.addSession({submitTurn(0, answer)});
        fixture.addSession({submitTurn(1, answer)});
        const auto outcome = fixture.planner.plan(
            capacityRequest(capacityGuard(24, /*automatic=*/false)), {});
        require(!outcome.success && outcome.errorCode == "planner.repair_exhausted"
                && findingsContain(outcome.primaryFindings, "planner.capacity_run_wide"),
            "The same answer exceeds an explicit 24-turn limit");
    }
}

// Above the Backend ceiling one task can never fit a worker session: the finding names it,
// the repair may change only it, and a long task below the ceiling is simply accepted.
void testTaskAboveTheBackendCeilingIsNamedAndRepaired()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0, manifestJson({
        readHeavyTaskJson("task-big", "src/big.qml", 64),
        readHeavyTaskJson("task-small", "src/small.qml", 1)}))});
    fixture.addSession({submitTurn(1, manifestJson({
        readHeavyTaskJson("task-big", "src/big.qml", 20),
        readHeavyTaskJson("task-small", "src/small.qml", 1)}))});

    const auto outcome = fixture.planner.plan(
        capacityRequest(capacityGuard(24, /*automatic=*/true)), {});
    require(outcome.success && outcome.repairAttempts == 1 && outcome.manifest.has_value(),
        "A task above the ceiling must be repaired once");
    const auto named = std::find_if(outcome.primaryFindings.begin(), outcome.primaryFindings.end(),
        [](const ExecutionManifestFinding& finding) {
            return finding.code == "planner.capacity_task_floor"; });
    require(named != outcome.primaryFindings.end() && named->taskId == "task-big"
            && named->message.find("65 provider turn(s)") != std::string::npos
            && named->message.find("ceiling of 64") != std::string::npos,
        "The finding must name the task and the numbers");
    require(findingsContain(outcome.primaryFindings, "planner.capacity_run_wide"),
        "A task above the ceiling also exceeds the Run's total: the Run-wide numbers come with it");
    require(outcome.manifest->tasks[0].requiredReadPaths.size() == 20,
        "A 20-read task needs 21 turns: below the ceiling, so accepted (its own session grows)");
}

// The protected retry reservation is part of what the Run must afford: four tasks of seven reads
// (floors 8 x 4 = 32) fit the 64-turn ceiling for a single attempt each (33 turns) but not with the
// retry reservations (1 + 64 = 65), so the Planner is told BEFORE the Manifest is frozen; a repaired
// answer (floors 6 x 4, twice = 48 + 2 used) is accepted.
void testRetryReservationsCountTowardsThePlannerCapacityCheck()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0, manifestJson({
        readHeavyTaskJson("task-a", "src/a.qml", 7),
        readHeavyTaskJson("task-b", "src/b.qml", 7),
        readHeavyTaskJson("task-c", "src/c.qml", 7),
        readHeavyTaskJson("task-d", "src/d.qml", 7)}))});
    fixture.addSession({submitTurn(1, manifestJson({
        readHeavyTaskJson("task-a", "src/a.qml", 5),
        readHeavyTaskJson("task-b", "src/b.qml", 5),
        readHeavyTaskJson("task-c", "src/c.qml", 5),
        readHeavyTaskJson("task-d", "src/d.qml", 5)}))});

    const auto guard = capacityGuard(24, /*automatic=*/true);
    const auto outcome = fixture.planner.plan(capacityRequest(guard), {});
    require(outcome.success && outcome.repairAttempts == 1 && outcome.manifest.has_value()
            && outcome.primaryRejectionCode == "planner.capacity_run_wide",
        "A Manifest that only fits without the retry reservations must be repaired once");
    const auto& prompt = fixture.sessionFactory.log.requests[1].userContent;
    require(prompt.find("64 provider turn(s) in total") != std::string::npos
            && prompt.find("protected retry reservation") != std::string::npos
            && prompt.find("saves two") != std::string::npos,
        "The feedback must carry the doubled total and say that one turn less in a floor saves two");

    // Without the retry reservations (33 turns) the first answer would have been accepted.
    require(outcome.manifest->tasks.size() == 4
            && outcome.manifest->tasks[0].requiredReadPaths.size() == 5,
        "Only the repaired Manifest may be frozen");
}

// Without a shared guard (direct callers) nothing is judged: source compatible.
void testCapacityIsNotJudgedWithoutASharedGuard()
{
    Fixture fixture;
    fixture.addSession({submitTurn(0, manifestJson({
        readHeavyTaskJson("task-big", "src/big.qml", 64)}))});
    auto plannerRequest = capacityRequest(nullptr);
    const auto outcome = fixture.planner.plan(std::move(plannerRequest), {});
    if (!outcome.success || outcome.repairAttempts != 0) {
        throw std::runtime_error(
            "Direct callers without a guard keep their pre-capacity behaviour; got "
            + outcome.errorCode + " / " + outcome.primaryRejectionCode + " / "
            + (outcome.primaryFindings.empty() ? std::string {} : outcome.primaryFindings.front().message));
    }
}

int main()
{
    try {
    testValidPlanningUsesInspectOnlyToolsAndFreezesManifest();
    testPlannerPreservesCacheUsageAcrossAllTurns();
    testMalformedSubmissionGetsExactlyOneRepair();
    testRequireManifestRejectsAuditOnlyGraphBeforeFreeze();
    testAcceptanceOutputOutsideTaskScopeIsRepairedBeforeFreeze();
    testRepairCannotRewriteTaskUnrelatedToPrimaryFinding();
    testRegressedRepairNamesTheChangedTopLevelControl();
    testRepairControlsAreRestoredFromThePrimaryDraft();
    testRepairControlsRestoreNullAndValueInBothDirections();
    testGraphWideOrEmptyFindingsRestoreNothing();
    testAFindingOnOneControlExcludesOnlyThatField();
    testRestoringTheControlsTwiceIsIdempotent();
    testRestorationLeavesTheHardControlsAlone();
    testRepairNudgingNumericControlsIsAcceptedWithPrimaryValues();
    testStructuralChangeNextToANumericOneIsStillRejected();
    testGraphWidePrimaryFindingKeepsTheRepairsControls();
    testRequiredReadRemovesTheSameTasksExcludedEntry();
    testReferenceOnlyRemovesTheSameTasksExcludedEntry();
    testEquivalentNormalizedPathsConflict();
    testSeveralExcludedDuplicatesGiveOneNormalization();
    testWriteAndDeferredConflictsAreNotResolved();
    testTheSamePathInAnotherTaskIsNotCanonicalized();
    testExcludedReadCanonicalizationIsIdempotentAndOrdered();
    testWriteAndDeferredExcludedConflictsStillFailThroughPlan();
    testMachinesAndAlertsScenarioPassesWithoutRepair();
    testTaskLocalRepairPreservesUnrelatedTaskAndSucceeds();
    testRepairCannotTradePrimaryFindingForNewFinding();
    testStringExcludedPathIsRepairedToPathReasonObject();
    testRedundantOwnedAndRequiredReadRoleIsCanonicalizedWithoutRepair();
    testAllRedundantActiveScopeRolesUseStableNonExpandingPrecedence();
    testContradictoryActiveAndExcludedRolesRemainFailClosed();
    testAbsentWritePathClassifiesAsPlannedCreateWithoutAnyRepair();
    testInvalidVerificationProfileUsesExactlyOneRepairBeforeFreeze();
    testCmakeTargetConsistencySubmissionIsRejectedThenRepairedToBuild();
    testCmakeTargetConsistencyRepeatedSubmissionExhaustsRepair();
    testOwnershipOverlapIsRejectedThenRepaired();
    testRequestedParallelismAboveTheCeilingIsLoweredNotRejected();
    testRequestedParallelismWithinTheCeilingIsKeptAsSubmitted();
    testZeroOrOversizedParallelismRequestIsStillRejected();
    testNormalizationAuditDescribesOnlyTheAcceptedSubmission();
    testRequirementIdRepeatedByAnotherTaskIsRenamedNotRejected();
    testRequirementIdRenameNeverCollidesWithAnotherDeclaredId();
    testDistinctRequirementIdsAreKeptAsSubmitted();
    testRequirementIdRepeatedInsideOneTaskIsStillRejected();
    testDependencyCycleIsRejectedThenRepaired();
    testDeniedCapabilityCannotSelfAuthorize();
    testExhaustedRepairFailsWithoutSnapshot();
    testRawGitReadStaysDeniedAndNeverBecomesProposalPath();
    testUnsafeTypedProjectionFailsBeforeProviderOrAudit();
    testEvidenceBackedNoChangesNeededSucceedsWithoutManifestSnapshot();
    testRequireManifestRejectsNoChangesAndRepairsToManifest();
    testNoChangesNeededWithoutEvidenceIsRejectedThenRepaired();
    testNoChangesNeededWithoutSuccessfulInspectionIsRejected();
    testEmptyManifestTasksStillRejectedAsEmptyTasks();
    testWorkTaskMissingAcceptanceContractKeyFailsToParseNotARuntimeRule();
    testMissingSubmissionRepairsIntoValidNoChangesNeeded();
    testSuccessfulRepairPreservesPrimaryManifestFindings();
    testPrimaryAndFinalRejectionCodesBothSurvive();
    testIncidentChainRecoversAfterTerminalizationAndValidationRepair();
    testRepeatedMissingSubmissionCannotLoop();
    testPartialCompletionStillReturnsNonEmptyManifest();
    testTerminalSubmissionMustBeAloneInTurnEvenForNoChanges();
    testEvidencePathNotBackedByThisAttemptsInspectionIsRejected();
    testRepairCannotUsePrimarysInspectionForNoChangesNeeded();
    testRepairOwnMatchingInspectionRecoversNoChangesNeeded();
    testExplicitActionRequiredAcceptedWhenSetupRequired();
    testExplicitActionRequiredRejectedWhenExistingProfile();
    testProviderFailurePassesThroughWithoutBecomingActionRequired();
    testMalformedSubmissionNeverBecomesActionRequiredWhenSetupRequired();
    testNoChangesCannotDisguiseSetupRequired();
    testValidBootstrapManifestRemainsAllowedWhenSetupRequired();
    testMissingScopeDependencyIsCanonicalizedBeforeManifestFreeze();
    testCanonicalScopeDependencyIsAddedExactlyOnce();
    testMissingScopeDependencyRepairThatCreatesCycleStaysRejected();
    testPlannerPromptDistinguishesExcludedFromRequiredReadOnlyPaths();
    testValidManifestPlacesConsumedContractFileInRequiredReadPaths();
    testInspectTaskCommandVerificationIsRepairedToInspectPaths();
    testInspectTaskCommandVerificationRepairPromptNamesBothCorrectionOptions();
    testInspectCommandVerificationAndDependencyCycleBothRepairedInOneAttempt();
    testDependencyCycleRepairedByFixingEdgeDirection();
    testDependencyCycleRepairedByMergingMutuallyDependentTasks();
    testDependencyCycleFromMutualRequiredReadPathsIsRepairedByMerging();
    testConflictingPathRoleRepairPromptWarnsAboutCycleRisk();
    testConflictingPathRoleRepairThatMultipliesConflictsIsRegression();
    testConflictingPathRoleRepairWithDifferentPathIsRegression();
    testConflictingPathRoleSuccessfulRepairRemovesConflict();
    testConflictingPathRoleRepairPromptWarnsAboutReferenceExcludedConfusion();
    testMissingFieldRepairThatIntroducesPathConflictsIsRegression();
    testMissingFieldRepairWithDifferentMissingFieldIsRegression();
    testSystemPromptWarnsAgainstFalseSharedReferenceOwnership();
    testDependencyCycleRepairPromptChecksFalseOwnershipBeforeMerging();
    testSharedPreexistingReferenceFileOwnedByNeitherTaskHasNoCycle();
    testRepeatedDependencyCycleExhaustsRepairHonestly();
    testTurnBudgetPartitionForcesTerminalBoundaryBeforeRunWideLimit();
    testTurnBudgetPartitionV3CapsInspectionAndDirectsSurplusToWorkers();
    testWorkerTurnReserveRemainsAfterSuccessfulForcedTerminalSubmission();
    testInsufficientRunWideTurnBudgetFailsBeforeProviderDispatch();
    testInvalidSubmissionOnForcedTerminalTurnStillFailsClosedWithinRecoveryReserve();
    testBothRecoveryKindsConsumeExactReserveAndLeaveWorkerFloor();
    testForcedTerminalPolicyIncludesNoChangesOnlyWhenApplicable();
    testForcedTerminalPolicyIncludesVerificationActionRequiredOnlyWhenSetupRequired();
    testMandatoryPolicyInjectsOmittedLintWithoutAnyRepair();
    testMandatoryPolicyFailureEndsPlanningWithoutRepairOrFreeze();
    testMandatoryPolicyFailureAfterARepairKeepsTheTypedCode();
    testNoVerificationEnvironmentMeansThePolicyIsNotEvaluated();
    testProviderCanNeverSubmitARequirementOrigin();
    testTrustedRoutingIsStampedOnTheFrozenManifest();
    testInvalidTrustedRoutingFailsBeforeAnyProviderSession();
    testWithoutTrustedRoutingTheManifestCarriesNone();
    testRosterPlanningShowsTheCatalogAndRequiresAnAssignee();
    testSingleAssignmentPlanningIsUnchangedByTheRosterContract();
    testRosterTaskWithoutAnAssigneeIsRepairedOnce();
    testRosterRejectsLeadUnknownAndNonWorkspaceAssignees();
    testRepairChangingAnUnrelatedTasksAssigneeIsARegression();
    testRosterCatalogDefectsFailBeforeAnyProviderSession();
    testSingleAssignmentAcceptsOnlyItsOwnMemberAsExplicitAssignee();
    testProviderCanNeverSubmitARoutingRecord();
    testNonRepairableClassifierCoversOnlyPolicyCodes();
    testCapacityRefusalIsRepairedBeforeTheManifestIsFrozen();
    testCapacityRefusalThatTheRepairDoesNotFixNeverFreezes();
    testAutomaticLimitAcceptsWhatPinnedLimitRefuses();
    testTaskAboveTheBackendCeilingIsNamedAndRepaired();
    testRetryReservationsCountTowardsThePlannerCapacityCheck();
    testCapacityIsNotJudgedWithoutASharedGuard();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
