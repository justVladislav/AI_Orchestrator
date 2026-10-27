// Tests for AgentToolTurnLoopRunner in isolation, extracted from
// ReadOnlyAnalysisWorkflowRunner (2026-07-26) so ExecutionProfileWorkflowRunner
// can reuse the exact same tool-use turn loop for individual roster agents.
// A fake IProviderAgentSession drives a scripted turn sequence, a real
// AgentToolExecutionService dispatches against an in-memory tool executor,
// and a real ChangeProposalService with a real FileChangeProposalStore (temp
// directory) validates and stages the final proposal.v1 output. No network,
// no CURL, no real provider adapter.
#include "ai/application/AgentToolExecutionService.hpp"
#include "ai/application/AgentToolTurnLoopRunner.hpp"
#include "ai/application/proposals/ChangeProposalService.hpp"
#include "ai/application/FilesystemWorkspaceToolExecutor.hpp"
#include "ai/application/InMemoryRepositories.hpp"
#include "ai/application/InMemoryRunWorkspaceScopeRepository.hpp"
#include "ai/application/ProjectWritePolicy.hpp"
#include "ai/application/RunPathClaimRegistry.hpp"
#include "ai/application/RunWorkspaceScopeService.hpp"
#include "ai/application/Sha256.hpp"
#include "ai/infrastructure/FileChangeProposalStore.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <stop_token>
#include <unordered_map>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace common = ai::common;
using namespace ai::application;
using namespace ai::providers;
using namespace ai::infrastructure;

// ---------------------------------------------------------------------
// Fixtures / fakes (same shape as ReadOnlyAnalysisWorkflowRunnerTests.cpp;
// kept local since each test file in this codebase is self-contained).
// ---------------------------------------------------------------------

struct TempDir {
    fs::path path;

    TempDir()
    {
        path = fs::temp_directory_path()
            / ("ai-orchestrator-turnloop-test-"
                + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }

    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    void writeFile(const std::string& relative, const std::string& content) const
    {
        const fs::path full = path / relative;
        fs::create_directories(full.parent_path());
        std::ofstream out(full, std::ios::binary);
        out << content;
    }
};

class AlwaysSucceedExecutor final : public IWorkspaceToolExecutor {
public:
    common::Result<AgentToolResult> execute(
        const AgentToolCall& call, const WorkspaceReadPolicy&) override
    {
        ++callCount;
        capturedCalls.emplace_back(call.toolName, call.argumentsJson);
        AgentToolResult r;
        r.callId = call.callId;
        r.toolName = call.toolName;
        r.success = succeed;
        if (succeed) {
            r.contentJson = call.toolName == "workspace_stat_path"
                ? R"({"relativePath":"missing.txt","exists":false})"
                : R"({"relativePath":"README.md","entries":["src","README.md"]})";
        } else {
            r.errorCode = "workspace.test_failure";
        }
        r.returnedBytes = r.contentJson.size();
        return common::Result<AgentToolResult>::ok(std::move(r));
    }

    int callCount {0};
    bool succeed {true};
    // Records every dispatched call (toolName, argumentsJson) - used to
    // prove the hash preflight's own dispatch behavior (skips Create, dedupes
    // identical subject paths, uses source_path for Rename) without needing
    // the preflight's hash results to actually resolve.
    std::vector<std::pair<std::string, std::string>> capturedCalls;
};

// AG-15: unlike AlwaysSucceedExecutor (which always reports "README.md"
// regardless of what was actually requested), this executor echoes the
// requested relativePath back so a test can prove the same-attempt read
// gate correlates the exact path the model asked to read - required for the
// scope-expansion tests, which need to distinguish "read the expanded path"
// from "read some other, unrelated path".
class PathEchoingExecutor final : public IWorkspaceToolExecutor {
public:
    common::Result<AgentToolResult> execute(
        const AgentToolCall& call, const WorkspaceReadPolicy& policy) override
    {
        ++callCount;
        AgentToolResult r;
        r.callId = call.callId;
        r.toolName = call.toolName;
        r.success = true;
        std::string path = "README.md";
        try {
            const auto args = nlohmann::json::parse(call.argumentsJson);
            if (args.contains("path") && args.at("path").is_string())
                path = args.at("path").get<std::string>();
        } catch (const std::exception&) {
            // Malformed arguments: fall back to the default path above.
        }
        const auto checked = WorkspaceReadPolicyValidator::validateRelativePath(path, policy);
        authorized.push_back(checked.success());
        if (!checked.success()) {
            r.success = false;
            r.errorCode = checked.error()->code;
            return common::Result<AgentToolResult>::ok(std::move(r));
        }
        nlohmann::json body;
        body["relativePath"] = path;
        body["exists"] = true;
        r.contentJson = body.dump();
        r.returnedBytes = r.contentJson.size();
        return common::Result<AgentToolResult>::ok(std::move(r));
    }

    int callCount {0};
    std::vector<bool> authorized;
};

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
    common::Result<std::vector<ProposedChangeSet>> listForRun(const RunId&) const override
    {
        return common::Result<std::vector<ProposedChangeSet>>::ok({});
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

struct SessionCallLog {
    int beginCalls {0};
    int submitCalls {0};
};

class ScriptedProviderAgentSession final : public IProviderAgentSession {
public:
    std::queue<common::Result<ProviderAgentTurnResponse>> responses;
    SessionCallLog* log {nullptr};
    // When set, captures the exact request this session's beginSession() was
    // called with - used to inspect a repair session's userContent, which is
    // otherwise built and discarded entirely inside AgentToolTurnLoopRunner.
    ProviderAgentSessionRequest* capturedBeginRequest {nullptr};
    // When set, records the NextTurnToolPolicy of every submitToolResults()
    // call (std::nullopt for an unrestricted turn) - used by the Backend
    // terminal-forcing tests.
    std::vector<std::optional<NextTurnToolPolicy>>* capturedPolicies {nullptr};
    // When set, records the tool results of every submitToolResults() call - used to prove what
    // the Backend actually hands back to the model (for example a denied read's guidance).
    std::vector<std::vector<AgentToolResult>>* capturedResults {nullptr};

    common::Result<ProviderAgentTurnResponse> beginSession(
        const ProviderAgentSessionRequest& request, std::stop_token) override
    {
        if (log != nullptr) ++log->beginCalls;
        if (capturedBeginRequest != nullptr) *capturedBeginRequest = request;
        return nextResponse();
    }

    common::Result<ProviderAgentTurnResponse> submitToolResults(
        std::vector<AgentToolResult> results, std::stop_token,
        std::optional<NextTurnToolPolicy> policy = std::nullopt) override
    {
        if (log != nullptr) ++log->submitCalls;
        if (capturedPolicies != nullptr) capturedPolicies->push_back(policy);
        if (capturedResults != nullptr) capturedResults->push_back(std::move(results));
        return nextResponse();
    }

private:
    common::Result<ProviderAgentTurnResponse> nextResponse()
    {
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
    std::queue<std::unique_ptr<ScriptedProviderAgentSession>> preparedSessions;
    int createCalls {0};

    common::Result<std::unique_ptr<IProviderAgentSession>> createSession(
        const ProviderId&, const std::string&) override
    {
        ++createCalls;
        if (preparedSessions.empty()) {
            return common::Result<std::unique_ptr<IProviderAgentSession>>::failure(
                {"test.no_more_sessions",
                    "ScriptedProviderAgentSessionFactory ran out of prepared sessions"});
        }
        auto session = std::move(preparedSessions.front());
        preparedSessions.pop();
        return common::Result<std::unique_ptr<IProviderAgentSession>>::ok(std::move(session));
    }
};

struct Fixture {
    TempDir tempDir;
    InMemoryAgentToolSessionRepository toolSessions;
    InMemoryAgentToolAuditRepository toolAudits;
    AlwaysSucceedExecutor executor;
    ScriptedProviderAgentSessionFactory sessionFactory;
    FakeChangeProposalRepository changeRepo;
    std::unique_ptr<FileChangeProposalStore> changeStore;

    Fixture()
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

    ChangeProposalService makeChangeProposals(RunPathClaimRegistry* registry = nullptr)
    {
        return ChangeProposalService(changeRepo, *changeStore, registry);
    }
};

// AG-15: same shape as Fixture, but with PathEchoingExecutor so the
// same-attempt read gate tests can prove correlation with the exact
// requested path instead of every read looking like "README.md".
struct PathEchoFixture {
    TempDir tempDir;
    InMemoryAgentToolSessionRepository toolSessions;
    InMemoryAgentToolAuditRepository toolAudits;
    PathEchoingExecutor executor;
    ScriptedProviderAgentSessionFactory sessionFactory;
    FakeChangeProposalRepository changeRepo;
    std::unique_ptr<FileChangeProposalStore> changeStore;

    PathEchoFixture()
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

    ChangeProposalService makeChangeProposals(RunPathClaimRegistry* registry = nullptr)
    {
        return ChangeProposalService(changeRepo, *changeStore, registry);
    }
};

AgentToolTurnLoopRequest makeRequest(const fs::path& workspaceRoot)
{
    AgentToolTurnLoopRequest req;
    req.sessionRequest.providerId = "provider-1";
    req.sessionRequest.modelId = "model-1";
    req.sessionRequest.apiModelId = "test-model";
    req.sessionRequest.systemPrompt = kWorkspaceToolsSystemPrompt;
    req.sessionRequest.userContent = "Find dead code and propose a cleanup";
    req.sessionRequest.tools.push_back(proposeChangeSetToolDefinition());
    req.sessionRequest.maxTurns = kAgentToolTurnLoopMaxTurns;
    req.sessionRequest.maxTotalToolCalls = kAgentToolTurnLoopMaxTotalToolCalls;
    req.sessionRequest.maxToolCallsPerTurn = kAgentToolTurnLoopMaxToolCallsPerTurn;
    req.sessionRequest.maxOutputTokens = kAgentToolTurnLoopMaxOutputTokens;
    req.runId = "run-1";
    req.providerKind = "openai";
    req.policy = AgentToolExecutionService::buildPolicy(workspaceRoot);
    req.proposalTemplate.runId = "run-1";
    req.proposalTemplate.projectId = "project-1";
    req.proposalTemplate.providerId = "provider-1";
    req.proposalTemplate.modelId = "model-1";
    req.proposalTemplate.agentRole = "test-agent";
    ProjectWritePolicy writePolicy;
    writePolicy.projectRoot = workspaceRoot;
    req.proposalTemplate.policy = writePolicy;
    return req;
}

void testScopedProposalToolSchemaAdvertisesOnlyFrozenWritePaths()
{
    const auto definition = proposeChangeSetToolDefinition({
        fs::path("qml/AppShell.qml"),
        fs::path("qml/components/navigation/AppSidebar.qml"),
        fs::path("qml/AppShell.qml")});
    assert(definition.strictInputSchema);
    const auto schema = nlohmann::json::parse(definition.inputSchemaJson);
    const auto& properties =
        schema.at("properties").at("changes").at("items").at("properties");
    const auto& targets = properties.at("relative_path").at("enum");
    assert(targets == nlohmann::json::array({
        "qml/AppShell.qml", "qml/components/navigation/AppSidebar.qml"}));
    const auto& sources = properties.at("source_path").at("enum");
    assert(sources == nlohmann::json::array({
        "qml/AppShell.qml", "qml/components/navigation/AppSidebar.qml", nullptr}));
    assert(std::find(targets.begin(), targets.end(), "qml/Main.qml") == targets.end());
}

ProviderAgentTurnResponse stopResponse(std::size_t turnIndex, std::string text)
{
    ProviderAgentTurnResponse resp;
    resp.turnIndex = turnIndex;
    resp.finishReason = ProviderAgentFinishReason::Stop;
    resp.assistantText = std::move(text);
    resp.usage.inputTokens = 10;
    resp.usage.outputTokens = 5;
    return resp;
}

ProviderAgentTurnResponse toolCallResponse(
    std::size_t turnIndex, const std::string& callId,
    std::string toolName, std::string argumentsJson)
{
    ProviderAgentTurnResponse resp;
    resp.turnIndex = turnIndex;
    resp.finishReason = ProviderAgentFinishReason::ToolUse;
    AgentToolCall call;
    call.callId = callId;
    call.toolName = std::move(toolName);
    call.argumentsJson = std::move(argumentsJson);
    call.turnIndex = turnIndex;
    resp.toolCalls = {call};
    resp.usage.inputTokens = 8;
    resp.usage.outputTokens = 4;
    return resp;
}

ProviderAgentTurnResponse toolUseResponse(std::size_t turnIndex, const std::string& callId)
{
    return toolCallResponse(turnIndex, callId, "workspace_read_file", R"({"path":"README.md"})");
}

ProviderAgentTurnResponse proposeChangeSetToolCallResponse(
    std::size_t turnIndex, const std::string& callId, std::string argumentsJson)
{
    ProviderAgentTurnResponse resp;
    resp.turnIndex = turnIndex;
    resp.finishReason = ProviderAgentFinishReason::ToolUse;
    AgentToolCall call;
    call.callId = callId;
    call.toolName = kProposeChangeSetToolName;
    call.argumentsJson = std::move(argumentsJson);
    call.turnIndex = turnIndex;
    resp.toolCalls = {call};
    resp.usage.inputTokens = 8;
    resp.usage.outputTokens = 4;
    return resp;
}

// A minimal but non-empty proposal, used by every test below except
// testEmptyChangesArrayProducesNoChangesNeeded: most of this suite exercises
// the turn loop's session/tool-dispatch/repair mechanics, not change-set
// content semantics, and a ReadyForReview proposal keeps those assertions
// simple.
const char* kSimpleProposal =
    "```json\n"
    "{\"schemaVersion\":\"proposal-submission.v2\","
    "\"summary\":\"Add a notes file.\",\"changes\":[{"
    "\"change_id\":\"create-notes\",\"operation\":\"create\","
    "\"relative_path\":\"NOTES.md\",\"source_path\":null,"
    "\"proposed_content\":\"notes\"}]}\n"
    "```";

// ---------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------

// Phase 1 audit-evidence gate (REMAINING_GAPS_EXECUTION_PLAN.md): proves
// AgentToolTurnLoopOutcome now actually carries the tool-session evidence
// (turn/call/inspection/byte counts) it always tracked internally but never
// returned, and that a caller-supplied modelExecutionId on the request
// template reaches the persisted proposal - both were silently lost before.
void testToolEvidenceAndModelExecutionIdArePersistedAndReported()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto request = makeRequest(workspace.path);
    request.proposalTemplate.modelExecutionId = "exec-42";

    auto session = std::make_unique<ScriptedProviderAgentSession>();
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolUseResponse(0, "call-1")));
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        stopResponse(1, kSimpleProposal)));
    fx.sessionFactory.preparedSessions.push(std::move(session));

    std::stop_source ss;
    const auto outcome = runner.run(std::move(request), ss.get_token(), nullptr, nullptr);

    assert(outcome.success);
    assert(outcome.proposal.has_value());
    assert(outcome.proposal->state == ChangeSetState::ReadyForReview);
    assert(outcome.proposal->modelExecutionId.has_value());
    assert(*outcome.proposal->modelExecutionId == "exec-42");

    // The two turns above (tool-use, then stop) plus the one dispatched,
    // successful call between them - all previously reported as 0 no matter
    // what actually happened in the session.
    assert(outcome.totalTurns == 2);
    assert(outcome.totalToolCalls == 1);
    assert(outcome.successfulInspectionCalls == 1);
    assert(outcome.totalReturnedBytes > 0);

    // The durable AgentToolSession row (not just this call's returned
    // outcome) must show the same non-zero totals - this is what a UI/
    // incident review actually reads back later.
    const auto sessions = fx.toolSessions.listForRun("run-1");
    assert(sessions.success());
    assert(sessions.value().size() == 1);
    const auto& persistedSession = sessions.value().front();
    assert(persistedSession.totalTurns == outcome.totalTurns);
    assert(persistedSession.totalCalls == outcome.totalToolCalls);
    assert(persistedSession.totalReturnedBytes == outcome.totalReturnedBytes);
}

