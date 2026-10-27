// Stage 8 tests for execution-profile-aware run routing, the production
// ExecutionProfileWorkflowRunner, and CredentialManagementService. Every
// scenario here uses fake in-memory adapters and a fake IProviderClient/
// IProviderClientFactory - no CURL, no real network call, no real Windows
// Credential Manager access. Real network/provider verification is
// explicitly out of scope for this file (and for ordinary ctest in
// general - see CODEX_CONTEXT.md).
#include "ai/application/CredentialManagementService.hpp"
#include "ai/application/catalog/ExecutionProfileService.hpp"
#include "ai/application/ExecutionProfileWorkflowRunner.hpp"
#include "ai/application/ICredentialStore.hpp"
#include "ai/application/conversations/IProjectMemoryRepository.hpp"
#include "ai/application/InMemoryRepositories.hpp"
#include "ai/application/agents/ModelExecutionService.hpp"
#include "ai/application/MockWorkflowRunner.hpp"
#include "ai/application/OrchestrationService.hpp"
#include "ai/application/projects/ProjectService.hpp"
#include "ai/application/catalog/ProviderCatalogService.hpp"
#include "ai/application/WorkflowRouter.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace common = ai::common;
using namespace std::chrono_literals;
using namespace ai::application;

// ---------------------------------------------------------------------
// Fakes
// ---------------------------------------------------------------------

// A single canned response or error, keyed by stageType, so a test can make
// different stages behave differently (including cancellation checks).
class FakeProviderClient final : public IProviderClient {
public:
    struct Outcome {
        bool success {true};
        std::string content;
        std::string errorCode;
        std::string errorMessage;
        bool observeCancellation {false};
        // The usage a successful reply reports; empty keeps the historical 7 input / 5 output.
        std::optional<ai::models::TokenUsage> usage;
    };

    void setOutcomeForStage(std::string stageApiModelId, Outcome outcome)
    {
        std::lock_guard lock(mutex_);
        outcomes_[stageApiModelId] = std::move(outcome);
    }

    [[nodiscard]] std::vector<ProviderChatRequest> capturedRequests() const
    {
        std::lock_guard lock(mutex_);
        return requests_;
    }

    common::Result<ProviderChatResponse> send(
        const ProviderChatRequest& request, std::stop_token stopToken) override
    {
        {
            std::lock_guard lock(mutex_);
            requests_.push_back(request);
        }
        Outcome outcome;
        {
            std::lock_guard lock(mutex_);
            const auto it = outcomes_.find(request.apiModelId);
            if (it != outcomes_.end()) {
                outcome = it->second;
            }
        }
        if (outcome.observeCancellation && stopToken.stop_requested()) {
            return common::Result<ProviderChatResponse>::failure(
                {"provider.cancelled", "Cancelled during the request"});
        }
        if (!outcome.success) {
            return common::Result<ProviderChatResponse>::failure({outcome.errorCode, outcome.errorMessage});
        }
        ProviderChatResponse response;
        response.content = outcome.content;
        if (outcome.usage.has_value()) {
            response.usage = *outcome.usage;
        } else {
            response.usage.inputTokens = 7;
            response.usage.outputTokens = 5;
        }
        return common::Result<ProviderChatResponse>::ok(std::move(response));
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, Outcome> outcomes_;
    std::vector<ProviderChatRequest> requests_;
};

class FakeProviderClientFactory final : public IProviderClientFactory {
public:
    void registerClient(ProviderId providerId, IProviderClient& client)
    {
        clients_[std::move(providerId)] = &client;
    }

    // When set, every clientFor() call fails with this error regardless of
    // providerId - stands in for "no real provider adapter compiled in" or
    // "no credential configured".
    void failEveryCallWith(std::string code, std::string message)
    {
        forcedFailure_ = common::Error {std::move(code), std::move(message)};
    }

    common::Result<std::shared_ptr<IProviderClient>> clientFor(
        const ProviderId& providerId, const std::string& /*providerKind*/) override
    {
        if (forcedFailure_.has_value()) {
            return common::Result<std::shared_ptr<IProviderClient>>::failure(*forcedFailure_);
        }
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
    std::optional<common::Error> forcedFailure_;
};

class FixedProjectMemoryRepository final : public IProjectMemoryRepository {
public:
    std::vector<ProjectMemorySegment> segments;

    common::Result<ProjectMemorySegment> insertMemorySegment(
        ProjectMemorySegment value) override
    {
        segments.push_back(value);
        return common::Result<ProjectMemorySegment>::ok(std::move(value));
    }

    common::Result<std::vector<ProjectMemorySegment>> listMemorySegments(
        const ProjectId& projectId) const override
    {
        std::vector<ProjectMemorySegment> selected;
        std::copy_if(segments.begin(), segments.end(), std::back_inserter(selected),
            [&projectId](const auto& segment) {
                return segment.projectId == projectId;
            });
        return common::Result<std::vector<ProjectMemorySegment>>::ok(
            std::move(selected));
    }

    common::Result<std::optional<ProjectMemorySegment>> findMemorySegment(
        const ProjectMemorySegmentId& id) const override
    {
        const auto found = std::find_if(segments.begin(), segments.end(),
            [&id](const auto& segment) { return segment.id == id; });
        return common::Result<std::optional<ProjectMemorySegment>>::ok(
            found == segments.end()
                ? std::optional<ProjectMemorySegment> {}
                : std::optional<ProjectMemorySegment> {*found});
    }

    common::Result<void> removeMemorySegments(
        const ProjectId& projectId) override
    {
        std::erase_if(segments, [&projectId](const auto& segment) {
            return segment.projectId == projectId;
        });
        return common::Result<void>::ok();
    }
};

// Exercises the durable usage boundary independently of a provider adapter.
// The terminal WorkflowResult deliberately contains different token totals:
// RunManager must retain the incrementally persisted authoritative snapshot.
class BudgetPersistingWorkflowRunner final : public IWorkflowRunner {
public:
    explicit BudgetPersistingWorkflowRunner(bool awaitCancellation = false)
        : awaitCancellation_(awaitCancellation)
    {
    }

    std::vector<RunStage> createStages(
        const RunId&, const ResolvedExecutionPlan&) const override
    {
        return {};
    }

    WorkflowResult run(
        const WorkflowRunContext& context,
        std::stop_token stopToken,
        IRunEventSink& eventSink) override
    {
        ProviderExecutionBudgetUsage usage;
        usage.turnsStarted = 2;
        usage.turnsCompleted = 1;
        usage.inputTokens = 321;
        usage.outputTokens = 45;
        usage.toolCalls = 3;
        usage.toolResultBytes = 987;
        // Real-Run observation (af4e5703-90c7-401d-9235-808872719abf,
        // 2026-09-15): non-zero so the two tests below can catch
        // RunManager::persistProviderBudgetUsage silently dropping cache
        // fields off run.usage, or collapsing run.usage.inputTokens down to
        // this budget-countable figure instead of the honest full total.
        usage.cacheReadInputTokens = 1'000;
        usage.cacheCreationInputTokens = 100;
        const auto persisted = eventSink.persistProviderBudgetUsage(context.run.id, usage);
        assert(persisted.success());

        if (awaitCancellation_) {
            while (!stopToken.stop_requested()) {
                std::this_thread::sleep_for(1ms);
            }
            return {false, RunStatus::Cancelled, "cancelled", {9'999, 9'999}};
        }
        return {false, RunStatus::Failed, "expected failure", {9'999, 9'999}};
    }

private:
    bool awaitCancellation_ {false};
};

class GraphProviderClient final : public IProviderClient {
public:
    struct Reply {
        bool success {true};
        std::string content;
        std::string errorCode;
        std::chrono::milliseconds delay {0};
    };

    void setScript(std::string apiModelId, std::vector<Reply> replies)
    {
        std::lock_guard lock(mutex_);
        scripts_[std::move(apiModelId)] = std::move(replies);
    }

    [[nodiscard]] int maximumConcurrency() const noexcept { return maximum_.load(); }

    [[nodiscard]] std::vector<ProviderChatRequest> capturedRequests() const
    {
        std::lock_guard lock(mutex_);
        return requests_;
    }

