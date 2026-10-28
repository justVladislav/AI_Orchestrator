// Tests for ExecutionProfileWorkflowRunner's workspaceAccess branch: an
// individual roster agent runs through the shared AgentToolTurnLoopRunner
// (read-only workspace tools + propose_change_set) instead of a single-shot
// completion, alongside ordinary non-workspace agents in the same parallel
// dependency graph. The concurrency tests here are the direct proof the
// user asked for: the Architect must submit a validated, non-overlapping
// execution manifest before fan-out; each worker receives only its allocation
// and ProjectWritePolicy.allowedRoots hard-rejects foreign paths before the
// RunPathClaimRegistry race guard. A manifest worker also receives a compact,
// self-contained handoff rather than the Architect's full project context.
#include "ai/application/agents/AgentToolExecutionService.hpp"
#include "ai/application/proposals/ChangeProposalService.hpp"
#include "ai/application/execution/ExecutionAssignmentValidation.hpp"
#include "ai/application/execution/ExecutionManifestService.hpp"
#include "ai/application/execution/ExecutionProfileWorkflowRunner.hpp"
#include "ai/application/workspace/FilesystemWorkspaceToolExecutor.hpp"
#include "ai/application/InMemoryExecutionManifestRepository.hpp"
#include "ai/application/InMemoryRepositories.hpp"
#include "ai/application/execution/MockWorkflowRunner.hpp"
#include "ai/application/agents/ModelExecutionService.hpp"
#include "ai/application/execution/OrchestrationService.hpp"
#include "ai/application/projects/ProjectService.hpp"
#include "ai/application/catalog/ProviderCatalogService.hpp"
#include "ai/application/execution/RunPathClaimRegistry.hpp"
#include "ai/application/execution/WorkflowRouter.hpp"
#include "ai/infrastructure/FileChangeProposalStore.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace common = ai::common;
using namespace ai::application;
using namespace ai::providers;
using namespace ai::infrastructure;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------
// Fixtures / fakes
// ---------------------------------------------------------------------

struct TempDir {
    fs::path path;