void testStopFinishReasonReturnsProposal()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto session = std::make_unique<ScriptedProviderAgentSession>();
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        stopResponse(0, kSimpleProposal)));
    fx.sessionFactory.preparedSessions.push(std::move(session));

    std::stop_source ss;
    const auto outcome = runner.run(makeRequest(workspace.path), ss.get_token(), nullptr, nullptr);

    assert(outcome.success);
    assert(!outcome.cancelled);
    assert(outcome.proposal.has_value());
    assert(outcome.proposal->state == ChangeSetState::ReadyForReview);
    assert(fx.sessionFactory.createCalls == 1);
}

// An empty changes[] is a legitimate "findings only, nothing to change"
// answer (the tool schema and system prompt both document this as allowed),
// not a mistake — so it must not trigger the format-repair retry, and it
// must land in its own terminal state rather than being conflated with a
// genuinely Rejected proposal. This is only true once the agent has actually
// looked at something, though - see
// testZeroToolNoChangesNeededIsRejectedAsMissingEvidence directly below for
// the corresponding failure case an empty answer with zero tool calls must
// hit instead (the real production incident this pair of tests locks in).
void testEmptyChangesArrayAfterRealInspectionSucceeds()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto session = std::make_unique<ScriptedProviderAgentSession>();
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolUseResponse(0, "call-1")));
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        stopResponse(1, R"({"schemaVersion":"proposal-submission.v2",)"
            R"("summary":"No dead code found.","changes":[]})")));
    fx.sessionFactory.preparedSessions.push(std::move(session));

    std::stop_source ss;
    const auto outcome = runner.run(makeRequest(workspace.path), ss.get_token(), nullptr, nullptr);

    assert(outcome.success);
    assert(!outcome.cancelled);
    assert(outcome.proposal.has_value());
    assert(outcome.proposal->state == ChangeSetState::NoChangesNeeded);
    assert(outcome.proposal->changes.empty());
    // No repair retry: only the one primary session was ever created.
    assert(fx.sessionFactory.createCalls == 1);
}

// The real incident this guards against: two workspace-enabled agents (Run
// f5058a12-746a-4ab6-8deb-ca5bfc5500ca, agents 3/4) made zero tool calls and
// still returned an empty changes[], which the runner previously treated as
// a fully successful task simply because the proposal parsed and persisted
// cleanly. An unexamined "nothing to change" is not proven evidence; it must
// fail so a caller can tell "genuinely nothing to do" apart from "never
// actually looked". Gets the same one-shot repair chance as a format error;
// the repair session here repeats the same zero-tool mistake, so the final
// outcome is deterministically a failure.
void testZeroToolNoChangesNeededIsRejectedAsMissingEvidence()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto firstSession = std::make_unique<ScriptedProviderAgentSession>();
    firstSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        stopResponse(0, R"({"schemaVersion":"proposal-submission.v2",)"
            R"("summary":"Nothing to do.","changes":[]})")));
    fx.sessionFactory.preparedSessions.push(std::move(firstSession));

    auto repairSession = std::make_unique<ScriptedProviderAgentSession>();
    repairSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        stopResponse(0, R"({"schemaVersion":"proposal-submission.v2",)"
            R"("summary":"Still nothing to do.","changes":[]})")));
    fx.sessionFactory.preparedSessions.push(std::move(repairSession));

    std::stop_source ss;
    const auto outcome = runner.run(makeRequest(workspace.path), ss.get_token(), nullptr, nullptr);

    assert(!outcome.success);
    assert(!outcome.cancelled);
    assert(outcome.errorCode == "proposal.no_changes_missing_evidence");
    assert(outcome.proposal.has_value());
    assert(outcome.proposal->state == ChangeSetState::NoChangesNeeded);
    // One repair session was attempted (and also made zero tool calls).
    assert(fx.sessionFactory.createCalls == 2);

    // Phase 1 audit-evidence gate: the first (superseded) attempt must be
    // independently reconstructable from what was actually persisted, not
    // just from this one call's returned outcome (which only ever reflects
    // the final/repaired attempt). This fake's own listForRun() is a
    // permanent empty stub (unused by every other test in this file), so
    // look the superseded row up directly by the id the final attempt
    // links back to instead.
    assert(outcome.proposal->supersedesChangeSetId.has_value());
    const auto firstAttempt = changeProposals.findChangeSet(*outcome.proposal->supersedesChangeSetId);
    assert(firstAttempt.success());
    assert(firstAttempt.value().state == ChangeSetState::NoChangesNeeded);
    assert(firstAttempt.value().missingEvidenceCode.has_value());
    assert(*firstAttempt.value().missingEvidenceCode == "proposal.no_changes_missing_evidence");
    assert(firstAttempt.value().missingEvidenceMessage.has_value());
    assert(!firstAttempt.value().supersedesChangeSetId.has_value());
}

// A dispatched call is audit evidence that an attempt happened, but a
// tool-level failure is not evidence that any workspace state was observed.
// Both the primary and repair attempts fail their inspection here, so an
// empty proposal must remain a deterministic failure.
void testFailedInspectionDoesNotProveNoChangesNeeded()
{
    Fixture fx;
    fx.executor.succeed = false;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto makeFailedInspectionSession = [] {
        auto session = std::make_unique<ScriptedProviderAgentSession>();
        session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
            toolUseResponse(0, "call-list")));
        session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
            stopResponse(1, R"({"schemaVersion":"proposal-submission.v2",)"
                R"("summary":"Nothing to do.","changes":[]})")));
        return session;
    };
    fx.sessionFactory.preparedSessions.push(makeFailedInspectionSession());
    fx.sessionFactory.preparedSessions.push(makeFailedInspectionSession());

    std::stop_source ss;
    const auto outcome = runner.run(makeRequest(workspace.path), ss.get_token(), nullptr, nullptr);

    assert(!outcome.success);
    assert(!outcome.cancelled);
    assert(outcome.errorCode == "proposal.no_changes_missing_evidence");
    assert(outcome.proposal.has_value());
    assert(outcome.proposal->state == ChangeSetState::NoChangesNeeded);
    assert(fx.executor.callCount == 2);
    assert(fx.sessionFactory.createCalls == 2);
}

void testContradictoryNoChangesGetsOneRepairAndFailsIfRepeated()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto firstSession = std::make_unique<ScriptedProviderAgentSession>();
    firstSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolUseResponse(0, "call-list")));
    firstSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        stopResponse(1, R"({"schemaVersion":"proposal-submission.v2","summary":)"
            R"("Required files are missing; changes were not made and require a )"
            R"(separate apply pass.","changes":[]})")));
    fx.sessionFactory.preparedSessions.push(std::move(firstSession));

    auto repairSession = std::make_unique<ScriptedProviderAgentSession>();
    repairSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        stopResponse(0, R"({"schemaVersion":"proposal-submission.v2",)"
            R"("summary":"Required files are missing; changes were not made.","changes":[]})")));
    fx.sessionFactory.preparedSessions.push(std::move(repairSession));

    std::stop_source ss;
    const auto outcome = runner.run(makeRequest(workspace.path), ss.get_token(), nullptr, nullptr);

    assert(!outcome.success);
    assert(outcome.errorCode == "proposal.no_changes_contradicts_summary");
    assert(outcome.proposal.has_value());
    assert(outcome.proposal->state == ChangeSetState::NoChangesNeeded);
    assert(fx.sessionFactory.createCalls == 2);
}

void testContradictoryNoChangesRepairCanProduceChanges()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto firstSession = std::make_unique<ScriptedProviderAgentSession>();
    firstSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolUseResponse(0, "call-list")));
    firstSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        stopResponse(1, R"({"schemaVersion":"proposal-submission.v2","summary":)"
            R"("Изменения в файлы не внесены; предлагаю отдельный apply-проход.",)"
            R"("changes":[]})")));
    fx.sessionFactory.preparedSessions.push(std::move(firstSession));

    auto repairSession = std::make_unique<ScriptedProviderAgentSession>();
    repairSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        stopResponse(0, kSimpleProposal)));
    fx.sessionFactory.preparedSessions.push(std::move(repairSession));

    std::stop_source ss;
    const auto outcome = runner.run(makeRequest(workspace.path), ss.get_token(), nullptr, nullptr);

    assert(outcome.success);
    assert(outcome.proposal.has_value());
    assert(outcome.proposal->state == ChangeSetState::ReadyForReview);
    assert(fx.sessionFactory.createCalls == 2);
}

// MANIFEST_EXECUTION_CORRECTNESS_FIX_PLAN.md Package 2: a repair session has
// no memory of the primary attempt, so the repair prompt must stand on its
// own - the complete original authoritative allocation verbatim, every
// validation finding (not only the first), the previous output inside a
// clearly delimited untrusted block, and the terminal tool-call requirement.
// Uses a valid Create plus a Modify whose target is missing, so both
// proposal.missing_hash_evidence and proposal.invariant_violated findings
// must appear together -
// ChangeProposalService only reaches its later filesystem-existence
// preconditions when ChangeProposalValidator::validate() itself already
// found nothing, so both findings used here must come from validate() itself.
void testRepairPromptPreservesOriginalAllocationAndAllFindings()
{
    Fixture fx;
    TempDir workspace;
    FilesystemWorkspaceToolExecutor realExecutor;
    auto registry = ToolRegistry::createDefaultReadOnly();
    assert(registry.success());
    AgentToolExecutionService toolExecution(
        std::move(registry.value()), realExecutor, fx.toolSessions, fx.toolAudits, true);
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto request = makeRequest(workspace.path);
    request.sessionRequest.userContent =
        "# Authoritative task allocation\nTask ID: alpha\nRole: worker\n\n"
        "# Owned paths -- authoritative write boundary\n- NOTES.md\n- src/Main.qml\n\n"
        "# Task objective\nAdd notes and update Main.qml.";

    // src/Main.qml deliberately does not exist on disk: the Backend's own
    // hash preflight cannot hash a path that is not there, so both
    // proposal.missing_hash_evidence (checkHashEvidence) and
    // proposal.invariant_violated ("Modify requires expectedOriginalHash",
    // from checkOperationInvariants) come purely from
    // ChangeProposalValidator::validate() itself, alongside NOTES.md's
    // proposal.missing_proposed_content - ChangeProposalService only reaches
    // its later filesystem-existence-precondition checks once validate()
    // itself has already found nothing (see the `if (allFindings.empty())`
    // gate in ChangeProposalService::createFromModelOutput), so a finding
    // that instead relied on that later stage (e.g. proposal.create_target_
    // exists) would never appear here alongside an earlier validate()-only
    // finding like this one. The Create is structurally valid because v2's
    // Backend parser now rejects operation/null mismatches before validation.
    const std::string primaryProposalJson =
        R"({"schemaVersion":"proposal-submission.v2","summary":"Fix files","changes":[)"
        R"({"change_id":"create-notes","operation":"create","relative_path":"NOTES.md",)"
        R"("source_path":null,"proposed_content":"notes"},)"
        R"({"change_id":"modify-main","operation":"modify","relative_path":"src/Main.qml",)"
        R"("source_path":null,)"
        R"("proposed_content":"import QtQuick\nItem { visible: true }\n"}]})";

    auto firstSession = std::make_unique<ScriptedProviderAgentSession>();
    firstSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "call-propose", primaryProposalJson)));
    fx.sessionFactory.preparedSessions.push(std::move(firstSession));

    ProviderAgentSessionRequest capturedRepairRequest;
    auto repairSession = std::make_unique<ScriptedProviderAgentSession>();
    repairSession->capturedBeginRequest = &capturedRepairRequest;
    // Repair preserves NOTES.md and correctly drops the modify-main change,
    // since src/Main.qml genuinely does not exist.
    repairSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "call-propose-2",
            R"({"schemaVersion":"proposal-submission.v2",)"
            R"("summary":"Add notes; Main.qml does not exist yet.","changes":[)"
            R"({"change_id":"create-notes","operation":"create","relative_path":"NOTES.md",)"
            R"("source_path":null,"proposed_content":"notes"}]})")));
    fx.sessionFactory.preparedSessions.push(std::move(repairSession));

    std::stop_source ss;
    const auto outcome = runner.run(std::move(request), ss.get_token(), nullptr, nullptr);

    assert(outcome.success);
    assert(outcome.repairAttempted);
    assert(outcome.proposal.has_value());
    assert(outcome.proposal->state == ChangeSetState::ReadyForReview);
    assert(fx.sessionFactory.createCalls == 2);

    const std::string& repaired = capturedRepairRequest.userContent;
    // (1) fresh-session statement.
    assert(repaired.find("fresh session") != std::string::npos);
    // (2) complete original allocation verbatim.
    assert(repaired.find("Task ID: alpha") != std::string::npos);
    assert(repaired.find("- NOTES.md") != std::string::npos);
    assert(repaired.find("- src/Main.qml") != std::string::npos);
    assert(repaired.find("Add notes and update Main.qml.") != std::string::npos);
    // (3) every validation finding, not only the first.
    assert(repaired.find("proposal.missing_hash_evidence") != std::string::npos);
    assert(repaired.find("proposal.invariant_violated") != std::string::npos);
    // (4) previous attempt inside a clearly delimited untrusted block.
    assert(repaired.find("BEGIN PREVIOUS ATTEMPT (UNTRUSTED)") != std::string::npos);
    assert(repaired.find("END PREVIOUS ATTEMPT (UNTRUSTED)") != std::string::npos);
    assert(repaired.find("modify-main") != std::string::npos);
    // (6) terminal tool-call requirement.
    assert(repaired.find("propose_change_set") != std::string::npos);
}

