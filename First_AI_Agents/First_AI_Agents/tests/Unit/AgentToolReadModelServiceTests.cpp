// Stage 7 tests for AgentToolReadModelService: verifies the join across
// IAgentToolSessionRepository / IAgentToolAuditRepository / ChangeProposalService
// and, critically, that proposal file-change bodies (proposedContent /
// capturedOriginalContent) are never eagerly populated in the projection.
#include "ai/application/agents/AgentToolReadModelService.hpp"
#include "ai/application/inmemory/InMemoryRepositories.hpp"
#include "ai/infrastructure/FileChangeProposalStore.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace common = ai::common;
using namespace ai::application;
using namespace ai::infrastructure;

void req(bool cond, const char* msg)
{
    if (!cond) throw std::runtime_error(msg);
}

struct TempDir {
    fs::path path;
    TempDir()
    {
        path = fs::temp_directory_path()
            / ("ai-orchestrator-agent-readmodel-test-"
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

// Same minimal shape as ChangeProposalTests.cpp's fake - kept local since
// each test file here is self-contained.
class FakeChangeProposalRepository final : public IChangeProposalRepository {
public:
    common::Result<void> insertChangeSet(const ProposedChangeSet& cs) override
    {
        std::lock_guard lock(mu_);
        if (byId_.count(cs.changeSetId))
            return common::Result<void>::failure({"proposal.duplicate_id", "Duplicate changeSetId"});
        byId_[cs.changeSetId] = cs;
        byId_[cs.changeSetId].changes.clear();
        byId_[cs.changeSetId].validationFindings.clear();
        changeLists_[cs.changeSetId] = cs.changes;
        findingLists_[cs.changeSetId] = cs.validationFindings;
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
        for (const auto& [id, cs] : byId_) if (cs.runId == runId) result.push_back(cs);
        return common::Result<std::vector<ProposedChangeSet>>::ok(std::move(result));
    }
    common::Result<std::vector<ProposedChangeSet>> listForProject(const ProjectId&) const override
    {
        return common::Result<std::vector<ProposedChangeSet>>::ok({});
    }
    common::Result<std::vector<ProposedFileChange>> listChanges(const ChangeSetId& id) const override
    {
        std::lock_guard lock(mu_);
        ++listChangesCalls_;
        const auto it = changeLists_.find(id);
        if (it == changeLists_.end()) return common::Result<std::vector<ProposedFileChange>>::ok({});
        return common::Result<std::vector<ProposedFileChange>>::ok(it->second);
    }
    common::Result<std::vector<ValidationFinding>> listValidationFindings(const ChangeSetId& id) const override
    {
        std::lock_guard lock(mu_);
        ++listValidationFindingsCalls_;
        const auto it = findingLists_.find(id);
        if (it == findingLists_.end()) return common::Result<std::vector<ValidationFinding>>::ok({});
        return common::Result<std::vector<ValidationFinding>>::ok(it->second);
    }
    common::Result<std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>>>
    listChangesForSets(const std::vector<ChangeSetId>& ids) const override
    {
        std::lock_guard lock(mu_);
        ++listChangesForSetsCalls_;
        std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>> result;
        for (const auto& id : ids) {
            const auto it = changeLists_.find(id);
            if (it != changeLists_.end()) result[id] = it->second;
        }
        return common::Result<std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>>>::ok(
            std::move(result));
    }
    common::Result<std::unordered_map<ChangeSetId, std::vector<ValidationFinding>>>
    listValidationFindingsForSets(const std::vector<ChangeSetId>& ids) const override
    {
        std::lock_guard lock(mu_);
        ++listValidationFindingsForSetsCalls_;
        std::unordered_map<ChangeSetId, std::vector<ValidationFinding>> result;
        for (const auto& id : ids) {
            const auto it = findingLists_.find(id);
            if (it != findingLists_.end()) result[id] = it->second;
        }
        return common::Result<std::unordered_map<ChangeSetId, std::vector<ValidationFinding>>>::ok(
            std::move(result));
    }

    // Call counters proving AgentToolReadModelService::session() actually
    // routes through the batch API instead of one listChanges()/
    // listValidationFindings() call per proposal - see
    // testSessionUsesBatchApiNotPerChangeSetQueries().
    int listChangesCalls() const { std::lock_guard lock(mu_); return listChangesCalls_; }
    int listValidationFindingsCalls() const { std::lock_guard lock(mu_); return listValidationFindingsCalls_; }
    int listChangesForSetsCalls() const { std::lock_guard lock(mu_); return listChangesForSetsCalls_; }
    int listValidationFindingsForSetsCalls() const { std::lock_guard lock(mu_); return listValidationFindingsForSetsCalls_; }

private:
    mutable std::mutex mu_;
    std::map<ChangeSetId, ProposedChangeSet> byId_;
    std::map<ChangeSetId, std::vector<ProposedFileChange>> changeLists_;
    std::map<ChangeSetId, std::vector<ValidationFinding>> findingLists_;
    mutable int listChangesCalls_ = 0;
    mutable int listValidationFindingsCalls_ = 0;
    mutable int listChangesForSetsCalls_ = 0;
    mutable int listValidationFindingsForSetsCalls_ = 0;
};

struct Fixture {
    TempDir tempDir;
    InMemoryAgentToolSessionRepository sessions;
    InMemoryAgentToolAuditRepository audits;
    FakeChangeProposalRepository changeRepo;
    std::unique_ptr<FileChangeProposalStore> changeStore;

    Fixture()
    {
        auto storeResult = FileChangeProposalStore::open(tempDir.path / "proposals");
        assert(storeResult.success());
        changeStore = std::move(storeResult.value());
    }

    ChangeProposalService makeChangeProposals()
    {
        return ChangeProposalService(changeRepo, *changeStore);
    }
};

AgentToolSession makeSession(const std::string& sessionId, const std::string& runId)
{
    AgentToolSession s;
    s.sessionId = sessionId;
    s.runId = runId;
    s.providerId = "provider-1";
    s.modelId = "model-1";
    s.state = AgentToolSessionState::Active;
    s.createdAt = std::chrono::system_clock::now();
    return s;
}

void testSessionNotFound()
{
    Fixture fx;
    auto changeProposals = fx.makeChangeProposals();
    AgentToolReadModelService readModel(fx.sessions, fx.audits, changeProposals);

    const auto result = readModel.session("does-not-exist");
    req(!result.success(), "notFound: expected failure");
    req(result.error()->code == "agent_session.not_found", "notFound: wrong error code");
}

void testEmptySessionJoinsToEmptyLists()
{
    Fixture fx;
    auto changeProposals = fx.makeChangeProposals();
    AgentToolReadModelService readModel(fx.sessions, fx.audits, changeProposals);

    auto session = makeSession("sess-1", "run-1");
    req(fx.sessions.insertSession(session).success(), "empty: insertSession failed");

    const auto result = readModel.session("sess-1");
    req(result.success(), "empty: session() failed");
    req(result.value().session.sessionId == "sess-1", "empty: wrong sessionId");
    req(result.value().turns.empty(), "empty: expected no turns");
    req(result.value().toolCalls.empty(), "empty: expected no tool calls");
    req(result.value().proposals.empty(), "empty: expected no proposals");
}

void testFullJoinAndNoContentLeak()
{
    Fixture fx;
    auto changeProposals = fx.makeChangeProposals();
    AgentToolReadModelService readModel(fx.sessions, fx.audits, changeProposals);

    auto session = makeSession("sess-2", "run-2");
    req(fx.sessions.insertSession(session).success(), "full: insertSession failed");

    AgentProviderTurn turn;
    turn.turnId = "turn-1";
    turn.sessionId = "sess-2";
    turn.turnIndex = 0;
    turn.finishReason = "tool_use";
    turn.callCount = 1;
    turn.createdAt = std::chrono::system_clock::now();
    req(fx.sessions.insertTurn(turn).success(), "full: insertTurn failed");

    AgentToolAuditRecord audit;
    audit.auditId = "audit-1";
    audit.sessionId = "sess-2";
    audit.runId = "run-2";
    audit.providerCallId = "call-1";
    audit.toolName = "workspace_read_file";
    audit.state = AgentToolCallState::Completed;
    audit.argumentsSha256 = "deadbeef";
    audit.resultSha256 = "cafef00d";
    audit.returnedBytes = 128;
    audit.truncated = false;
    audit.createdAt = std::chrono::system_clock::now();
    req(fx.audits.insert(audit).success(), "full: audit insert failed");

    CreateChangeProposalRequest proposalRequest;
    proposalRequest.runId = "run-2";
    proposalRequest.projectId = "project-2";
    proposalRequest.providerId = "provider-1";
    proposalRequest.modelId = "model-1";
    proposalRequest.agentRole = "read-only-analysis";
    proposalRequest.rawModelOutput =
        "```json\n"
        "{\"schemaVersion\":\"proposal-submission.v2\","
        "\"summary\":\"Found dead code.\",\"changes\":[{"
        "\"change_id\":\"c1\",\"operation\":\"modify\",\"relative_path\":\"src/x.cpp\","
        "\"source_path\":null,"
        "\"proposed_content\":\"int main() { return 0; }\"}]}\n"
        "```";
    proposalRequest.policy.projectRoot = fx.tempDir.path;
    fx.tempDir.path.empty(); // silence unused-warning concerns on some compilers
    // The Modify precondition (the Backend's own hash preflight must confirm
    // the target exists) will fail here since no such file exists on disk -
    // that is fine for this test: a Rejected proposal is still a valid,
    // fully-joined projection.
    const auto created = changeProposals.createFromModelOutput(std::move(proposalRequest));
    req(created.success(), "full: createFromModelOutput failed");

    const auto result = readModel.session("sess-2");
    req(result.success(), "full: session() failed");
    const auto& model = result.value();

    req(model.turns.size() == 1, "full: expected 1 turn");
    req(model.turns[0].turnId == "turn-1", "full: wrong turnId");

    req(model.toolCalls.size() == 1, "full: expected 1 tool call");
    req(model.toolCalls[0].toolName == "workspace_read_file", "full: wrong toolName");
    req(model.toolCalls[0].state == AgentToolCallState::Completed, "full: wrong call state");

    req(model.proposals.size() == 1, "full: expected 1 proposal");
    const auto& proposal = model.proposals[0];
    req(proposal.header.changeSetId == created.value().changeSetId, "full: wrong changeSetId");
    req(proposal.header.changes.empty(), "full: header.changes must stay empty (structural data lives in .changes)");
    req(proposal.changes.size() == 1, "full: expected 1 structural change");
    req(proposal.changes[0].relativePath == fs::path("src/x.cpp"),
        "full: relativePath must be relative, not absolute");
    // The safety guarantee this test exists to prove: no file body ever
    // rides along in the read model, regardless of what the repository
    // returns internally.
    req(!proposal.changes[0].proposedContent.has_value(),
        "full: proposedContent must never leak into the read model");
    req(!proposal.changes[0].capturedOriginalContent.has_value(),
        "full: capturedOriginalContent must never leak into the read model");
    // Rejected because the Modify target file does not exist on disk.
    req(proposal.header.state == ChangeSetState::Rejected, "full: expected Rejected state");
    req(!proposal.findings.empty(), "full: expected at least one validation finding");
}

void testListForRunSortedNewestFirst()
{
    Fixture fx;
    auto changeProposals = fx.makeChangeProposals();
    AgentToolReadModelService readModel(fx.sessions, fx.audits, changeProposals);

    auto older = makeSession("sess-old", "run-3");
    older.createdAt = std::chrono::system_clock::now() - std::chrono::seconds(60);
    auto newer = makeSession("sess-new", "run-3");
    newer.createdAt = std::chrono::system_clock::now();
    req(fx.sessions.insertSession(older).success(), "sorted: insert older failed");
    req(fx.sessions.insertSession(newer).success(), "sorted: insert newer failed");

    const auto result = readModel.listForRun("run-3");
    req(result.success(), "sorted: listForRun failed");
    req(result.value().size() == 2, "sorted: expected 2 sessions");
    req(result.value()[0].sessionId == "sess-new", "sorted: newest must come first");
}

// Proves session() routes proposal changes/findings through the batched
// listChangesForSets()/listValidationFindingsForSets() API instead of one
// listChanges()/listValidationFindings() call per proposal - the exact N+1
// this batching exists to eliminate (see IChangeProposalRepository.hpp).
void testSessionUsesBatchApiNotPerChangeSetQueries()
{
    Fixture fx;
    auto changeProposals = fx.makeChangeProposals();
    AgentToolReadModelService readModel(fx.sessions, fx.audits, changeProposals);

    auto session = makeSession("sess-batch", "run-batch");
    req(fx.sessions.insertSession(session).success(), "batchApi: insertSession failed");

    for (int i = 0; i < 3; ++i) {
        ProposedChangeSet cs;
        cs.schemaVersion = "proposal.v1";
        cs.changeSetId = "cs-batch-" + std::to_string(i);
        cs.runId = "run-batch";
        cs.projectId = "project-batch";
        cs.providerId = "provider-1";
        cs.modelId = "model-1";
        cs.agentRole = "test-agent";
        cs.summary = "batch test " + std::to_string(i);
        cs.state = ChangeSetState::ReadyForReview;
        cs.createdAt = std::chrono::system_clock::now();

        ProposedFileChange ch;
        ch.changeId = "chg-" + std::to_string(i);
        ch.operation = FileChangeOperation::Create;
        ch.relativePath = "src/file" + std::to_string(i) + ".cpp";
        cs.changes = {ch};

        req(fx.changeRepo.insertChangeSet(cs).success(), "batchApi: insertChangeSet failed");
    }

    const auto result = readModel.session("sess-batch");
    req(result.success(), "batchApi: session() failed");
    req(result.value().proposals.size() == 3, "batchApi: expected 3 proposals");

    req(fx.changeRepo.listChangesForSetsCalls() == 1,
        "batchApi: expected exactly one batched listChangesForSets call");
    req(fx.changeRepo.listValidationFindingsForSetsCalls() == 1,
        "batchApi: expected exactly one batched listValidationFindingsForSets call");
    req(fx.changeRepo.listChangesCalls() == 0,
        "batchApi: session() must not fall back to per-change-set listChanges");
    req(fx.changeRepo.listValidationFindingsCalls() == 0,
        "batchApi: session() must not fall back to per-change-set listValidationFindings");
}

} // namespace

int runAgentToolReadModelServiceTests()
{
    testSessionNotFound();
    testEmptySessionJoinsToEmptyLists();
    testFullJoinAndNoContentLeak();
    testListForRunSortedNewestFirst();
    testSessionUsesBatchApiNotPerChangeSetQueries();
    return 0;
}