    TempDir()
    {
        path = fs::temp_directory_path()
            / ("ai-orchestrator-workspace-graph-test-"
                + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }

    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Single-shot completion client for the non-workspace lead architect / the
// synthesis stage (which reuses the architect's provider/model).
class FakeProviderClient final : public IProviderClient {
public:
    std::string content {"ARCHITECT_OUTPUT"};
    std::atomic<int> callCount {0};
    std::vector<std::string> scriptedContents;
    std::string firstUserContent;

    common::Result<ProviderChatResponse> send(
        const ProviderChatRequest& request, std::stop_token) override
    {
        const int callIndex = callCount.fetch_add(1);
        if (callIndex == 0) firstUserContent = request.userContent;
        ProviderChatResponse response;
        response.content = callIndex >= 0
                && static_cast<std::size_t>(callIndex) < scriptedContents.size()
            ? scriptedContents[static_cast<std::size_t>(callIndex)] : content;
        response.usage.inputTokens = 7;
        response.usage.outputTokens = 5;
        return common::Result<ProviderChatResponse>::ok(std::move(response));
    }
};

class FakeProviderClientFactory final : public IProviderClientFactory {
public:
    void registerClient(ProviderId providerId, IProviderClient& client)
    {
        clients_[std::move(providerId)] = &client;
    }

    common::Result<std::shared_ptr<IProviderClient>> clientFor(
        const ProviderId& providerId, const std::string&) override
    {
        const auto it = clients_.find(providerId);
        if (it == clients_.end()) {
            return common::Result<std::shared_ptr<IProviderClient>>::failure(
                {"provider.unsupported_kind", "No fake client registered for this provider"});
        }
        return common::Result<std::shared_ptr<IProviderClient>>::ok(
            std::shared_ptr<IProviderClient>(it->second, [](IProviderClient*) {}));
    }

private:
    std::unordered_map<ProviderId, IProviderClient*> clients_;
};

class AlwaysSucceedExecutor final : public IWorkspaceToolExecutor {
public:
    common::Result<AgentToolResult> execute(
        const AgentToolCall& call, const WorkspaceReadPolicy&) override
    {
        AgentToolResult r;
        r.callId = call.callId;
        r.toolName = call.toolName;
        r.success = true;
        r.contentJson = R"({"entries":["src","README.md"]})";
        r.returnedBytes = r.contentJson.size();
        return common::Result<AgentToolResult>::ok(std::move(r));
    }
};

// Same shape as ChangeProposalTests.cpp's fake, extended with a working
// listForRun() - needed here since the test verifies terminal proposal state
// for changesets whose ids are generated internally by the runner, not
// visible to the test any other way.
class FakeChangeProposalRepository final : public IChangeProposalRepository {
public:
    common::Result<void> insertChangeSet(const ProposedChangeSet& cs) override
    {
        std::lock_guard lock(mu_);
        if (byId_.count(cs.changeSetId))
            return common::Result<void>::failure({"proposal.duplicate_id", "Duplicate changeSetId"});
        byId_[cs.changeSetId] = cs;
        return common::Result<void>::ok();
    }
    common::Result<ProposedChangeSet> updateChangeSetState(
        const ChangeSetId& id, ChangeSetState newState) override
    {
        std::lock_guard lock(mu_);
        const auto it = byId_.find(id);
        if (it == byId_.end())
            return common::Result<ProposedChangeSet>::failure({"proposal.not_found", "Change set not found"});
        it->second.state = newState;
        return common::Result<ProposedChangeSet>::ok(it->second);
    }
    common::Result<void> markMissingEvidence(
        const ChangeSetId& id, std::string code, std::string message) override
    {
        std::lock_guard lock(mu_);
        const auto it = byId_.find(id);
        if (it == byId_.end())
            return common::Result<void>::failure({"proposal.not_found", "Change set not found"});
        it->second.missingEvidenceCode = std::move(code);
        it->second.missingEvidenceMessage = std::move(message);
        return common::Result<void>::ok();
    }
    common::Result<std::optional<ProposedChangeSet>> findChangeSet(const ChangeSetId& id) const override
    {
        std::lock_guard lock(mu_);
        const auto it = byId_.find(id);
        if (it == byId_.end()) return common::Result<std::optional<ProposedChangeSet>>::ok(std::nullopt);
        return common::Result<std::optional<ProposedChangeSet>>::ok(it->second);
    }
    common::Result<std::vector<ProposedChangeSet>> listForRun(const RunId& runId) const override
    {
        std::lock_guard lock(mu_);
        std::vector<ProposedChangeSet> result;
        for (const auto& [id, cs] : byId_) {
            if (cs.runId == runId) result.push_back(cs);
        }
        return common::Result<std::vector<ProposedChangeSet>>::ok(std::move(result));
    }
    common::Result<std::vector<ProposedChangeSet>> listForProject(const ProjectId&) const override
    {
        return common::Result<std::vector<ProposedChangeSet>>::ok({});
    }
    common::Result<std::vector<ProposedFileChange>> listChanges(const ChangeSetId&) const override
    {
        return common::Result<std::vector<ProposedFileChange>>::ok({});
    }
    common::Result<std::vector<ValidationFinding>> listValidationFindings(const ChangeSetId&) const override
    {
        return common::Result<std::vector<ValidationFinding>>::ok({});
    }
    common::Result<std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>>>
    listChangesForSets(const std::vector<ChangeSetId>&) const override
    {
        return common::Result<std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>>>::ok({});
    }
    common::Result<std::unordered_map<ChangeSetId, std::vector<ValidationFinding>>>
    listValidationFindingsForSets(const std::vector<ChangeSetId>&) const override
    {
        return common::Result<std::unordered_map<ChangeSetId, std::vector<ValidationFinding>>>::ok({});
    }

private:
    mutable std::mutex mu_;
    std::map<ChangeSetId, ProposedChangeSet> byId_;
};

// Scripted IProviderAgentSession performs one successful workspace inspection
// before propose_change_set. This keeps every successful/Rejected fixture
// evidence-backed and remains valid when the runner rejects zero-tool empty
// proposals fail-closed.
class ScriptedProviderAgentSession final : public IProviderAgentSession {
public:
    std::string proposeChangeSetArgumentsJson;
    // Test-only introspection: when set, records the tool names actually
    // present on the ProviderAgentSessionRequest the runner built for this
    // agent. Catches a real regression where ExecutionProfileWorkflowRunner
    // only advertised propose_change_set and silently dropped the read-only
    // workspace tools its own system prompt promises the model.
    std::vector<std::string>* capturedToolNames {nullptr};
    std::string* capturedUserContent {nullptr};
    // AG-17 M2b: optional per-turn usage (otherwise 8 in / 4 out) so a test can script a mixed
    // short/long session, and an optional action that runs inside the started session (a catalog
    // edit while the attempt is in flight).
    std::optional<ai::models::TokenUsage> firstTurnUsage;
    std::optional<ai::models::TokenUsage> secondTurnUsage;
    std::function<void()> duringSession;

    common::Result<ProviderAgentTurnResponse> beginSession(
        const ProviderAgentSessionRequest& request, std::stop_token) override
    {
        if (capturedToolNames != nullptr) {
            for (const auto& tool : request.tools) capturedToolNames->push_back(tool.name);
        }
        if (capturedUserContent != nullptr) *capturedUserContent = request.userContent;
        if (duringSession) duringSession();
        ProviderAgentTurnResponse resp;
        resp.turnIndex = 0;
        resp.finishReason = ProviderAgentFinishReason::ToolUse;
        AgentToolCall call;
        call.callId = "inspect-1";
        call.toolName = "workspace_list_directory";
        call.argumentsJson = R"({"path":"."})";
        call.turnIndex = 0;
        resp.toolCalls = {call};
        resp.usage.inputTokens = 8;
        resp.usage.outputTokens = 4;
        if (firstTurnUsage.has_value()) resp.usage = *firstTurnUsage;
        return common::Result<ProviderAgentTurnResponse>::ok(resp);
    }

    common::Result<ProviderAgentTurnResponse> submitToolResults(
        std::vector<AgentToolResult> results, std::stop_token,
        std::optional<NextTurnToolPolicy> = std::nullopt) override
    {
        if (results.size() != 1 || !results.front().success
                || results.front().callId != "inspect-1") {
            return common::Result<ProviderAgentTurnResponse>::failure(
                {"test.invalid_tool_result", "Expected one successful inspection result"});
        }
        ProviderAgentTurnResponse resp;
        resp.turnIndex = 1;
        resp.finishReason = ProviderAgentFinishReason::ToolUse;
        AgentToolCall call;
        call.callId = "proposal-1";
        call.toolName = kProposeChangeSetToolName;
        call.argumentsJson = proposeChangeSetArgumentsJson;
        call.turnIndex = 1;
        resp.toolCalls = {call};
        resp.usage.inputTokens = 8;
        resp.usage.outputTokens = 4;
        if (secondTurnUsage.has_value()) resp.usage = *secondTurnUsage;
        return common::Result<ProviderAgentTurnResponse>::ok(std::move(resp));
    }
};

// Thread-safe, keyed by providerId (each workspaceAccess agent in these
// tests is given its own distinct provider) so two agents' genuinely
// parallel std::async tasks can each call createSession() concurrently
// without racing a single shared queue.
class ScriptedProviderAgentSessionFactory final : public IProviderAgentSessionFactory {
public:
    void prepareSessionFor(const ProviderId& providerId, std::string argumentsJson)
    {
        std::lock_guard lock(mutex_);
        scriptByProvider_[providerId] = std::move(argumentsJson);
    }

    // toolNamesOut must outlive this factory's created sessions (a local in
    // the calling test, kept alive for the duration of runner.run()).
    void captureToolNamesFor(const ProviderId& providerId, std::vector<std::string>* toolNamesOut)
    {
        std::lock_guard lock(mutex_);
        toolCaptureByProvider_[providerId] = toolNamesOut;
    }

    void captureUserContentFor(const ProviderId& providerId, std::string* contentOut)
    {
        std::lock_guard lock(mutex_);
        userContentCaptureByProvider_[providerId] = contentOut;
    }

    // AG-17 M2b: the usage of the two scripted turns of this provider's sessions.
    void prepareUsageFor(const ProviderId& providerId,
        const ai::models::TokenUsage& firstTurn, const ai::models::TokenUsage& secondTurn)
    {
        std::lock_guard lock(mutex_);
        usageByProvider_[providerId] = {firstTurn, secondTurn};
    }

    // Runs inside this provider's started session (before its first response).
    void duringSessionFor(const ProviderId& providerId, std::function<void()> action)
    {
        std::lock_guard lock(mutex_);
        duringSessionByProvider_[providerId] = std::move(action);
    }

    [[nodiscard]] int createCallsFor(const ProviderId& providerId)
    {
        std::lock_guard lock(mutex_);
        return createCalls_[providerId];
    }

    common::Result<std::unique_ptr<IProviderAgentSession>> createSession(
        const ProviderId& providerId, const std::string&) override
    {
        std::lock_guard lock(mutex_);
        ++createCalls_[providerId];
        const auto it = scriptByProvider_.find(providerId);
        if (it == scriptByProvider_.end()) {
            return common::Result<std::unique_ptr<IProviderAgentSession>>::failure(
                {"test.no_script", "No scripted propose_change_set response prepared for this provider"});
        }
        auto session = std::make_unique<ScriptedProviderAgentSession>();
        session->proposeChangeSetArgumentsJson = it->second;
        const auto captureIt = toolCaptureByProvider_.find(providerId);
        if (captureIt != toolCaptureByProvider_.end()) {
            session->capturedToolNames = captureIt->second;
        }
        const auto userContentCapture = userContentCaptureByProvider_.find(providerId);
        if (userContentCapture != userContentCaptureByProvider_.end()) {
            session->capturedUserContent = userContentCapture->second;
        }
        const auto usage = usageByProvider_.find(providerId);
        if (usage != usageByProvider_.end()) {
            session->firstTurnUsage = usage->second.first;
            session->secondTurnUsage = usage->second.second;
        }
        const auto during = duringSessionByProvider_.find(providerId);
        if (during != duringSessionByProvider_.end()) {
            session->duringSession = during->second;
        }
        return common::Result<std::unique_ptr<IProviderAgentSession>>::ok(std::move(session));
    }

private:
    std::mutex mutex_;
    std::map<ProviderId, std::string> scriptByProvider_;
    std::map<ProviderId, int> createCalls_;
    std::map<ProviderId, std::vector<std::string>*> toolCaptureByProvider_;
    std::map<ProviderId, std::string*> userContentCaptureByProvider_;
    std::map<ProviderId, std::pair<ai::models::TokenUsage, ai::models::TokenUsage>> usageByProvider_;
    std::map<ProviderId, std::function<void()>> duringSessionByProvider_;
};

// Phase 6 needs several distinct scripts per provider because the production
// turn loop creates a fresh provider session for its one bounded repair.
class IncidentProviderAgentSession final : public IProviderAgentSession {
public:
    std::queue<common::Result<ProviderAgentTurnResponse>> responses;

    common::Result<ProviderAgentTurnResponse> beginSession(
        const ProviderAgentSessionRequest&, std::stop_token) override
    {
        return next();
    }

    common::Result<ProviderAgentTurnResponse> submitToolResults(
        std::vector<AgentToolResult>, std::stop_token,
        std::optional<NextTurnToolPolicy> = std::nullopt) override
    {
        return next();
    }

private:
    common::Result<ProviderAgentTurnResponse> next()
    {
        if (responses.empty()) {
            return common::Result<ProviderAgentTurnResponse>::failure(
                {"test.no_more_responses", "Incident session exhausted its script"});
        }
        auto response = std::move(responses.front());
        responses.pop();
        return response;
    }
};

class IncidentProviderAgentSessionFactory final : public IProviderAgentSessionFactory {
public:
    void prepare(const ProviderId& providerId, std::unique_ptr<IncidentProviderAgentSession> session)
    {
        std::lock_guard lock(mutex_);
        sessions_[providerId].push(std::move(session));
    }

    [[nodiscard]] int createCallsFor(const ProviderId& providerId)
    {
        std::lock_guard lock(mutex_);
        return createCalls_[providerId];
    }

    common::Result<std::unique_ptr<IProviderAgentSession>> createSession(
        const ProviderId& providerId, const std::string&) override
    {
        std::lock_guard lock(mutex_);
        ++createCalls_[providerId];
        auto found = sessions_.find(providerId);
        if (found == sessions_.end() || found->second.empty()) {
            return common::Result<std::unique_ptr<IProviderAgentSession>>::failure(
                {"test.no_incident_script", "No incident script prepared for provider"});
        }
        auto session = std::move(found->second.front());
        found->second.pop();
        return common::Result<std::unique_ptr<IProviderAgentSession>>::ok(std::move(session));
    }

private:
    std::mutex mutex_;
    std::map<ProviderId, std::queue<std::unique_ptr<IncidentProviderAgentSession>>> sessions_;
    std::map<ProviderId, int> createCalls_;
};

class NullEventSink final : public IRunEventSink {
public:
    void publish(const RunEvent&) override {}
};

// ---------------------------------------------------------------------
// Common fixture wiring (mirrors WorkflowRoutingAndProductionTests.cpp's
// Catalog/makeProvider/makeModel helpers)
// ---------------------------------------------------------------------

struct Catalog {
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    ProviderCatalogService catalog {providers, models};
};

Provider makeProvider(Catalog& catalog, const char* name)
{
    const auto created = catalog.catalog.createProvider(
        {name, "openai", "https://example.invalid", true, std::nullopt});
    assert(created.success());
    return *catalog.catalog.findProvider(created.value()).value();
}

Model makeModel(Catalog& catalog, const ProviderId& providerId, const char* apiModelId)
{
    const auto created = catalog.catalog.createModel(
        {providerId, apiModelId, apiModelId, 128000, 8192, 1000000, 3000000, 0, 0, true});
    assert(created.success());
    return *catalog.catalog.findModel(created.value()).value();
}

struct WorkspaceFixture {
    TempDir tempDir;
    TempDir workspaceDir;
    Catalog catalog;
    InMemoryAgentToolSessionRepository toolSessions;
    InMemoryAgentToolAuditRepository toolAudits;
    AlwaysSucceedExecutor executor;
    ScriptedProviderAgentSessionFactory sessionFactory;
    FakeChangeProposalRepository changeRepo;
    std::unique_ptr<FileChangeProposalStore> changeStore;
    RunPathClaimRegistry pathClaimRegistry;

    WorkspaceFixture()
    {
        auto storeResult = FileChangeProposalStore::open(tempDir.path / "proposals");
        assert(storeResult.success());
        changeStore = std::move(storeResult.value());
    }

    AgentToolExecutionService makeToolExecution()
    {
        auto registry = ToolRegistry::createDefaultReadOnly();
        assert(registry.success());
        return AgentToolExecutionService(
            std::move(registry.value()), executor, toolSessions, toolAudits, true);
    }

    ChangeProposalService makeChangeProposals(bool withRegistry = true)
    {
        return ChangeProposalService(changeRepo, *changeStore, withRegistry ? &pathClaimRegistry : nullptr);
    }
};

Project makeProject(const fs::path& root)
{
    Project project;
    project.id = "workspace-graph-project";
    project.name = "Workspace graph project";
    project.rootPath = root;
    return project;
}

const char* kCreateNotesArgs =
    R"({"schemaVersion":"proposal-submission.v2","summary":"Add notes.","changes":[{)"
    R"("change_id":"create-notes","operation":"create",)"
    R"("relative_path":"notes.md","source_path":null,"proposed_content":"notes"}]})";

std::string createFileArgs(const char* changeId, const char* relativePath)
{
    return std::string(R"({"schemaVersion":"proposal-submission.v2","summary":"Create a file.","changes":[{)")
        + R"("change_id":")" + changeId + R"(","operation":"create",)"
        + R"("relative_path":")" + relativePath + R"(","source_path":null,"proposed_content":"content"}]})";
}

struct ArchitectTaskSpec {
    std::string id;
    std::vector<std::string> ownedPaths;
    std::vector<std::string> dependencies;
};

std::string architectManifest(
    std::vector<ArchitectTaskSpec> specs,
    std::uint32_t requestedMaxParallelism)
{
    nlohmann::json tasks = nlohmann::json::array();
    for (const auto& spec : specs) {
        tasks.push_back({
            {"id", spec.id},
            {"objective", "Complete only the allocated work for " + spec.id},
            {"role", spec.id},
            {"dependencies", spec.dependencies},
            {"ownedPaths", spec.ownedPaths},
            {"plannedCreatePaths", nlohmann::json::array()},
            {"requiredReadPaths", nlohmann::json::array()},
            {"referencePaths", nlohmann::json::array()},
            {"excludedPaths", nlohmann::json::array()},
            {"deferredPaths", nlohmann::json::array()},
            {"acceptanceCriteria", {"Only owned paths are present in the proposal"}},
            {"contextReferences", nlohmann::json::array()},
            {"requestedCapability", "propose"}
        });
    }
    return nlohmann::json {
        {"schemaVersion", "execution-manifest.v1"},
        {"tasks", std::move(tasks)},
        {"requestedMaxParallelism", requestedMaxParallelism}
    }.dump();
}

ProviderAgentTurnResponse incidentToolCall(
    std::size_t turn, std::string callId, std::string toolName, std::string arguments)
{
    ProviderAgentTurnResponse response;
    response.turnIndex = turn;
    response.finishReason = ProviderAgentFinishReason::ToolUse;
    AgentToolCall call;
    call.callId = std::move(callId);
    call.toolName = std::move(toolName);
    call.argumentsJson = std::move(arguments);
    call.turnIndex = turn;
    response.toolCalls = {std::move(call)};
    response.usage.inputTokens = 8;
    response.usage.outputTokens = 4;
    return response;
}

std::unique_ptr<IncidentProviderAgentSession> incidentSession(
    std::vector<ProviderAgentTurnResponse> responses)
{
    auto session = std::make_unique<IncidentProviderAgentSession>();
    for (auto& response : responses) {
        session->responses.push(
            common::Result<ProviderAgentTurnResponse>::ok(std::move(response)));
    }
    return session;
}

// ---------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------

void testWorkspaceAccessAgentProducesProposal()
{
    WorkspaceFixture fx;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();

    const Provider architectProvider = makeProvider(fx.catalog, "Architect Provider");
    const Model architectModel = makeModel(fx.catalog, architectProvider.id, "architect-model");
    FakeProviderClient architectClient;
    architectClient.content = architectManifest({{"worker", {"notes.md"}, {}}}, 2);
    FakeProviderClientFactory clientFactory;
    clientFactory.registerClient(architectProvider.id, architectClient);

    const Provider workerProvider = makeProvider(fx.catalog, "Worker Provider");
    const Model workerModel = makeModel(fx.catalog, workerProvider.id, "worker-model");
    fx.sessionFactory.prepareSessionFor(workerProvider.id, kCreateNotesArgs);

    ExecutionProfileWorkflowRunner runner(
        clientFactory, fx.catalog.providers, fx.catalog.models,
        nullptr, nullptr, nullptr,
        &fx.sessionFactory, &toolExecution, &changeProposals);

    StageModelAssignment architect {"architect", architectProvider.id, architectModel.id, 5000ms};
    architect.leadArchitect = true;
    StageModelAssignment worker {"worker", workerProvider.id, workerModel.id, 5000ms};
    worker.workspaceAccess = true;

    ResolvedExecutionPlan plan;
    plan.workflowId = kExecutionProfileWorkflowId;
    plan.assignments = {architect, worker};
    plan.maxParallelism = 2;

    Run run;
    run.id = "workspace-run-1";
    run.projectId = "workspace-graph-project";
    run.title = "Workspace agent run";
    run.task = "Exercise a single workspaceAccess agent";
    run.workflowId = kExecutionProfileWorkflowId;
    run.assignmentSnapshot = plan.assignments;
    run.maxParallelism = 2;
    run.workspacePath = fx.workspaceDir.path;
    run.status = RunStatus::Running;

    const Project project = makeProject(fx.workspaceDir.path);
    const auto stages = runner.createStages(run.id, plan);
    const WorkflowRunContext context {run, project, stages};
    NullEventSink sink;

    const WorkflowResult result = runner.run(context, std::stop_token {}, sink);
    assert(result.success);
    assert(result.finalStatus == RunStatus::Completed);
    assert(architectClient.callCount == 1
        && "Proposal-only graphs must not spend a second Architect call on synthesis");
    assert(result.finalOutput.find("provider_synthesis: skipped") != std::string::npos);
    assert(result.finalOutput.find("state: ready_for_review") != std::string::npos);
    assert(result.finalOutput.find("notes.md") != std::string::npos);

    const auto proposals = fx.changeRepo.listForRun(run.id);
    assert(proposals.success() && proposals.value().size() == 1);
    assert(proposals.value().front().state == ChangeSetState::ReadyForReview);
}

// Regression test for a real bug: executeWorkspaceTask never recorded a
// model execution, unlike the plain single-shot path. A run made entirely of
// workspaceAccess agents (the actual "propose changes" production workflow)
// therefore showed zero cost on Overview/Comparison, no matter how the
// provider's models were priced in the catalog, because nothing ever reached
// the model_executions ledger those read from.
void testWorkspaceAccessAgentRecordsModelExecution()
{
    WorkspaceFixture fx;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    InMemoryModelExecutionRepository executionRepository;
    ModelExecutionService executionService {executionRepository};

    const Provider architectProvider = makeProvider(fx.catalog, "Architect Provider");
    const Model architectModel = makeModel(fx.catalog, architectProvider.id, "architect-model");
    FakeProviderClient architectClient;
    architectClient.content = architectManifest({{"worker", {"notes.md"}, {}}}, 2);
    FakeProviderClientFactory clientFactory;
    clientFactory.registerClient(architectProvider.id, architectClient);

    const Provider workerProvider = makeProvider(fx.catalog, "Worker Provider");
    const Model workerModel = makeModel(fx.catalog, workerProvider.id, "worker-model");
    fx.sessionFactory.prepareSessionFor(workerProvider.id, kCreateNotesArgs);

    ExecutionProfileWorkflowRunner runner(
        clientFactory, fx.catalog.providers, fx.catalog.models,
        nullptr, &executionService, nullptr,
        &fx.sessionFactory, &toolExecution, &changeProposals);

    StageModelAssignment architect {"architect", architectProvider.id, architectModel.id, 5000ms};
    architect.leadArchitect = true;
    architect.position = 0;
    StageModelAssignment worker {"worker", workerProvider.id, workerModel.id, 5000ms};
    worker.workspaceAccess = true;
    worker.position = 1;

    ResolvedExecutionPlan plan;
    plan.workflowId = kExecutionProfileWorkflowId;
    plan.assignments = {architect, worker};
    plan.maxParallelism = 2;

    Run run;
    run.id = "workspace-run-cost";
    run.projectId = "workspace-graph-project";
    run.title = "Workspace agent cost run";
    run.task = "Exercise model-execution recording for a workspaceAccess agent";
    run.workflowId = kExecutionProfileWorkflowId;
    run.assignmentSnapshot = plan.assignments;
    run.maxParallelism = 2;
    run.workspacePath = fx.workspaceDir.path;
    run.status = RunStatus::Running;

    const Project project = makeProject(fx.workspaceDir.path);
    const auto stages = runner.createStages(run.id, plan);
    const WorkflowRunContext context {run, project, stages};
    NullEventSink sink;

    const WorkflowResult result = runner.run(context, std::stop_token {}, sink);
    assert(result.success);
    assert(result.finalStatus == RunStatus::Completed);

    const auto attempts = executionService.listExecutions(ModelExecutionSource::Run, run.id);
    assert(attempts.success());
    const auto workspaceAttempt = std::find_if(
        attempts.value().begin(), attempts.value().end(),
        [](const auto& attempt) { return attempt.role == "worker"; });
    assert(workspaceAttempt != attempts.value().end()
        && "A workspaceAccess agent's stage must be recorded in the model-execution ledger");
    assert(workspaceAttempt->providerId == workerProvider.id);
    assert(workspaceAttempt->modelId == workerModel.id);
    assert(workspaceAttempt->status == ModelExecutionStatus::Completed);
    assert(workspaceAttempt->projectId.has_value() && *workspaceAttempt->projectId == project.id);
    assert(workspaceAttempt->costMicros > 0
        && "Real usage against a priced model must produce a non-zero recorded cost");
}

// AG-17 gpt-6-sol checkpoint M2b-2a: a workspaceAccess agent's execution (the legacy agent-session
// loop) is priced per RESPONSE, from the ONE Model copy its attempt looked up.
//   flat : $2 input / $0.20 cache-read / $2.50 cache-write / $10 output  (per million tokens)
//   long : $4 input / $0.40 cache-read / $5    cache-write / $15 output  above 272,000 input tokens
//   turn 1 (short): 100k in, 2k out, 10k write, 60k read = 117,000 micros flat
//   turn 2 (long) : 300k in, 1k out, 50k write, 200k read = 545,000 micros long
void testLegacyWorkspaceAgentIsPricedPerResponseFromTheModelCopyOfTheAttempt()
{
    WorkspaceFixture fx;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    InMemoryModelExecutionRepository executionRepository;
    ModelExecutionService executionService {executionRepository};

    const Provider architectProvider = makeProvider(fx.catalog, "Architect Provider");
    const Model architectModel = makeModel(fx.catalog, architectProvider.id, "architect-model");
    FakeProviderClient architectClient;
    architectClient.content = architectManifest({{"worker", {"notes.md"}, {}}}, 2);
    FakeProviderClientFactory clientFactory;
    clientFactory.registerClient(architectProvider.id, architectClient);

    const Provider workerProvider = makeProvider(fx.catalog, "Worker Provider");
    const Model workerModel = makeModel(fx.catalog, workerProvider.id, "worker-model");
    fx.sessionFactory.prepareSessionFor(workerProvider.id, kCreateNotesArgs);
    ai::models::TokenUsage shortTurn;
    shortTurn.inputTokens = 100'000;
    shortTurn.outputTokens = 2'000;
    shortTurn.cacheCreationInputTokens = 10'000;
    shortTurn.cacheReadInputTokens = 60'000;
    ai::models::TokenUsage longTurn;
    longTurn.inputTokens = 300'000;
    longTurn.outputTokens = 1'000;
    longTurn.cacheCreationInputTokens = 50'000;
    longTurn.cacheReadInputTokens = 200'000;
    fx.sessionFactory.prepareUsageFor(workerProvider.id, shortTurn, longTurn);

    const auto setPricing = [&](std::uint64_t input, std::uint64_t output, std::uint64_t cacheWrite,
                                 std::uint64_t cacheRead, const LongContextPricing& tier) {
        const auto updated = fx.catalog.catalog.updateModel({workerModel.id, workerModel.apiModelId,
            workerModel.displayName, 1'050'000, 128'000, input, output, cacheWrite, cacheRead, tier});
        assert(updated.success());
    };
    setPricing(2'000'000, 10'000'000, 2'500'000, 200'000,
        LongContextPricing {272'000, 4'000'000, 15'000'000, 5'000'000, 400'000});

    ExecutionProfileWorkflowRunner runner(
        clientFactory, fx.catalog.providers, fx.catalog.models,
        nullptr, &executionService, nullptr,
        &fx.sessionFactory, &toolExecution, &changeProposals);

    StageModelAssignment architect {"architect", architectProvider.id, architectModel.id, 5000ms};
    architect.leadArchitect = true;
    architect.position = 0;
    StageModelAssignment worker {"worker", workerProvider.id, workerModel.id, 5000ms};
    worker.workspaceAccess = true;
    worker.position = 1;
    ResolvedExecutionPlan plan;
    plan.workflowId = kExecutionProfileWorkflowId;
    plan.assignments = {architect, worker};
    plan.maxParallelism = 2;
    const Project project = makeProject(fx.workspaceDir.path);

    const auto runAndFindWorker = [&](const char* runId) {
        Run run;
        run.id = runId;
        run.projectId = project.id;
        run.title = "Tier cost run";
        run.task = "Exercise per-response pricing of a workspaceAccess agent";
        run.workflowId = kExecutionProfileWorkflowId;
        run.assignmentSnapshot = plan.assignments;
        run.maxParallelism = 2;
        run.workspacePath = fx.workspaceDir.path;
        run.status = RunStatus::Running;
        const auto stages = runner.createStages(run.id, plan);
        const WorkflowRunContext context {run, project, stages};
        NullEventSink sink;
        const WorkflowResult result = runner.run(context, std::stop_token {}, sink);
        assert(result.success && result.finalStatus == RunStatus::Completed);
        const auto attempts = executionService.listExecutions(ModelExecutionSource::Run, run.id);
        assert(attempts.success());
        const auto found = std::find_if(attempts.value().begin(), attempts.value().end(),
            [](const auto& attempt) { return attempt.role == "worker"; });
        assert(found != attempts.value().end());
        return *found;
    };

    // Run A: the tier is REMOVED and every price changed once the worker's session is running. The
    // execution in flight keeps the Model copy its attempt looked up - the original threshold AND
    // the original prices - so a mixed short+long agent is priced exactly per response.
    fx.sessionFactory.duringSessionFor(workerProvider.id, [&] {
        setPricing(5'000'000, 7'000'000, 1'000'000, 100'000, LongContextPricing {});
    });
    const ModelExecution inFlight = runAndFindWorker("legacy-tier-run-a");
    assert(inFlight.inputTokens == 400'000 && inFlight.outputTokens == 3'000);
    assert(inFlight.costMicros == 117'000 + 545'000 && inFlight.costPriceKnown);

    // Run B starts after the edit: flat, at the new prices (plain 80k*$5 + write 60k*$1 + read
    // 260k*$0.10 + out 3k*$7 = 400,000 + 60,000 + 26,000 + 21,000).
    fx.sessionFactory.duringSessionFor(workerProvider.id, {});
    const ModelExecution next = runAndFindWorker("legacy-tier-run-b");
    assert(next.costMicros == 507'000 && next.costPriceKnown);

    // Run C: a tier whose long cache-write price was never entered (the long response uses
    // cache-write): unknown, the known part kept as a lower bound.
    setPricing(2'000'000, 10'000'000, 2'500'000, 200'000,
        LongContextPricing {272'000, 4'000'000, 15'000'000, 0, 400'000});
    const ModelExecution unpriced = runAndFindWorker("legacy-tier-run-c");
    assert(!unpriced.costPriceKnown);
    assert(unpriced.costMicros == 117'000 + 545'000 - 250'000);
}

// Regression test for a real bug: ExecutionProfileWorkflowRunner used to
// build the workspace-access session request with ONLY propose_change_set
// in its tools array, silently dropping the read-only workspace tools
// (workspace_list_directory/read_file/search_text/find_files/hash_file/
// stat_path) that AgentToolExecutionService::toolDefinitions() exposes and
// that kWorkspaceToolsSystemPrompt tells the model it has. A model given
// that request genuinely had no way to inspect the project, so it correctly
// (from its own perspective) declined to propose anything instead of
// guessing blindly - observed in production as an instructed "build the
// architecture" run coming back as a no-op "no changes needed" proposal.
void testWorkspaceAccessSessionIncludesReadOnlyToolDefinitions()
{
    WorkspaceFixture fx;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();

    const Provider architectProvider = makeProvider(fx.catalog, "Architect Provider");
    const Model architectModel = makeModel(fx.catalog, architectProvider.id, "architect-model");
    FakeProviderClient architectClient;
    architectClient.content = architectManifest({{"worker", {"notes.md"}, {}}}, 2);
    FakeProviderClientFactory clientFactory;
    clientFactory.registerClient(architectProvider.id, architectClient);

    const Provider workerProvider = makeProvider(fx.catalog, "Worker Provider");
    const Model workerModel = makeModel(fx.catalog, workerProvider.id, "worker-model");
    fx.sessionFactory.prepareSessionFor(workerProvider.id, kCreateNotesArgs);
    std::vector<std::string> capturedToolNames;
    fx.sessionFactory.captureToolNamesFor(workerProvider.id, &capturedToolNames);

    ExecutionProfileWorkflowRunner runner(
        clientFactory, fx.catalog.providers, fx.catalog.models,
        nullptr, nullptr, nullptr,
        &fx.sessionFactory, &toolExecution, &changeProposals);

    StageModelAssignment architect {"architect", architectProvider.id, architectModel.id, 5000ms};
    architect.leadArchitect = true;
    StageModelAssignment worker {"worker", workerProvider.id, workerModel.id, 5000ms};
    worker.workspaceAccess = true;

    ResolvedExecutionPlan plan;
    plan.workflowId = kExecutionProfileWorkflowId;
    plan.assignments = {architect, worker};
    plan.maxParallelism = 2;

    Run run;
    run.id = "workspace-run-tools";
    run.projectId = "workspace-graph-project";
    run.title = "Workspace agent tool wiring run";
    run.task = "Exercise workspaceAccess tool wiring";
    run.workflowId = kExecutionProfileWorkflowId;
    run.assignmentSnapshot = plan.assignments;
    run.maxParallelism = 2;
    run.workspacePath = fx.workspaceDir.path;
    run.status = RunStatus::Running;

    const Project project = makeProject(fx.workspaceDir.path);
    const auto stages = runner.createStages(run.id, plan);
    const WorkflowRunContext context {run, project, stages};
    NullEventSink sink;

    const WorkflowResult result = runner.run(context, std::stop_token {}, sink);
    assert(result.success);

    for (const char* expected : {
             "workspace_list_directory", "workspace_read_file", "workspace_search_text",
             "workspace_find_files", "workspace_hash_file", "workspace_stat_path",
             kProposeChangeSetToolName}) {
        assert(std::find(capturedToolNames.begin(), capturedToolNames.end(), expected)
            != capturedToolNames.end());
    }
}

void testTwoConcurrentWorkspaceAgentsDifferentFilesBothSucceed()
{
    WorkspaceFixture fx;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifestRepository;
    ExecutionManifestService manifests {manifestRepository, runs};

    const Provider architectProvider = makeProvider(fx.catalog, "Architect Provider");
    const Model architectModel = makeModel(fx.catalog, architectProvider.id, "architect-model");
    FakeProviderClient architectClient;
    architectClient.scriptedContents = {
        "not valid JSON",
        architectManifest({
            {"agent-a", {"agent-a.txt"}, {}}, {"agent-b", {"agent-b.txt"}, {}}}, 4)};
    FakeProviderClientFactory clientFactory;
    clientFactory.registerClient(architectProvider.id, architectClient);

    const Provider providerA = makeProvider(fx.catalog, "Agent A Provider");
    const Model modelA = makeModel(fx.catalog, providerA.id, "agent-a-model");
    fx.sessionFactory.prepareSessionFor(providerA.id, createFileArgs("create-a", "agent-a.txt"));

    const Provider providerB = makeProvider(fx.catalog, "Agent B Provider");
    const Model modelB = makeModel(fx.catalog, providerB.id, "agent-b-model");
    fx.sessionFactory.prepareSessionFor(providerB.id, createFileArgs("create-b", "agent-b.txt"));
    std::string agentAContext;
    std::string agentBContext;
    fx.sessionFactory.captureUserContentFor(providerA.id, &agentAContext);
    fx.sessionFactory.captureUserContentFor(providerB.id, &agentBContext);

    ExecutionProfileWorkflowRunner runner(
        clientFactory, fx.catalog.providers, fx.catalog.models,
        nullptr, nullptr, nullptr,
        &fx.sessionFactory, &toolExecution, &changeProposals,
        &manifests);

    StageModelAssignment architect {"architect", architectProvider.id, architectModel.id, 5000ms};
    architect.leadArchitect = true;
    StageModelAssignment agentA {"agent-a", providerA.id, modelA.id, 5000ms};
    agentA.workspaceAccess = true;
    StageModelAssignment agentB {"agent-b", providerB.id, modelB.id, 5000ms};
    agentB.workspaceAccess = true;
    // Neither declares its own dependencies, so graphAssignments() defaults
    // both to depending only on the architect - landing them in the SAME
    // ready batch, dispatched via the same std::async round.

    ResolvedExecutionPlan plan;
    plan.workflowId = kExecutionProfileWorkflowId;
    plan.assignments = {architect, agentA, agentB};
    plan.maxParallelism = 4;

    Run run;
    run.id = "workspace-run-2";
    run.projectId = "workspace-graph-project";
    run.title = "Two concurrent workspace agents, different files";
    run.task = "Prove genuine parallel workspace-access execution";
    run.workflowId = kExecutionProfileWorkflowId;
    run.assignmentSnapshot = plan.assignments;
    run.maxParallelism = 4;
    run.workspacePath = fx.workspaceDir.path;
    run.status = RunStatus::Running;
    const auto insertedRun = runs.insertRun(run);
    assert(insertedRun.success());

    Project project = makeProject(fx.workspaceDir.path);
    const std::string projectDescriptionMarker =
        "PROJECT_DESCRIPTION_MUST_REACH_ARCHITECT_ONLY";
    project.description = projectDescriptionMarker + std::string(64 * 1024, 'x');
    const auto stages = runner.createStages(run.id, plan);
    const WorkflowRunContext context {run, project, stages};
    NullEventSink sink;

    const WorkflowResult result = runner.run(context, std::stop_token {}, sink);
    assert(result.success);
    assert(result.finalStatus == RunStatus::Completed);
    assert(architectClient.callCount == 2
        && "A malformed Architect manifest must receive exactly one successful repair");
    assert(architectClient.firstUserContent.find(projectDescriptionMarker)
        != std::string::npos);

    const auto proposals = fx.changeRepo.listForRun(run.id);
    assert(proposals.success() && proposals.value().size() == 2);
    for (const auto& cs : proposals.value()) {
        assert(cs.state == ChangeSetState::ReadyForReview);
    }
    assert(fx.sessionFactory.createCallsFor(providerA.id) == 1);
    assert(fx.sessionFactory.createCallsFor(providerB.id) == 1);
    assert(agentAContext.find("agent-a.txt") != std::string::npos);
    assert(agentAContext.find("agent-b.txt") == std::string::npos);
    assert(agentBContext.find("agent-b.txt") != std::string::npos);
    assert(agentBContext.find("agent-a.txt") == std::string::npos);
    for (const auto* workerContext : {&agentAContext, &agentBContext}) {
        assert(workerContext->find(projectDescriptionMarker) == std::string::npos);
        assert(workerContext->find("# Project Description") == std::string::npos);
        assert(workerContext->find("# Project Memory") == std::string::npos);
        assert(workerContext->find("# Run Task") != std::string::npos);
        assert(workerContext->find("# Current Task") != std::string::npos);
        assert(workerContext->find("Only owned paths are present in the proposal")
            != std::string::npos);
        assert(workerContext->size() < 4096
            && "Manifest worker context must remain bounded independently of project description");
    }
    const auto persistedManifest = manifests.findForRun(run.id);
    assert(persistedManifest.success() && persistedManifest.value().has_value());
    assert(persistedManifest.value()->tasks.size() == 2);
    const auto persistedAgentA = std::find_if(
        persistedManifest.value()->tasks.begin(), persistedManifest.value()->tasks.end(),
        [](const auto& task) { return task.id == "agent-a"; });
    assert(persistedAgentA != persistedManifest.value()->tasks.end());
    assert(persistedAgentA->ownedPaths == std::vector<fs::path> {"agent-a.txt"});
}

void testOverlappingArchitectOwnershipStopsBeforeWorkers()
{
    WorkspaceFixture fx;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();

    const Provider architectProvider = makeProvider(fx.catalog, "Architect Provider");
    const Model architectModel = makeModel(fx.catalog, architectProvider.id, "architect-model");
    FakeProviderClient architectClient;
    architectClient.content = architectManifest({
        {"agent-a", {"shared.txt"}, {}}, {"agent-b", {"shared.txt"}, {}}}, 4);
    FakeProviderClientFactory clientFactory;
    clientFactory.registerClient(architectProvider.id, architectClient);

    const Provider providerA = makeProvider(fx.catalog, "Agent A Provider");
    const Model modelA = makeModel(fx.catalog, providerA.id, "agent-a-model");
    fx.sessionFactory.prepareSessionFor(providerA.id, createFileArgs("create-a", "shared.txt"));

    const Provider providerB = makeProvider(fx.catalog, "Agent B Provider");
    const Model modelB = makeModel(fx.catalog, providerB.id, "agent-b-model");
    fx.sessionFactory.prepareSessionFor(providerB.id, createFileArgs("create-b", "shared.txt"));

    ExecutionProfileWorkflowRunner runner(
        clientFactory, fx.catalog.providers, fx.catalog.models,
        nullptr, nullptr, nullptr,
        &fx.sessionFactory, &toolExecution, &changeProposals);

    StageModelAssignment architect {"architect", architectProvider.id, architectModel.id, 5000ms};
    architect.leadArchitect = true;
    StageModelAssignment agentA {"agent-a", providerA.id, modelA.id, 5000ms};
    agentA.workspaceAccess = true;
    StageModelAssignment agentB {"agent-b", providerB.id, modelB.id, 5000ms};
    agentB.workspaceAccess = true;

    ResolvedExecutionPlan plan;
    plan.workflowId = kExecutionProfileWorkflowId;
    plan.assignments = {architect, agentA, agentB};
    plan.maxParallelism = 4;

    Run run;
    run.id = "workspace-run-3";
    run.projectId = "workspace-graph-project";
    run.title = "Two concurrent workspace agents, same file";
    run.task = "Reject overlapping Architect ownership before worker execution";
    run.workflowId = kExecutionProfileWorkflowId;
    run.assignmentSnapshot = plan.assignments;
    run.maxParallelism = 4;
    run.workspacePath = fx.workspaceDir.path;
    run.status = RunStatus::Running;

    const Project project = makeProject(fx.workspaceDir.path);
    const auto stages = runner.createStages(run.id, plan);
    const WorkflowRunContext context {run, project, stages};
    NullEventSink sink;

    // Overlapping ownership is rejected before the ready worker batch is
    // formed, so no worker session, proposal, or path claim can be created.
    const WorkflowResult result = runner.run(context, std::stop_token {}, sink);
    assert(!result.success);
    assert(result.finalStatus == RunStatus::Failed);
    assert(architectClient.callCount == 2
        && "One bounded Architect manifest repair must be exhausted before failing");
    assert(result.message.find("manifest.ownership_overlap") != std::string::npos);
    assert(fx.sessionFactory.createCallsFor(providerA.id) == 0);
    assert(fx.sessionFactory.createCallsFor(providerB.id) == 0);

    const auto proposals = fx.changeRepo.listForRun(run.id);
    assert(proposals.success() && proposals.value().empty());
}

void testWorkspaceAccessAgentFailsClosedWhenNotWired()
{
    Catalog catalog;
    const Provider architectProvider = makeProvider(catalog, "Architect Provider");
    const Model architectModel = makeModel(catalog, architectProvider.id, "architect-model");
    FakeProviderClient architectClient;
    architectClient.content = architectManifest({{"worker", {"notes.md"}, {}}}, 2);
    FakeProviderClientFactory clientFactory;
    clientFactory.registerClient(architectProvider.id, architectClient);

    const Provider workerProvider = makeProvider(catalog, "Worker Provider");
    const Model workerModel = makeModel(catalog, workerProvider.id, "worker-model");

    // Legacy 6-arg construction: no session factory/tool execution/change
    // proposals wired at all.
    ExecutionProfileWorkflowRunner runner(clientFactory, catalog.providers, catalog.models);

    StageModelAssignment architect {"architect", architectProvider.id, architectModel.id, 5000ms};
    architect.leadArchitect = true;
    StageModelAssignment worker {"worker", workerProvider.id, workerModel.id, 5000ms};
    worker.workspaceAccess = true;

    ResolvedExecutionPlan plan;
    plan.workflowId = kExecutionProfileWorkflowId;
    plan.assignments = {architect, worker};
    plan.maxParallelism = 2;

    Run run;
    run.id = "workspace-run-unwired";
    run.projectId = "workspace-graph-project";
    run.title = "workspaceAccess without wiring";
    run.task = "Fail closed when the runner never received workspace-access dependencies";
    run.workflowId = kExecutionProfileWorkflowId;
    run.assignmentSnapshot = plan.assignments;
    run.maxParallelism = 2;
    run.status = RunStatus::Running;

    TempDir workspace;
    Project project = makeProject(workspace.path);
    const auto stages = runner.createStages(run.id, plan);
    const WorkflowRunContext context {run, project, stages};
    NullEventSink sink;

    const WorkflowResult result = runner.run(context, std::stop_token {}, sink);
    assert(!result.success);
    assert(result.message.find("run.workspace_access_unavailable") != std::string::npos);
}

void testWorkspaceAccessRosterRejectedAtSubmitWhenRetryConfigured()
{
    Catalog catalog;
    const Provider provider = makeProvider(catalog, "P1");
    const Model model = makeModel(catalog, provider.id, "model-a");

    StageModelAssignment architect {"architect", provider.id, model.id, 5000ms};
    architect.leadArchitect = true;
    architect.role = "architect";
    architect.position = 0;
    StageModelAssignment worker {"worker", provider.id, model.id, 5000ms};
    worker.workspaceAccess = true;
    worker.maxAttempts = 3;
    worker.role = "worker";
    worker.position = 1;

    const auto result = validateRunRoster({architect, worker}, catalog.providers, catalog.models);
    assert(!result.success());
    assert(result.error()->code == "run.workspace_access_no_retry");
}

// One run-level reproduction of the audited retry incident. It covers the
// successful sibling outcomes and every no-post-failure-spend invariant.
void testCombinedRetryIncidentStopsDependentsAndPostFailureSpend()
{
    WorkspaceFixture fx;
    const fs::path sharedPath = fx.workspaceDir.path / "src" / "shared.cpp";
    fs::create_directories(sharedPath.parent_path());
    {
        std::ofstream output(sharedPath, std::ios::binary);
        output << "int shared_value = 1;\n";
    }

    FilesystemWorkspaceToolExecutor realExecutor;
    auto registry = ToolRegistry::createDefaultReadOnly();
    assert(registry.success());
    AgentToolExecutionService toolExecution(
        std::move(registry.value()), realExecutor,
        fx.toolSessions, fx.toolAudits, true);
    auto changeProposals = fx.makeChangeProposals();
    IncidentProviderAgentSessionFactory incidentSessions;
    InMemoryModelExecutionRepository executionRepository;
    ModelExecutionService executions {executionRepository};

    const Provider architectProvider = makeProvider(fx.catalog, "Incident Architect");
    const Model architectModel = makeModel(fx.catalog, architectProvider.id, "gpt-5.6-sol");
    const Provider ownerProvider = makeProvider(fx.catalog, "Owner Agent");
    const Model ownerModel = makeModel(fx.catalog, ownerProvider.id, "gpt-5.6-sol-owner");
    const Provider noChangesProvider = makeProvider(fx.catalog, "No Changes Agent");
    const Model noChangesModel = makeModel(fx.catalog, noChangesProvider.id, "gpt-5.6-sol-empty");
    const Provider retryProvider = makeProvider(fx.catalog, "Retry Agent");
    const Model retryModel = makeModel(fx.catalog, retryProvider.id, "gpt-5.6-sol-retry");
    const Provider dependentProvider = makeProvider(fx.catalog, "Dependent Agent");
    const Model dependentModel = makeModel(fx.catalog, dependentProvider.id, "gpt-5.6-sol-dependent");

    FakeProviderClient architectClient;
    architectClient.content = architectManifest({
        {"owner", {"src/shared.cpp"}, {}},
        {"no-changes", {"src/no-changes.cpp"}, {"owner"}},
        {"retry", {"src/retry.cpp"}, {"owner"}},
        {"dependent", {"src/dependent.cpp"}, {"retry"}}}, 4);
    FakeProviderClientFactory clients;
    clients.registerClient(architectProvider.id, architectClient);

    // Neither owner nor retry sends a hash field any more (proposal-submission.v2
    // has none) - both rely entirely on the Backend's own hash preflight
    // (AgentToolTurnLoopRunner::preflightTrustedHashes), which hashes
    // src/shared.cpp via the same real FilesystemWorkspaceToolExecutor this
    // test already uses regardless of which agent's proposal it belongs to.
    // Owner and retry's own workspace_hash_file calls below are therefore no
    // longer required for either proposal to obtain evidence, but remain
    // harmless (and are kept to prove the preflight also tolerates a model
    // that redundantly hashes the same path itself).
    const std::string ownerProposal =
        R"({"schemaVersion":"proposal-submission.v2","summary":"Own shared.cpp","changes":[{)"
        R"("change_id":"owner-change","operation":"modify",)"
        R"("relative_path":"src/shared.cpp","source_path":null,)"
        R"("proposed_content":"int shared_value = 2;\n"}]})";
    const std::string retryProposal =
        R"({"schemaVersion":"proposal-submission.v2","summary":"Also update shared.cpp","changes":[{)"
        R"("change_id":"retry-change","operation":"modify",)"
        R"("relative_path":"src/shared.cpp","source_path":null,)"
        R"("proposed_content":"int shared_value = 3;\n"}]})";
    const std::string emptyProposal =
        R"({"schemaVersion":"proposal-submission.v2",)"
        R"("summary":"Inspection found no additional changes","changes":[]})";

    incidentSessions.prepare(ownerProvider.id, incidentSession({
        incidentToolCall(0, "owner-hash", "workspace_hash_file", R"({"path":"src/shared.cpp"})"),
        incidentToolCall(1, "owner-proposal", kProposeChangeSetToolName, ownerProposal)}));
    incidentSessions.prepare(noChangesProvider.id, incidentSession({
        incidentToolCall(0, "empty-first", kProposeChangeSetToolName, emptyProposal)}));
    incidentSessions.prepare(noChangesProvider.id, incidentSession({
        incidentToolCall(0, "empty-inspect", "workspace_list_directory", R"({"path":"src"})"),
        incidentToolCall(1, "empty-repaired", kProposeChangeSetToolName, emptyProposal)}));
    incidentSessions.prepare(retryProvider.id, incidentSession({
        incidentToolCall(0, "retry-read", "workspace_read_file", R"({"path":"src/shared.cpp"})"),
        incidentToolCall(1, "retry-fabricated", kProposeChangeSetToolName, retryProposal)}));
    incidentSessions.prepare(retryProvider.id, incidentSession({
        incidentToolCall(0, "retry-hash", "workspace_hash_file", R"({"path":"src/shared.cpp"})"),
        incidentToolCall(1, "retry-repaired", kProposeChangeSetToolName, retryProposal)}));

    ExecutionProfileWorkflowRunner runner(
        clients, fx.catalog.providers, fx.catalog.models,
        nullptr, &executions, nullptr,
        &incidentSessions, &toolExecution, &changeProposals);

    StageModelAssignment architect {"architect", architectProvider.id, architectModel.id, 5000ms};
    architect.leadArchitect = true;
    architect.position = 0;
    StageModelAssignment owner {"owner", ownerProvider.id, ownerModel.id, 5000ms};
    owner.workspaceAccess = true;
    owner.position = 1;
    StageModelAssignment noChanges {"no-changes", noChangesProvider.id, noChangesModel.id, 5000ms};
    noChanges.workspaceAccess = true;
    noChanges.dependencies = {"owner"};
    noChanges.position = 2;
    StageModelAssignment retry {"retry", retryProvider.id, retryModel.id, 5000ms};
    retry.workspaceAccess = true;
    retry.dependencies = {"owner"};
    retry.position = 3;
    StageModelAssignment dependent {"dependent", dependentProvider.id, dependentModel.id, 5000ms};
    dependent.workspaceAccess = true;
    dependent.dependencies = {"retry"};
    dependent.position = 4;

    ResolvedExecutionPlan plan;
    plan.workflowId = kExecutionProfileWorkflowId;
    plan.assignments = {architect, owner, noChanges, retry, dependent};
    plan.maxParallelism = 4;

    Run run;
    run.id = "phase6-combined-retry-incident";
    run.projectId = "workspace-graph-project";
    run.title = "Combined audited retry incident";
    run.task = "Reproduce sibling retry, evidence and conflict failure semantics";
    run.workflowId = kExecutionProfileWorkflowId;
    run.assignmentSnapshot = plan.assignments;
    run.maxParallelism = plan.maxParallelism;
    run.workspacePath = fx.workspaceDir.path;
    run.status = RunStatus::Running;

    const Project project = makeProject(fx.workspaceDir.path);
    const auto stages = runner.createStages(run.id, plan);
    const WorkflowRunContext context {run, project, stages};
    NullEventSink sink;
    const WorkflowResult result = runner.run(context, std::stop_token {}, sink);

    assert(!result.success);
    assert(result.finalStatus == RunStatus::Failed);
    assert(architectClient.callCount == 1);
    assert(incidentSessions.createCallsFor(ownerProvider.id) == 1);
    assert(incidentSessions.createCallsFor(noChangesProvider.id) == 2);
    assert(incidentSessions.createCallsFor(retryProvider.id) == 2);
    assert(incidentSessions.createCallsFor(dependentProvider.id) == 0);
    assert(result.finalOutput.find("provider_synthesis: skipped") != std::string::npos);
    assert(result.finalOutput.find("## Task dependent") != std::string::npos);
    assert(result.finalOutput.find("- state: not_run") != std::string::npos);
    assert(result.finalOutput.find("proposal.outside_allowed_roots") != std::string::npos);

    std::ifstream unchangedInput(sharedPath, std::ios::binary);
    const std::string unchanged {
        std::istreambuf_iterator<char>(unchangedInput), std::istreambuf_iterator<char>()};
    assert(unchanged == "int shared_value = 1;\n"
        && "A proposal-only run must never apply files");

    const auto proposalsResult = fx.changeRepo.listForRun(run.id);
    assert(proposalsResult.success());
    const auto& proposals = proposalsResult.value();
    assert(proposals.size() == 5);
    bool sawReadyOwner = false;
    bool sawNoChangesRepair = false;
    bool sawMissingEvidenceAttempt = false;
    bool sawOwnershipViolation = false;
    for (const auto& proposal : proposals) {
        assert(proposal.state != ChangeSetState::Applied);
        if (proposal.agentRole == "owner" && proposal.state == ChangeSetState::ReadyForReview)
            sawReadyOwner = true;
        if (proposal.agentRole == "no-changes"
            && proposal.state == ChangeSetState::NoChangesNeeded
            && proposal.supersedesChangeSetId.has_value())
            sawNoChangesRepair = true;
        if (proposal.missingEvidenceCode == std::optional<std::string>(
                "proposal.no_changes_missing_evidence"))
            sawMissingEvidenceAttempt = true;
        for (const auto& finding : proposal.validationFindings) {
            if (finding.code == "proposal.outside_allowed_roots") sawOwnershipViolation = true;
        }
    }
    assert(sawReadyOwner && sawNoChangesRepair
        && sawMissingEvidenceAttempt && sawOwnershipViolation);

    const auto executionResult = executions.listExecutions(ModelExecutionSource::Run, run.id);
    assert(executionResult.success());
    assert(executionResult.value().size() == 4
        && "Only Architect plus the three launched agents may consume provider budget");
    std::uint64_t ledgerInput = 0;
    std::uint64_t ledgerOutput = 0;
    std::uint64_t ledgerCost = 0;
    for (const auto& execution : executionResult.value()) {
        assert(execution.role != "dependent");
        assert(execution.role != "synthesis");
        ledgerInput += execution.inputTokens;
        ledgerOutput += execution.outputTokens;
        ledgerCost += execution.costMicros;
    }
    assert(ledgerInput == result.usage.inputTokens);
    assert(ledgerOutput == result.usage.outputTokens);
    assert(ledgerCost > 0
        && "Failed-task usage remains counted, but no later spend is added");
}

Run waitForTerminalRun(IRunRepository& repository, const RunId& runId)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto found = repository.findRun(runId);
        assert(found.success() && found.value().has_value());
        if (isTerminal(found.value()->status)) return *found.value();
        std::this_thread::sleep_for(5ms);
    }
    assert(false && "Run did not reach a terminal state before timeout");
    return {};
}

// AG-17.7A characterization (cause). The legacy runner needs an Architect-owned path
// allocation for every workspace-access agent (commit 8bc6abd, 2026-08-08). A lone
// workspace lead has no Architect above it to allocate anything, so it fails closed
// - before any provider session, accounting row or proposal exists. This is the
// legacy runner's intended fail-closed contract and stays true after AG-17.7C; what
// changes there is that nothing routes a lone workspace lead to this runner any more.
void testCharacterizationLoneWorkspaceLeadFailsClosedBeforeAnyProviderCall()
{
    WorkspaceFixture fx;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    InMemoryModelExecutionRepository executionRepository;
    ModelExecutionService executionService {executionRepository};

    const Provider provider = makeProvider(fx.catalog, "Lone Lead Provider");
    const Model model = makeModel(fx.catalog, provider.id, "lone-lead-model");
    // A valid scripted proposal: if the runner could run this agent it would succeed, so
    // the failure below cannot be blamed on a missing provider script.
    fx.sessionFactory.prepareSessionFor(provider.id, kCreateNotesArgs);
    FakeProviderClientFactory clientFactory;

    ExecutionProfileWorkflowRunner runner(
        clientFactory, fx.catalog.providers, fx.catalog.models,
        nullptr, &executionService, nullptr,
        &fx.sessionFactory, &toolExecution, &changeProposals);

    StageModelAssignment lead {"architect", provider.id, model.id, 5000ms};
    lead.leadArchitect = true;
    lead.workspaceAccess = true;
    lead.role = "Workspace analysis agent";

    ResolvedExecutionPlan plan;
    plan.workflowId = kExecutionProfileWorkflowId;
    plan.assignments = {lead};
    plan.maxParallelism = 1;

    Run run;
    run.id = "lone-workspace-lead-run";
    run.projectId = "workspace-graph-project";
    run.title = "Lone workspace lead";
    run.task = "Characterize the legacy runner with a lone workspace lead";
    run.workflowId = kExecutionProfileWorkflowId;
    run.assignmentSnapshot = plan.assignments;
    run.maxParallelism = 1;
    run.workspacePath = fx.workspaceDir.path;
    run.status = RunStatus::Running;

    const Project project = makeProject(fx.workspaceDir.path);
    const auto stages = runner.createStages(run.id, plan);
    const WorkflowRunContext context {run, project, stages};
    NullEventSink sink;

    const WorkflowResult result = runner.run(context, std::stop_token {}, sink);
    assert(!result.success && result.finalStatus == RunStatus::Failed);
    assert(result.message.find("run.workspace_ownership_missing") != std::string::npos
        && "A lone workspace lead has no Architect allocation and must fail closed");
    assert(fx.sessionFactory.createCallsFor(provider.id) == 0
        && "The failure must happen before any provider session is created");
    const auto executions = executionService.listExecutions(ModelExecutionSource::Run, run.id);
    assert(executions.success() && executions.value().empty()
        && "No accounting row may exist for work that never started");
    const auto proposals = fx.changeRepo.listForRun(run.id);
    assert(proposals.success() && proposals.value().empty());
}

// AG-17.7A characterized the regression here: retrying a historical read-only-analysis-v1
// Run adapted its snapshot to a lone WORKSPACE lead and enqueued it on the legacy runner,
// which since 8bc6abd cannot execute such a roster (the retry was accepted, then failed
// closed with run.workspace_ownership_missing). AG-17.7C FLIPPED this test: the same retry now
// converts to a single-assignment Manifest Run (LegacyRosterAdapter, safe automatic Retry)
// and never reaches the legacy runner. The Manifest workflow itself is a stand-in here; its
// real execution is proved in ManifestWorkflowRunnerTests.
void testRetryOfRetiredReadOnlyRunConvertsAndNeverReachesTheLegacyRunner()
{
    WorkspaceFixture fx;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    InMemoryModelExecutionRepository executionRepository;
    ModelExecutionService executionService {executionRepository};

    const Provider provider = makeProvider(fx.catalog, "Read-only Provider");
    const Model model = makeModel(fx.catalog, provider.id, "read-only-model");
    fx.sessionFactory.prepareSessionFor(provider.id, kCreateNotesArgs);
    FakeProviderClientFactory clientFactory;

    ExecutionProfileWorkflowRunner runner(
        clientFactory, fx.catalog.providers, fx.catalog.models,
        nullptr, &executionService, nullptr,
        &fx.sessionFactory, &toolExecution, &changeProposals);

    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryExecutionProfileRepository profiles;
    ProjectService projectService {projects};
    WorkflowRouter router {profiles, fx.catalog.providers, fx.catalog.models};
    router.registerRunner(kExecutionProfileWorkflowId, runner);
    // The Manifest workflow is a stand-in: this test only proves WHERE the retry goes.
    MockWorkflowRunner manifestStandIn {1ms, 1};
    router.registerRunner(kExecutionManifestWorkflowId, manifestStandIn);
    RunManager manager {projects, runs, router};
    OrchestrationService orchestration {projects, runs, manager};

    const auto created = projectService.createProject(
        {"read-only-retry", "Characterization project", fx.workspaceDir.path});
    assert(created.success());
    const Project project = created.value();

    // Exactly what the retired runner persisted: one "analysis" agent, no lead flag, and
    // workspaceAccess false because that column did not exist yet.
    StageModelAssignment analysis {"analysis", provider.id, model.id, 5000ms};
    analysis.agentName = "Read-only analysis";
    analysis.role = "Read-only workspace analysis agent";
    analysis.position = 0;

    Run historical;
    historical.id = "retired-read-only-run";
    historical.projectId = project.id;
    historical.title = "Historical analysis";
    historical.task = "Inspect the workspace";
    historical.workflowId = "read-only-analysis-v1";
    historical.assignmentSnapshot = {analysis};
    historical.maxParallelism = 1;
    historical.workspacePath = fx.workspaceDir.path;
    historical.status = RunStatus::Completed;
    historical.createdAt = std::chrono::system_clock::now();
    historical.finishedAt = historical.createdAt;
    assert(runs.insertRun(historical).success());

    const auto retried = orchestration.retryRun(historical.id);
    assert(retried.success());
    const Run retry = waitForTerminalRun(runs, retried.value());
    assert(retry.parentRunId.has_value() && *retry.parentRunId == historical.id);
    assert(retry.workflowId == kExecutionManifestWorkflowId
        && "The retired Run converts to a Manifest Run instead of the legacy runner that cannot run it");
    assert(retry.status == RunStatus::Completed && "The stand-in Manifest runner completed it");
    assert(retry.errorMessage.find("run.workspace_ownership_missing") == std::string::npos);
    const auto conversion = orchestration.findLegacyConversion(retry.id);
    assert(conversion.success() && conversion.value().has_value()
        && conversion.value()->confirmationMode == LegacyConversionConfirmationMode::NotRequiredSafeRetry
        && !conversion.value()->confirmedAt.has_value()
        && "A safe automatic Retry records its decision and claims no confirmation");
    assert(fx.sessionFactory.createCallsFor(provider.id) == 0
        && "The legacy runner must never be reached by this Retry");
    const auto executions = executionService.listExecutions(ModelExecutionSource::Run, retry.id);
    assert(executions.success() && executions.value().empty());

    // The immutable source is untouched by the conversion.
    const auto source = runs.findRun(historical.id);
    assert(source.success() && source.value().has_value()
        && source.value()->workflowId == "read-only-analysis-v1"
        && source.value()->status == RunStatus::Completed);
    manager.shutdown();
}

} // namespace

int runExecutionProfileWorkflowRunnerWorkspaceTests()
{
    testWorkspaceAccessAgentProducesProposal();
    testWorkspaceAccessAgentRecordsModelExecution();
    testLegacyWorkspaceAgentIsPricedPerResponseFromTheModelCopyOfTheAttempt();
    testWorkspaceAccessSessionIncludesReadOnlyToolDefinitions();
    testTwoConcurrentWorkspaceAgentsDifferentFilesBothSucceed();
    testOverlappingArchitectOwnershipStopsBeforeWorkers();
    testWorkspaceAccessAgentFailsClosedWhenNotWired();
    testWorkspaceAccessRosterRejectedAtSubmitWhenRetryConfigured();
    testCombinedRetryIncidentStopsDependentsAndPostFailureSpend();
    testCharacterizationLoneWorkspaceLeadFailsClosedBeforeAnyProviderCall();
    testRetryOfRetiredReadOnlyRunConvertsAndNeverReachesTheLegacyRunner();
    return 0;
}