// Reproduces the real incident (real Run 112967a7-2c44-4145-884a-4822f1c1e101):
// three independent Create changes each carried expected_original_hash set to
// sha256Hex("") - the model reasoning "a new file has no prior content, so
// I'll hash empty content" instead of omitting the field entirely - and a
// bounded repair attempt reproduced the exact same field verbatim, proving
// the wire contract itself (not prompt wording) was the problem. Under
// proposal-submission.v2, expected_original_hash is not a recognized field at
// all, so this is now caught structurally by the parser as
// proposal.unexpected_field before any per-change validation ever runs - a
// stronger, earlier rejection than the old proposal.invariant_violated path,
// and still repair-eligible (see isModelFormatError()) so a corrected
// resubmission recovers in one bounded retry. Reproduces one
// Create+forbidden-hash change (sufficient to prove the fix; the full
// four-change case is the same mechanism repeated).
void testRepairPromptCorrectsCreateWithForbiddenHashField()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    const std::string emptyContentHash = sha256Hex("");
    auto firstSession = std::make_unique<ScriptedProviderAgentSession>();
    firstSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "call-1",
            std::string(R"({"schemaVersion":"proposal-submission.v2",)")
            + R"("summary":"Add a new component","changes":[)"
            + R"({"change_id":"add-widget","operation":"create",)"
            + R"("relative_path":"qml/Widget.qml","source_path":null,)"
            + R"("expected_original_hash":")"
            + emptyContentHash
            + R"(","proposed_content":"import QtQuick\nItem {}\n"}]})")));
    fx.sessionFactory.preparedSessions.push(std::move(firstSession));

    ProviderAgentSessionRequest capturedRepairRequest;
    auto repairSession = std::make_unique<ScriptedProviderAgentSession>();
    repairSession->capturedBeginRequest = &capturedRepairRequest;
    repairSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "call-2",
            R"({"schemaVersion":"proposal-submission.v2","summary":"Add a new component","changes":[)"
            R"({"change_id":"add-widget","operation":"create",)"
            R"("relative_path":"qml/Widget.qml","source_path":null,)"
            R"("proposed_content":"import QtQuick\nItem {}\n"}]})")));
    fx.sessionFactory.preparedSessions.push(std::move(repairSession));

    std::stop_source ss;
    const auto outcome = runner.run(makeRequest(workspace.path), ss.get_token(), nullptr, nullptr);

    assert(outcome.success);
    assert(outcome.repairAttempted);
    assert(outcome.proposal.has_value());
    assert(outcome.proposal->state == ChangeSetState::ReadyForReview);
    assert(fx.sessionFactory.createCalls == 2);
    // The proposal that actually landed never carries a hash - Create is
    // always exempt in this contract.
    assert(!outcome.proposal->changes.front().expectedOriginalHash.has_value());

    const std::string& repaired = capturedRepairRequest.userContent;
    assert(repaired.find("proposal.unexpected_field") != std::string::npos);
    // The dedicated forbidden-field guidance, not the generic "you probably
    // wrote prose instead of calling the tool" fallback.
    assert(repaired.find("no hash field of any kind in this contract") != std::string::npos);
}

// MANIFEST_EXECUTION_CORRECTNESS_FIX_PLAN.md Package 3, reproducing run
// 29caea1c-b8d8-47bc-81ec-1b60c10e8d1c: the primary proposal is Rejected
// while still containing a concrete, non-empty change (create README.md,
// which already exists). The repair session inspects the workspace (a real
// tool call, so it is not the already-covered zero-evidence case) but then
// abandons the rejected change entirely and reports an empty NoChangesNeeded
// instead of correcting it. This must fail rather than be laundered into a
// successful no-op.
void testRepairAbandoningRejectedChangesFailsInstead()
{
    Fixture fx;
    TempDir workspace;
    workspace.writeFile("README.md", "existing content");
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto firstSession = std::make_unique<ScriptedProviderAgentSession>();
    firstSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "call-1",
            R"({"schemaVersion":"proposal-submission.v2","summary":"Create README","changes":[{)"
            R"("change_id":"create-readme","operation":"create",)"
            R"("relative_path":"README.md","source_path":null,"proposed_content":"new"}]})")));
    fx.sessionFactory.preparedSessions.push(std::move(firstSession));

    auto repairSession = std::make_unique<ScriptedProviderAgentSession>();
    repairSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolUseResponse(0, "call-list")));
    repairSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        stopResponse(1, R"({"schemaVersion":"proposal-submission.v2",)"
            R"("summary":"README already has the desired content.","changes":[]})")));
    fx.sessionFactory.preparedSessions.push(std::move(repairSession));

    std::stop_source ss;
    const auto outcome = runner.run(makeRequest(workspace.path), ss.get_token(), nullptr, nullptr);

    assert(!outcome.success);
    assert(!outcome.cancelled);
    assert(outcome.repairAttempted);
    assert(outcome.errorCode == "proposal.repair_abandoned_changes");
    assert(outcome.proposal.has_value());
    assert(outcome.proposal->state == ChangeSetState::NoChangesNeeded);
    assert(outcome.proposal->changes.empty());
    assert(fx.sessionFactory.createCalls == 2);

    // Both attempts remain durably persisted and linked - the repair's row
    // supersedes the original Rejected, non-empty attempt; it must never be
    // silently dropped just because the task ultimately failed.
    assert(outcome.proposal->supersedesChangeSetId.has_value());
    const auto firstAttempt = changeProposals.findChangeSet(*outcome.proposal->supersedesChangeSetId);
    assert(firstAttempt.success());
    assert(firstAttempt.value().state == ChangeSetState::Rejected);
    assert(!firstAttempt.value().changes.empty());
    bool foundCreateTargetExists = false;
    for (const auto& f : firstAttempt.value().validationFindings) {
        if (f.code == "proposal.create_target_exists") { foundCreateTargetExists = true; break; }
    }
    assert(foundCreateTargetExists);
}

void testToolUseThenStop()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto session = std::make_unique<ScriptedProviderAgentSession>();
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolUseResponse(0, "call-1")));
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        stopResponse(1, kSimpleProposal)));
    fx.sessionFactory.preparedSessions.push(std::move(session));

    std::stop_source ss;
    const auto outcome = runner.run(makeRequest(workspace.path), ss.get_token(), nullptr, nullptr);

    assert(outcome.success);
    assert(outcome.proposal.has_value());
    assert(outcome.proposal->state == ChangeSetState::ReadyForReview);
    assert(fx.executor.callCount == 1);
}

void testMaxTurnsBudgetFailsClosed()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto session = std::make_unique<ScriptedProviderAgentSession>();
    // A single ToolUse turn with 3 calls against a request whose
    // maxTotalToolCalls is set to 2 exceeds the budget on the first turn.
    ProviderAgentTurnResponse overBudget = toolUseResponse(0, "call-1");
    AgentToolCall second;
    second.callId = "call-2";
    second.toolName = "workspace_read_file";
    second.argumentsJson = R"({"path":"README.md"})";
    second.turnIndex = 0;
    AgentToolCall third = second;
    third.callId = "call-3";
    overBudget.toolCalls = {overBudget.toolCalls.front(), second, third};
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(overBudget));
    fx.sessionFactory.preparedSessions.push(std::move(session));

    auto request = makeRequest(workspace.path);
    request.sessionRequest.maxTotalToolCalls = 2;

    std::stop_source ss;
    const auto outcome = runner.run(std::move(request), ss.get_token(), nullptr, nullptr);

    assert(!outcome.success);
    assert(!outcome.cancelled);
    assert(outcome.errorCode == "run.tool_call_budget_exceeded");
    assert(!outcome.proposal.has_value());
}

// The real production regression this repair path targets: the model wrote
// prose instead of calling propose_change_set (proposal.parse_error, a
// format error), gets exactly one repair session that this time calls the
// tool correctly.
void testFormatErrorTriggersOneRepairRetry()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto firstSession = std::make_unique<ScriptedProviderAgentSession>();
    firstSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        stopResponse(0, "I can't create folders with read-only tools. Please run mkdir yourself.")));
    fx.sessionFactory.preparedSessions.push(std::move(firstSession));

    SessionCallLog repairLog;
    auto repairSession = std::make_unique<ScriptedProviderAgentSession>();
    repairSession->log = &repairLog;
    repairSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "call-1",
            R"({"schemaVersion":"proposal-submission.v2","summary":"Add a notes file.","changes":[{)"
            R"("change_id":"create-notes","operation":"create",)"
            R"("relative_path":"NOTES.md","source_path":null,"proposed_content":"notes"}]})")));
    fx.sessionFactory.preparedSessions.push(std::move(repairSession));

    std::stop_source ss;
    const auto outcome = runner.run(makeRequest(workspace.path), ss.get_token(), nullptr, nullptr);

    assert(outcome.success);
    assert(outcome.proposal.has_value());
    assert(outcome.proposal->state == ChangeSetState::ReadyForReview);
    assert(fx.sessionFactory.createCalls == 2);
    assert(repairLog.beginCalls == 1);
}

// A model that proposes creating a file which already exists on disk (the
// exact real-world rejection found auditing production logs on 2026-07-26,
// where 32 otherwise-correct changes were rejected alongside this one stale
// belief) gets the same one-shot repair treatment as a parse-format error.
void testCreateTargetExistsTriggersRepairRetry()
{
    Fixture fx;
    TempDir workspace;
    workspace.writeFile("README.md", "existing content");
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto firstSession = std::make_unique<ScriptedProviderAgentSession>();
    firstSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "call-1",
            R"({"schemaVersion":"proposal-submission.v2","summary":"Create README","changes":[{)"
            R"("change_id":"create-readme","operation":"create",)"
            R"("relative_path":"README.md","source_path":null,"proposed_content":"new"}]})")));
    fx.sessionFactory.preparedSessions.push(std::move(firstSession));

    auto repairSession = std::make_unique<ScriptedProviderAgentSession>();
    repairSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "call-1",
            R"({"schemaVersion":"proposal-submission.v2","summary":"Add a notes file.","changes":[{)"
            R"("change_id":"create-notes","operation":"create",)"
            R"("relative_path":"NOTES.md","source_path":null,"proposed_content":"notes"}]})")));
    fx.sessionFactory.preparedSessions.push(std::move(repairSession));

    std::stop_source ss;
    const auto outcome = runner.run(makeRequest(workspace.path), ss.get_token(), nullptr, nullptr);

    assert(outcome.success);
    assert(outcome.proposal.has_value());
    assert(outcome.proposal->state == ChangeSetState::ReadyForReview);
    assert(fx.sessionFactory.createCalls == 2);
}

// A finding that reflects another agent's concurrent claim on the same run
// (proposal.cross_agent_conflict) must NOT trigger the repair retry: the
// model's own output was otherwise fine, and re-prompting it alone cannot
// resolve a conflict with a *different* agent's work.
void testCrossAgentConflictFindingDoesNotTriggerRepair()
{
    Fixture fx;
    TempDir workspace;
    RunPathClaimRegistry registry;
    // Simulate a different agent in the same run already owning this path.
    const auto preClaim = registry.tryClaim(
        "run-1", "other-agents-changeset", {{"src/shared.cpp", "other-change"}});
    assert(preClaim.claimed);

    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals(&registry);
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto session = std::make_unique<ScriptedProviderAgentSession>();
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "call-1",
            R"({"schemaVersion":"proposal-submission.v2","summary":"Create shared.cpp","changes":[{)"
            R"("change_id":"create-shared","operation":"create",)"
            R"("relative_path":"src/shared.cpp","source_path":null,"proposed_content":"mine"}]})")));
    fx.sessionFactory.preparedSessions.push(std::move(session));

    std::stop_source ss;
    const auto outcome = runner.run(makeRequest(workspace.path), ss.get_token(), nullptr, nullptr);

    // A Rejected proposal is a durably-persisted record, never a successful
    // task outcome - see AgentToolTurnLoopOutcome::success's doc comment.
    assert(!outcome.success);
    assert(outcome.proposal.has_value());
    assert(outcome.proposal->state == ChangeSetState::Rejected);
    assert(outcome.errorCode == "proposal.cross_agent_conflict");
    assert(outcome.errorMessage.find("change 'other-change'") != std::string::npos);
    assert(outcome.errorMessage.find("change set 'other-agents-changeset'")
        != std::string::npos);
    bool foundConflict = false;
    for (const auto& f : outcome.proposal->validationFindings) {
        if (f.code == "proposal.cross_agent_conflict") { foundConflict = true; break; }
    }
    assert(foundConflict);
    // No repair session was ever requested for this code.
    assert(fx.sessionFactory.createCalls == 1);
}

