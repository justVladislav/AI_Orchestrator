// Application-layer unit tests for ComparisonService.
// All I/O goes through the in-memory repository; no network, no SQLite.

#include "ai/application/ComparisonService.hpp"
#include "ai/application/ComparisonTypes.hpp"
#include "ai/application/IComparisonRepository.hpp"
#include "ai/application/IProjectRepository.hpp"
#include "ai/application/ProjectService.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ai::application;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// Minimal in-memory project repository for these tests.
class FakeProjectRepository final : public IProjectRepository {
public:
    explicit FakeProjectRepository(std::vector<Project> seed = {})
        : projects_(std::move(seed))
    {
    }

    common::Result<Project> insert(Project p) override
    {
        projects_.push_back(p);
        return common::Result<Project>::success(p);
    }
    common::Result<Project> update(Project p) override
    {
        for (auto& existing : projects_) {
            if (existing.id == p.id) { existing = p; return common::Result<Project>::success(p); }
        }
        return common::Result<Project>::failure({"not_found", "project not found"});
    }
    [[nodiscard]] common::Result<std::optional<Project>> findById(const ProjectId& id) const override
    {
        for (const auto& p : projects_) {
            if (p.id == id) return common::Result<std::optional<Project>>::success(p);
        }
        return common::Result<std::optional<Project>>::success(std::nullopt);
    }
    [[nodiscard]] common::Result<std::optional<Project>> findByRootPath(
        const std::filesystem::path& path) const override
    {
        for (const auto& p : projects_) {
            if (p.rootPath == path)
                return common::Result<std::optional<Project>>::success(p);
        }
        return common::Result<std::optional<Project>>::success(std::nullopt);
    }
    [[nodiscard]] common::Result<std::vector<Project>> listAll() const override
    {
        return common::Result<std::vector<Project>>::success(projects_);
    }

private:
    std::vector<Project> projects_;
};

int idCounter = 0;
std::string nextId() { return "id-" + std::to_string(++idCounter); }

// ---- Test helpers -----------------------------------------------------------

Project makeProject(const std::string& id = "proj-1", const std::string& name = "Test Project")
{
    Project p;
    p.id = id;
    p.name = name;
    p.rootPath = std::filesystem::temp_directory_path() / name;
    return p;
}

CreateComparisonRequest makeRequest(const std::string& projectId = "proj-1")
{
    CreateComparisonRequest req;
    req.projectId = projectId;
    req.title = "Test comparison";
    req.task = "Compare provider outputs for X";
    ComparisonProviderSlot slot;
    slot.providerId = "openai";
    slot.providerName = "OpenAI";
    slot.modelId = "gpt-4o";
    slot.modelName = "GPT-4o";
    slot.providerKind = "openai";
    slot.apiModelId = "gpt-4o";
    req.slots = {slot};
    return req;
}

// ── Test 1: createSession inserts session and one result stub per slot ────────

void test_createSession_insertsSessionAndResultStubs()
{
    FakeProjectRepository projects({makeProject()});
    auto repo = makeInMemoryComparisonRepository();
    ComparisonService svc(*repo, projects, nextId);

    CreateComparisonRequest req = makeRequest();
    ComparisonProviderSlot slot2;
    slot2.providerId = "anthropic";
    slot2.providerName = "Anthropic";
    slot2.modelId = "claude-3";
    slot2.modelName = "Claude 3";
    req.slots.push_back(slot2);

    const auto result = svc.createSession(req);
    require(result.success(), "createSession must succeed");

    const auto& session = result.value();
    require(session.id.empty() == false, "session.id must not be empty");
    require(session.projectId == "proj-1", "session.projectId must match");
    require(session.title == req.title, "session.title must match");
    require(session.status == ComparisonStatus::Active, "new session must be Active");

    const auto listRes = svc.listResultsBySession(session.id);
    require(listRes.success(), "listResultsBySession must succeed");
    require(listRes.value().size() == 2, "must have one result per slot");
    require(listRes.value()[0].status == ComparisonResultStatus::Pending, "new results must be Pending");
}

// ── Test 2: createSession fails with unknown projectId ───────────────────────

void test_createSession_failsWithUnknownProject()
{
    FakeProjectRepository projects; // empty
    auto repo = makeInMemoryComparisonRepository();
    ComparisonService svc(*repo, projects, nextId);

    const auto result = svc.createSession(makeRequest("nonexistent"));
    require(!result.success(), "must fail");
    require(result.error()->code == "comparison.project_not_found", "wrong error code");
}

// ── Test 3: createSession fails with no slots ─────────────────────────────────

void test_createSession_failsWithNoSlots()
{
    FakeProjectRepository projects({makeProject()});
    auto repo = makeInMemoryComparisonRepository();
    ComparisonService svc(*repo, projects, nextId);

    CreateComparisonRequest req = makeRequest();
    req.slots.clear();
    const auto result = svc.createSession(req);
    require(!result.success(), "must fail with empty slots");
    require(result.error()->code == "comparison.no_slots", "wrong error code");
}

// ── Test 4: setResultCompleted stores content and transitions status ──────────