    common::Result<ProviderChatResponse> send(
        const ProviderChatRequest& request, std::stop_token stopToken) override
    {
        Reply reply;
        {
            std::lock_guard lock(mutex_);
            requests_.push_back(request);
            auto& next = nextReply_[request.apiModelId];
            const auto found = scripts_.find(request.apiModelId);
            assert(found != scripts_.end() && !found->second.empty());
            reply = found->second[std::min(next, found->second.size() - 1)];
            ++next;
        }

        const int active = active_.fetch_add(1) + 1;
        int observed = maximum_.load();
        while (active > observed
            && !maximum_.compare_exchange_weak(observed, active)) {
        }
        if (reply.delay > 0ms) std::this_thread::sleep_for(reply.delay);
        active_.fetch_sub(1);
        if (stopToken.stop_requested()) {
            return common::Result<ProviderChatResponse>::failure(
                {"provider.cancelled", "Cancelled by graph test"});
        }
        if (!reply.success) {
            return common::Result<ProviderChatResponse>::failure(
                {reply.errorCode, "Scripted graph failure"});
        }
        ProviderChatResponse response;
        response.content = reply.content;
        response.usage.inputTokens = 11;
        response.usage.outputTokens = 7;
        return common::Result<ProviderChatResponse>::ok(std::move(response));
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::vector<Reply>> scripts_;
    std::unordered_map<std::string, std::size_t> nextReply_;
    std::vector<ProviderChatRequest> requests_;
    std::atomic<int> active_ {0};
    std::atomic<int> maximum_ {0};
};

// Minimal in-memory ICredentialStore test double - deliberately not part of
// production code (a real secure store must never be a plain map), used only
// to exercise CredentialManagementService's own coordination logic.
class FakeCredentialStore final : public ICredentialStore {
public:
    common::Result<void> store(const std::string& reference, const std::string& secret) override
    {
        std::lock_guard lock(mutex_);
        secrets_[reference] = secret;
        return common::Result<void>::ok();
    }
    [[nodiscard]] common::Result<std::optional<std::string>> load(const std::string& reference) const override
    {
        std::lock_guard lock(mutex_);
        const auto it = secrets_.find(reference);
        if (it == secrets_.end()) return common::Result<std::optional<std::string>>::ok(std::nullopt);
        return common::Result<std::optional<std::string>>::ok(it->second);
    }
    common::Result<void> remove(const std::string& reference) override
    {
        std::lock_guard lock(mutex_);
        secrets_.erase(reference);
        return common::Result<void>::ok();
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> secrets_;
};

// ---------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------

struct Catalog {
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    InMemoryExecutionProfileRepository profiles;
    ProviderCatalogService catalog {providers, models};
    ExecutionProfileService profileService {profiles, providers, models};
};

fs::path uniqueProjectPath(const char* label)
{
    return fs::temp_directory_path()
        / (std::string("ai-orchestrator-workflow-routing-test-") + label + "-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

Project makeActiveProject(ProjectService& projectService, const char* label, std::string description = "")
{
    const fs::path path = uniqueProjectPath(label);
    std::error_code ignored;
    fs::create_directories(path, ignored);
    const auto created = projectService.createProject(
        {label, description.empty() ? "Workflow routing test project" : description, path});
    assert(created.success());
    return created.value();
}

Provider makeProvider(Catalog& catalog, const char* name, const char* kind = "openai")
{
    const auto created = catalog.catalog.createProvider({name, kind, "https://example.invalid", true, std::nullopt});
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

Run waitForTerminalRun(IRunRepository& repository, const RunId& runId, std::chrono::milliseconds timeout = 3s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto found = repository.findRun(runId);
        assert(found.success() && found.value().has_value());
        if (isTerminal(found.value()->status)) {
            return *found.value();
        }
        std::this_thread::sleep_for(5ms);
    }
    assert(false && "Run did not reach a terminal state before timeout");
    return {};
}

// ---------------------------------------------------------------------
// WorkflowRouter::resolvePlan scenarios
// ---------------------------------------------------------------------

void testExplicitProfileSelectionResolves()
{
    Catalog catalog;
    const Provider provider = makeProvider(catalog, "P1");
    const Model model = makeModel(catalog, provider.id, "model-a");
    StageModelAssignment architect {"architect", provider.id, model.id, 5000ms};
    const auto profile = catalog.profileService.createProfile(
        {"Profile", "mock-sequential-v1", true, {architect}});
    assert(profile.success());

    WorkflowRouter router(catalog.profiles, catalog.providers, catalog.models);
    StartRunRequest request;
    request.projectId = "p";
    request.title = "t";
    request.task = "task";
    request.executionProfileId = profile.value();
    const auto plan = router.resolvePlan(request);
    assert(plan.success());
    assert(plan.value().workflowId == kExecutionProfileWorkflowId);
    assert(plan.value().executionProfileId == profile.value());
    assert(plan.value().assignments.size() == 1);
}

void testDirectRunRosterResolvesAndKeepsAgentMetadata()
{
    Catalog catalog;
    const Provider provider = makeProvider(catalog, "Roster Provider");
    const Model model = makeModel(catalog, provider.id, "roster-model");

    StageModelAssignment worker {"agent-2", provider.id, model.id, 5000ms};
    worker.agentName = "Backend Agent";
    worker.role = "Backend engineer";
    worker.instructions = "Implement the service boundary";
    worker.position = 1;

    StageModelAssignment architect {"architect", provider.id, model.id, 5000ms};
    architect.agentName = "Lead Architect";
    architect.role = "Software Architect";
    architect.position = 0;
    architect.leadArchitect = true;

    WorkflowRouter router(catalog.profiles, catalog.providers, catalog.models);
    StartRunRequest request;
    request.projectId = "project";
    request.title = "Dynamic roster";
    request.task = "Build it";
    request.assignmentOverrides = {worker, architect};

    const auto plan = router.resolvePlan(request);
    assert(plan.success());
    assert(plan.value().workflowId == kExecutionProfileWorkflowId);
    assert(!plan.value().executionProfileId.has_value());
    assert(plan.value().assignments.size() == 2);
    assert(plan.value().assignments[0].leadArchitect);
    assert(plan.value().assignments[1].role == "Backend engineer");

    request.executionProfileId = "ambiguous";
    const auto ambiguous = router.resolvePlan(request);
    assert(!ambiguous.success());
    assert(ambiguous.error()->code == "run.ambiguous_execution_plan");
}

void testDefaultProfileSelectionForMatchingWorkflow()
{
    Catalog catalog;
    const Provider provider = makeProvider(catalog, "P1");
    const Model model = makeModel(catalog, provider.id, "model-a");
    StageModelAssignment architect {"architect", provider.id, model.id, 5000ms};
    const auto profile = catalog.profileService.createProfile(
        {"Profile", "custom-workflow", true, {architect}});
    assert(profile.success());
    assert(catalog.profileService.setDefaultProfile(profile.value()).success());

    WorkflowRouter router(catalog.profiles, catalog.providers, catalog.models);
    StartRunRequest request;
    request.projectId = "p";
    request.title = "t";
    request.task = "task";
    request.workflowId = "custom-workflow";
    const auto plan = router.resolvePlan(request);
    assert(plan.success());
    assert(plan.value().executionProfileId == profile.value());

    // No default profile targets a *different* workflow id.
    StartRunRequest other = request;
    other.workflowId = "another-workflow";
    const auto otherPlan = router.resolvePlan(other);
    assert(!otherPlan.success());
    assert(otherPlan.error()->code == "execution_profile.no_default_for_workflow");
}

void testDisabledOrMissingProfileRejected()
{
    Catalog catalog;
    WorkflowRouter router(catalog.profiles, catalog.providers, catalog.models);

    StartRunRequest missing;
    missing.projectId = "p";
    missing.title = "t";
    missing.task = "task";
    missing.executionProfileId = "no-such-profile";
    const auto missingPlan = router.resolvePlan(missing);
    assert(!missingPlan.success() && missingPlan.error()->code == "execution_profile.not_found");

    const Provider provider = makeProvider(catalog, "P1");
    const Model model = makeModel(catalog, provider.id, "model-a");
    StageModelAssignment architect {"architect", provider.id, model.id, 5000ms};
    const auto profile = catalog.profileService.createProfile(
        {"Profile", "mock-sequential-v1", /*enabled=*/false, {architect}});
    assert(profile.success());

    StartRunRequest disabled = missing;
    disabled.executionProfileId = profile.value();
    const auto disabledPlan = router.resolvePlan(disabled);
    assert(!disabledPlan.success() && disabledPlan.error()->code == "execution_profile.disabled");
}

void testInvalidAssignmentRevalidatedAtResolveTime()
{
    // A profile is valid when saved, but a provider disabled *afterward*
    // must still be caught when the profile is actually resolved for a run.
    Catalog catalog;
    const Provider provider = makeProvider(catalog, "P1");
    const Model model = makeModel(catalog, provider.id, "model-a");
    StageModelAssignment architect {"architect", provider.id, model.id, 5000ms};
    const auto profile = catalog.profileService.createProfile(
        {"Profile", "mock-sequential-v1", true, {architect}});
    assert(profile.success());
    assert(catalog.catalog.setProviderEnabled(provider.id, false).success());

    WorkflowRouter router(catalog.profiles, catalog.providers, catalog.models);
    StartRunRequest request;
    request.projectId = "p";
    request.title = "t";
    request.task = "task";
    request.executionProfileId = profile.value();
    const auto plan = router.resolvePlan(request);
    assert(!plan.success() && plan.error()->code == "provider.disabled");
}

// ---------------------------------------------------------------------
// End-to-end through RunManager + ExecutionProfileWorkflowRunner
// ---------------------------------------------------------------------

struct ProductionHarness {
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    Catalog catalog;
    FakeProviderClientFactory clientFactory;
    ExecutionProfileWorkflowRunner productionRunner {clientFactory, catalog.providers, catalog.models};
    MockWorkflowRunner mockRunner {1ms, 1};
    WorkflowRouter router {catalog.profiles, catalog.providers, catalog.models};
    RunManager manager {projects, runs, router};
    OrchestrationService orchestration {projects, runs, manager};
    ProjectService projectService {projects};

    ProductionHarness()
    {
        router.registerRunner(kMockWorkflowId, mockRunner);
        router.registerRunner(kExecutionProfileWorkflowId, productionRunner);
    }
};

void testStageAssignmentsRouteToCorrectClientsAndContextPropagates()
{
    ProductionHarness harness;
    const Provider providerA = makeProvider(harness.catalog, "Architect Provider");
    const Model modelA = makeModel(harness.catalog, providerA.id, "architect-model");
    const Provider providerB = makeProvider(harness.catalog, "Implementation Provider", "anthropic");
    const Model modelB = makeModel(harness.catalog, providerB.id, "implementation-model");

    FakeProviderClient clientA;
    FakeProviderClient clientB;
    clientA.setOutcomeForStage("architect-model", {true, "ARCHITECTURE_PLAN_TEXT", "", "", false});
    clientB.setOutcomeForStage("implementation-model", {true, "IMPLEMENTATION_TEXT", "", "", false});
    harness.clientFactory.registerClient(providerA.id, clientA);
    harness.clientFactory.registerClient(providerB.id, clientB);

    StageModelAssignment architect {"architect", providerA.id, modelA.id, 5000ms};
    StageModelAssignment implementation {"implementation", providerB.id, modelB.id, 5000ms};
    const auto profile = harness.catalog.profileService.createProfile(
        {"Profile", "mock-sequential-v1", true, {architect, implementation}});
    assert(profile.success());

    const std::string description = "UNIQUE_PROJECT_DESCRIPTION_MARKER";
    const Project project = makeActiveProject(harness.projectService, "route", description);
    const std::string task = "UNIQUE_RUN_TASK_MARKER";

    StartRunRequest request;
    request.projectId = project.id;
    request.title = "Routing run";
    request.task = task;
    request.executionProfileId = profile.value();
    const auto started = harness.orchestration.startRun(request);
    assert(started.success());

    const Run finished = waitForTerminalRun(harness.runs, started.value());
    assert(finished.status == RunStatus::Completed);

    // Each fake client only ever saw the request meant for its own stage.
    const auto requestsA = clientA.capturedRequests();
    const auto requestsB = clientB.capturedRequests();
    assert(requestsA.size() == 2 && requestsA.front().apiModelId == "architect-model");
    assert(requestsB.size() == 1 && requestsB.front().apiModelId == "implementation-model");

    // Project description and run task reached the provider request.
    assert(requestsA.front().userContent.find(description) != std::string::npos);
    assert(requestsA.front().userContent.find(task) != std::string::npos);
    assert(requestsA.front().userContent.find("# Project Memory")
        == std::string::npos
        && "A run without selected memory must keep the old prompt shape");
    assert(requestsA.front().userContent.find(
        "# Run Task (authoritative - this is what you must do now)")
        != std::string::npos
        && "The Run Task heading must stay explicitly authoritative even without memory");
    assert(requestsA.front().userContent.find("background context only")
        == std::string::npos
        && "No memory framing text should appear when there is no memory");

    // The implementation stage's context includes the architect stage's
    // output text (prior-stage propagation).
    assert(requestsB.front().userContent.find("ARCHITECTURE_PLAN_TEXT") != std::string::npos);
    // The architect is invoked once more for the synthetic final synthesis,
    // and that request receives the completed worker output.
    assert(requestsA.back().userContent.find("IMPLEMENTATION_TEXT") != std::string::npos);

    // The immutable snapshot captured on the Run matches what was used.
    assert(finished.executionProfileId == profile.value());
    assert(finished.assignmentSnapshot.size() == 2);
}

void testIncrementalProviderUsageSurvivesFailedRunFinalization()
{
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    Catalog catalog;
    ProjectService projectService {projects};
    BudgetPersistingWorkflowRunner runner;
    WorkflowRouter router {catalog.profiles, catalog.providers, catalog.models};
    router.registerRunner(kMockWorkflowId, runner);
    RunManager manager {projects, runs, router};
    OrchestrationService orchestration {projects, runs, manager};

    const Project project = makeActiveProject(projectService, "budget-failed-run");
    StartRunRequest request;
    request.projectId = project.id;
    request.title = "Budget persistence failure";
    request.task = "Persist usage before failure";
    request.workflowId = kMockWorkflowId;
    const auto started = orchestration.startRun(request);
    assert(started.success());

    const Run finished = waitForTerminalRun(runs, started.value());
    assert(finished.status == RunStatus::Failed);
    assert(finished.providerBudgetUsage.turnsStarted == 2);
    assert(finished.providerBudgetUsage.turnsCompleted == 1);
    assert(finished.providerBudgetUsage.inputTokens == 321);
    assert(finished.providerBudgetUsage.outputTokens == 45);
    assert(finished.providerBudgetUsage.toolCalls == 3);
    assert(finished.providerBudgetUsage.toolResultBytes == 987);
    assert(finished.providerBudgetUsage.cacheReadInputTokens == 1'000);
    assert(finished.providerBudgetUsage.cacheCreationInputTokens == 100);
    // run.usage must stay the honest full total (fresh + cache-write +
    // cache-read), never collapse to providerBudgetUsage's budget-countable
    // inputTokens (321, which excludes the 1,000 cache-read tokens above).
    assert(finished.usage.inputTokens == 321 + 1'000 && finished.usage.outputTokens == 45);
    assert(finished.usage.cacheCreationInputTokens == 100);
    assert(finished.usage.cacheReadInputTokens == 1'000);
    manager.shutdown();
}

void testIncrementalProviderUsageSurvivesCancellation()
{
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    Catalog catalog;
    ProjectService projectService {projects};
    BudgetPersistingWorkflowRunner runner {/*awaitCancellation=*/true};
    WorkflowRouter router {catalog.profiles, catalog.providers, catalog.models};
    router.registerRunner(kMockWorkflowId, runner);
    RunManager manager {projects, runs, router};
    OrchestrationService orchestration {projects, runs, manager};

    const Project project = makeActiveProject(projectService, "budget-cancelled-run");
    StartRunRequest request;
    request.projectId = project.id;
    request.title = "Budget persistence cancellation";
    request.task = "Persist usage before cancellation";
    request.workflowId = kMockWorkflowId;
    const auto started = orchestration.startRun(request);
    assert(started.success());

    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto found = runs.findRun(started.value());
        assert(found.success() && found.value().has_value());
        if (found.value()->providerBudgetUsage.turnsStarted == 2) break;
        std::this_thread::sleep_for(1ms);
    }
    const auto beforeCancel = runs.findRun(started.value());
    assert(beforeCancel.success() && beforeCancel.value().has_value());
    assert(beforeCancel.value()->providerBudgetUsage.turnsStarted == 2);
    assert(orchestration.cancelRun(started.value()).success());

    const Run finished = waitForTerminalRun(runs, started.value());
    assert(finished.status == RunStatus::Cancelled);
    assert(finished.providerBudgetUsage.inputTokens == 321);
    assert(finished.providerBudgetUsage.outputTokens == 45);
    assert(finished.usage.inputTokens == 321 + 1'000 && finished.usage.outputTokens == 45);
    assert(finished.usage.cacheCreationInputTokens == 100);
    assert(finished.usage.cacheReadInputTokens == 1'000);
    manager.shutdown();
}

void testImmutableSnapshotSurvivesProfileChangeAndRetryReusesOriginal()
{
    ProductionHarness harness;
    const Provider provider = makeProvider(harness.catalog, "P1");
    const Model modelOld = makeModel(harness.catalog, provider.id, "model-old");
    const Model modelNew = makeModel(harness.catalog, provider.id, "model-new");

    FakeProviderClient client;
    client.setOutcomeForStage("model-old", {true, "OLD_OUTPUT", "", "", false});
    client.setOutcomeForStage("model-new", {true, "NEW_OUTPUT", "", "", false});
    harness.clientFactory.registerClient(provider.id, client);

    StageModelAssignment architectOld {"architect", provider.id, modelOld.id, 5000ms};
    const auto profile = harness.catalog.profileService.createProfile(
        {"Profile", "mock-sequential-v1", true, {architectOld}});
    assert(profile.success());
    assert(harness.catalog.profileService.setDefaultProfile(profile.value()).success());

    const Project project = makeActiveProject(harness.projectService, "retry-snapshot");
    StartRunRequest request;
    request.projectId = project.id;
    request.title = "Snapshot run";
    request.task = "task";
    request.executionProfileId = profile.value();
    const auto started = harness.orchestration.startRun(request);
    assert(started.success());
    const Run original = waitForTerminalRun(harness.runs, started.value());
    assert(original.status == RunStatus::Completed);
    assert(original.assignmentSnapshot.front().modelId == modelOld.id);

    // Now repoint the profile to a different model - the *saved* profile
    // changes, but the original run's own snapshot must not.
    StageModelAssignment architectNew {"architect", provider.id, modelNew.id, 5000ms};
    const auto updated = harness.catalog.profileService.updateProfile(
        {profile.value(), "Profile", "mock-sequential-v1", {architectNew}});
    assert(updated.success());

    const auto draft = harness.orchestration.prepareRerunDraft(original.id);
    assert(draft.success());
    assert(draft.value().launchMode == RunLaunchMode::SavedExecutionProfile);
    assert(draft.value().request.executionProfileId == profile.value());
    assert(draft.value().request.assignmentOverrides.empty());
    auto editedRequest = draft.value().request;
    editedRequest.title = "Edited profile rerun";
    const auto edited = harness.orchestration.startEditedRerun(
        original.id, std::move(editedRequest));
    assert(edited.success());
    const Run editedRun = waitForTerminalRun(harness.runs, edited.value());
    assert(editedRun.parentRunId == original.id);
    assert(editedRun.assignmentSnapshot.front().modelId == modelNew.id);

    const auto retried = harness.orchestration.retryRun(original.id);
    assert(retried.success());
    const Run retry = waitForTerminalRun(harness.runs, retried.value());
    assert(retry.status == RunStatus::Completed);

    // The retry executed against the *original* snapshot (model-old), not
    // the now-updated profile (model-new). Both the original run and the
    // retry are lone-architect graphs, so each makes exactly one call (no
    // synthesis stage is appended when there is nothing else to reconcile).
    assert(retry.assignmentSnapshot.front().modelId == modelOld.id);
    const auto requests = client.capturedRequests();
    const bool sawOldModelTwice = std::count_if(requests.begin(), requests.end(),
        [](const ProviderChatRequest& r) { return r.apiModelId == "model-old"; }) == 2;
    assert(sawOldModelTwice && "Retry must reuse the original run's model");
}

void testLegacyReadOnlyRunProducesUnifiedRerunDraft()
{
    ProductionHarness harness;
    const Provider provider = makeProvider(harness.catalog, "Legacy Provider");
    const Model model = makeModel(harness.catalog, provider.id, "legacy-model");
    const Project project = makeActiveProject(harness.projectService, "legacy-rerun");

    StageModelAssignment legacyAssignment {
        "analysis", provider.id, model.id, 5000ms};
    legacyAssignment.position = 0;

    Run legacy;
    legacy.id = "legacy-read-only-run";
    legacy.projectId = project.id;
    legacy.title = "Historical analysis";
    legacy.task = "Inspect workspace";
    legacy.workflowId = "read-only-analysis-v1";
    legacy.assignmentSnapshot = {legacyAssignment};
    legacy.maxParallelism = 1;
    legacy.workspacePath = project.rootPath;
    legacy.status = RunStatus::Completed;
    legacy.createdAt = std::chrono::system_clock::now();
    legacy.finishedAt = legacy.createdAt;
    assert(harness.runs.insertRun(legacy).success());

    const auto draft = harness.orchestration.prepareRerunDraft(legacy.id);
    assert(draft.success());
    assert(draft.value().migratedFromLegacyWorkflow);
    assert(draft.value().launchMode == RunLaunchMode::CustomRoster);
    assert(draft.value().request.workflowId == kExecutionProfileWorkflowId);
    assert(draft.value().request.assignmentOverrides.size() == 1);
    assert(draft.value().request.assignmentOverrides.front().workspaceAccess);
    assert(draft.value().request.assignmentOverrides.front().leadArchitect);
    assert(draft.value().request.assignmentOverrides.front().maxAttempts == 1);

    // AG-17.7C: a Retry of this retired read-only Run is NOT adapted onto the legacy roster
    // runner any more: that runner cannot execute a lone workspace lead (it fails closed with
    // run.workspace_ownership_missing). The Retry converts to a single-assignment Manifest Run -
    // automatically, because nothing is dropped - and records a not_required_safe_retry
    // conversion. (The Edit & rerun DRAFT above still reopens the legacy form; submitting it
    // unchanged is refused by startEditedRerun, see LegacyConversionCommandTests.)
    harness.router.registerRunner(kExecutionManifestWorkflowId, harness.mockRunner);
    const auto retried = harness.orchestration.retryRun(legacy.id);
    assert(retried.success());
    const Run retry = waitForTerminalRun(harness.runs, retried.value());
    assert(retry.parentRunId == legacy.id);
    assert(retry.workflowId == kExecutionManifestWorkflowId);
    const auto conversion = harness.orchestration.findLegacyConversion(retry.id);
    assert(conversion.success() && conversion.value().has_value()
        && conversion.value()->confirmationMode == LegacyConversionConfirmationMode::NotRequiredSafeRetry
        && !conversion.value()->confirmedAt.has_value());
}

void testExecutionGraphRunsWorkersInParallelRetriesFallsBackAndSynthesizes()
{
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryModelExecutionRepository executionRepository;
    Catalog catalog;
    ProjectService projectService {projects};
    ModelExecutionService executionService {executionRepository};

    const Provider primary = makeProvider(catalog, "Primary");
    const Provider fallback = makeProvider(catalog, "Fallback", "anthropic");
    const Model architectModel = makeModel(catalog, primary.id, "architect-model");
    const Model workerAModel = makeModel(catalog, primary.id, "worker-a-model");
    const Model workerBModel = makeModel(catalog, primary.id, "worker-b-model");
    const Model fallbackModel = makeModel(catalog, fallback.id, "fallback-model");

    GraphProviderClient client;
    client.setScript("architect-model", {
        {true, "ARCHITECT_PLAN", {}, 1ms},
        {true, "FINAL_SYNTHESIS", {}, 1ms}});
    client.setScript("worker-a-model", {
        {true, "WORKER_A_RESULT", {}, 80ms}});
    client.setScript("worker-b-model", {
        {false, {}, "provider.timeout", 60ms},
        {false, {}, "provider.timeout", 1ms}});
    client.setScript("fallback-model", {
        {true, "WORKER_B_FALLBACK_RESULT", {}, 1ms}});

    FakeProviderClientFactory factory;
    factory.registerClient(primary.id, client);
    factory.registerClient(fallback.id, client);
    ExecutionProfileWorkflowRunner runner(
        factory, catalog.providers, catalog.models, nullptr, &executionService);
    MockWorkflowRunner mock {1ms, 1};
    WorkflowRouter router {catalog.profiles, catalog.providers, catalog.models};
    router.registerRunner(kMockWorkflowId, mock);
    router.registerRunner(kExecutionProfileWorkflowId, runner);
    RunManager manager {projects, runs, router};
    OrchestrationService orchestration {projects, runs, manager};

    StageModelAssignment architect;
    architect.stageType = "architect";
    architect.providerId = primary.id;
    architect.modelId = architectModel.id;
    architect.timeout = 5s;
    architect.agentName = "Lead architect";
    architect.role = "architect";
    architect.position = 0;
    architect.leadArchitect = true;

    StageModelAssignment workerA;
    workerA.stageType = "worker-a";
    workerA.providerId = primary.id;
    workerA.modelId = workerAModel.id;
    workerA.timeout = 5s;
    workerA.agentName = "Worker A";
    workerA.role = "implementation";
    workerA.position = 1;

    StageModelAssignment workerB;
    workerB.stageType = "worker-b";
    workerB.providerId = primary.id;
    workerB.modelId = workerBModel.id;
    workerB.timeout = 5s;
    workerB.agentName = "Worker B";
    workerB.role = "review";
    workerB.position = 2;
    workerB.maxAttempts = 2;
    workerB.fallbackProviderId = fallback.id;
    workerB.fallbackModelId = fallbackModel.id;

    const Project project = makeActiveProject(projectService, "graph-run");
    StartRunRequest request;
    request.projectId = project.id;
    request.title = "Graph run";
    request.task = "Implement and review the feature";
    request.assignmentOverrides = {architect, workerA, workerB};
    request.maxParallelism = 2;
    const auto started = orchestration.startRun(request);
    assert(started.success());

    const Run finished = waitForTerminalRun(runs, started.value());
    assert(finished.status == RunStatus::Completed);
    assert(finished.maxParallelism == 2);
    assert(finished.usage.inputTokens == 44 && finished.usage.outputTokens == 28);
    assert(client.maximumConcurrency() >= 2);

    const auto stages = orchestration.listStages(finished.id);
    assert(stages.success() && stages.value().size() == 4);
    assert(stages.value().back().type == "synthesis");
    assert(std::all_of(stages.value().begin(), stages.value().end(), [](const auto& stage) {
        return stage.status == StageStatus::Completed;
    }));

    const auto attempts = executionService.listExecutions(
        ModelExecutionSource::Run, finished.id);
    assert(attempts.success() && attempts.value().size() == 6);
    const auto workerBAttempts = std::count_if(
        attempts.value().begin(), attempts.value().end(), [](const auto& attempt) {
            return attempt.role == "review";
        });
    assert(workerBAttempts == 3);
    assert(std::any_of(attempts.value().begin(), attempts.value().end(),
        [&](const auto& attempt) {
            return attempt.role == "review" && attempt.attempt == 3
                && attempt.providerId == fallback.id
                && attempt.modelId == fallbackModel.id
                && attempt.status == ModelExecutionStatus::Completed;
        }));

    const auto requests = client.capturedRequests();
    assert(std::all_of(requests.begin(), requests.end(), [](const auto& providerRequest) {
        return providerRequest.maxOutputTokens == 8192;
    }) && "Run requests must use each selected catalog model's output ceiling");
    assert(std::all_of(requests.begin(), requests.end(), [](const auto& providerRequest) {
        return providerRequest.disableTotalTimeout;
    }) && "Production Run requests must wait for a provider response until cancellation");
    const auto synthesis = std::find_if(requests.rbegin(), requests.rend(),
        [](const auto& providerRequest) {
            return providerRequest.apiModelId == "architect-model";
        });
    assert(synthesis != requests.rend());
    assert(synthesis->userContent.find("WORKER_A_RESULT") != std::string::npos);
    assert(synthesis->userContent.find("WORKER_B_FALLBACK_RESULT") != std::string::npos);
    manager.shutdown();
}

void testRunRosterRejectsCyclesAndInvalidParallelism()
{
    Catalog catalog;
    const Provider provider = makeProvider(catalog, "P1");
    const Model model = makeModel(catalog, provider.id, "model-a");
    MockWorkflowRunner mock {1ms, 1};
    WorkflowRouter router {catalog.profiles, catalog.providers, catalog.models};
    router.registerRunner(kMockWorkflowId, mock);

    StageModelAssignment architect;
    architect.stageType = "architect";
    architect.providerId = provider.id;
    architect.modelId = model.id;
    architect.timeout = 5s;
    architect.role = "architect";
    architect.position = 0;
    architect.leadArchitect = true;
    StageModelAssignment first = architect;
    first.stageType = "first";
    first.role = "worker";
    first.position = 1;
    first.leadArchitect = false;
    first.dependencies = {"second"};
    StageModelAssignment second = first;
    second.stageType = "second";
    second.position = 2;
    second.dependencies = {"first"};

    StartRunRequest request;
    request.projectId = "p";
    request.title = "t";
    request.task = "task";
    request.assignmentOverrides = {architect, first, second};
    const auto cyclic = router.resolvePlan(request);
    assert(!cyclic.success() && cyclic.error()->code == "run.dependency_cycle");

    request.assignmentOverrides.clear();
    request.maxParallelism = 0;
    const auto invalidLimit = router.resolvePlan(request);
    assert(!invalidLimit.success()
        && invalidLimit.error()->code == "run.invalid_parallelism");
}

void testCancellationDuringProductionRun()
{
    Catalog catalog;
    const Provider provider = makeProvider(catalog, "P1");
    const Model model = makeModel(catalog, provider.id, "model-a");
    FakeProviderClient client;
    client.setOutcomeForStage("model-a", {true, "unused", "", "", /*observeCancellation=*/true});
    FakeProviderClientFactory factory;
    factory.registerClient(provider.id, client);

    ExecutionProfileWorkflowRunner runner(factory, catalog.providers, catalog.models);
    StageModelAssignment architect {"architect", provider.id, model.id, 5000ms};
    ResolvedExecutionPlan plan;
    plan.workflowId = kExecutionProfileWorkflowId;
    plan.assignments = {architect};
    const auto stages = runner.createStages("run-1", plan);
    assert(stages.size() == 1);

    Run run;
    run.id = "run-1";
    run.assignmentSnapshot = {architect};
    Project project;
    project.description = "d";
    run.task = "t";

    std::stop_source source;
    source.request_stop();

    struct NullSink final : IRunEventSink {
        void publish(const RunEvent&) override { }
    } sink;

    const WorkflowRunContext context {run, project, stages};
    const auto result = runner.run(context, source.get_token(), sink);
    assert(!result.success);
    assert(result.finalStatus == RunStatus::Cancelled);
}

// MW-P0 regression: ManifestWorkflowRunner is not the only provider-call
// loop in this codebase - ExecutionProfileWorkflowRunner drives its own
// single-shot Architect/Implementation/Documentation/synthesis calls
// directly through IProviderClientFactory, entirely independent of
// AgentToolTurnLoopRunner/ExecutionManifestPlanner. A Run-wide
// ProviderSessionBudgetGuard constructed only for the Manifest workflow
// would leave this second, real production path completely unbounded -
// exactly the kind of drift the shared-guard requirement exists to
// prevent. This proves the guard is actually consulted here too: a
// single-architect Run whose frozen policy caps input tokens below what
// the (fixed 7/5) FakeProviderClient response reports must fail closed
// with a stable provider_budget.* code, never silently succeed.
void testExecutionProfileWorkflowRunnerEnforcesProviderBudget()
{
    Catalog catalog;
    const Provider provider = makeProvider(catalog, "P1");
    const Model model = makeModel(catalog, provider.id, "model-a");
    FakeProviderClient client;
    client.setOutcomeForStage("model-a", {true, "ARCHITECT_OUTPUT", "", "", false});
    FakeProviderClientFactory factory;
    factory.registerClient(provider.id, client);

    ExecutionProfileWorkflowRunner runner(factory, catalog.providers, catalog.models);
    StageModelAssignment architect {"architect", provider.id, model.id, 5000ms};
    architect.leadArchitect = true;
    ResolvedExecutionPlan plan;
    plan.workflowId = kExecutionProfileWorkflowId;
    plan.assignments = {architect};
    const auto stages = runner.createStages("run-budget", plan);
    assert(stages.size() == 1);

    Run run;
    run.id = "run-budget";
    run.assignmentSnapshot = {architect};
    // Frozen policy: the fake client always reports 7 input / 5 output
    // tokens, so a 5-token cap must be crossed by the very first turn.
    run.providerBudget.maxInputTokens = 5;
    Project project;
    project.description = "d";
    run.task = "t";

    struct NullSink final : IRunEventSink {
        void publish(const RunEvent&) override { }
    } sink;

    const WorkflowRunContext context {run, project, stages};
    const auto result = runner.run(context, std::stop_token {}, sink);
    assert(!result.success);
    assert(result.finalStatus == RunStatus::Failed);
    assert(result.message.find("provider_budget.input_token_limit_reached") == 0);
    // The already-incurred cost of the one admitted, budget-crossing turn is
    // still truthfully accounted for - the guard fails future requests
    // closed, it does not pretend the spend never happened.
    assert(result.usage.inputTokens == 7);
    assert(client.capturedRequests().size() == 1);
}

// M2b: the legacy single-shot attempt is ONE provider response, so its execution row is priced
// by classifying that response against the model's long-context threshold by its own
// input_tokens. Each case is a real single-architect Run through runner.run() with a
// ModelExecutionService, answered by a scripted client.
void testLegacySingleShotAttemptIsPricedPerResponseByTheModelsLongContextTier()
{
    const auto usageOf = [](std::uint64_t input, std::uint64_t output,
                            std::uint64_t cacheWrite, std::uint64_t cacheRead) {
        ai::models::TokenUsage usage;
        usage.inputTokens = input;
        usage.outputTokens = output;
        usage.cacheCreationInputTokens = cacheWrite;
        usage.cacheReadInputTokens = cacheRead;
        return usage;
    };
    // $2 / $0.2 / $2.5 / $10 flat and `tier` above the threshold.
    const auto executionRowFor = [](const ai::models::TokenUsage& usage, const LongContextPricing& tier) {
        Catalog catalog;
        const Provider provider = makeProvider(catalog, "P1");
        const auto created = catalog.catalog.createModel(
            {provider.id, "tier-model", "tier-model", 1'050'000, 4096, 2'000'000, 10'000'000,
             2'500'000, 200'000, true, tier});
        assert(created.success());
        FakeProviderClient client;
        FakeProviderClient::Outcome outcome {true, "ARCHITECT_OUTPUT", "", "", false};
        outcome.usage = usage;
        client.setOutcomeForStage("tier-model", outcome);
        FakeProviderClientFactory factory;
        factory.registerClient(provider.id, client);

        InMemoryModelExecutionRepository executionRepository;
        ModelExecutionService executionService {executionRepository};
        ExecutionProfileWorkflowRunner runner(
            factory, catalog.providers, catalog.models, nullptr, &executionService);
        StageModelAssignment architect {"architect", provider.id, created.value(), 5000ms};
        architect.leadArchitect = true;
        ResolvedExecutionPlan plan;
        plan.workflowId = kExecutionProfileWorkflowId;
        plan.assignments = {architect};
        const auto stages = runner.createStages("run-priced", plan);
        assert(stages.size() == 1);

        Run run;
        run.id = "run-priced";
        run.assignmentSnapshot = {architect};
        run.task = "t";
        Project project;
        project.description = "d";
        struct NullSink final : IRunEventSink {
            void publish(const RunEvent&) override { }
        } sink;
        const WorkflowRunContext context {run, project, stages};
        const auto result = runner.run(context, std::stop_token {}, sink);
        assert(result.success);

        const auto rows = executionService.listExecutions(ModelExecutionSource::Run, run.id);
        assert(rows.success() && rows.value().size() == 1);
        return rows.value().front();
    };

    const LongContextPricing complete {272'000, 4'000'000, 15'000'000, 5'000'000, 400'000};

    // A long Response with cache usage (300,000 input, 50,000 written, 200,000 read) is billed
    // entirely at the long tariff: 50,000*4 + 200,000*0.4 + 50,000*5 + 1,000*15 = 545,000
    // (flat prices would give 275,000).
    const auto longMixed = executionRowFor(usageOf(300'000, 1'000, 50'000, 200'000), complete);
    assert(longMixed.inputTokens == 300'000 && longMixed.outputTokens == 1'000);
    assert(longMixed.costMicros == 545'000 && longMixed.costPriceKnown);

    // Exactly ON the threshold is still short; one token above flips the WHOLE Response.
    assert(executionRowFor(usageOf(272'000, 0, 0, 0), complete).costMicros == 544'000);
    assert(executionRowFor(usageOf(272'001, 0, 0, 0), complete).costMicros == 1'088'004);

    // A long Response using cache-write while its long cache-write price was never entered is a
    // lower bound - never a flat price for those tokens: 250,000*4 + 1,000*15 = 1,015,000, unknown.
    const LongContextPricing noLongCacheWrite {272'000, 4'000'000, 15'000'000, 0, 400'000};
    const auto partial = executionRowFor(usageOf(300'000, 1'000, 50'000, 0), noLongCacheWrite);
    assert(partial.costMicros == 1'015'000 && !partial.costPriceKnown);
}

void testProviderErrorMapping()
{
    Catalog catalog;
    const Provider provider = makeProvider(catalog, "P1");
    const Model model = makeModel(catalog, provider.id, "model-a");
    FakeProviderClientFactory factory;

    struct Case {
        const char* code;
        const char* message;
    };
    const Case cases[] = {
        {"provider.auth_failed", "401 unauthorized"},
        {"provider.rate_limited", "429 too many requests"},
        {"provider.timeout", "request timed out"},
        {"provider.network_error", "curl: could not connect"},
    };

    for (const auto& testCase : cases) {
        FakeProviderClient client;
        client.setOutcomeForStage("model-a", {false, "", testCase.code, testCase.message, false});
        FakeProviderClientFactory localFactory;
        localFactory.registerClient(provider.id, client);
        ExecutionProfileWorkflowRunner runner(localFactory, catalog.providers, catalog.models);

        StageModelAssignment architect {"architect", provider.id, model.id, 5000ms};
        ResolvedExecutionPlan plan;
        plan.workflowId = kExecutionProfileWorkflowId;
        plan.assignments = {architect};
        const auto stages = runner.createStages("run-err", plan);

        Run run;
        run.id = "run-err";
        run.assignmentSnapshot = {architect};
        Project project;
        project.description = "d";
        run.task = "t";

        struct NullSink final : IRunEventSink {
            void publish(const RunEvent&) override { }
        } sink;

        std::stop_source source;
        const WorkflowRunContext context {run, project, stages};
        const auto result = runner.run(context, source.get_token(), sink);
        assert(!result.success);
        assert(result.finalStatus == RunStatus::Failed);
        assert(result.message.find(testCase.code) != std::string::npos);
    }
}

void testMissingCredentialAndUnavailableProviderRuntime()
{
    Catalog catalog;
    const Provider provider = makeProvider(catalog, "P1");
    const Model model = makeModel(catalog, provider.id, "model-a");
    StageModelAssignment architect {"architect", provider.id, model.id, 5000ms};

    {
        FakeProviderClientFactory factory;
        factory.failEveryCallWith("credential.missing", "No credential is configured for this provider");
        ExecutionProfileWorkflowRunner runner(factory, catalog.providers, catalog.models);
        ResolvedExecutionPlan plan;
        plan.workflowId = kExecutionProfileWorkflowId;
        plan.assignments = {architect};
        const auto stages = runner.createStages("run-cred", plan);
        Run run;
        run.id = "run-cred";
        run.assignmentSnapshot = {architect};
        Project project;
        run.task = "t";
        struct NullSink final : IRunEventSink {
            void publish(const RunEvent&) override { }
        } sink;
        std::stop_source source;
        const auto result = runner.run({run, project, stages}, source.get_token(), sink);
        assert(!result.success && result.message.find("credential.missing") != std::string::npos);
    }
    {
        FakeProviderClientFactory factory;
        factory.failEveryCallWith("provider_runtime.unavailable", "No real provider adapter is compiled into this build");
        ExecutionProfileWorkflowRunner runner(factory, catalog.providers, catalog.models);
        ResolvedExecutionPlan plan;
        plan.workflowId = kExecutionProfileWorkflowId;
        plan.assignments = {architect};
        const auto stages = runner.createStages("run-unavail", plan);
        Run run;
        run.id = "run-unavail";
        run.assignmentSnapshot = {architect};
        Project project;
        run.task = "t";
        struct NullSink final : IRunEventSink {
            void publish(const RunEvent&) override { }
        } sink;
        std::stop_source source;
        const auto result = runner.run({run, project, stages}, source.get_token(), sink);
        assert(!result.success && result.message.find("provider_runtime.unavailable") != std::string::npos);
    }
}

// ---------------------------------------------------------------------
// CredentialManagementService
// ---------------------------------------------------------------------

void testCredentialManagementConfigureRemoveStatus()
{
    Catalog catalog;
    FakeCredentialStore store;
    CredentialManagementService credentials(store, catalog.catalog);
    const Provider provider = makeProvider(catalog, "P1");

    const auto initialStatus = credentials.credentialStatus(provider.id);
    assert(initialStatus.success() && initialStatus.value() == CredentialStatus::NotConfigured);

    constexpr const char* secret = "sk-super-secret-value";
    const auto configured = credentials.setCredential(provider.id, secret);
    assert(configured.success());
    const auto afterConfigure = credentials.credentialStatus(provider.id);
    assert(afterConfigure.success() && afterConfigure.value() == CredentialStatus::Configured);
    // The public status/configure surface never returns the secret itself.
    const auto providerAfter = catalog.catalog.findProvider(provider.id);
    assert(providerAfter.value()->credentialReference.has_value());
    assert(*providerAfter.value()->credentialReference != secret);

    const auto removed = credentials.removeCredential(provider.id);
    assert(removed.success());
    const auto afterRemove = credentials.credentialStatus(provider.id);
    assert(afterRemove.success() && afterRemove.value() == CredentialStatus::NotConfigured);
    // Idempotent second removal.
    assert(credentials.removeCredential(provider.id).success());
}

void testCredentialErrorsNeverLeakSecret()
{
    class AlwaysFailingCredentialStore final : public ICredentialStore {
    public:
        common::Result<void> store(const std::string&, const std::string&) override
        {
            return common::Result<void>::failure({"credential.store_failed", "Simulated failure"});
        }
        common::Result<std::optional<std::string>> load(const std::string&) const override
        {
            return common::Result<std::optional<std::string>>::failure({"credential.load_failed", "Simulated failure"});
        }
        common::Result<void> remove(const std::string&) override
        {
            return common::Result<void>::failure({"credential.remove_failed", "Simulated failure"});
        }
    } store;
    Catalog catalog;
    CredentialManagementService credentials(store, catalog.catalog);
    const Provider provider = makeProvider(catalog, "P1");

    constexpr const char* secret = "sk-must-not-leak-anywhere";
    const auto result = credentials.setCredential(provider.id, secret);
    assert(!result.success());
    assert(result.error()->message.find(secret) == std::string::npos);
    assert(result.error()->code.find(secret) == std::string::npos);
}

// graphAssignments() only appends the synthetic "synthesis" stage when the
// architect is joined by at least one other agent - for a lone architect
// there is nothing to reconcile, so synthesizing would just resend the
// architect's own output through a second, redundant call to the same
// model. This calls ExecutionProfileWorkflowRunner::run() directly
// (bypassing RunManager, which never exposes the WorkflowResult it receives
// to a caller) to confirm a lone-architect run makes exactly one provider
// call and its finalOutput is the architect's own output.
void testExecutionProfileWorkflowRunnerLoneArchitectSkipsSynthesis()
{
    Catalog catalog;
    FakeProviderClientFactory clientFactory;
    ExecutionProfileWorkflowRunner runner(clientFactory, catalog.providers, catalog.models);

    const Provider provider = makeProvider(catalog, "Solo Provider");
    const Model model = makeModel(catalog, provider.id, "solo-model");
    FakeProviderClient client;
    client.setOutcomeForStage("solo-model", {true, "SOLO_SYNTHESIS_OUTPUT_MARKER", "", "", false});
    clientFactory.registerClient(provider.id, client);

    StageModelAssignment architect {"architect", provider.id, model.id, 5000ms};
    architect.leadArchitect = true;

    ResolvedExecutionPlan plan;
    plan.workflowId = kExecutionProfileWorkflowId;
    plan.assignments = {architect};
    plan.maxParallelism = 1;

    Run run;
    run.id = "direct-production-run";
    run.projectId = "direct-production-project";
    run.title = "Direct runner call";
    run.task = "Directly exercise the production runner's WorkflowResult";
    run.workflowId = kExecutionProfileWorkflowId;
    run.assignmentSnapshot = plan.assignments;
    run.maxParallelism = 1;
    run.status = RunStatus::Running;

    Project project;
    project.id = run.projectId;
    project.name = "Direct project";

    const auto stages = runner.createStages(run.id, plan);
    const WorkflowRunContext context {run, project, stages};

    class NullEventSink final : public IRunEventSink {
    public:
        void publish(const RunEvent&) override {}
    } sink;

    const WorkflowResult result = runner.run(context, std::stop_token {}, sink);
    assert(result.success);
    assert(result.finalStatus == RunStatus::Completed);
    assert(!result.finalOutput.empty());
    assert(result.finalOutput.find("SOLO_SYNTHESIS_OUTPUT_MARKER") != std::string::npos);
    assert(client.capturedRequests().size() == 1
        && "A lone architect must not trigger a redundant synthesis call");
    assert(stages.size() == 1 && stages.front().type == "architect"
        && "No synthetic synthesis stage should be persisted for a lone architect");
}

void testProductionRunReceivesPriorProjectMemory()
{
    Catalog catalog;
    FakeProviderClientFactory clientFactory;
    FixedProjectMemoryRepository memoryRepository;

    const Provider provider = makeProvider(catalog, "Memory Provider");
    const Model model = makeModel(catalog, provider.id, "memory-model");
    FakeProviderClient client;
    client.setOutcomeForStage(
        "memory-model", {true, "MEMORY_RUN_OUTPUT", "", "", false});
    clientFactory.registerClient(provider.id, client);

    ProjectMemorySegment older;
    older.id = "memory-older";
    older.projectId = "memory-project";
    older.runId = "previous-run-1";
    older.summary = "PRIOR_PROJECT_DECISION_ALPHA";
    older.createdAt = std::chrono::system_clock::time_point {1s};
    ProjectMemorySegment newer = older;
    newer.id = "memory-newer";
    newer.runId = "previous-run-2";
    newer.summary = "PRIOR_PROJECT_DECISION_BETA";
    newer.createdAt = std::chrono::system_clock::time_point {2s};
    memoryRepository.segments = {newer, older};

    ExecutionProfileWorkflowRunner runner(
        clientFactory, catalog.providers, catalog.models,
        nullptr, nullptr, &memoryRepository);

    StageModelAssignment architect {
        "architect", provider.id, model.id, 5000ms};
    architect.leadArchitect = true;
    ResolvedExecutionPlan plan;
    plan.workflowId = kExecutionProfileWorkflowId;
    plan.assignments = {architect};

    Run run;
    run.id = "memory-current-run";
    run.projectId = "memory-project";
    run.title = "Use prior memory";
    run.task = "CURRENT_RUN_TASK_MARKER";
    run.workflowId = kExecutionProfileWorkflowId;
    run.assignmentSnapshot = plan.assignments;
    run.status = RunStatus::Running;
    Project project;
    project.id = run.projectId;
    project.description = "PROJECT_DESCRIPTION_MARKER";
    const auto stages = runner.createStages(run.id, plan);

    class NullEventSink final : public IRunEventSink {
    public:
        void publish(const RunEvent&) override {}
    } sink;

    const auto result = runner.run(
        {run, project, stages}, std::stop_token {}, sink);
    assert(result.success);

    const auto requests = client.capturedRequests();
    assert(!requests.empty());
    for (const auto& request : requests) {
        const auto descriptionPosition =
            request.userContent.find("PROJECT_DESCRIPTION_MARKER");
        const auto memoryHeadingPosition =
            request.userContent.find("# Project Memory");
        const auto olderPosition =
            request.userContent.find("PRIOR_PROJECT_DECISION_ALPHA");
        const auto newerPosition =
            request.userContent.find("PRIOR_PROJECT_DECISION_BETA");
        const auto taskPosition =
            request.userContent.find("CURRENT_RUN_TASK_MARKER");
        assert(descriptionPosition < memoryHeadingPosition);
        assert(memoryHeadingPosition < olderPosition);
        assert(olderPosition < newerPosition);
        assert(newerPosition < taskPosition);

        // Explicit primacy framing: memory must be marked background-only
        // before the model sees it, and the Run Task must be reiterated as
        // authoritative with an explicit instruction to disregard unrelated
        // memory content - restores the 2026-07-24 fix that was lost when
        // ReadOnlyAnalysisWorkflowRunner was merged into this runner.
        const auto backgroundNoticePosition =
            request.userContent.find("background context only");
        const auto authoritativeHeadingPosition = request.userContent.find(
            "# Run Task (authoritative - this is what you must do now)");
        const auto ignoreInstructionPosition =
            request.userContent.find("Ignore any unrelated content from Project Memory");
        assert(memoryHeadingPosition != std::string::npos);
        assert(backgroundNoticePosition != std::string::npos
            && backgroundNoticePosition < memoryHeadingPosition
            && "The background-only notice must precede the memory section itself");
        assert(authoritativeHeadingPosition != std::string::npos
            && newerPosition < authoritativeHeadingPosition
            && authoritativeHeadingPosition < taskPosition
            && "The Run Task heading must stay authoritative and follow the memory section");
        assert(ignoreInstructionPosition != std::string::npos
            && taskPosition < ignoreInstructionPosition
            && "The ignore-unrelated-memory instruction must follow the actual task text");
    }
}

} // namespace

// AG-17.7D-1: `enabled=false` means no new provider session - for a Retry too, which replays a
// frozen snapshot and never re-validates the catalog. The legacy runner holds a Run to the switch at
// its ENTRY, before any stage starts and before any provider is called: the primary AND the fallback
// provider/model of every agent.
void testLegacyRunnerRefusesADisabledProviderOrModelBeforeAnyProviderCall()
{
    enum class Disabled { Nothing, PrimaryProvider, PrimaryModel, FallbackProvider, FallbackModel };
    struct Case {
        Disabled disabled;
        const char* expected;
    };
    const std::vector<Case> cases {
        {Disabled::Nothing, nullptr},
        {Disabled::PrimaryProvider, "run.execution_target_disabled: The primary provider of agent 'architect' is disabled"},
        {Disabled::PrimaryModel, "run.execution_target_disabled: The primary model of agent 'architect' is disabled"},
        {Disabled::FallbackProvider, "run.execution_target_disabled: The fallback provider of agent 'architect' is disabled"},
        {Disabled::FallbackModel, "run.execution_target_disabled: The fallback model of agent 'architect' is disabled"},
    };
    for (const auto& item : cases) {
        Catalog catalog;
        const Provider primary = makeProvider(catalog, "P1");
        const Model primaryModel = makeModel(catalog, primary.id, "model-a");
        const Provider fallback = makeProvider(catalog, "P2");
        const Model fallbackModel = makeModel(catalog, fallback.id, "model-b");
        FakeProviderClient client;
        client.setOutcomeForStage("model-a", {true, "ARCHITECT_OUTPUT", "", "", false});
        FakeProviderClientFactory factory;
        factory.registerClient(primary.id, client);
        factory.registerClient(fallback.id, client);
        switch (item.disabled) {
        case Disabled::Nothing: break;
        case Disabled::PrimaryProvider: assert(catalog.catalog.setProviderEnabled(primary.id, false).success()); break;
        case Disabled::PrimaryModel: assert(catalog.catalog.setModelEnabled(primaryModel.id, false).success()); break;
        case Disabled::FallbackProvider: assert(catalog.catalog.setProviderEnabled(fallback.id, false).success()); break;
        case Disabled::FallbackModel: assert(catalog.catalog.setModelEnabled(fallbackModel.id, false).success()); break;
        }

        ExecutionProfileWorkflowRunner runner(factory, catalog.providers, catalog.models);
        StageModelAssignment architect {"architect", primary.id, primaryModel.id, 5000ms};
        architect.fallbackProviderId = fallback.id;
        architect.fallbackModelId = fallbackModel.id;
        ResolvedExecutionPlan plan;
        plan.workflowId = kExecutionProfileWorkflowId;
        plan.assignments = {architect};
        const auto stages = runner.createStages("run-1", plan);

        Run run;
        run.id = "run-1";
        run.assignmentSnapshot = {architect};
        run.task = "t";
        Project project;
        project.description = "d";
        std::stop_source neverStopped;
        struct NullSink final : IRunEventSink {
            void publish(const RunEvent&) override { }
        } sink;

        const WorkflowRunContext context {run, project, stages};
        const auto result = runner.run(context, neverStopped.get_token(), sink);
        if (item.expected == nullptr) {
            // The control: with everything enabled the same snapshot runs, so the refusal below is
            // the switch and nothing else.
            assert(result.success && client.capturedRequests().size() == 1);
        } else {
            assert(!result.success && result.finalStatus == RunStatus::Failed && result.message == item.expected);
            assert(client.capturedRequests().empty() && "a disabled target must see no provider call at all");
        }
    }
}

int runWorkflowRoutingAndProductionTests()
{
    testExplicitProfileSelectionResolves();
    testDirectRunRosterResolvesAndKeepsAgentMetadata();
    testDefaultProfileSelectionForMatchingWorkflow();
    testDisabledOrMissingProfileRejected();
    testInvalidAssignmentRevalidatedAtResolveTime();
    testStageAssignmentsRouteToCorrectClientsAndContextPropagates();
    testIncrementalProviderUsageSurvivesFailedRunFinalization();
    testIncrementalProviderUsageSurvivesCancellation();
    testImmutableSnapshotSurvivesProfileChangeAndRetryReusesOriginal();
    testLegacyReadOnlyRunProducesUnifiedRerunDraft();
    testExecutionGraphRunsWorkersInParallelRetriesFallsBackAndSynthesizes();
    testRunRosterRejectsCyclesAndInvalidParallelism();
    testCancellationDuringProductionRun();
    testLegacyRunnerRefusesADisabledProviderOrModelBeforeAnyProviderCall();
    testExecutionProfileWorkflowRunnerEnforcesProviderBudget();
    testLegacySingleShotAttemptIsPricedPerResponseByTheModelsLongContextTier();
    testProviderErrorMapping();
    testMissingCredentialAndUnavailableProviderRuntime();
    testCredentialManagementConfigureRemoveStatus();
    testCredentialErrorsNeverLeakSecret();
    testExecutionProfileWorkflowRunnerLoneArchitectSkipsSynthesis();
    testProductionRunReceivesPriorProjectMemory();
    return 0;
}