// Mandated fail-closed regression: proposal-submission.v2 has no hash field
// at all, so a fake/malicious provider that still tries to send the old
// expected_original_hash (from before this contract, or a deliberate
// attempt to smuggle a self-typed hash back in) must be rejected - both on
// the primary attempt (proposal.unexpected_field, caught structurally by the
// parser before any per-change validation) and, if the repair session
// repeats the exact same mistake, on the repair attempt too. Unlike a
// validation-finding rejection, a structural parse failure never persists a
// ProposedChangeSet at all (see ProposedChangeSetParser's "malformed output
// is NEVER stored as a proposal" contract) - outcome.proposal stays empty.
void testFakeProviderStillSendingOldHashFieldIsFailClosedRejected()
{
    Fixture fx;
    TempDir workspace;
    workspace.writeFile("src/Main.qml", "import QtQuick\nItem {}\n");
    FilesystemWorkspaceToolExecutor realExecutor;
    auto registry = ToolRegistry::createDefaultReadOnly();
    assert(registry.success());
    AgentToolExecutionService toolExecution(
        std::move(registry.value()), realExecutor, fx.toolSessions, fx.toolAudits, true);
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    const std::string fabricatedHash(64, 'f');
    const std::string proposalJson =
        std::string(R"({"schemaVersion":"proposal-submission.v2","summary":"Update Main.qml","changes":[{)")
        + R"("change_id":"modify-main","operation":"modify",)"
        + R"("relative_path":"src/Main.qml","source_path":null,"expected_original_hash":")"
        + fabricatedHash
        + R"(","proposed_content":"import QtQuick\nItem { visible: true }\n"}]})";

    auto firstSession = std::make_unique<ScriptedProviderAgentSession>();
    firstSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "call-propose", proposalJson)));
    fx.sessionFactory.preparedSessions.push(std::move(firstSession));

    // The repair round (proposal.unexpected_field is repair-eligible) repeats
    // the exact same forbidden field, so the final outcome is deterministic.
    auto repairSession = std::make_unique<ScriptedProviderAgentSession>();
    repairSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "call-propose-2", proposalJson)));
    fx.sessionFactory.preparedSessions.push(std::move(repairSession));

    std::stop_source ss;
    const auto outcome = runner.run(makeRequest(workspace.path), ss.get_token(), nullptr, nullptr);

    assert(!outcome.success);
    assert(!outcome.cancelled);
    assert(outcome.repairAttempted);
    assert(outcome.errorCode == "proposal.unexpected_field");
    // Never laundered into a persisted (even Rejected) proposal record.
    assert(!outcome.proposal.has_value());
    assert(fx.sessionFactory.createCalls == 2);
}

// Mandated hash-preflight regression: a model that never calls
// workspace_hash_file at all (it structurally cannot supply a hash either,
// since proposal-submission.v2 has no such field) still gets a trusted hash
// for its Modify target - the Backend's own preflight
// (AgentToolTurnLoopRunner::preflightTrustedHashes) derives it from a real
// audited workspace_hash_file call the Backend makes itself, before
// validation ever runs, and the proposal reaches ReadyForReview in a single
// primary attempt with no repair needed.
void testModifyWithoutModelHashCallStillGetsTrustedHashFromPreflight()
{
    Fixture fx;
    TempDir workspace;
    workspace.writeFile("src/Main.qml", "import QtQuick\nItem {}\n");
    FilesystemWorkspaceToolExecutor realExecutor;
    auto registry = ToolRegistry::createDefaultReadOnly();
    assert(registry.success());
    AgentToolExecutionService toolExecution(
        std::move(registry.value()), realExecutor, fx.toolSessions, fx.toolAudits, true);
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    const std::string proposalJson =
        R"({"schemaVersion":"proposal-submission.v2","summary":"Update Main.qml","changes":[{)"
        R"("change_id":"modify-main","operation":"modify","relative_path":"src/Main.qml",)"
        R"("source_path":null,)"
        R"("proposed_content":"import QtQuick\nItem { visible: true }\n"}]})";

    // A single propose_change_set call, no workspace_hash_file call at all -
    // the model never even attempts to establish evidence itself.
    auto session = std::make_unique<ScriptedProviderAgentSession>();
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "call-propose", proposalJson)));
    fx.sessionFactory.preparedSessions.push(std::move(session));

    std::stop_source ss;
    const auto outcome = runner.run(makeRequest(workspace.path), ss.get_token(), nullptr, nullptr);

    assert(outcome.success);
    assert(outcome.proposal.has_value());
    assert(outcome.proposal->state == ChangeSetState::ReadyForReview);
    assert(outcome.proposal->changes.size() == 1);
    assert(outcome.proposal->changes.front().expectedOriginalHash.has_value());
    assert(*outcome.proposal->changes.front().expectedOriginalHash
        == sha256Hex("import QtQuick\nItem {}\n"));
    // One provider terminal call plus one Backend-owned audited preflight.
    assert(outcome.totalToolCalls == 2);
    assert(outcome.successfulInspectionCalls == 1);
    assert(outcome.totalReturnedBytes > 0);
    const auto sessions = fx.toolSessions.listForRun("run-1");
    assert(sessions.success() && sessions.value().size() == 1);
    assert(sessions.value().front().totalCalls == 1);
    assert(sessions.value().front().totalReturnedBytes == outcome.totalReturnedBytes);
    const auto audits = fx.toolAudits.listForSession(
        sessions.value().front().sessionId);
    assert(audits.success() && audits.value().size() == 1);
    assert(audits.value().front().toolName == "workspace_hash_file");
    // No repair retry needed: the Backend's own preflight supplied evidence
    // the model never had to ask for.
    assert(fx.sessionFactory.createCalls == 1);
}

// Mandated hash-preflight dispatch test: Create must never trigger a
// preflight hash call; Modify and Rename's source must; two changes that
// resolve to the same subject path (here, a Modify's relativePath and a
// Rename's sourcePath both naming "shared.md") must dedupe to exactly one
// workspace_hash_file call, not two - and if Rename incorrectly used its own
// relativePath (the rename target) instead of sourcePath, this would show up
// as two distinct hashed paths instead of one shared one. Uses the fixture's
// AlwaysSucceedExecutor (no real filesystem) purely to observe the
// preflight's own dispatch decisions - the resulting proposal's own
// ReadyForReview/Rejected outcome is irrelevant here.
void testHashPreflightSkipsCreateAndDedupesSharedSubjectPaths()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    const std::string proposalJson =
        R"({"schemaVersion":"proposal-submission.v2","summary":"Mixed ops","changes":[)"
        R"({"change_id":"create-1","operation":"create","relative_path":"new.md",)"
        R"("source_path":null,"proposed_content":"hi"},)"
        R"({"change_id":"modify-1","operation":"modify","relative_path":"shared.md",)"
        R"("source_path":null,"proposed_content":"updated"},)"
        R"({"change_id":"rename-1","operation":"rename","relative_path":"target.md",)"
        R"("source_path":"shared.md","proposed_content":null}]})";

    auto session = std::make_unique<ScriptedProviderAgentSession>();
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "call-propose", proposalJson)));
    fx.sessionFactory.preparedSessions.push(std::move(session));

    std::stop_source ss;
    const auto outcome = runner.run(makeRequest(workspace.path), ss.get_token(), nullptr, nullptr);
    (void)outcome;

    int hashCallCount = 0;
    for (const auto& [toolName, argsJson] : fx.executor.capturedCalls) {
        if (toolName != "workspace_hash_file") continue;
        ++hashCallCount;
        assert(argsJson.find(R"("path":"shared.md")") != std::string::npos);
    }
    assert(hashCallCount == 1);
}

void testHashPreflightRespectsTotalToolCallBudget()
{
    Fixture fx;
    TempDir workspace;
    workspace.writeFile("existing.txt", "baseline");
    FilesystemWorkspaceToolExecutor realExecutor;
    auto registry = ToolRegistry::createDefaultReadOnly();
    assert(registry.success());
    AgentToolExecutionService toolExecution(
        std::move(registry.value()), realExecutor, fx.toolSessions, fx.toolAudits, true);
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    const std::string proposalJson =
        R"({"schemaVersion":"proposal-submission.v2","summary":"Modify","changes":[)"
        R"({"change_id":"modify","operation":"modify","relative_path":"existing.txt",)"
        R"("source_path":null,"proposed_content":"updated"}]})";
    auto session = std::make_unique<ScriptedProviderAgentSession>();
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "terminal", proposalJson)));
    fx.sessionFactory.preparedSessions.push(std::move(session));

    auto request = makeRequest(workspace.path);
    request.sessionRequest.maxTotalToolCalls = 1; // consumed by terminal call
    std::stop_source ss;
    const auto outcome = runner.run(std::move(request), ss.get_token(), nullptr, nullptr);

    assert(!outcome.success);
    assert(outcome.errorCode == "run.tool_call_budget_exceeded");
    assert(outcome.totalToolCalls == 1);
    const auto sessions = fx.toolSessions.listForRun("run-1");
    assert(sessions.success() && sessions.value().size() == 1);
    assert(sessions.value().front().state == AgentToolSessionState::Interrupted);
    assert(sessions.value().front().totalCalls == 0);
    const auto audits = fx.toolAudits.listForSession(sessions.value().front().sessionId);
    assert(audits.success() && audits.value().empty());
}

void testHashPreflightHonoursCancellationBeforeFilesystemDispatch()
{
    Fixture fx;
    TempDir workspace;
    workspace.writeFile("existing.txt", "baseline");
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    const std::string proposalJson =
        R"({"schemaVersion":"proposal-submission.v2","summary":"Modify","changes":[)"
        R"({"change_id":"modify","operation":"modify","relative_path":"existing.txt",)"
        R"("source_path":null,"proposed_content":"updated"}]})";
    auto session = std::make_unique<ScriptedProviderAgentSession>();
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "terminal", proposalJson)));
    fx.sessionFactory.preparedSessions.push(std::move(session));

    std::stop_source ss;
    ss.request_stop();
    const auto outcome = runner.run(
        makeRequest(workspace.path), ss.get_token(), nullptr, nullptr);

    assert(!outcome.success);
    assert(outcome.cancelled);
    assert(fx.executor.callCount == 0);
}

void testRepairRehashesSamePathWithAttemptScopedEvidence()
{
    Fixture fx;
    TempDir workspace;
    workspace.writeFile("existing.txt", "baseline");
    FilesystemWorkspaceToolExecutor realExecutor;
    auto registry = ToolRegistry::createDefaultReadOnly();
    assert(registry.success());
    AgentToolExecutionService toolExecution(
        std::move(registry.value()), realExecutor, fx.toolSessions, fx.toolAudits, true);
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    const std::string proposalJson =
        R"({"schemaVersion":"proposal-submission.v2","summary":"Modify","changes":[)"
        R"({"change_id":"modify","operation":"modify","relative_path":"existing.txt",)"
        R"("source_path":null,"proposed_content":"updated"}]})";
    auto primary = std::make_unique<ScriptedProviderAgentSession>();
    primary->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "primary", proposalJson)));
    fx.sessionFactory.preparedSessions.push(std::move(primary));
    auto repair = std::make_unique<ScriptedProviderAgentSession>();
    repair->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "repair", proposalJson)));
    fx.sessionFactory.preparedSessions.push(std::move(repair));

    auto request = makeRequest(workspace.path);
    request.proposalTemplate.policy.allowedRoots = {fs::path("different-root")};
    std::stop_source ss;
    const auto outcome = runner.run(std::move(request), ss.get_token(), nullptr, nullptr);

    assert(!outcome.success);
    assert(outcome.repairAttempted);
    assert(outcome.totalToolCalls == 4);
    assert(outcome.successfulInspectionCalls == 2);
    const auto sessions = fx.toolSessions.listForRun("run-1");
    assert(sessions.success() && sessions.value().size() == 1);
    assert(sessions.value().front().totalCalls == 2);
    const auto audits = fx.toolAudits.listForSession(sessions.value().front().sessionId);
    assert(audits.success() && audits.value().size() == 2);
    assert(std::all_of(audits.value().begin(), audits.value().end(),
        [](const AgentToolAuditRecord& audit) {
            return audit.toolName == "workspace_hash_file";
        }));
}

std::string typedNoChangesJson(
    std::string path = "README.md", std::string requirementId = "req-1")
{
    return std::string(R"({"schemaVersion":"manifest-worker-no-changes.v1","reason":"already_satisfied_in_baseline","summary":"The assigned requirement is already satisfied.","criterionEvidence":[{"requirementId":")")
        + requirementId
        + R"(","inspectedPath":")"
        + path
        + R"(","observation":"The required repository state is present."}]})";
}

void testTypedWorkerNoChangesRequiresSameAttemptEvidence()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto primary = std::make_unique<ScriptedProviderAgentSession>();
    primary->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolCallResponse(0, "read-primary", "workspace_read_file", R"({"path":"README.md"})")));
    primary->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolCallResponse(1, "no-op-primary", kSubmitTaskNoChangesToolName,
            typedNoChangesJson("other.md"))));
    fx.sessionFactory.preparedSessions.push(std::move(primary));

    // Fresh repair cites the primary's README.md but performs no inspection
    // of its own. Attempt-scoped evidence must reset and reject it.
    auto repair = std::make_unique<ScriptedProviderAgentSession>();
    repair->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolCallResponse(0, "no-op-repair", kSubmitTaskNoChangesToolName,
            typedNoChangesJson("README.md"))));
    fx.sessionFactory.preparedSessions.push(std::move(repair));

    auto request = makeRequest(workspace.path);
    request.requireTypedNoChangesTerminal = true;
    request.requiredRequirementIds = {"req-1"};
    request.sessionRequest.tools.push_back(submitTaskNoChangesToolDefinition());
    const auto outcome = runner.run(std::move(request), {}, nullptr, nullptr);
    assert(!outcome.success);
    assert(outcome.repairAttempted);
    assert(outcome.errorCode == "worker_no_changes.missing_inspection");
    assert(!outcome.proposal.has_value());
}