void test_setResultCompleted_storesContentAndStatus()
{
    FakeProjectRepository projects({makeProject()});
    auto repo = makeInMemoryComparisonRepository();
    ComparisonService svc(*repo, projects, nextId);

    const auto session = svc.createSession(makeRequest()).value();
    const auto results = svc.listResultsBySession(session.id).value();
    const std::string resultId = results[0].id;

    const auto res = svc.setResultCompleted(resultId, "Great answer", 100, 200, 1500, "$0.003");
    require(res.success(), "setResultCompleted must succeed");

    const auto found = svc.findResult(resultId).value();
    require(found.has_value(), "result must be findable");
    require(found->status == ComparisonResultStatus::Completed, "status must be Completed");
    require(found->content == "Great answer", "content must match");
    require(found->inputTokens == 100, "inputTokens must match");
    require(found->outputTokens == 200, "outputTokens must match");
    require(found->latencyMs == 1500, "latencyMs must match");
    require(found->totalCost == "$0.003", "totalCost must match");
}

// ── Test 5: setResultBest clears isBest on siblings ──────────────────────────

void test_setResultBest_clearsSiblingBestFlags()
{
    FakeProjectRepository projects({makeProject()});
    auto repo = makeInMemoryComparisonRepository();
    ComparisonService svc(*repo, projects, nextId);

    CreateComparisonRequest req = makeRequest();
    ComparisonProviderSlot s2;
    s2.providerId = "anthropic";
    s2.providerName = "Anthropic";
    s2.modelId = "claude";
    s2.modelName = "Claude";
    req.slots.push_back(s2);

    const auto session = svc.createSession(req).value();
    const auto results = svc.listResultsBySession(session.id).value();
    const std::string idA = results[0].id;
    const std::string idB = results[1].id;

    require(svc.setResultBest(session.id, idA).success(), "setResultBest A must succeed");
    require(svc.findResult(idA).value()->isBest, "A must be best");
    require(!svc.findResult(idB).value()->isBest, "B must not be best");

    require(svc.setResultBest(session.id, idB).success(), "setResultBest B must succeed");
    require(!svc.findResult(idA).value()->isBest, "A must lose best flag");
    require(svc.findResult(idB).value()->isBest, "B must gain best flag");
}

// ── Test 6: setResultSynthesisSelected only works on Completed results ────────

void test_setResultSynthesisSelected_requiresCompletedStatus()
{
    FakeProjectRepository projects({makeProject()});
    auto repo = makeInMemoryComparisonRepository();
    ComparisonService svc(*repo, projects, nextId);

    const auto session = svc.createSession(makeRequest()).value();
    const auto results = svc.listResultsBySession(session.id).value();
    const std::string resultId = results[0].id;

    // Pending → synthesis selection must be rejected
    const auto res = svc.setResultSynthesisSelected(resultId, true);
    require(!res.success(), "must reject synthesis selection for Pending result");

    // Complete it first
    svc.setResultCompleted(resultId, "ok", 10, 20, 500, "$0.001");
    const auto res2 = svc.setResultSynthesisSelected(resultId, true);
    require(res2.success(), "must accept synthesis selection for Completed result");
    require(svc.findResult(resultId).value()->selectedForSynthesis, "selectedForSynthesis must be true");
}

// ── Test 7: resetResultPending re-enables retry ───────────────────────────────

void test_resetResultPending_allowsRetry()
{
    FakeProjectRepository projects({makeProject()});
    auto repo = makeInMemoryComparisonRepository();
    ComparisonService svc(*repo, projects, nextId);

    const auto session = svc.createSession(makeRequest()).value();
    const auto results = svc.listResultsBySession(session.id).value();
    const std::string resultId = results[0].id;

    svc.setResultFailed(resultId, "timeout");
    require(svc.findResult(resultId).value()->status == ComparisonResultStatus::Error,
            "must be Error after failure");

    svc.resetResultPending(resultId);
    require(svc.findResult(resultId).value()->status == ComparisonResultStatus::Pending,
            "must be Pending after reset");
}

// ── Test 8: listSessionsByProject returns sessions for the correct project ────

void test_listSessionsByProject_filtersCorrectly()
{
    FakeProjectRepository projects({makeProject("proj-A", "A"), makeProject("proj-B", "B")});
    auto repo = makeInMemoryComparisonRepository();
    ComparisonService svc(*repo, projects, nextId);

    svc.createSession(makeRequest("proj-A"));
    svc.createSession(makeRequest("proj-A"));
    svc.createSession(makeRequest("proj-B"));

    const auto resA = svc.listSessionsByProject("proj-A");
    require(resA.success(), "listSessionsByProject must succeed");
    require(resA.value().size() == 2, "proj-A must have 2 sessions");

    const auto resB = svc.listSessionsByProject("proj-B");
    require(resB.value().size() == 1, "proj-B must have 1 session");
}

} // namespace

int main()
{
    test_createSession_insertsSessionAndResultStubs();
    test_createSession_failsWithUnknownProject();
    test_createSession_failsWithNoSlots();
    test_setResultCompleted_storesContentAndStatus();
    test_setResultBest_clearsSiblingBestFlags();
    test_setResultSynthesisSelected_requiresCompletedStatus();
    test_resetResultPending_allowsRetry();
    test_listSessionsByProject_filtersCorrectly();
    return 0;
}