void testTypedWorkerNoChangesSucceedsWithoutEmptyProposalRecord()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);
    auto session = std::make_unique<ScriptedProviderAgentSession>();
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolCallResponse(0, "read", "workspace_read_file", R"({"path":"README.md"})")));
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolCallResponse(1, "no-op", kSubmitTaskNoChangesToolName,
            typedNoChangesJson())));
    fx.sessionFactory.preparedSessions.push(std::move(session));
    auto request = makeRequest(workspace.path);
    request.requireTypedNoChangesTerminal = true;
    request.requiredRequirementIds = {"req-1"};
    request.sessionRequest.tools.push_back(submitTaskNoChangesToolDefinition());
    const auto outcome = runner.run(std::move(request), {}, nullptr, nullptr);
    assert(outcome.success);
    assert(outcome.workerNoChanges.has_value());
    assert(!outcome.proposal.has_value());
    assert(outcome.workerNoChanges->inspectedRelativePaths
        == std::vector<std::string> {"readme.md"});
}

void testCreateMissingOutputCannotBecomeTypedNoChanges()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);
    auto session = std::make_unique<ScriptedProviderAgentSession>();
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolCallResponse(0, "stat", "workspace_stat_path", R"({"path":"missing.txt"})")));
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolCallResponse(1, "no-op", kSubmitTaskNoChangesToolName,
            typedNoChangesJson("missing.txt"))));
    fx.sessionFactory.preparedSessions.push(std::move(session));
    auto request = makeRequest(workspace.path);
    request.requireTypedNoChangesTerminal = true;
    request.requiredRequirementIds = {"req-1"};
    request.requiredOutputsByRequirement["req-1"] = {"missing.txt"};
    request.sessionRequest.tools.push_back(submitTaskNoChangesToolDefinition());
    const auto outcome = runner.run(std::move(request), {}, nullptr, nullptr);
    assert(!outcome.success);
    assert(outcome.errorCode == "worker_no_changes.required_output_missing");
    assert(!outcome.workerNoChanges.has_value());
}

void testMissingInspectOutputRepairsToConcreteCreateProposal()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto primary = std::make_unique<ScriptedProviderAgentSession>();
    primary->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolCallResponse(0, "no-op-primary", kSubmitTaskNoChangesToolName,
            typedNoChangesJson("missing.txt"))));
    fx.sessionFactory.preparedSessions.push(std::move(primary));

    ProviderAgentSessionRequest capturedRepairRequest;
    auto repair = std::make_unique<ScriptedProviderAgentSession>();
    repair->capturedBeginRequest = &capturedRepairRequest;
    repair->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "create-repair",
            R"({"schemaVersion":"proposal-submission.v2",)"
            R"("summary":"Create the required missing output.","changes":[)"
            R"({"change_id":"create-required","operation":"create",)"
            R"("relative_path":"missing.txt","source_path":null,)"
            R"("proposed_content":"required content"}]})")));
    fx.sessionFactory.preparedSessions.push(std::move(repair));

    auto request = makeRequest(workspace.path);
    request.requireTypedNoChangesTerminal = true;
    request.requiredRequirementIds = {"req-1"};
    request.requiredOutputsByRequirement["req-1"] = {"missing.txt"};
    request.verificationKindsByRequirement["req-1"] =
        ExecutionVerificationKind::InspectPaths;
    request.proposalTemplate.policy.allowedRoots = {fs::path("missing.txt")};
    request.sessionRequest.tools.push_back(submitTaskNoChangesToolDefinition());
    const auto outcome = runner.run(std::move(request), {}, nullptr, nullptr);

    assert(outcome.success);
    assert(outcome.repairAttempted);
    assert(outcome.proposal.has_value());
    assert(outcome.proposal->state == ChangeSetState::ReadyForReview);
    assert(!outcome.workerNoChanges.has_value());
    assert(fx.sessionFactory.createCalls == 2);
    assert(capturedRepairRequest.userContent.find(
        "worker_no_changes.required_output_missing") != std::string::npos);
    assert(capturedRepairRequest.userContent.find(
        "call propose_change_set with a create operation") != std::string::npos);
}

void testRepeatedNoChangesForMissingInspectOutputFailsWithoutProposal()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    for (const auto* callId : {"no-op-primary", "no-op-repair"}) {
        auto session = std::make_unique<ScriptedProviderAgentSession>();
        session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
            toolCallResponse(0, callId, kSubmitTaskNoChangesToolName,
                typedNoChangesJson("missing.txt"))));
        fx.sessionFactory.preparedSessions.push(std::move(session));
    }

    auto request = makeRequest(workspace.path);
    request.requireTypedNoChangesTerminal = true;
    request.requiredRequirementIds = {"req-1"};
    request.requiredOutputsByRequirement["req-1"] = {"missing.txt"};
    request.verificationKindsByRequirement["req-1"] =
        ExecutionVerificationKind::InspectPaths;
    request.sessionRequest.tools.push_back(submitTaskNoChangesToolDefinition());
    const auto outcome = runner.run(std::move(request), {}, nullptr, nullptr);

    assert(!outcome.success);
    assert(outcome.repairAttempted);
    assert(outcome.errorCode == "worker_no_changes.required_output_missing");
    assert(!outcome.proposal.has_value());
    assert(!outcome.workerNoChanges.has_value());
    assert(fx.sessionFactory.createCalls == 2);
    // One Backend-owned stat for each fresh terminal attempt.
    assert(fx.executor.callCount == 2);
}

void testNoChangesRequiresEvidenceForEveryInspectOutput()
{
    Fixture fx;
    TempDir workspace;
    workspace.writeFile("README.md", "ready");
    workspace.writeFile("LICENSE", "ready");
    FilesystemWorkspaceToolExecutor realExecutor;
    auto registry = ToolRegistry::createDefaultReadOnly();
    assert(registry.success());
    AgentToolExecutionService toolExecution(
        std::move(registry.value()), realExecutor,
        fx.toolSessions, fx.toolAudits, true);
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto session = std::make_unique<ScriptedProviderAgentSession>();
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolCallResponse(0, "read", "workspace_read_file",
            R"({"path":"README.md"})")));
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolCallResponse(1, "no-op", kSubmitTaskNoChangesToolName,
            typedNoChangesJson())));
    fx.sessionFactory.preparedSessions.push(std::move(session));

    auto request = makeRequest(workspace.path);
    request.requireTypedNoChangesTerminal = true;
    request.requiredRequirementIds = {"req-1"};
    request.requiredOutputsByRequirement["req-1"] = {"README.md", "LICENSE"};
    request.verificationKindsByRequirement["req-1"] =
        ExecutionVerificationKind::InspectPaths;
    request.sessionRequest.tools.push_back(submitTaskNoChangesToolDefinition());
    const auto outcome = runner.run(std::move(request), {}, nullptr, nullptr);

    assert(!outcome.success);
    assert(outcome.errorCode == "worker_no_changes.required_output_uninspected");
    assert(!outcome.workerNoChanges.has_value());
}

void testCommandBackedRequirementsRemainForBackendVerifier()
{
    Fixture fx;
    TempDir workspace;
    workspace.writeFile("README.md", "ready");
    FilesystemWorkspaceToolExecutor realExecutor;
    auto registry = ToolRegistry::createDefaultReadOnly();
    assert(registry.success());
    AgentToolExecutionService toolExecution(
        std::move(registry.value()), realExecutor,
        fx.toolSessions, fx.toolAudits, true);
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto session = std::make_unique<ScriptedProviderAgentSession>();
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolCallResponse(0, "read", "workspace_read_file",
            R"({"path":"README.md"})")));
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolCallResponse(1, "no-op", kSubmitTaskNoChangesToolName,
            typedNoChangesJson("README.md", "inspect"))));
    fx.sessionFactory.preparedSessions.push(std::move(session));

    auto request = makeRequest(workspace.path);
    request.requireTypedNoChangesTerminal = true;
    request.requiredRequirementIds = {"inspect", "build", "lint"};
    request.requiredOutputsByRequirement["inspect"] = {"README.md"};
    request.requiredOutputsByRequirement["build"] = {"build-log"};
    request.requiredOutputsByRequirement["lint"] = {"qml-lint-log"};
    request.verificationKindsByRequirement["inspect"] =
        ExecutionVerificationKind::InspectPaths;
    request.verificationKindsByRequirement["build"] =
        ExecutionVerificationKind::CMakeBuild;
    request.verificationKindsByRequirement["lint"] =
        ExecutionVerificationKind::QmlLint;
    request.sessionRequest.tools.push_back(submitTaskNoChangesToolDefinition());
    const auto outcome = runner.run(std::move(request), {}, nullptr, nullptr);

    assert(outcome.success);
    assert(outcome.workerNoChanges.has_value());
    assert(outcome.workerNoChanges->criterionEvidence.size() == 1);
    assert(outcome.workerNoChanges->criterionEvidence.front().requirementId == "inspect");
    assert(fx.sessionFactory.createCalls == 1);
}

// ---------------------------------------------------------------------
// AG-15: same-attempt required-read gate and audited scope expansion.
// ---------------------------------------------------------------------

// A worker that reads every one of its task's requiredReadPathKeys before
// proposing is accepted with no repair needed - the ordinary, unpenalized
// path.
void testSameAttemptReadGatePassesWhenRequiredPathRead()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto session = std::make_unique<ScriptedProviderAgentSession>();
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolUseResponse(0, "call-1")));
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(1, "call-2", kSimpleProposal)));
    fx.sessionFactory.preparedSessions.push(std::move(session));

    auto request = makeRequest(workspace.path);
    // AlwaysSucceedExecutor always reports relativePath "README.md" for a
    // read-style call - see its own doc comment.
    request.requiredReadPathKeys = {"readme.md"};
    const auto outcome = runner.run(std::move(request), {}, nullptr, nullptr);

    assert(outcome.success);
    assert(outcome.proposal.has_value());
    assert(outcome.proposal->state == ChangeSetState::ReadyForReview);
    assert(!outcome.repairAttempted);
    assert(fx.sessionFactory.createCalls == 1);
}

// A worker that proposes without reading a required path gets exactly one
// bounded repair; if the repair session actually reads it, the repaired
// proposal is accepted.
void testSameAttemptReadGateTriggersRepairThenSucceeds()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto firstSession = std::make_unique<ScriptedProviderAgentSession>();
    firstSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "call-1", kSimpleProposal)));
    fx.sessionFactory.preparedSessions.push(std::move(firstSession));

    SessionCallLog repairLog;
    auto repairSession = std::make_unique<ScriptedProviderAgentSession>();
    repairSession->log = &repairLog;
    repairSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolUseResponse(0, "call-1")));
    repairSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(1, "call-2", kSimpleProposal)));
    fx.sessionFactory.preparedSessions.push(std::move(repairSession));

    auto request = makeRequest(workspace.path);
    request.requiredReadPathKeys = {"readme.md"};
    const auto outcome = runner.run(std::move(request), {}, nullptr, nullptr);

    assert(outcome.success);
    assert(outcome.repairAttempted);
    assert(outcome.proposal.has_value());
    assert(outcome.proposal->state == ChangeSetState::ReadyForReview);
    assert(fx.sessionFactory.createCalls == 2);
    assert(repairLog.beginCalls == 1);
}

// A worker whose one-shot repair STILL never reads the required path is a
// hard, terminal failure with the stable diagnostic code - no second repair
// is scheduled, and the durably-persisted primary proposal is superseded by
// the (also-gated) repaired one rather than silently accepted.
void testSameAttemptReadGateFailsAfterRepairStillMissing()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto firstSession = std::make_unique<ScriptedProviderAgentSession>();
    firstSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "call-1", kSimpleProposal)));
    fx.sessionFactory.preparedSessions.push(std::move(firstSession));

    auto repairSession = std::make_unique<ScriptedProviderAgentSession>();
    repairSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "call-1", kSimpleProposal)));
    fx.sessionFactory.preparedSessions.push(std::move(repairSession));

    auto request = makeRequest(workspace.path);
    request.requiredReadPathKeys = {"readme.md"};
    const auto outcome = runner.run(std::move(request), {}, nullptr, nullptr);

    assert(!outcome.success);
    assert(outcome.repairAttempted);
    assert(outcome.errorCode == "workspace_scope.required_read_not_inspected");
    assert(fx.sessionFactory.createCalls == 2);
}

// A worker that discovers a genuine additional dependency mid-attempt can
// call request_scope_expansion for a required_read role; the approval
// widens the effective required-read set for THIS attempt, so it must still
// actually read the newly-approved path before its proposal is accepted -
// an unread approved expansion triggers the exact same repair/failure path
// as a statically-declared required-read path.
void testScopeExpansionApprovedRequiredReadMustBeReadBeforeAccepted()
{
    PathEchoFixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);
    InMemoryRunWorkspaceScopeRepository scopeRepository;
    RunWorkspaceScopeService scopeService(scopeRepository);
    ExecutionManifest scopeManifest;
    scopeManifest.runId = "run-1";
    ExecutionManifestTask scopeTask;
    scopeTask.id = "task-1";
    scopeTask.objective = "Test bounded expansion";
    scopeTask.role = "worker";
    scopeTask.requestedCapability = WorkspaceCapability::Propose;
    scopeManifest.tasks = {std::move(scopeTask)};
    WorkspaceBaselineSnapshot scopeBaseline;
    scopeBaseline.files.push_back({"extra.md", 5, "hash-extra"});
    assert(scopeService.freezeForManifest("run-1", scopeManifest, scopeBaseline).success());

    // Primary attempt: requests the expansion, then proposes without ever
    // reading the newly-approved path - must be rejected into repair.
    auto firstSession = std::make_unique<ScriptedProviderAgentSession>();
    firstSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolCallResponse(0, "read-before-expansion", "workspace_read_file",
            R"({"path":"extra.md"})")));
    firstSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolCallResponse(1, "expand-1", kRequestScopeExpansionToolName,
            R"({"relativePath":"extra.md","role":"required_read","reason":"needed for context"})")));
    firstSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(2, "call-1", kSimpleProposal)));
    fx.sessionFactory.preparedSessions.push(std::move(firstSession));

    // Repair attempt: re-requests the expansion (its own attempt-scoped
    // approvals start empty too), actually reads extra.md this time, then
    // proposes successfully.
    auto repairSession = std::make_unique<ScriptedProviderAgentSession>();
    repairSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolCallResponse(0, "expand-2", kRequestScopeExpansionToolName,
            R"({"relativePath":"extra.md","role":"required_read","reason":"needed for context"})")));
    repairSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolCallResponse(1, "read-1", "workspace_read_file", R"({"path":"extra.md"})")));
    repairSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(2, "call-2", kSimpleProposal)));
    fx.sessionFactory.preparedSessions.push(std::move(repairSession));

    auto request = makeRequest(workspace.path);
    request.runWorkspaceScopeService = &scopeService;
    request.runId = "run-1";
    request.taskId = "task-1";
    request.scopeBaseline = scopeBaseline;
    request.policy.allowedRoots = {"README.md"};
    request.policy.allowedRootsAreExactPaths = true;
    request.scopeExpansionPolicy = AgentToolExecutionService::buildPolicy(workspace.path);
    const auto outcome = runner.run(std::move(request), {}, nullptr, nullptr);

    assert(outcome.success);
    assert(outcome.repairAttempted);
    assert(outcome.proposal.has_value());
    assert(outcome.proposal->state == ChangeSetState::ReadyForReview);

    const auto expansions = scopeService.listExpansionsForRun("run-1");
    assert(expansions.success());
    assert(expansions.value().size() == 1);
    assert(expansions.value().front().relativePath == "extra.md");
    assert(expansions.value().front().taskId == "task-1");
    assert(fx.executor.authorized == std::vector<bool>({false, true}));
}

void testRunWideBudgetPreventsWorkerThirteenthProviderRequest()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    SessionCallLog log;
    ProviderAgentSessionRequest capturedBeginRequest;
    auto session = std::make_unique<ScriptedProviderAgentSession>();
    session->log = &log;
    session->capturedBeginRequest = &capturedBeginRequest;
    for (std::size_t i = 0; i < 13; ++i) {
        session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
            toolUseResponse(i, "call-" + std::to_string(i))));
    }
    fx.sessionFactory.preparedSessions.push(std::move(session));

    ProviderExecutionBudgetPolicy policy;
    policy.maxTurns = 12;
    policy.maxRequestTimeMs = 25;
    auto guard = std::make_shared<ProviderSessionBudgetGuard>(policy);
    auto request = makeRequest(workspace.path);
    request.providerBudgetGuard = guard;

    const auto outcome = runner.run(std::move(request), {}, nullptr, nullptr);
    assert(!outcome.success);
    assert(outcome.errorCode == "provider_budget.turn_limit_reached");
    assert(log.beginCalls == 1);
    assert(log.submitCalls == 11);
    assert(guard->usage().turnsStarted == 12);
    assert(guard->usage().turnsCompleted == 12);
    assert(capturedBeginRequest.turnTimeout == std::chrono::milliseconds(25));
}

// A scope-expansion request for a globally denied path (a secret file, here)
// is rejected by RunWorkspaceScopeService and never becomes part of the
// effective required-read set - the turn loop keeps running (the tool call
// itself is not terminal) and a proposal needing no other reads still
// succeeds normally.
void testScopeExpansionRejectsDeniedPathAndProposalStillSucceeds()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);
    InMemoryRunWorkspaceScopeRepository scopeRepository;
    RunWorkspaceScopeService scopeService(scopeRepository);
    ExecutionManifest scopeManifest;
    scopeManifest.runId = "run-1";
    ExecutionManifestTask scopeTask;
    scopeTask.id = "task-1";
    scopeTask.objective = "Test denied expansion";
    scopeTask.role = "worker";
    scopeTask.requestedCapability = WorkspaceCapability::Propose;
    scopeManifest.tasks = {std::move(scopeTask)};
    assert(scopeService.freezeForManifest(
        "run-1", scopeManifest, WorkspaceBaselineSnapshot {}).success());

    auto session = std::make_unique<ScriptedProviderAgentSession>();
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolCallResponse(0, "expand-1", kRequestScopeExpansionToolName,
            R"({"relativePath":".env","role":"required_read","reason":"looks interesting"})")));
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(1, "call-1", kSimpleProposal)));
    fx.sessionFactory.preparedSessions.push(std::move(session));

    auto request = makeRequest(workspace.path);
    request.runWorkspaceScopeService = &scopeService;
    request.runId = "run-1";
    request.taskId = "task-1";
    const auto outcome = runner.run(std::move(request), {}, nullptr, nullptr);

    assert(outcome.success);
    assert(!outcome.repairAttempted);

    const auto expansions = scopeService.listExpansionsForRun("run-1");
    assert(expansions.success());
    assert(expansions.value().empty());
}

// ---------------------------------------------------------------------
// Worker capacity fairness: reservation owner accounting and Backend-forced
// terminal turns (AgentToolTurnLoopRequest::budgetReservationOwner /
// forceTerminalOnFinalTaskTurn).
// ---------------------------------------------------------------------

std::shared_ptr<ProviderSessionBudgetGuard> reservedGuard(
    std::uint64_t maxTurns, const std::string& owner, std::uint64_t reserved)
{
    ProviderExecutionBudgetPolicy policy;
    policy.maxTurns = maxTurns;
    auto guard = std::make_shared<ProviderSessionBudgetGuard>(policy);
    assert(guard->reserveTurns({{owner, reserved}}).success());
    return guard;
}

AgentToolTurnLoopRequest makeForcedTerminalRequest(
    const fs::path& workspaceRoot, std::shared_ptr<ProviderSessionBudgetGuard> guard)
{
    auto request = makeRequest(workspaceRoot);
    request.providerBudgetGuard = std::move(guard);
    request.budgetReservationOwner = "owner";
    request.forceTerminalOnFinalTaskTurn = true;
    return request;
}

// The turn that is the LAST one the owner may start is Backend-forced to a
// terminal-only, single-call policy; every earlier turn stays unrestricted.
// With only propose_change_set advertised, exactly that one tool is allowed
// (the adapters then force it by name); with the typed no-op terminal also
// advertised, both terminals are allowed (required/any within that set).
void testFinalOwnerTurnIsForcedToTerminalOnlyPolicy()
{
    {
        Fixture fx;
        TempDir workspace;
        auto toolExecution = fx.makeToolExecution();
        auto changeProposals = fx.makeChangeProposals();
        AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

        std::vector<std::optional<NextTurnToolPolicy>> policies;
        auto session = std::make_unique<ScriptedProviderAgentSession>();
        session->capturedPolicies = &policies;
        session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
            toolUseResponse(0, "c0")));
        session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
            toolUseResponse(1, "c1")));
        session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
            proposeChangeSetToolCallResponse(2, "c2", kSimpleProposal)));
        fx.sessionFactory.preparedSessions.push(std::move(session));

        const auto outcome = runner.run(
            makeForcedTerminalRequest(workspace.path, reservedGuard(3, "owner", 3)),
            {}, nullptr, nullptr);
        assert(outcome.success);
        assert(policies.size() == 2);
        assert(!policies[0].has_value());   // 2 turns still available after this one
        assert(policies[1].has_value());    // the owner's last turn
        assert(policies[1]->allowedToolNames
            == std::vector<std::string>({kProposeChangeSetToolName}));
        assert(policies[1]->disallowParallelToolCalls);
    }
    {
        Fixture fx;
        TempDir workspace;
        auto toolExecution = fx.makeToolExecution();
        auto changeProposals = fx.makeChangeProposals();
        AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

        std::vector<std::optional<NextTurnToolPolicy>> policies;
        auto session = std::make_unique<ScriptedProviderAgentSession>();
        session->capturedPolicies = &policies;
        session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
            toolUseResponse(0, "c0")));
        session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
            proposeChangeSetToolCallResponse(1, "c1", kSimpleProposal)));
        fx.sessionFactory.preparedSessions.push(std::move(session));

        auto request = makeForcedTerminalRequest(workspace.path, reservedGuard(2, "owner", 2));
        request.requireTypedNoChangesTerminal = true;
        request.sessionRequest.tools.push_back(submitTaskNoChangesToolDefinition());
        const auto outcome = runner.run(std::move(request), {}, nullptr, nullptr);
        assert(outcome.success);
        assert(policies.size() == 1 && policies[0].has_value());
        // The typed no-change terminal is advertised, yet the LAST turn still allows only
        // propose_change_set: a multi-tool "any" policy is not enforceable on the wire (Claude
        // kept calling inspection tools on it in Real Runs ee528eed and 8b23fbba), a single tool
        // is forced by name on both adapters. The shape of the failed task-c: no
        // plannedCreatePaths, two terminals advertised.
        assert(policies[0]->allowedToolNames
            == std::vector<std::string>({kProposeChangeSetToolName}));
        assert(policies[0]->disallowParallelToolCalls);
    }
}

// Inspect mode has exactly one terminal: its forced final turn allows only
// submit_task_verification (named on the wire), never a proposal tool.
void testInspectModeForcesOnlyTheVerificationTerminalOnItsFinalTurn()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    std::vector<std::optional<NextTurnToolPolicy>> policies;
    auto session = std::make_unique<ScriptedProviderAgentSession>();
    session->capturedPolicies = &policies;
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolUseResponse(0, "c0")));
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolCallResponse(1, "c1", kSubmitTaskVerificationToolName,
            R"({"schemaVersion":"manifest-task-verification.v1","summary":"README inspected.",)"
            R"("criterionEvidence":[{"requirementId":"req","inspectedPath":"README.md",)"
            R"("observation":"The file exists.","status":"passed"}]})")));
    fx.sessionFactory.preparedSessions.push(std::move(session));

    auto request = makeForcedTerminalRequest(workspace.path, reservedGuard(2, "owner", 2));
    request.mode = AgentToolTurnLoopMode::Inspect;
    request.sessionRequest.tools = {submitTaskVerificationToolDefinition()};
    request.requiredRequirementIds = {"req"};
    const auto outcome = runner.run(std::move(request), {}, nullptr, nullptr);
    assert(outcome.success);
    assert(outcome.taskVerification.has_value());
    assert(policies.size() == 1 && policies[0].has_value());
    assert(policies[0]->allowedToolNames
        == std::vector<std::string>({kSubmitTaskVerificationToolName}));
    assert(policies[0]->disallowParallelToolCalls);
}

// A non-terminal (inspection) tool call on a forced turn is rejected before it
// is executed or counted as an executed call, with a stable code.
void testNonTerminalToolOnForcedTurnIsRejectedBeforeExecution()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto session = std::make_unique<ScriptedProviderAgentSession>();
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolUseResponse(0, "c0")));
    // The forced final turn misbehaves: another workspace read.
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolUseResponse(1, "c1")));
    fx.sessionFactory.preparedSessions.push(std::move(session));

    const auto outcome = runner.run(
        makeForcedTerminalRequest(workspace.path, reservedGuard(2, "owner", 2)),
        {}, nullptr, nullptr);
    assert(!outcome.success);
    assert(outcome.errorCode == kWorkerForcedTerminalViolationCode);
    assert(outcome.totalToolCalls == 1);
    assert(outcome.successfulInspectionCalls == 1);
    assert(!outcome.proposal.has_value());
}

// When the task's whole allowance is ONE turn, its first turn is already the
// last one: the policy has to ride on beginSession via firstTurnToolPolicy.
void testFirstTurnIsForcedWhenTheTaskAllocationIsOneTurn()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    ProviderAgentSessionRequest capturedBegin;
    auto session = std::make_unique<ScriptedProviderAgentSession>();
    session->capturedBeginRequest = &capturedBegin;
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "c0", kSimpleProposal)));
    fx.sessionFactory.preparedSessions.push(std::move(session));

    const auto outcome = runner.run(
        makeForcedTerminalRequest(workspace.path, reservedGuard(1, "owner", 1)),
        {}, nullptr, nullptr);
    assert(outcome.success);
    assert(capturedBegin.firstTurnToolPolicy.has_value());
    assert(capturedBegin.firstTurnToolPolicy->allowedToolNames
        == std::vector<std::string>({kProposeChangeSetToolName}));
    assert(capturedBegin.firstTurnToolPolicy->disallowParallelToolCalls);
}

// Without the force flag nothing changes for legacy callers: no policy is
// ever attached, no matter how few turns remain.
void testLegacyCallersNeverReceiveAForcedTerminalPolicy()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    std::vector<std::optional<NextTurnToolPolicy>> policies;
    ProviderAgentSessionRequest capturedBegin;
    auto session = std::make_unique<ScriptedProviderAgentSession>();
    session->capturedPolicies = &policies;
    session->capturedBeginRequest = &capturedBegin;
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolUseResponse(0, "c0")));
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(1, "c1", kSimpleProposal)));
    fx.sessionFactory.preparedSessions.push(std::move(session));

    auto request = makeForcedTerminalRequest(workspace.path, reservedGuard(2, "owner", 2));
    request.forceTerminalOnFinalTaskTurn = false;
    const auto outcome = runner.run(std::move(request), {}, nullptr, nullptr);
    assert(outcome.success);
    assert(!capturedBegin.firstTurnToolPolicy.has_value());
    assert(policies.size() == 1 && !policies[0].has_value());
}

// The bounded repair after rejected CONCRETE work may never be answered with a
// no-op, so its forced terminal set is exactly propose_change_set - the one
// case in which a single named tool is forced even though the typed no-op
// terminal is advertised.
void testRepairAfterRejectedConcreteWorkForcesTheSingleProposalTool()
{
    Fixture fx;
    TempDir workspace;
    workspace.writeFile("README.md", "existing content");
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto firstSession = std::make_unique<ScriptedProviderAgentSession>();
    firstSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "call-1",
            R"({"schemaVersion":"proposal-submission.v2","summary":"Create README","changes":[{)"
            R"("change_id":"create-readme","operation":"create",)"
            R"("relative_path":"README.md","source_path":null,"proposed_content":"new"}]})")));
    fx.sessionFactory.preparedSessions.push(std::move(firstSession));

    ProviderAgentSessionRequest repairBegin;
    auto repairSession = std::make_unique<ScriptedProviderAgentSession>();
    repairSession->capturedBeginRequest = &repairBegin;
    repairSession->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "call-2", kSimpleProposal)));
    fx.sessionFactory.preparedSessions.push(std::move(repairSession));

    auto request = makeForcedTerminalRequest(workspace.path, reservedGuard(2, "owner", 2));
    request.requireTypedNoChangesTerminal = true;
    request.sessionRequest.tools.push_back(submitTaskNoChangesToolDefinition());
    const auto outcome = runner.run(std::move(request), {}, nullptr, nullptr);
    assert(outcome.success && outcome.repairAttempted);
    assert(repairBegin.firstTurnToolPolicy.has_value());
    assert(repairBegin.firstTurnToolPolicy->allowedToolNames
        == std::vector<std::string>({kProposeChangeSetToolName}));
}

// ---------------------------------------------------------------------
// Worker no-op invariants and the final-turn guidance (3b), and the repair
// reservation owner (3a).
// ---------------------------------------------------------------------

const char* kEmptyProposal =
    R"({"schemaVersion":"proposal-submission.v2","summary":"Nothing to do.","changes":[]})";

// A task that declares plannedCreatePaths has no valid no-op. The typed no-change terminal is
// rejected by the loop with a stable code - even though a scripted session (standing in for an
// adapter that did not honour the policy) returned it - before any repair session or persisted
// proposal exists. The very same script succeeds for a task that creates nothing.
void testPlannedCreateRejectsTypedNoChangesWithoutRepairOrProposal()
{
    const auto script = [] {
        auto session = std::make_unique<ScriptedProviderAgentSession>();
        session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
            toolCallResponse(0, "read", "workspace_read_file", R"({"path":"README.md"})")));
        session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
            toolCallResponse(1, "no-op", kSubmitTaskNoChangesToolName, typedNoChangesJson())));
        return session;
    };
    const auto makeNoOpRequest = [](const fs::path& root, bool forbidden) {
        auto request = makeRequest(root);
        request.requireTypedNoChangesTerminal = true;
        request.noChangesForbidden = forbidden;
        request.requiredRequirementIds = {"req-1"};
        request.sessionRequest.tools.push_back(submitTaskNoChangesToolDefinition());
        return request;
    };
    {
        Fixture fx;
        TempDir workspace;
        auto toolExecution = fx.makeToolExecution();
        auto changeProposals = fx.makeChangeProposals();
        AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);
        fx.sessionFactory.preparedSessions.push(script());
        const auto outcome = runner.run(
            makeNoOpRequest(workspace.path, /*forbidden=*/true), {}, nullptr, nullptr);
        assert(!outcome.success);
        assert(outcome.errorCode == kWorkerNoChangesPlannedCreateRequiredCode);
        assert(!outcome.workerNoChanges.has_value());
        assert(!outcome.proposal.has_value());
        assert(!outcome.repairAttempted && fx.sessionFactory.createCalls == 1);
    }
    {
        Fixture fx;
        TempDir workspace;
        auto toolExecution = fx.makeToolExecution();
        auto changeProposals = fx.makeChangeProposals();
        AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);
        fx.sessionFactory.preparedSessions.push(script());
        const auto outcome = runner.run(
            makeNoOpRequest(workspace.path, /*forbidden=*/false), {}, nullptr, nullptr);
        assert(outcome.success && outcome.workerNoChanges.has_value());
    }
}

// An empty propose_change_set must not become the bypass the typed terminal is closed to: for a
// planned-create task it is rejected with the same stable code, with and without the typed
// terminal required, before anything is persisted and without a repair session.
void testPlannedCreateRejectsEmptyProposeChangeSet()
{
    for (const bool requireTyped : {true, false}) {
        Fixture fx;
        TempDir workspace;
        auto toolExecution = fx.makeToolExecution();
        auto changeProposals = fx.makeChangeProposals();
        AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);
        auto session = std::make_unique<ScriptedProviderAgentSession>();
        session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
            toolUseResponse(0, "read")));
        session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
            proposeChangeSetToolCallResponse(1, "empty", kEmptyProposal)));
        fx.sessionFactory.preparedSessions.push(std::move(session));
        auto request = makeRequest(workspace.path);
        request.requireTypedNoChangesTerminal = requireTyped;
        request.noChangesForbidden = true;
        const auto outcome = runner.run(std::move(request), {}, nullptr, nullptr);
        assert(!outcome.success);
        assert(outcome.errorCode == kWorkerNoChangesPlannedCreateRequiredCode);
        assert(!outcome.proposal.has_value());
        assert(!outcome.repairAttempted && fx.sessionFactory.createCalls == 1);
    }
}

// The bounded repair after a rejected proposal may not answer with a no-op either: an empty
// propose_change_set from the repair session is the same rejection (and the repair is spent).
void testPlannedCreateRepairCannotReturnAnEmptyChangeSet()
{
    Fixture fx;
    TempDir workspace;
    workspace.writeFile("README.md", "existing content");
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto primary = std::make_unique<ScriptedProviderAgentSession>();
    primary->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "call-1",
            R"({"schemaVersion":"proposal-submission.v2","summary":"Create README","changes":[{)"
            R"("change_id":"create-readme","operation":"create",)"
            R"("relative_path":"README.md","source_path":null,"proposed_content":"new"}]})")));
    fx.sessionFactory.preparedSessions.push(std::move(primary));
    auto repair = std::make_unique<ScriptedProviderAgentSession>();
    repair->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "call-2", kEmptyProposal)));
    fx.sessionFactory.preparedSessions.push(std::move(repair));

    auto request = makeRequest(workspace.path);
    request.requireTypedNoChangesTerminal = true;
    request.noChangesForbidden = true;
    request.sessionRequest.tools.push_back(submitTaskNoChangesToolDefinition());
    const auto outcome = runner.run(std::move(request), {}, nullptr, nullptr);
    assert(!outcome.success && outcome.repairAttempted);
    assert(outcome.errorCode == kWorkerNoChangesPlannedCreateRequiredCode);
    assert(fx.sessionFactory.createCalls == 2);
}

// A planned-create task still succeeds with a concrete proposal.
void testPlannedCreateStillAcceptsAConcreteProposal()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);
    auto session = std::make_unique<ScriptedProviderAgentSession>();
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "call-1", kSimpleProposal)));
    fx.sessionFactory.preparedSessions.push(std::move(session));
    auto request = makeRequest(workspace.path);
    request.requireTypedNoChangesTerminal = true;
    request.noChangesForbidden = true;
    const auto outcome = runner.run(std::move(request), {}, nullptr, nullptr);
    assert(outcome.success && outcome.proposal.has_value());
}

// A read outside the task's exact allow-list comes back with a small Backend-authored answer
// instead of the bare code: the exact allowed paths (bounded), whether every required read is
// done, what is still unread, and the next action - and it never echoes what the model sent.
void testDeniedReadCarriesBoundedBackendGuidance()
{
    PathEchoFixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    std::vector<std::vector<AgentToolResult>> delivered;
    auto session = std::make_unique<ScriptedProviderAgentSession>();
    session->capturedResults = &delivered;
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolCallResponse(0, "denied-1", "workspace_read_file", R"({"path":"secret-looking.md"})")));
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolCallResponse(1, "read-1", "workspace_read_file", R"({"path":"README.md"})")));
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolCallResponse(2, "read-2", "workspace_read_file", R"({"path":"src/Panel.qml"})")));
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolCallResponse(3, "denied-2", "workspace_read_file", R"({"path":"docs/other.md"})")));
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(4, "propose", kSimpleProposal)));
    fx.sessionFactory.preparedSessions.push(std::move(session));

    auto request = makeRequest(workspace.path);
    request.policy.allowedRoots = {"README.md", "src/Panel.qml"};
    request.policy.allowedRootsAreExactPaths = true;
    request.requiredReadPathKeys = {"readme.md", "src/panel.qml"};
    const auto outcome = runner.run(std::move(request), {}, nullptr, nullptr);
    assert(outcome.success);
    assert(delivered.size() == 4);

    const auto& firstDenied = delivered[0].front();
    assert(!firstDenied.success && firstDenied.errorCode == "workspace_path.outside_allowed_roots");
    const auto first = nlohmann::json::parse(firstDenied.contentJson);
    assert(first.at("error") == "workspace_path.outside_allowed_roots");
    assert(first.at("allowedReadPaths") == nlohmann::json::array({"README.md", "src/Panel.qml"}));
    assert(first.at("requiredReadsComplete") == false);
    // Original case, in the task's own order - not the lowercased comparison keys.
    assert(first.at("unreadRequiredPaths") == nlohmann::json::array({"README.md", "src/Panel.qml"}));
    assert(first.at("nextAction") == "read_unreadRequiredPaths");
    assert(firstDenied.contentJson.find("secret-looking") == std::string::npos
        && "The guidance must never echo what the model sent");

    assert(delivered[1].front().success && delivered[1].front().contentJson.find("allowedReadPaths")
        == std::string::npos);

    const auto& secondDenied = delivered[3].front();
    assert(!secondDenied.success && secondDenied.errorCode == "workspace_path.outside_allowed_roots");
    const auto second = nlohmann::json::parse(secondDenied.contentJson);
    assert(second.at("requiredReadsComplete") == true);
    assert(second.at("unreadRequiredPaths").empty());
    assert(second.at("nextAction") == kProposeChangeSetToolName);
    assert(second.at("message").get<std::string>().find("propose_change_set")
        != std::string::npos);
}

std::shared_ptr<ProviderSessionBudgetGuard> twoOwnerGuard(
    std::uint64_t maxTurns, std::uint64_t primaryReserved, std::uint64_t repairReserved)
{
    ProviderExecutionBudgetPolicy policy;
    policy.maxTurns = maxTurns;
    auto guard = std::make_shared<ProviderSessionBudgetGuard>(policy);
    assert(guard->reserveTurns(
        {{"owner", primaryReserved}, {"retry:owner", repairReserved}}).success());
    return guard;
}

// The primary session is forced to its terminal turn by its OWN reservation. A protected repair
// reservation is another owner's: with no unreserved surplus the primary session can never lease
// a turn from it, however many it would like.
void testPrimarySessionIsForcedAtItsOwnReservationNotTheRepairOne()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    std::vector<std::optional<NextTurnToolPolicy>> policies;
    auto session = std::make_unique<ScriptedProviderAgentSession>();
    session->capturedPolicies = &policies;
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolUseResponse(0, "c0")));
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(1, "c1", kSimpleProposal)));
    fx.sessionFactory.preparedSessions.push(std::move(session));

    const auto guard = twoOwnerGuard(5, /*primary=*/2, /*repair=*/3);
    auto request = makeForcedTerminalRequest(workspace.path, guard);
    request.budgetRepairReservationOwner = "retry:owner";
    const auto outcome = runner.run(std::move(request), {}, nullptr, nullptr);
    assert(outcome.success);
    assert(policies.size() == 1 && policies[0].has_value());   // the primary's 2nd turn is its last
    assert(policies[0]->allowedToolNames
        == std::vector<std::string>({kProposeChangeSetToolName}));
    const auto repairReservation = guard->turnReservation("retry:owner");
    assert(repairReservation.has_value()
        && repairReservation->reserved == 3 && repairReservation->used == 0
        && "The primary session must not have touched the repair reservation");
}

// A fresh repair session is charged to the repair owner; the primary owner's unused reservation
// goes back to the shared surplus the moment the primary session is over.
void testRepairSessionIsChargedToTheRepairOwnerAndReturnsThePrimaryLeftover()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto primary = std::make_unique<ScriptedProviderAgentSession>();
    primary->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        stopResponse(0, "prose instead of a tool call")));
    fx.sessionFactory.preparedSessions.push(std::move(primary));
    ProviderAgentSessionRequest repairBegin;
    auto repair = std::make_unique<ScriptedProviderAgentSession>();
    repair->capturedBeginRequest = &repairBegin;
    repair->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolUseResponse(0, "r0")));
    repair->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(1, "r1", kSimpleProposal)));
    fx.sessionFactory.preparedSessions.push(std::move(repair));

    const auto guard = twoOwnerGuard(6, /*primary=*/3, /*repair=*/3);
    auto request = makeForcedTerminalRequest(workspace.path, guard);
    request.budgetRepairReservationOwner = "retry:owner";
    const auto outcome = runner.run(std::move(request), {}, nullptr, nullptr);
    assert(outcome.success && outcome.repairAttempted);
    assert(!guard->turnReservation("owner").has_value()
        && "The primary owner's leftover must be returned when its session is over");
    const auto repairReservation = guard->turnReservation("retry:owner");
    assert(repairReservation.has_value() && repairReservation->used == 2);
    assert(guard->usage().turnsStarted == 3);
    // 6 - 3 started - 1 still reserved for the repair owner: the primary's two unused turns
    // are surplus again.
    assert(guard->unreservedTurnsRemaining() == 2);
}

// A provider that ignores tool_choice and simply Stops on a Backend-forced
// terminal-only turn (the MockEvents incident shape) is a policy violation,
// never a final answer to parse: same stable code as a non-terminal tool call,
// no repair, no proposal, and the bounded text stays on the outcome for audit.
void testStopOnForcedFinalTurnIsRejected()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto session = std::make_unique<ScriptedProviderAgentSession>();
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolUseResponse(0, "c0")));
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        stopResponse(1, "I will not call a tool.")));
    fx.sessionFactory.preparedSessions.push(std::move(session));

    const auto outcome = runner.run(
        makeForcedTerminalRequest(workspace.path, reservedGuard(2, "owner", 2)),
        {}, nullptr, nullptr);
    assert(!outcome.success);
    assert(outcome.errorCode == kWorkerForcedTerminalViolationCode);
    assert(!outcome.proposal.has_value());
    assert(!outcome.repairAttempted);
    assert(outcome.finalText == "I will not call a tool.");
    assert(fx.sessionFactory.createCalls == 1);
}

void testStopOnForcedFirstTurnIsRejected()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    ProviderAgentSessionRequest capturedBegin;
    auto session = std::make_unique<ScriptedProviderAgentSession>();
    session->capturedBeginRequest = &capturedBegin;
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        stopResponse(0, "")));
    fx.sessionFactory.preparedSessions.push(std::move(session));

    const auto outcome = runner.run(
        makeForcedTerminalRequest(workspace.path, reservedGuard(1, "owner", 1)),
        {}, nullptr, nullptr);
    assert(capturedBegin.firstTurnToolPolicy.has_value());
    assert(!outcome.success);
    assert(outcome.errorCode == kWorkerForcedTerminalViolationCode);
    assert(!outcome.repairAttempted && fx.sessionFactory.createCalls == 1);
}

// The repair session obeys the same terminal policy as the primary attempt: an
// unforced primary Stop is parsed (and repaired once), but a Stop on the
// repair session's forced turn is a violation, not a second parse failure.
void testStopOnForcedRepairSessionTurnIsRejected()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    auto primary = std::make_unique<ScriptedProviderAgentSession>();
    primary->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        stopResponse(0, "prose instead of a tool call")));
    fx.sessionFactory.preparedSessions.push(std::move(primary));
    ProviderAgentSessionRequest repairBegin;
    auto repair = std::make_unique<ScriptedProviderAgentSession>();
    repair->capturedBeginRequest = &repairBegin;
    repair->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        stopResponse(0, "still prose")));
    fx.sessionFactory.preparedSessions.push(std::move(repair));

    const auto outcome = runner.run(
        makeForcedTerminalRequest(workspace.path, reservedGuard(2, "owner", 2)),
        {}, nullptr, nullptr);
    assert(repairBegin.firstTurnToolPolicy.has_value());
    assert(!outcome.success);
    assert(outcome.repairAttempted);
    assert(outcome.errorCode == kWorkerForcedTerminalViolationCode);
    assert(fx.sessionFactory.createCalls == 2);
}

// Run 80f904ed (2026-10-20): a retry attempt spent the Run's turns on one-call-per-turn reads; its
// Backend-forced last turn produced an empty change set and the task failed as "an empty change
// set is not a worker no-change submission", hiding that the turns had run out.
AgentToolTurnLoopOutcome runForcedFinalWithEmptyChangeSet(
    const std::string& owner, std::uint64_t reservedTurns, std::uint64_t sessionMaxTurns)
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);
    auto session = std::make_unique<ScriptedProviderAgentSession>();
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        toolUseResponse(0, "read")));
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(1, "empty", kEmptyProposal)));
    fx.sessionFactory.preparedSessions.push(std::move(session));
    auto request = makeForcedTerminalRequest(
        workspace.path, reservedGuard(reservedTurns, owner, reservedTurns));
    request.budgetReservationOwner = owner;
    request.requireTypedNoChangesTerminal = true;
    request.sessionRequest.maxTurns = sessionMaxTurns;
    return runner.run(std::move(request), {}, nullptr, nullptr);
}

void testEmptyChangeSetOnTheForcedLastTurnReportsTheExhaustedAllocation()
{
    const auto outcome = runForcedFinalWithEmptyChangeSet("owner", 2, 12);
    assert(!outcome.success && !outcome.proposal.has_value());
    assert(outcome.errorCode == "worker.turn_budget_exhausted");
    const auto& message = outcome.errorMessage;
    assert(message.find("primary attempt") != std::string::npos);
    assert(message.find("session turn 2 of 12") != std::string::npos);
    assert(message.find("task allocation 2 of 2 reserved turn(s) used") != std::string::npos);
    assert(message.find("Run 2 of 2 turn(s) started") != std::string::npos);
    assert(message.find("the task's turn allocation ran out") != std::string::npos);
    assert(message.find("session turn limit") == std::string::npos);
    assert(message.find("Secondary: proposal.empty_changes_requires_no_changes_terminal")
        != std::string::npos);
}

void testEmptyChangeSetOnTheSessionLimitTurnNamesTheSessionLimit()
{
    // A large allocation: only the session's own turn limit forces the last turn.
    const auto outcome = runForcedFinalWithEmptyChangeSet("owner", 6, 2);
    assert(!outcome.success);
    assert(outcome.errorCode == "worker.turn_budget_exhausted");
    assert(outcome.errorMessage.find("session turn 2 of 2") != std::string::npos);
    assert(outcome.errorMessage.find("the session turn limit was reached") != std::string::npos);
    assert(outcome.errorMessage.find("allocation ran out") == std::string::npos);
}

void testAllocationAndSessionLimitReachedTogetherAreBothNamed()
{
    const auto outcome = runForcedFinalWithEmptyChangeSet("owner", 2, 2);
    assert(!outcome.success && outcome.errorCode == "worker.turn_budget_exhausted");
    assert(outcome.errorMessage.find("the task's turn allocation ran out together with the "
                                     "session turn limit")
        != std::string::npos);
}

// The one typed repair session is charged to the protected retry reservation, but the message
// still names the phase of the ATTEMPT it belongs to.
void testExhaustionInARepairSessionNamesTheRepairSession()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);

    // The primary session voluntarily submits an empty set (its turn is not the last one), so the
    // typed terminal repair starts; that repair's own forced last turn is empty again.
    auto primary = std::make_unique<ScriptedProviderAgentSession>();
    primary->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(0, "p0", kEmptyProposal)));
    fx.sessionFactory.preparedSessions.push(std::move(primary));
    auto repair = std::make_unique<ScriptedProviderAgentSession>();
    repair->responses.push(common::Result<ProviderAgentTurnResponse>::ok(toolUseResponse(0, "r0")));
    repair->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
        proposeChangeSetToolCallResponse(1, "r1", kEmptyProposal)));
    fx.sessionFactory.preparedSessions.push(std::move(repair));

    auto request = makeForcedTerminalRequest(workspace.path, twoOwnerGuard(3, 2, 1));
    request.budgetRepairReservationOwner = "retry:owner";
    request.requireTypedNoChangesTerminal = true;
    const auto outcome = runner.run(std::move(request), {}, nullptr, nullptr);
    assert(!outcome.success && outcome.repairAttempted);
    assert(outcome.errorCode == "worker.turn_budget_exhausted");
    // The attempt itself is a primary one; only its repair session is charged to "retry:owner".
    assert(outcome.errorMessage.find("primary attempt, repair session") != std::string::npos);
    assert(outcome.errorMessage.find("retry attempt") == std::string::npos);
}

void testExhaustionMessageNamesTheRetryPhase()
{
    const auto outcome = runForcedFinalWithEmptyChangeSet("retry:owner", 2, 12);
    assert(!outcome.success && outcome.errorCode == "worker.turn_budget_exhausted");
    assert(outcome.errorMessage.find("retry attempt") != std::string::npos);
    assert(outcome.errorMessage.find("primary attempt") == std::string::npos);
}

// An empty change set BEFORE the allocation is used up is the worker's own decision and keeps the
// historical code; so does a concrete proposal on the forced turn (testFinalOwnerTurnIsForced...).
void testVoluntaryEmptyChangeSetKeepsTheEmptyChangeSetCode()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);
    for (int index = 0; index < 2; ++index) {   // the primary session and its one typed repair
        auto session = std::make_unique<ScriptedProviderAgentSession>();
        session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(
            proposeChangeSetToolCallResponse(0, "empty", kEmptyProposal)));
        fx.sessionFactory.preparedSessions.push(std::move(session));
    }
    auto request = makeForcedTerminalRequest(workspace.path, reservedGuard(8, "owner", 8));
    request.requireTypedNoChangesTerminal = true;
    const auto outcome = runner.run(std::move(request), {}, nullptr, nullptr);
    assert(!outcome.success);
    assert(outcome.errorCode == "proposal.empty_changes_requires_no_changes_terminal");
    assert(outcome.errorMessage.find("turn") == std::string::npos);
}

// Run 80f904ed: a worker response was cut at 4 054 output tokens while the code believed it had asked
// for 16 384, and nothing recorded the limit the session really requested. The failure now carries
// both numbers (the requested limit and what the turn produced).
void testMaxOutputTokensFailureNamesTheRequestedLimitAndTheProducedTokens()
{
    Fixture fx;
    TempDir workspace;
    auto toolExecution = fx.makeToolExecution();
    auto changeProposals = fx.makeChangeProposals();
    AgentToolTurnLoopRunner runner(fx.sessionFactory, toolExecution, changeProposals);
    auto session = std::make_unique<ScriptedProviderAgentSession>();
    ProviderAgentTurnResponse cut;
    cut.turnIndex = 0;
    cut.finishReason = ProviderAgentFinishReason::MaxOutputTokens;
    cut.usage.inputTokens = 100;
    cut.usage.outputTokens = 4054;
    session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(std::move(cut)));
    fx.sessionFactory.preparedSessions.push(std::move(session));

    auto request = makeRequest(workspace.path);
    request.sessionRequest.maxOutputTokens = 16384;
    const auto outcome = runner.run(std::move(request), {}, nullptr, nullptr);
    assert(!outcome.success && outcome.errorCode == "provider.max_output_tokens");
    assert(outcome.errorMessage
        == "Analysis response reached its output-token limit (requested limit 16384, produced "
           "4054 in that turn)");
}

} // namespace

int runAgentToolTurnLoopRunnerTests()
{
    testFinalOwnerTurnIsForcedToTerminalOnlyPolicy();
    testEmptyChangeSetOnTheForcedLastTurnReportsTheExhaustedAllocation();
    testEmptyChangeSetOnTheSessionLimitTurnNamesTheSessionLimit();
    testMaxOutputTokensFailureNamesTheRequestedLimitAndTheProducedTokens();
    testAllocationAndSessionLimitReachedTogetherAreBothNamed();
    testExhaustionInARepairSessionNamesTheRepairSession();
    testExhaustionMessageNamesTheRetryPhase();
    testVoluntaryEmptyChangeSetKeepsTheEmptyChangeSetCode();
    testInspectModeForcesOnlyTheVerificationTerminalOnItsFinalTurn();
    testNonTerminalToolOnForcedTurnIsRejectedBeforeExecution();
    testStopOnForcedFinalTurnIsRejected();
    testStopOnForcedFirstTurnIsRejected();
    testStopOnForcedRepairSessionTurnIsRejected();
    testFirstTurnIsForcedWhenTheTaskAllocationIsOneTurn();
    testLegacyCallersNeverReceiveAForcedTerminalPolicy();
    testRepairAfterRejectedConcreteWorkForcesTheSingleProposalTool();
    testPlannedCreateRejectsTypedNoChangesWithoutRepairOrProposal();
    testPlannedCreateRejectsEmptyProposeChangeSet();
    testPlannedCreateRepairCannotReturnAnEmptyChangeSet();
    testPlannedCreateStillAcceptsAConcreteProposal();
    testDeniedReadCarriesBoundedBackendGuidance();
    testPrimarySessionIsForcedAtItsOwnReservationNotTheRepairOne();
    testRepairSessionIsChargedToTheRepairOwnerAndReturnsThePrimaryLeftover();
    testScopedProposalToolSchemaAdvertisesOnlyFrozenWritePaths();
    testToolEvidenceAndModelExecutionIdArePersistedAndReported();
    testStopFinishReasonReturnsProposal();
    testEmptyChangesArrayAfterRealInspectionSucceeds();
    testZeroToolNoChangesNeededIsRejectedAsMissingEvidence();
    testFailedInspectionDoesNotProveNoChangesNeeded();
    testContradictoryNoChangesGetsOneRepairAndFailsIfRepeated();
    testContradictoryNoChangesRepairCanProduceChanges();
    testRepairPromptPreservesOriginalAllocationAndAllFindings();
    testRepairPromptCorrectsCreateWithForbiddenHashField();
    testRepairAbandoningRejectedChangesFailsInstead();
    testToolUseThenStop();
    testMaxTurnsBudgetFailsClosed();
    testRunWideBudgetPreventsWorkerThirteenthProviderRequest();
    testFormatErrorTriggersOneRepairRetry();
    testCreateTargetExistsTriggersRepairRetry();
    testCrossAgentConflictFindingDoesNotTriggerRepair();
    testFakeProviderStillSendingOldHashFieldIsFailClosedRejected();
    testModifyWithoutModelHashCallStillGetsTrustedHashFromPreflight();
    testHashPreflightSkipsCreateAndDedupesSharedSubjectPaths();
    testHashPreflightRespectsTotalToolCallBudget();
    testHashPreflightHonoursCancellationBeforeFilesystemDispatch();
    testRepairRehashesSamePathWithAttemptScopedEvidence();
    testTypedWorkerNoChangesRequiresSameAttemptEvidence();
    testTypedWorkerNoChangesSucceedsWithoutEmptyProposalRecord();
    testCreateMissingOutputCannotBecomeTypedNoChanges();
    testMissingInspectOutputRepairsToConcreteCreateProposal();
    testRepeatedNoChangesForMissingInspectOutputFailsWithoutProposal();
    testNoChangesRequiresEvidenceForEveryInspectOutput();
    testCommandBackedRequirementsRemainForBackendVerifier();
    testSameAttemptReadGatePassesWhenRequiredPathRead();
    testSameAttemptReadGateTriggersRepairThenSucceeds();
    testSameAttemptReadGateFailsAfterRepairStillMissing();
    testScopeExpansionApprovedRequiredReadMustBeReadBeforeAccepted();
    testScopeExpansionRejectsDeniedPathAndProposalStillSucceeds();
    return 0;
}
