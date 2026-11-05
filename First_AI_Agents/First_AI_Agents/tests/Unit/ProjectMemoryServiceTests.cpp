// Task B (PROJECT_MEMORY_PLAN.md): tests for the deterministic, LLM-free
// ProjectMemoryService writer, its RunManager best-effort hook, and
// MockWorkflowRunner's WorkflowResult::finalOutput. ExecutionProfileWorkflowRunner's
// finalOutput is covered separately in WorkflowRoutingAndProductionTests.cpp,
// which already has the fake provider-client fixtures that test needs.
#include "ai/application/budget/CostCalculation.hpp"
#include "ai/application/inmemory/InMemoryRepositories.hpp"
#include "ai/application/execution/IWorkflowResolver.hpp"
#include "ai/application/execution/MockWorkflowRunner.hpp"
#include "ai/application/execution/OrchestrationService.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionService.hpp"
#include "ai/application/conversations/ProjectMemoryService.hpp"
#include "ai/application/projects/ProjectService.hpp"
#include "ai/application/execution/RunManager.hpp"

#include "RecordingOutcomeBriefTrigger.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace common = ai::common;
using namespace std::chrono_literals;
using namespace ai::application;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct TempProjectDirectory {
    fs::path path;

    TempProjectDirectory()
    {
        path = fs::temp_directory_path()
            / ("ai-orchestrator-project-memory-test-"
                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }

    ~TempProjectDirectory()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

// A minimal, test-only IProjectMemoryRepository double. Not production code:
// the real adapter (SqliteProjectMemoryRepository, Task A) is exercised in
// SqliteRepositoryTests.cpp/BackendRuntimeTests.cpp. failNextInsert lets a
// single test simulate a persistence failure without touching SQLite.
class FakeProjectMemoryRepository final : public IProjectMemoryRepository {
public:
    common::Result<ProjectMemorySegment> insertMemorySegment(ProjectMemorySegment value) override
    {
        std::lock_guard lock(mutex_);
        if (failNextInsert_) {
            failNextInsert_ = false;
            return common::Result<ProjectMemorySegment>::failure(
                {"project_memory.simulated_failure", "Simulated repository failure"});
        }
        segments_[value.projectId].push_back(value);
        return common::Result<ProjectMemorySegment>::ok(std::move(value));
    }

    [[nodiscard]] common::Result<std::vector<ProjectMemorySegment>> listMemorySegments(
        const ProjectId& projectId) const override
    {
        std::lock_guard lock(mutex_);
        const auto it = segments_.find(projectId);
        return common::Result<std::vector<ProjectMemorySegment>>::ok(
            it == segments_.end() ? std::vector<ProjectMemorySegment> {} : it->second);
    }

    [[nodiscard]] common::Result<std::optional<ProjectMemorySegment>> findMemorySegment(
        const ProjectMemorySegmentId& id) const override
    {
        std::lock_guard lock(mutex_);
        for (const auto& [projectId, list] : segments_) {
            static_cast<void>(projectId);
            const auto found = std::find_if(list.begin(), list.end(),
                [&id](const ProjectMemorySegment& segment) { return segment.id == id; });
            if (found != list.end()) {
                return common::Result<std::optional<ProjectMemorySegment>>::ok(*found);
            }
        }
        return common::Result<std::optional<ProjectMemorySegment>>::ok(std::nullopt);
    }

    common::Result<void> removeMemorySegments(const ProjectId& projectId) override
    {
        std::lock_guard lock(mutex_);
        segments_.erase(projectId);
        return common::Result<void>::ok();
    }

    common::Result<void> commitCompactedSegment(
        const ProjectMemoryCompactionRecord& record,
        ProjectMemorySegment newSegment)
    {
        std::lock_guard lock(mutex_);
        auto found = segments_.find(record.projectId);
        if (found == segments_.end()) {
            return common::Result<void>::failure(
                {"project_memory.compaction_conflict", "Project memory changed while compaction was running"});
        }
        auto& list = found->second;
        for (const auto& id : record.sourceSegmentIds) {
            if (std::count_if(list.begin(), list.end(), [&](const ProjectMemorySegment& segment) {
                    return segment.id == id;
                }) != 1) {
                return common::Result<void>::failure(
                    {"project_memory.compaction_conflict", "Project memory changed while compaction was running"});
            }
        }
        list.erase(std::remove_if(list.begin(), list.end(),
            [&record](const ProjectMemorySegment& segment) {
                return std::find(record.sourceSegmentIds.begin(), record.sourceSegmentIds.end(), segment.id)
                    != record.sourceSegmentIds.end();
            }), list.end());
        list.push_back(std::move(newSegment));
        return common::Result<void>::ok();
    }

    void failNextInsert()
    {
        std::lock_guard lock(mutex_);
        failNextInsert_ = true;
    }

    [[nodiscard]] std::size_t totalSegments() const
    {
        std::lock_guard lock(mutex_);
        std::size_t total = 0;
        for (const auto& [projectId, list] : segments_) {
            static_cast<void>(projectId);
            total += list.size();
        }
        return total;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<ProjectId, std::vector<ProjectMemorySegment>> segments_;
    bool failNextInsert_ {false};
};

// Every UTF-8 continuation byte (10xxxxxx) must be preceded by exactly the
// right number of lead-byte-declared continuation bytes; a naive byte-count
// clip could otherwise cut a multi-byte codepoint in half.
bool isValidUtf8(std::string_view text)
{
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        std::size_t continuationBytes = 0;
        if ((lead & 0x80) == 0x00) {
            continuationBytes = 0;
        } else if ((lead & 0xE0) == 0xC0) {
            continuationBytes = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            continuationBytes = 2;
        } else if ((lead & 0xF8) == 0xF0) {
            continuationBytes = 3;
        } else {
            return false;
        }
        if (index + continuationBytes + 1 > text.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuationBytes; ++offset) {
            const unsigned char continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xC0) != 0x80) {
                return false;
            }
        }
        index += continuationBytes + 1;
    }
    return true;
}

Project makeProject(const char* id, const char* name)
{
    Project project;
    project.id = id;
    project.name = name;
    project.status = ProjectStatus::Active;
    project.createdAt = std::chrono::system_clock::now();
    project.updatedAt = project.createdAt;
    return project;
}

Run makeRun(const char* id, const ProjectId& projectId, RunStatus status, std::string title, std::string task)
{
    Run run;
    run.id = id;
    run.projectId = projectId;
    run.title = std::move(title);
    run.task = std::move(task);
    run.status = status;
    run.createdAt = std::chrono::system_clock::now();
    return run;
}

// ---------------------------------------------------------------------
// ProjectMemoryService unit tests (no RunManager involved)
// ---------------------------------------------------------------------

void testRecordRunOutcomeRejectsNonCompletedRun()
{
    FakeProjectMemoryRepository repository;
    ProjectMemoryService service(repository);
    const Project project = makeProject("p1", "Project One");
    const Run run = makeRun("r1", project.id, RunStatus::Running, "Still running", "Do the thing");

    const auto result = service.recordRunOutcome(project, run, "irrelevant output");
    require(!result.success(), "A non-completed run must not be recorded");
    require(result.error()->code == "project_memory.run_not_completed",
        "Expected the typed project_memory.run_not_completed error code");
    require(repository.totalSegments() == 0, "Repository must not have been called for a rejected run");
}

void testRecordRunOutcomeKeepsFullTextWithinBudget()
{
    FakeProjectMemoryRepository repository;
    ProjectMemoryService service(repository);
    const Project project = makeProject("p2", "Project Two");
    const Run run = makeRun("r2", project.id, RunStatus::Completed, "Small run", "SHORT_TASK_MARKER");
    const std::string finalOutput = "SHORT_OUTPUT_MARKER";

    const auto result = service.recordRunOutcome(project, run, finalOutput);
    require(result.success(), "recordRunOutcome must succeed for a completed run");
    const ProjectMemorySegment& segment = result.value();
    require(segment.projectId == project.id, "Segment must carry the project id");
    require(segment.runId == run.id, "Segment must carry the run id");
    require(!segment.id.empty(), "Segment must receive a generated id");
    require(segment.summary.find("SHORT_TASK_MARKER") != std::string::npos,
        "A task within budget must appear unclipped in the summary");
    require(segment.summary.find("SHORT_OUTPUT_MARKER") != std::string::npos,
        "A final output within budget must appear unclipped in the summary");
    require(segment.summary.find(project.name) != std::string::npos,
        "The summary must identify the project by name");
    require(segment.summary.find(run.title) != std::string::npos,
        "The summary must identify the run by title");
    require(segment.summaryTokenCount > 0, "summaryTokenCount must reflect the stored summary");
    require(repository.totalSegments() == 1, "Exactly one segment must have been inserted");
}

void testRecordRunOutcomeClipsOversizedTextWithoutSplittingUtf8()
{
    FakeProjectMemoryRepository repository;
    ProjectMemoryService service(repository);
    const Project project = makeProject("p3", "Project Three");

    // Two-byte UTF-8 codepoints (Cyrillic characters) repeated well past both
    // the ~1000 and ~2000 token budgets, so both the task and the final
    // output must be clipped, and clipping must never land mid-codepoint.
    std::string longTask;
    std::string longOutput;
    for (int i = 0; i < 6000; ++i) {
        longTask += "\xD0\xB0"; // U+0430 CYRILLIC SMALL LETTER A
    }
    for (int i = 0; i < 12000; ++i) {
        longOutput += "\xD0\xB1"; // U+0431 CYRILLIC SMALL LETTER BE
    }
    const Run run = makeRun("r3", project.id, RunStatus::Completed, "Large run", longTask);

    const auto result = service.recordRunOutcome(project, run, longOutput);
    require(result.success(), "recordRunOutcome must succeed even when clipping is required");
    const ProjectMemorySegment& segment = result.value();

    require(isValidUtf8(segment.summary), "Clipped summary must remain valid UTF-8");
    require(segment.summary.size() < longTask.size() + longOutput.size(),
        "An oversized task/output must actually be clipped, not stored in full");
    require(segment.sourceTokenCount > segment.summaryTokenCount,
        "sourceTokenCount must reflect the full unclipped source, larger than the clipped summary");
    // The whole assembled document (header + clipped task (~1000 tok) +
    // clipped output (~2000 tok)) must stay bounded; a generous ceiling
    // avoids coupling this test to the exact heading text.
    require(segment.summaryTokenCount < 3500,
        "Clipped summary token count must stay within the explicit task+output budgets");
}

void testProjectMemoryServiceListAndRemovePassThrough()
{
    FakeProjectMemoryRepository repository;
    ProjectMemoryService service(repository);
    const Project project = makeProject("p4", "Project Four");
    const Run run = makeRun("r4", project.id, RunStatus::Completed, "Pass-through run", "Task");

    require(service.recordRunOutcome(project, run, "Output").success(), "Setup insert must succeed");
    const auto listed = service.listMemorySegments(project.id);
    require(listed.success() && listed.value().size() == 1, "listMemorySegments must pass through to the repository");

    const auto removed = service.removeMemorySegments(project.id);
    require(removed.success(), "removeMemorySegments must pass through to the repository");
    const auto listedAfterRemove = service.listMemorySegments(project.id);
    require(listedAfterRemove.success() && listedAfterRemove.value().empty(),
        "Segments must be gone after removeMemorySegments");
}

// ---------------------------------------------------------------------
// RunManager best-effort hook tests
// ---------------------------------------------------------------------

Run waitForTerminalRun(IRunRepository& repository, const RunId& runId, std::chrono::milliseconds timeout = 3s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto found = repository.findRun(runId);
        require(found.success() && found.value().has_value(), "Run lookup failed while waiting");
        if (isTerminal(found.value()->status)) {
            return *found.value();
        }
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("Run did not reach a terminal state before timeout");
}

// RunManager writes the project memory segment (best-effort) only after the
// run's Completed status is already committed and its RunCompleted event
// already published (see RunManager::finishRun), so a caller that only waits
// for the terminal run status can observe Completed slightly before the
// segment exists. Poll instead of asserting immediately.
std::vector<ProjectMemorySegment> waitForMemorySegments(
    ProjectMemoryService& service, const ProjectId& projectId, std::size_t expectedCount)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto segments = service.listMemorySegments(projectId);
        require(segments.success(), "listMemorySegments failed");
        if (segments.value().size() >= expectedCount) {
            return segments.value();
        }
        std::this_thread::sleep_for(2ms);
    }
    throw std::runtime_error("Project memory segment(s) did not appear before timeout");
}

void waitUntilRunning(IRunRepository& repository, const RunId& runId)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto found = repository.findRun(runId);
        require(found.success() && found.value().has_value(), "Run lookup failed while waiting");
        if (found.value()->status == RunStatus::Running) {
            return;
        }
        std::this_thread::sleep_for(2ms);
    }
    throw std::runtime_error("Run did not start before timeout");
}

// A single failing stage so RunManager::finishRun sees RunStatus::Failed.
class AlwaysFailingWorkflowRunner final : public IWorkflowRunner {
public:
    [[nodiscard]] std::vector<RunStage> createStages(
        const RunId& runId, const ResolvedExecutionPlan& /*plan*/) const override
    {
        RunStage stage;
        stage.id = runId + ":failing";
        stage.runId = runId;
        stage.type = "failing";
        stage.title = "Always fails";
        return {std::move(stage)};
    }

    WorkflowResult run(
        const WorkflowRunContext& /*context*/,
        std::stop_token /*stopToken*/,
        IRunEventSink& /*eventSink*/) override
    {
        return {false, RunStatus::Failed, "Deliberately failing for a project memory test", {}};
    }
};

Project createProject(ProjectService& service, const fs::path& path)
{
    const auto created = service.createProject({"Project memory test", "Mock pipeline", path});
    require(created.success(), "Could not create project fixture");
    return created.value();
}

void testCompletedRunRecordsExactlyOneProjectMemorySegment()
{
    TempProjectDirectory directory;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ProjectService projectService(projects);
    const Project project = createProject(projectService, directory.path);

    FakeProjectMemoryRepository memoryRepository;
    ProjectMemoryService memoryService(memoryRepository);
    MockWorkflowRunner workflow(1ms, 1);
    SingleRunnerResolver resolver(workflow);
    RunManager manager(projects, runs, resolver, &memoryService);
    OrchestrationService service(projects, runs, manager);

    const auto started = service.startRun({project.id, "Memory run", "Task text for project memory"});
    require(started.success(), "startRun must succeed");
    const Run finished = waitForTerminalRun(runs, started.value());
    require(finished.status == RunStatus::Completed, "Mock run must complete");

    const auto segments = waitForMemorySegments(memoryService, project.id, 1);
    require(segments.size() == 1,
        "Exactly one project memory segment must be created for one completed run");
    const ProjectMemorySegment& segment = segments.front();
    require(segment.runId == finished.id, "Segment must reference the completed run");
    require(segment.summary.find("Mock Workflow Report") != std::string::npos,
        "Segment summary must include the Mock runner's finalOutput report");
    require(segment.summary.find("Task text for project memory") != std::string::npos,
        "Segment summary must include the run task");
}

void testFailedRunDoesNotRecordProjectMemorySegment()
{
    TempProjectDirectory directory;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ProjectService projectService(projects);
    const Project project = createProject(projectService, directory.path);

    FakeProjectMemoryRepository memoryRepository;
    ProjectMemoryService memoryService(memoryRepository);
    AlwaysFailingWorkflowRunner workflow;
    SingleRunnerResolver resolver(workflow);
    RunManager manager(projects, runs, resolver, &memoryService);
    OrchestrationService service(projects, runs, manager);

    const auto started = service.startRun({project.id, "Failing run", "Task that always fails"});
    require(started.success(), "startRun must succeed even though the workflow always fails");
    const Run finished = waitForTerminalRun(runs, started.value());
    require(finished.status == RunStatus::Failed, "This workflow must finish Failed");

    const auto segments = memoryService.listMemorySegments(project.id);
    require(segments.success() && segments.value().empty(),
        "A failed run must not create a project memory segment");
}

// PC-4C: every Run that reaches a terminal status nudges the Outcome Brief reconciler exactly once - and only after the status is
// committed (the reconciler reads the Run, and a brief of a Run that still looks active would be refused).
void testRunManagerNudgesTheOutcomeBriefOnceTheRunIsTerminal()
{
    for (const bool failing : {false, true}) {
        TempProjectDirectory directory;
        InMemoryProjectRepository projects;
        InMemoryRunRepository runs;
        ProjectService projectService(projects);
        const Project project = createProject(projectService, directory.path);

        outcomebrief_hooks::RecordingTrigger trigger;   // outlives the manager and its worker thread
        std::atomic<bool> terminalWhenNudged {false};
        trigger.onRun = [&](const std::string& runId) {
            const auto found = runs.findRun(runId);
            terminalWhenNudged = found.success() && found.value().has_value() && isTerminal(found.value()->status);
        };
        MockWorkflowRunner mockWorkflow(1ms, 1);
        AlwaysFailingWorkflowRunner failingWorkflow;
        IWorkflowRunner& workflow = failing ? static_cast<IWorkflowRunner&>(failingWorkflow) : mockWorkflow;
        SingleRunnerResolver resolver(workflow);
        RunManager manager(projects, runs, resolver);
        manager.setOutcomeBriefTrigger(&trigger);
        OrchestrationService service(projects, runs, manager);

        const auto started = service.startRun({project.id, "Nudged run", "Task whose outcome is briefed"});
        require(started.success(), "startRun must succeed");
        const Run finished = waitForTerminalRun(runs, started.value());
        require(finished.status == (failing ? RunStatus::Failed : RunStatus::Completed), "the Run ends as the workflow says");
        require(trigger.waitFor([&] { return trigger.runCalls(finished.id) >= 1; }), "the terminal Run nudges the brief");
        std::this_thread::sleep_for(20ms);
        require(trigger.runCalls(finished.id) == 1 && trigger.runs().size() == 1 && trigger.proposals().empty(),
            "...exactly once, and only about this Run");
        require(terminalWhenNudged.load(), "...after the terminal status was committed");
    }
}

void testCancelledRunDoesNotRecordProjectMemorySegment()
{
    TempProjectDirectory directory;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ProjectService projectService(projects);
    const Project project = createProject(projectService, directory.path);

    FakeProjectMemoryRepository memoryRepository;
    ProjectMemoryService memoryService(memoryRepository);
    MockWorkflowRunner workflow(20ms, 10);
    SingleRunnerResolver resolver(workflow);
    RunManager manager(projects, runs, resolver, &memoryService);
    OrchestrationService service(projects, runs, manager);

    const auto started = service.startRun({project.id, "Cancelled run", "Task cancelled mid-flight"});
    require(started.success(), "startRun must succeed");
    waitUntilRunning(runs, started.value());
    require(service.cancelRun(started.value()).success(), "cancelRun must succeed while running");
    const Run finished = waitForTerminalRun(runs, started.value());
    require(finished.status == RunStatus::Cancelled, "Run must finish Cancelled");

    const auto segments = memoryService.listMemorySegments(project.id);
    require(segments.success() && segments.value().empty(),
        "A cancelled run must not create a project memory segment");
}

void testMemoryRepositoryFailureDoesNotChangeCompletedRunResult()
{
    TempProjectDirectory directory;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ProjectService projectService(projects);
    const Project project = createProject(projectService, directory.path);

    FakeProjectMemoryRepository memoryRepository;
    memoryRepository.failNextInsert();
    ProjectMemoryService memoryService(memoryRepository);
    MockWorkflowRunner workflow(1ms, 1);
    SingleRunnerResolver resolver(workflow);
    RunManager manager(projects, runs, resolver, &memoryService);
    OrchestrationService service(projects, runs, manager);

    std::mutex eventsMutex;
    std::vector<RunEvent> events;
    service.setEventHandler([&](const RunEvent& event) {
        std::lock_guard lock(eventsMutex);
        events.push_back(event);
    });

    const auto started = service.startRun({project.id, "Memory failure run", "Task despite a broken writer"});
    require(started.success(), "startRun must succeed");
    const Run finished = waitForTerminalRun(runs, started.value());

    // The run's persisted terminal status/result must be exactly what a
    // successful Mock run always produces, unaffected by the best-effort
    // memory write failing underneath it.
    require(finished.status == RunStatus::Completed,
        "A project memory write failure must never change the run's Completed status");
    require(finished.progress == 1.0, "Completed run progress must still be 1.0");
    require(finished.errorMessage.empty(),
        "A project memory write failure must never appear as the run's error message");
    {
        std::lock_guard lock(eventsMutex);
        require(std::any_of(events.begin(), events.end(), [](const RunEvent& event) {
            return event.type == RunEventType::RunCompleted;
        }), "RunCompleted event must still be published");
    }

    require(memoryRepository.totalSegments() == 0,
        "The simulated repository failure must have prevented the segment from persisting");
}

// ---------------------------------------------------------------------
// ProjectMemoryCompactionService unit tests (AG-11): fake provider only,
// never a real network call. Covers the acceptance list from
// TEMP_ARCHITECTURE_GAPS_EXECUTION_PLAN.md's B5 section verbatim: success,
// refusal, malformed output, cancellation, persistence failure, and retry
// without duplicate loss.
// ---------------------------------------------------------------------

// A minimal, test-only IProjectMemoryCompactionRepository double, mirroring
// FakeProjectMemoryRepository above. The real adapter
// (SqliteProjectMemoryCompactionRepository) is exercised in
// SqliteRepositoryTests.cpp/BackendRuntimeTests.cpp.
class FakeProjectMemoryCompactionRepository final : public IProjectMemoryCompactionRepository {
public:
    explicit FakeProjectMemoryCompactionRepository(FakeProjectMemoryRepository& memoryRepository)
        : memoryRepository_(memoryRepository)
    {
    }

    common::Result<void> insertCompactionRecord(const ProjectMemoryCompactionRecord& record) override
    {
        std::lock_guard lock(mutex_);
        if (record.status == ProjectMemoryCompactionStatus::Completed) {
            return common::Result<void>::failure(
                {"project_memory.invalid_compaction_commit",
                 "Completed compaction records require an atomic memory commit"});
        }
        if (failNextInsert_) {
            failNextInsert_ = false;
            return common::Result<void>::failure(
                {"project_memory.simulated_failure", "Simulated compaction repository failure"});
        }
        records_[record.projectId].push_back(record);
        return common::Result<void>::ok();
    }

    common::Result<void> commitSuccessfulCompaction(
        const ProjectMemoryCompactionRecord& record,
        ProjectMemorySegment resultSegment) override
    {
        std::lock_guard lock(mutex_);
        if (failNextInsert_) {
            failNextInsert_ = false;
            return common::Result<void>::failure(
                {"project_memory.simulated_failure", "Simulated atomic compaction failure"});
        }
        const auto replaced = memoryRepository_.commitCompactedSegment(record, std::move(resultSegment));
        if (!replaced.success()) return replaced;
        records_[record.projectId].push_back(record);
        return common::Result<void>::ok();
    }

    [[nodiscard]] common::Result<std::vector<ProjectMemoryCompactionRecord>> listCompactionRecords(
        const ProjectId& projectId) const override
    {
        std::lock_guard lock(mutex_);
        const auto it = records_.find(projectId);
        return common::Result<std::vector<ProjectMemoryCompactionRecord>>::ok(
            it == records_.end() ? std::vector<ProjectMemoryCompactionRecord> {} : it->second);
    }

    void failNextInsert()
    {
        std::lock_guard lock(mutex_);
        failNextInsert_ = true;
    }

private:
    FakeProjectMemoryRepository& memoryRepository_;
    mutable std::mutex mutex_;
    std::unordered_map<ProjectId, std::vector<ProjectMemoryCompactionRecord>> records_;
    bool failNextInsert_ {false};
};

// Scripted, single-provider fake: never touches the network. content is
// returned verbatim from every send() call unless a test overrides it, so a
// single instance can be reused for the "malformed output" case (blank
// content) simply by mutating content between calls.
class FakeCompactionProviderClient final : public IProviderClient {
public:
    std::string content {"COMPACTED_SUMMARY"};
    std::optional<common::Error> nextError;
    int callCount {0};
    // The usage every successful send() reports; the default keeps the historical 40 in / 15 out.
    ai::models::TokenUsage usage = [] {
        ai::models::TokenUsage initial;
        initial.inputTokens = 40;
        initial.outputTokens = 15;
        return initial;
    }();

    // S2a: raised INSIDE the call, so the stop arrives after the client has produced its answer (window W3 for a client that
    // implements only send()).
    std::stop_source* stopDuringSend {nullptr};

    common::Result<ProviderChatResponse> send(const ProviderChatRequest&, std::stop_token) override
    {
        ++callCount;
        if (stopDuringSend != nullptr) stopDuringSend->request_stop();
        if (nextError.has_value()) {
            const auto failure = *nextError;
            nextError.reset();
            return common::Result<ProviderChatResponse>::failure(failure);
        }
        ProviderChatResponse response;
        response.content = content;
        response.usage = usage;
        return common::Result<ProviderChatResponse>::ok(std::move(response));
    }
};

class FakeCompactionProviderClientFactory final : public IProviderClientFactory {
public:
    explicit FakeCompactionProviderClientFactory(IProviderClient& client) : client_(client) {}

    int resolutions {0};

    common::Result<std::shared_ptr<IProviderClient>> clientFor(
        const ProviderId&, const std::string&) override
    {
        ++resolutions;
        return common::Result<std::shared_ptr<IProviderClient>>::ok(
            std::shared_ptr<IProviderClient>(&client_, [](IProviderClient*) {}));
    }

private:
    IProviderClient& client_;
};

// S2a: a client that implements sendObserved() itself, as the production client does. send() is a trap: the service must
// use sendObserved() exactly once. The scripted outcome is the failure it returns (or a success when `success` is set)
// plus the response it reports as received-and-discarded; `stopSource` is raised inside the call.
class ObservedCompactionClient final : public IProviderClient {
public:
    int sendCalls {0};
    int observedCalls {0};
    std::stop_source* stopSource {nullptr};
    std::optional<common::Error> failure;                 // the failure result
    std::optional<ProviderChatResponse> success;          // the successful result (wins over `failure`)
    std::optional<ProviderChatResponse> discarded;        // the discarded response payload

    common::Result<ProviderChatResponse> send(const ProviderChatRequest&, std::stop_token) override
    {
        ++sendCalls;
        return common::Result<ProviderChatResponse>::failure({"test.send_used", "the service must call sendObserved"});
    }

    ProviderChatOutcome sendObserved(const ProviderChatRequest&, std::stop_token) override
    {
        ++observedCalls;
        if (stopSource != nullptr) stopSource->request_stop();
        if (success.has_value()) return {common::Result<ProviderChatResponse>::ok(*success), discarded};
        return {common::Result<ProviderChatResponse>::failure(
                    failure.value_or(common::Error {"provider.cancelled", "Cancelled while the request was in flight"})),
                discarded};
    }
};

Provider makeProvider(const char* id)
{
    Provider provider;
    provider.id = id;
    provider.name = "Fake Provider";
    provider.kind = "openai_compatible";
    provider.enabled = true;
    provider.createdAt = std::chrono::system_clock::now();
    provider.updatedAt = provider.createdAt;
    return provider;
}

Model makeModel(const char* id, const ProviderId& providerId)
{
    Model model;
    model.id = id;
    model.providerId = providerId;
    model.apiModelId = "fake-model-1";
    model.displayName = "Fake Model";
    model.contextWindowTokens = 128000;
    model.maxOutputTokens = 4096;
    // Realistic per-million-token pricing (a few USD/million tokens): with
    // the fake client's small scripted usage (40 in/15 out tokens), a
    // too-small price would truncate costMicros to zero via integer
    // division and silently defeat the "record a nonzero cost" assertion.
    model.inputPriceMicrosPerMillionTokens = 5'000'000;
    model.outputPriceMicrosPerMillionTokens = 15'000'000;
    model.enabled = true;
    model.createdAt = std::chrono::system_clock::now();
    model.updatedAt = model.createdAt;
    return model;
}

// Fixture bundling everything a compaction test needs, so each test body
// stays focused on the behavior it is asserting.
struct CompactionFixture {
    InMemoryProjectRepository projects;
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    FakeProjectMemoryRepository memoryRepository;
    FakeProjectMemoryCompactionRepository compactionRepository {memoryRepository};
    FakeCompactionProviderClient client;
    FakeCompactionProviderClientFactory clientFactory {client};
    ProjectMemoryCompactionService service {
        memoryRepository, compactionRepository, projects, providers, models, clientFactory};
    Project project {makeProject("cp1", "Compaction Project")};
    Provider provider {makeProvider("prov1")};
    Model model {makeModel("model1", "prov1")};

    CompactionFixture()
    {
        require(projects.insert(project).success(), "Setup: project insert must succeed");
        require(providers.insert(provider).success(), "Setup: provider insert must succeed");
        require(models.insert(model).success(), "Setup: model insert must succeed");
    }

    void seedSegments(int count)
    {
        for (int i = 0; i < count; ++i) {
            ProjectMemorySegment segment;
            segment.id = "seg" + std::to_string(i);
            segment.projectId = project.id;
            segment.runId = "run" + std::to_string(i);
            segment.summary = "Segment " + std::to_string(i) + " summary text";
            segment.sourceTokenCount = 50;
            segment.summaryTokenCount = 20;
            segment.createdAt = std::chrono::system_clock::now();
            require(memoryRepository.insertMemorySegment(segment).success(),
                "Setup: seed segment insert must succeed");
        }
    }
};

void testCompactSucceedsAndReplacesSegments()
{
    CompactionFixture fixture;
    fixture.seedSegments(3);
    fixture.client.content = "CONSOLIDATED_SUMMARY_TEXT";

    const auto result = fixture.service.compact(
        fixture.project.id, fixture.provider.id, fixture.model.id, std::stop_token {});
    require(result.success(), "A well-formed compaction attempt must succeed");
    const ProjectMemoryCompactionRecord& record = result.value();
    require(record.status == ProjectMemoryCompactionStatus::Completed,
        "A successful provider call must produce a Completed record");
    require(record.resultSegmentId.has_value(), "A Completed record must reference the new segment");
    require(record.sourceSegmentIds.size() == 3, "Record must capture all three source segment ids");
    require(record.inputTokens == 40 && record.outputTokens == 15,
        "Record must capture the provider's reported token usage");
    require(record.costMicros > 0, "Record must compute a nonzero cost from priced token usage");
    require(!record.sourceContentHash.empty(), "Record must carry a source content hash");

    const auto segments = fixture.memoryRepository.listMemorySegments(fixture.project.id);
    require(segments.success() && segments.value().size() == 1,
        "The three source segments must be replaced by exactly one consolidated segment");
    require(segments.value().front().id == *record.resultSegmentId,
        "The surviving segment id must match the record's resultSegmentId");
    require(segments.value().front().summary == "CONSOLIDATED_SUMMARY_TEXT",
        "The surviving segment must carry the provider's compacted summary");

    const auto records = fixture.service.listCompactionRecords(fixture.project.id);
    require(records.success() && records.value().size() == 1,
        "Exactly one compaction record must be listed after one successful attempt");
}

// M2b: a compaction is ONE provider response, so its recorded cost is classified against the
// model's long-context threshold by that response's own input_tokens.
void testCompactionCostIsClassifiedPerResponseByTheModelsLongContextTier()
{
    // $2 / $0.2 / $2.5 / $10 flat, $4 / $0.4 / $5 / $15 above 272,000 input tokens.
    const auto costOf = [](std::uint64_t input, std::uint64_t output,
                           std::uint64_t cacheWrite, std::uint64_t cacheRead) {
        CompactionFixture fixture;
        Model tiered = makeModel("model-tier", fixture.provider.id);
        tiered.contextWindowTokens = 1'050'000;
        tiered.inputPriceMicrosPerMillionTokens = 2'000'000;
        tiered.outputPriceMicrosPerMillionTokens = 10'000'000;
        tiered.cacheWritePriceMicrosPerMillionTokens = 2'500'000;
        tiered.cacheReadPriceMicrosPerMillionTokens = 200'000;
        tiered.longContextPricing =
            LongContextPricing {272'000, 4'000'000, 15'000'000, 5'000'000, 400'000};
        require(fixture.models.insert(tiered).success(), "Setup: tiered model insert must succeed");
        fixture.seedSegments(2);
        fixture.client.usage.inputTokens = input;
        fixture.client.usage.outputTokens = output;
        fixture.client.usage.cacheCreationInputTokens = cacheWrite;
        fixture.client.usage.cacheReadInputTokens = cacheRead;

        const auto result = fixture.service.compact(
            fixture.project.id, fixture.provider.id, tiered.id, std::stop_token {});
        require(result.success()
                && result.value().status == ProjectMemoryCompactionStatus::Completed,
            "A priced compaction attempt must complete");
        require(result.value().inputTokens == input && result.value().outputTokens == output,
            "The record must keep the plain provider token totals");
        return result.value().costMicros;
    };

    // A long Response with cache usage is billed entirely at the long tariff:
    // 50,000*4 + 200,000*0.4 + 50,000*5 + 1,000*15 = 545,000 (flat prices would give 275,000).
    require(costOf(300'000, 1'000, 50'000, 200'000) == 545'000,
        "A long compaction Response must be priced at the long-context tariff");
    // Exactly ON the threshold is still short; one token above flips the WHOLE Response.
    require(costOf(272'000, 0, 0, 0) == 544'000, "272,000 input tokens is still short");
    require(costOf(272'001, 0, 0, 0) == 1'088'004, "272,001 input tokens is entirely long");
}

void testCompactRefusesWithFewerThanTwoSegments()
{
    CompactionFixture fixture;
    fixture.seedSegments(1);

    const auto result = fixture.service.compact(
        fixture.project.id, fixture.provider.id, fixture.model.id, std::stop_token {});
    require(!result.success(), "Compaction must refuse fewer than two segments");
    require(result.error()->code == "project_memory.insufficient_segments_to_compact",
        "Expected the typed insufficient_segments_to_compact error code");
    require(fixture.client.callCount == 0, "A refused attempt must never reach the provider");
    require(fixture.compactionRepository.listCompactionRecords(fixture.project.id).value().empty(),
        "A refused attempt (before any provider call) must not persist a record");
}

void testCompactRecordsProviderFailureWithoutTouchingSegments()
{
    CompactionFixture fixture;
    fixture.seedSegments(2);
    fixture.client.nextError = common::Error {"provider.rate_limited", "Simulated rate limit"};

    const auto result = fixture.service.compact(
        fixture.project.id, fixture.provider.id, fixture.model.id, std::stop_token {});
    require(result.success(), "A provider-side failure is reported through the record, not Result::failure");
    const ProjectMemoryCompactionRecord& record = result.value();
    require(record.status == ProjectMemoryCompactionStatus::Failed, "Provider failure must yield a Failed record");
    require(record.errorCode == "provider.rate_limited", "Record must carry the provider's typed error code");
    require(record.errorMessage == "Compaction provider request failed"
            && record.errorMessage.find("Simulated") == std::string::npos,
        "Provider response details must not be persisted in the compaction record");
    require(!record.resultSegmentId.has_value(), "A Failed record must not reference a result segment");

    const auto segments = fixture.memoryRepository.listMemorySegments(fixture.project.id);
    require(segments.success() && segments.value().size() == 2,
        "A failed attempt must leave the original source segments untouched");
}

void testCompactRecordsFailureOnEmptySummary()
{
    CompactionFixture fixture;
    fixture.seedSegments(2);
    fixture.client.content = "   \n\t  "; // malformed/blank provider output

    const auto result = fixture.service.compact(
        fixture.project.id, fixture.provider.id, fixture.model.id, std::stop_token {});
    require(result.success(), "A malformed-output attempt is reported through the record, not Result::failure");
    const ProjectMemoryCompactionRecord& record = result.value();
    require(record.status == ProjectMemoryCompactionStatus::Failed,
        "An empty/whitespace-only summary must yield a Failed record");
    require(record.errorCode == "project_memory.empty_summary",
        "Expected the typed empty_summary error code");

    const auto segments = fixture.memoryRepository.listMemorySegments(fixture.project.id);
    require(segments.success() && segments.value().size() == 2,
        "A malformed-output attempt must leave the original source segments untouched");
}

void testCompactRecordsCancellation()
{
    CompactionFixture fixture;
    fixture.seedSegments(2);
    std::stop_source source;
    source.request_stop();

    const auto result = fixture.service.compact(
        fixture.project.id, fixture.provider.id, fixture.model.id, source.get_token());
    require(result.success(), "A cancelled attempt is reported through the record, not Result::failure");
    const ProjectMemoryCompactionRecord& record = result.value();
    require(record.status == ProjectMemoryCompactionStatus::Cancelled,
        "A stop-requested token must yield a Cancelled record");
    require(record.errorCode == "provider.cancelled", "Expected the typed provider.cancelled error code");

    const auto segments = fixture.memoryRepository.listMemorySegments(fixture.project.id);
    require(segments.success() && segments.value().size() == 2,
        "A cancelled attempt must leave the original source segments untouched");

    // S2a: an operation cancelled BEFORE anything was sent never reaches the client: no client resolved, no send, ONE terminal
    // record with no recorded usage (NULL, state none - not a claim that it was free), and nothing was committed.
    require(fixture.clientFactory.resolutions == 0 && fixture.client.callCount == 0,
        "A compaction cancelled before the request resolves and calls no client");
    require(record.inputTokens == 0 && record.outputTokens == 0 && record.costMicros == 0 && !record.costPriceKnown.has_value()
            && compactionPriceState(record) == ExecutionPriceState::None && !record.resultSegmentId.has_value(),
        "...and records 0/0/NULL");
    const auto records = fixture.service.listCompactionRecords(fixture.project.id);
    require(records.success() && records.value().size() == 1 && records.value().front().id == record.id,
        "...as exactly one terminal record");
}

void testCompactSurfacesCompactionRepositoryPersistenceFailure()
{
    CompactionFixture fixture;
    fixture.seedSegments(2);
    fixture.compactionRepository.failNextInsert();

    const auto result = fixture.service.compact(
        fixture.project.id, fixture.provider.id, fixture.model.id, std::stop_token {});
    require(!result.success(),
        "A provenance-record persistence failure must surface as Result::failure so the caller is never told "
        "an unrecorded attempt succeeded");
    require(result.error()->code == "project_memory.simulated_failure",
        "Expected the simulated repository failure to propagate verbatim");

    const auto segments = fixture.memoryRepository.listMemorySegments(fixture.project.id);
    require(segments.success() && segments.value().size() == 2,
        "An atomic provenance failure must leave both source segments untouched");
    const auto records = fixture.service.listCompactionRecords(fixture.project.id);
    require(records.success() && records.value().empty(),
        "An atomic provenance failure must not leave a partial compaction record");

    const auto retry = fixture.service.compact(
        fixture.project.id, fixture.provider.id, fixture.model.id, std::stop_token {});
    require(retry.success() && retry.value().status == ProjectMemoryCompactionStatus::Completed,
        "A retry after an atomic persistence failure must succeed over the unchanged source set");
}

// ---------------------------------------------------------------------------
// C2-B/S1: a record owns its amount AND whether that price was complete.
// ---------------------------------------------------------------------------
ProjectMemoryCompactionRecord storedRecordOf(CompactionFixture& fixture, const ProjectMemoryCompactionRecord& returned)
{
    const auto listed = fixture.compactionRepository.listCompactionRecords(fixture.project.id);
    require(listed.success(), "Setup: the stored records must be readable");
    for (const auto& stored : listed.value()) {
        if (stored.id == returned.id) return stored;
    }
    require(false, "The returned record must be the stored one");
    return {};
}

ProjectMemoryCompactionRecord compactWith(CompactionFixture& fixture, const Model& model)
{
    fixture.seedSegments(2);
    const auto result = fixture.service.compact(fixture.project.id, fixture.provider.id, model.id, std::stop_token {});
    require(result.success(), "A compaction attempt must be reported through its record");
    return result.value();
}

void testCompactStoresTheCompletenessOfTheSingleCostCalculation()
{
    // Complete price: both used categories are priced -> true, and the stored copy carries it too.
    {
        CompactionFixture fixture;
        const auto record = compactWith(fixture, fixture.model);
        require(record.status == ProjectMemoryCompactionStatus::Completed && record.costMicros > 0,
            "A priced success keeps its amount");
        require(record.costPriceKnown == std::optional<bool> {true}, "A complete price is stored as true");
        require(storedRecordOf(fixture, record).costPriceKnown == std::optional<bool> {true}, "...and so is the persisted record");
        require(compactionPriceState(record) == ExecutionPriceState::Known, "...which is a known cost");
    }
    // A true zero: the prices are entered, the amount rounds to 0. Known, not unknown.
    {
        CompactionFixture fixture;
        Model cheap = makeModel("model-cheap", fixture.provider.id);
        cheap.inputPriceMicrosPerMillionTokens = 1;
        cheap.outputPriceMicrosPerMillionTokens = 1;
        require(fixture.models.insert(cheap).success(), "Setup: model");
        const auto record = compactWith(fixture, cheap);
        require(record.costMicros == 0 && record.costPriceKnown == std::optional<bool> {true}
                && compactionPriceState(record) == ExecutionPriceState::Known,
            "A complete price that rounds to zero is a known zero");
    }
    // A used category without a price: the amount is only what could be priced -> false, a positive partial estimate.
    {
        CompactionFixture fixture;
        Model noOutputPrice = makeModel("model-no-output", fixture.provider.id);
        noOutputPrice.outputPriceMicrosPerMillionTokens = 0;
        require(fixture.models.insert(noOutputPrice).success(), "Setup: model");
        const auto record = compactWith(fixture, noOutputPrice);
        require(record.costMicros > 0 && record.costPriceKnown == std::optional<bool> {false}
                && compactionPriceState(record) == ExecutionPriceState::Partial,
            "An unset output price makes the recorded amount an incomplete estimate");
        require(storedRecordOf(fixture, record).costPriceKnown == std::optional<bool> {false}, "...also in the persisted record");
    }
    // Nothing could be priced: false with no amount -> unknown, never a free zero.
    {
        CompactionFixture fixture;
        Model unpriced = makeModel("model-unpriced", fixture.provider.id);
        unpriced.inputPriceMicrosPerMillionTokens = 0;
        unpriced.outputPriceMicrosPerMillionTokens = 0;
        require(fixture.models.insert(unpriced).success(), "Setup: model");
        const auto record = compactWith(fixture, unpriced);
        require(record.costMicros == 0 && record.costPriceKnown == std::optional<bool> {false}
                && compactionPriceState(record) == ExecutionPriceState::Unknown,
            "A model with no prices yields an unknown cost, not zero");
    }
    // The stored flag is the record's own: a later catalog price change does not change what the record says.
    {
        CompactionFixture fixture;
        Model noOutputPrice = makeModel("model-edit", fixture.provider.id);
        noOutputPrice.outputPriceMicrosPerMillionTokens = 0;
        require(fixture.models.insert(noOutputPrice).success(), "Setup: model");
        const auto record = compactWith(fixture, noOutputPrice);
        noOutputPrice.outputPriceMicrosPerMillionTokens = 15'000'000;
        require(fixture.models.update(noOutputPrice).success(), "Setup: price edit");
        const auto stored = storedRecordOf(fixture, record);
        require(stored.costMicros == record.costMicros && stored.costPriceKnown == std::optional<bool> {false}
                && compactionPriceState(stored) == ExecutionPriceState::Partial,
            "A later catalog edit must not rewrite a stored amount or its completeness");
    }
}

void testFailedAttemptsKeepTheFlagOnlyWhenThereWasAResponseToPrice()
{
    // Empty summary: the answer was paid for -> the record keeps usage, amount AND flag.
    {
        CompactionFixture fixture;
        fixture.seedSegments(2);
        fixture.client.content = "   \n\t  ";
        const auto result = fixture.service.compact(fixture.project.id, fixture.provider.id, fixture.model.id, std::stop_token {});
        require(result.success() && result.value().status == ProjectMemoryCompactionStatus::Failed
                && result.value().errorCode == "project_memory.empty_summary",
            "An empty summary is a Failed record");
        const auto& record = result.value();
        require(record.inputTokens == 40 && record.costMicros > 0 && record.costPriceKnown == std::optional<bool> {true},
            "...which keeps the paid usage, the amount and the completeness flag");
        require(storedRecordOf(fixture, record).costPriceKnown == std::optional<bool> {true}, "...in the stored copy too");
    }
    // Oversize summary: same.
    {
        CompactionFixture fixture;
        fixture.seedSegments(2);
        fixture.client.content = std::string(ProjectMemoryCompactionService::kMaxSummaryBytes + 1, 'x');
        const auto result = fixture.service.compact(fixture.project.id, fixture.provider.id, fixture.model.id, std::stop_token {});
        require(result.success() && result.value().errorCode == "project_memory.summary_too_large"
                && result.value().costMicros > 0 && result.value().costPriceKnown == std::optional<bool> {true},
            "An oversize summary keeps the flag next to the existing amount");
    }
    // Provider error: no response, nothing was priced -> the flag stays NULL (not a computed known 0), state none.
    {
        CompactionFixture fixture;
        fixture.seedSegments(2);
        fixture.client.nextError = common::Error {"provider.rate_limited", "Simulated rate limit"};
        const auto result = fixture.service.compact(fixture.project.id, fixture.provider.id, fixture.model.id, std::stop_token {});
        require(result.success() && result.value().status == ProjectMemoryCompactionStatus::Failed, "A provider failure is a Failed record");
        require(!result.value().costPriceKnown.has_value() && result.value().costMicros == 0
                && compactionPriceState(result.value()) == ExecutionPriceState::None,
            "...without a flag and with no recorded cost");
    }
    // Cancelled (the S1 behaviour is unchanged: usage is not read on a stop): no flag, state none.
    {
        CompactionFixture fixture;
        fixture.seedSegments(2);
        std::stop_source source;
        source.request_stop();
        const auto result = fixture.service.compact(fixture.project.id, fixture.provider.id, fixture.model.id, source.get_token());
        require(result.success() && result.value().status == ProjectMemoryCompactionStatus::Cancelled
                && !result.value().costPriceKnown.has_value() && result.value().inputTokens == 0
                && compactionPriceState(result.value()) == ExecutionPriceState::None,
            "A cancelled attempt keeps no flag and is not changed by this package");
    }
}

// Two paid answers that could not be stored (empty summary) leave the source segments in place, so the same source set is available
// to the second call: two records, each with its own amount and flag; neither replaces the other and nothing is summed or doubled.
void testTwoPaidFailuresOverTheSameSourcesStayTwoIndependentRecords()
{
    CompactionFixture fixture;
    fixture.seedSegments(2);
    fixture.client.content = "   ";
    const auto first = fixture.service.compact(fixture.project.id, fixture.provider.id, fixture.model.id, std::stop_token {});
    fixture.client.usage.inputTokens = 80;
    const auto second = fixture.service.compact(fixture.project.id, fixture.provider.id, fixture.model.id, std::stop_token {});
    require(first.success() && second.success(), "Both attempts are reported through their records");
    require(first.value().id != second.value().id && first.value().sourceContentHash == second.value().sourceContentHash,
        "Two records, two ids, the same unchanged source set");
    require(first.value().inputTokens == 40 && second.value().inputTokens == 80 && first.value().costMicros < second.value().costMicros,
        "Each keeps the amount of its OWN answer");
    require(first.value().costPriceKnown == std::optional<bool> {true} && second.value().costPriceKnown == std::optional<bool> {true},
        "...and its own flag");
    const auto listed = fixture.service.listCompactionRecords(fixture.project.id);
    require(listed.success() && listed.value().size() == 2, "Exactly the two records are stored");
    const auto segments = fixture.memoryRepository.listMemorySegments(fixture.project.id);
    require(segments.success() && segments.value().size() == 2, "The source segments were never replaced");
}

// ---------------------------------------------------------------------------
// C2-B/S2a: a response that WAS received is paid for even if the operation is then cancelled.
// ---------------------------------------------------------------------------
ProviderChatResponse receivedResponse(std::uint64_t input, std::uint64_t output, std::uint64_t cacheRead = 0)
{
    ProviderChatResponse response;
    response.content = "CANCELLED_ANSWER_MUST_NOT_BE_KEPT";
    response.usage.inputTokens = input;
    response.usage.outputTokens = output;
    response.usage.cacheReadInputTokens = cacheRead;
    return response;
}

struct SourceSnapshot {
    std::vector<std::string> ids;
    bool operator==(const SourceSnapshot&) const = default;
};

SourceSnapshot sourceSnapshotOf(CompactionFixture& fixture)
{
    const auto listed = fixture.memoryRepository.listMemorySegments(fixture.project.id);
    require(listed.success(), "Setup: the segments must be readable");
    SourceSnapshot snapshot;
    for (const auto& segment : listed.value()) snapshot.ids.push_back(segment.id + "|" + segment.summary);
    return snapshot;
}

// Runs one cancelled compaction against `client` with the given model and checks everything that must hold for ANY cancelled
// record: Cancelled/provider.cancelled, no result segment, the stored copy equals the returned one, exactly one record, the
// source segments identical and the content of the cancelled answer nowhere.
ProjectMemoryCompactionRecord runCancelled(CompactionFixture& fixture, IProviderClient& client, const Model& model, std::stop_source& stop)
{
    FakeCompactionProviderClientFactory factory {client};
    ProjectMemoryCompactionService service {
        fixture.memoryRepository, fixture.compactionRepository, fixture.projects, fixture.providers, fixture.models, factory};
    fixture.seedSegments(2);
    const SourceSnapshot before = sourceSnapshotOf(fixture);
    const auto result = service.compact(fixture.project.id, fixture.provider.id, model.id, stop.get_token());
    require(result.success(), "A cancelled attempt is reported through its record");
    const ProjectMemoryCompactionRecord& record = result.value();
    require(record.status == ProjectMemoryCompactionStatus::Cancelled && record.errorCode == "provider.cancelled"
            && record.errorMessage == "Compaction was cancelled" && !record.resultSegmentId.has_value(),
        "The operation stays Cancelled and creates no result segment");
    require(sourceSnapshotOf(fixture) == before, "The source segments are identical (no summary replaced them)");
    const auto listed = fixture.compactionRepository.listCompactionRecords(fixture.project.id);
    require(listed.success() && listed.value().size() == 1, "Exactly ONE terminal record");
    const auto& stored = listed.value().front();
    require(stored.id == record.id && stored.status == ProjectMemoryCompactionStatus::Cancelled
            && stored.inputTokens == record.inputTokens && stored.outputTokens == record.outputTokens
            && stored.costMicros == record.costMicros && stored.costPriceKnown == record.costPriceKnown,
        "...and the stored copy carries the same usage, amount and flag");
    require(stored.errorMessage.find("CANCELLED_ANSWER") == std::string::npos
            && stored.errorCode.find("CANCELLED_ANSWER") == std::string::npos,
        "The content of a cancelled answer is never kept");
    return record;
}

void testACancelledCompactionKeepsTheUsageOfAResponseThatWasReceived()
{
    // W2: the client reports the response it received and discarded because of the cancellation. Four price outcomes of the ONE
    // calculation: complete, a true zero, an incomplete positive estimate, nothing priceable.
    struct Case { const char* what; std::uint64_t inputPrice; std::uint64_t outputPrice; ExecutionPriceState state; };
    const Case cases[] = {
        {"complete", 5'000'000, 15'000'000, ExecutionPriceState::Known},
        {"a true zero", 1, 1, ExecutionPriceState::Known},
        {"incomplete positive", 5'000'000, 0, ExecutionPriceState::Partial},
        {"nothing priceable", 0, 0, ExecutionPriceState::Unknown}};
    for (const Case& c : cases) {
        CompactionFixture fixture;
        Model model = makeModel("model-w2", fixture.provider.id);
        model.inputPriceMicrosPerMillionTokens = c.inputPrice;
        model.outputPriceMicrosPerMillionTokens = c.outputPrice;
        require(fixture.models.insert(model).success(), "Setup: model");
        std::stop_source stop;
        ObservedCompactionClient client;
        client.stopSource = &stop;
        client.discarded = receivedResponse(40, 15);
        const auto record = runCancelled(fixture, client, model, stop);
        const CostBreakdown oracle = computeSingleResponseCostMicros(model, client.discarded->usage);
        require(client.observedCalls == 1 && client.sendCalls == 0, c.what);
        require(record.inputTokens == 40 && record.outputTokens == 15 && record.costMicros == oracle.costMicros
                && record.costPriceKnown == std::optional<bool> {oracle.priceKnown},
            c.what);
        require(compactionPriceState(record) == c.state, c.what);
    }
    // Zero usage of a RECEIVED response is the calculator's result as it is: known $0 only if the price is complete (true),
    // otherwise unknown. Never "known" merely because a response existed.
    {
        CompactionFixture fixture;
        std::stop_source stop;
        ObservedCompactionClient client;
        client.stopSource = &stop;
        client.discarded = receivedResponse(0, 0);
        const auto record = runCancelled(fixture, client, fixture.model, stop);
        require(record.inputTokens == 0 && record.costMicros == 0 && record.costPriceKnown.has_value()
                && compactionPriceState(record) == (*record.costPriceKnown ? ExecutionPriceState::Known : ExecutionPriceState::Unknown),
            "A received zero is stored as the calculator reports it");
    }
    {
        CompactionFixture fixture;
        Model unpriced = makeModel("model-unpriced-zero", fixture.provider.id);
        unpriced.inputPriceMicrosPerMillionTokens = 0;
        unpriced.outputPriceMicrosPerMillionTokens = 0;
        require(fixture.models.insert(unpriced).success(), "Setup: model");
        std::stop_source stop;
        ObservedCompactionClient client;
        client.stopSource = &stop;
        client.discarded = receivedResponse(0, 0);
        const auto record = runCancelled(fixture, client, unpriced, stop);
        const CostBreakdown oracle = computeSingleResponseCostMicros(unpriced, client.discarded->usage);
        require(record.costPriceKnown == std::optional<bool> {oracle.priceKnown} && record.costMicros == 0
                && compactionPriceState(record) == (oracle.priceKnown ? ExecutionPriceState::Known : ExecutionPriceState::Unknown),
            "...and a zero that the calculator cannot call complete is never a free zero");
    }
    // The FULL usage reaches the one calculation (cache details), not only input/output.
    {
        CompactionFixture fixture;
        Model cached = makeModel("model-cache", fixture.provider.id);
        cached.cacheReadPriceMicrosPerMillionTokens = 500'000;
        require(fixture.models.insert(cached).success(), "Setup: model");
        std::stop_source stop;
        ObservedCompactionClient client;
        client.stopSource = &stop;
        client.discarded = receivedResponse(1000, 20, 800);
        const auto record = runCancelled(fixture, client, cached, stop);
        ai::models::TokenUsage plain;
        plain.inputTokens = 1000;
        plain.outputTokens = 20;
        const CostBreakdown full = computeSingleResponseCostMicros(cached, client.discarded->usage);
        require(full.costMicros != computeSingleResponseCostMicros(cached, plain).costMicros,
            "Setup: the cache detail must change the amount");
        require(record.costMicros == full.costMicros && record.costPriceKnown == std::optional<bool> {full.priceKnown},
            "The cache details of the received usage take part in the one calculation");
    }
}

void testACancelledCompactionTakesTheUsageFromExactlyOnePlace()
{
    // W3 (the stop came after the client returned): a successful RESULT is the evidence - for a client that implements only send().
    {
        CompactionFixture fixture;
        std::stop_source stop;
        fixture.client.stopDuringSend = &stop;
        const auto record = runCancelled(fixture, fixture.client, fixture.model, stop);
        require(fixture.client.callCount == 1, "The default sendObserved performs exactly ONE send");
        require(record.inputTokens == 40 && record.outputTokens == 15 && record.costMicros > 0
                && record.costPriceKnown == std::optional<bool> {true} && compactionPriceState(record) == ExecutionPriceState::Known,
            "A legacy client's response that raced the stop is accounted");
    }
    // A successful result AND a payload (the invariant says never): the result is used, the two are not added.
    {
        CompactionFixture fixture;
        std::stop_source stop;
        ObservedCompactionClient client;
        client.stopSource = &stop;
        client.success = receivedResponse(40, 15);
        client.discarded = receivedResponse(4000, 1500);
        const auto record = runCancelled(fixture, client, fixture.model, stop);
        require(record.inputTokens == 40 && record.outputTokens == 15, "The successful result wins; nothing is summed");
    }
    // No evidence of a response: 0/0/NULL, state none, never a computed known zero.
    {
        CompactionFixture fixture;
        std::stop_source stop;
        ObservedCompactionClient client;
        client.stopSource = &stop;
        const auto record = runCancelled(fixture, client, fixture.model, stop);
        require(record.inputTokens == 0 && record.costMicros == 0 && !record.costPriceKnown.has_value()
                && compactionPriceState(record) == ExecutionPriceState::None,
            "A cancellation without a received response records no estimate");
    }
    // An arbitrary failure that carries a payload is NOT evidence of a successful response.
    {
        CompactionFixture fixture;
        std::stop_source stop;
        ObservedCompactionClient client;
        client.stopSource = &stop;
        client.failure = common::Error {"provider.rate_limited", "Simulated rate limit"};
        client.discarded = receivedResponse(40, 15);
        const auto record = runCancelled(fixture, client, fixture.model, stop);
        require(record.inputTokens == 0 && record.costMicros == 0 && !record.costPriceKnown.has_value(),
            "A payload next to a foreign failure is not used");
    }
    // Without a stop the payload gives no permission to anything: the failure is a Failed record with no usage.
    {
        CompactionFixture fixture;
        fixture.seedSegments(2);
        ObservedCompactionClient client;
        client.failure = common::Error {"provider.cancelled", "Cancelled while the request was in flight"};
        client.discarded = receivedResponse(40, 15);
        FakeCompactionProviderClientFactory factory {client};
        ProjectMemoryCompactionService service {
            fixture.memoryRepository, fixture.compactionRepository, fixture.projects, fixture.providers, fixture.models, factory};
        const auto result = service.compact(fixture.project.id, fixture.provider.id, fixture.model.id, std::stop_token {});
        require(result.success() && result.value().status == ProjectMemoryCompactionStatus::Failed
                && result.value().inputTokens == 0 && !result.value().costPriceKnown.has_value() && !result.value().resultSegmentId.has_value(),
            "A payload with no stop never completes or prices anything");
        require(client.observedCalls == 1 && client.sendCalls == 0, "The service made exactly one observed call");
    }
}

// The pure state of one stored record, in the normative order (false is not NULL; a positive amount is never hidden).
void testCompactionPriceStateFollowsTheNormativeOrder()
{
    const auto record = [](std::optional<bool> flag, std::uint64_t micros, std::uint64_t tokens, ProjectMemoryCompactionStatus status) {
        ProjectMemoryCompactionRecord value;
        value.costPriceKnown = flag;
        value.costMicros = micros;
        value.inputTokens = tokens;
        value.status = status;
        return value;
    };
    constexpr auto Done = ProjectMemoryCompactionStatus::Completed;
    constexpr auto Failed = ProjectMemoryCompactionStatus::Failed;
    constexpr auto Cancelled = ProjectMemoryCompactionStatus::Cancelled;
    struct Row { std::optional<bool> flag; std::uint64_t micros; std::uint64_t tokens; ProjectMemoryCompactionStatus status; ExecutionPriceState expected; const char* what; };
    const Row table[] = {
        {true, 500, 10, Done, ExecutionPriceState::Known, "true + positive amount"},
        {true, 0, 10, Done, ExecutionPriceState::Known, "true + zero amount (a true zero)"},
        {true, 0, 0, Done, ExecutionPriceState::Known, "true + no usage at all (a known zero of the calculator)"},
        {true, 0, 0, Failed, ExecutionPriceState::Known, "true is known whatever the status"},
        {false, 500, 10, Done, ExecutionPriceState::Partial, "false + positive amount"},
        {false, 500, 0, Failed, ExecutionPriceState::Partial, "false + positive amount with zero tokens is still shown"},
        {std::nullopt, 500, 10, Done, ExecutionPriceState::Partial, "NULL + positive amount (legacy)"},
        {std::nullopt, 500, 0, Cancelled, ExecutionPriceState::Partial, "NULL + positive amount, zero tokens, cancelled"},
        {false, 0, 10, Done, ExecutionPriceState::Unknown, "false + zero amount"},
        {false, 0, 0, Failed, ExecutionPriceState::Unknown, "an explicit false with no tokens is NOT 'no operation'"},
        {std::nullopt, 0, 10, Done, ExecutionPriceState::Unknown, "NULL + zero amount + tokens"},
        {std::nullopt, 0, 10, Failed, ExecutionPriceState::Unknown, "NULL + zero amount + tokens, failed"},
        {std::nullopt, 0, 0, Done, ExecutionPriceState::Unknown, "NULL + zero + no tokens + Completed does not prove an exact zero"},
        {std::nullopt, 0, 0, Failed, ExecutionPriceState::None, "NULL + zero + no tokens + Failed: nothing recorded"},
        {std::nullopt, 0, 0, Cancelled, ExecutionPriceState::None, "NULL + zero + no tokens + Cancelled: nothing recorded"}};
    for (const Row& row : table) {
        require(compactionPriceState(record(row.flag, row.micros, row.tokens, row.status)) == row.expected, row.what);
    }
    // Output-only usage counts as usage too.
    ProjectMemoryCompactionRecord outputOnly = record(std::nullopt, 0, 0, Failed);
    outputOnly.outputTokens = 7;
    require(compactionPriceState(outputOnly) == ExecutionPriceState::Unknown, "Output tokens alone are usage");
}

void testCompactRejectsBoundedInputBeforeProviderCall()
{
    CompactionFixture fixture;
    fixture.seedSegments(static_cast<int>(ProjectMemoryCompactionService::kMaxSourceSegments + 1));

    const auto result = fixture.service.compact(
        fixture.project.id, fixture.provider.id, fixture.model.id, std::stop_token {});
    require(!result.success(), "An over-limit source set must be rejected");
    require(result.error()->code == "project_memory.compaction_input_too_large",
        "Expected the typed compaction_input_too_large code");
    require(fixture.client.callCount == 0, "Bounded-input rejection must happen before provider spend");
}

void testCompactRejectsOversizedSourceTextBeforeProviderCall()
{
    CompactionFixture fixture;
    fixture.seedSegments(1);
    ProjectMemorySegment oversized;
    oversized.id = "oversized-segment";
    oversized.projectId = fixture.project.id;
    oversized.runId = "run-oversized";
    oversized.summary.assign(ProjectMemoryCompactionService::kMaxSourceBytes, 'x');
    oversized.createdAt = std::chrono::system_clock::now();
    require(fixture.memoryRepository.insertMemorySegment(std::move(oversized)).success(),
        "Setup: oversized segment insert must succeed");

    const auto result = fixture.service.compact(
        fixture.project.id, fixture.provider.id, fixture.model.id, std::stop_token {});
    require(!result.success() && result.error()->code == "project_memory.compaction_input_too_large",
        "Oversized source text must fail with compaction_input_too_large");
    require(fixture.client.callCount == 0, "Source-byte limit must be checked before provider spend");
}

void testCompactRecordsOversizedProviderSummaryWithoutMutation()
{
    CompactionFixture fixture;
    fixture.seedSegments(2);
    fixture.client.content.assign(ProjectMemoryCompactionService::kMaxSummaryBytes + 1, 'x');

    const auto result = fixture.service.compact(
        fixture.project.id, fixture.provider.id, fixture.model.id, std::stop_token {});
    require(result.success() && result.value().status == ProjectMemoryCompactionStatus::Failed,
        "An oversized provider summary must produce a durable Failed attempt");
    require(result.value().errorCode == "project_memory.summary_too_large",
        "Expected the typed summary_too_large code");
    const auto segments = fixture.memoryRepository.listMemorySegments(fixture.project.id);
    require(segments.success() && segments.value().size() == 2,
        "An oversized provider summary must not mutate project memory");
}

void testCompactRetryAfterFailureDoesNotDuplicateOrLoseSegments()
{
    CompactionFixture fixture;
    fixture.seedSegments(3);
    fixture.client.nextError = common::Error {"provider.timeout", "Simulated timeout"};

    const auto firstAttempt = fixture.service.compact(
        fixture.project.id, fixture.provider.id, fixture.model.id, std::stop_token {});
    require(firstAttempt.success() && firstAttempt.value().status == ProjectMemoryCompactionStatus::Failed,
        "First attempt must fail as scripted");
    require(firstAttempt.value().sourceSegmentIds.size() == 3,
        "First (failed) attempt must still record which three segments it read");

    const auto segmentsAfterFailure = fixture.memoryRepository.listMemorySegments(fixture.project.id);
    require(segmentsAfterFailure.success() && segmentsAfterFailure.value().size() == 3,
        "Nothing must be lost or duplicated after a failed attempt");

    fixture.client.content = "RETRY_SUCCEEDED_SUMMARY";
    const auto secondAttempt = fixture.service.compact(
        fixture.project.id, fixture.provider.id, fixture.model.id, std::stop_token {});
    require(secondAttempt.success() && secondAttempt.value().status == ProjectMemoryCompactionStatus::Completed,
        "A retry after a failed attempt must be able to succeed");
    require(secondAttempt.value().sourceSegmentIds == firstAttempt.value().sourceSegmentIds,
        "A retry over an unchanged segment set must read exactly the same source segment ids");
    require(secondAttempt.value().sourceContentHash == firstAttempt.value().sourceContentHash,
        "A retry over an unchanged segment set must hash to the same sourceContentHash");

    const auto segmentsAfterRetry = fixture.memoryRepository.listMemorySegments(fixture.project.id);
    require(segmentsAfterRetry.success() && segmentsAfterRetry.value().size() == 1,
        "A successful retry must replace all three original segments with exactly one consolidated segment");

    const auto records = fixture.service.listCompactionRecords(fixture.project.id);
    require(records.success() && records.value().size() == 2,
        "Both the failed and the successful attempt must remain in the provenance trail");
}

void testCompactRejectsUnknownProject()
{
    CompactionFixture fixture;
    fixture.seedSegments(2);

    const auto result = fixture.service.compact(
        "no-such-project", fixture.provider.id, fixture.model.id, std::stop_token {});
    require(!result.success(), "Compaction must reject an unknown project");
    require(result.error()->code == "project_memory.project_not_found",
        "Expected the typed project_not_found error code");
    require(fixture.client.callCount == 0, "A rejected attempt must never reach the provider");
}

void testCompactRejectsDisabledModel()
{
    CompactionFixture fixture;
    fixture.seedSegments(2);
    Model disabledModel = fixture.model;
    disabledModel.id = "model-disabled";
    disabledModel.enabled = false;
    require(fixture.models.insert(disabledModel).success(), "Setup: disabled model insert must succeed");

    const auto result = fixture.service.compact(
        fixture.project.id, fixture.provider.id, disabledModel.id, std::stop_token {});
    require(!result.success(), "Compaction must reject a disabled model");
    require(result.error()->code == "project_memory.model_unavailable",
        "Expected the typed model_unavailable error code");
    require(fixture.client.callCount == 0, "A rejected attempt must never reach the provider");
}

} // namespace

int runProjectMemoryServiceTests()
{
    testRecordRunOutcomeRejectsNonCompletedRun();
    testRecordRunOutcomeKeepsFullTextWithinBudget();
    testRecordRunOutcomeClipsOversizedTextWithoutSplittingUtf8();
    testProjectMemoryServiceListAndRemovePassThrough();
    testCompletedRunRecordsExactlyOneProjectMemorySegment();
    testFailedRunDoesNotRecordProjectMemorySegment();
    testRunManagerNudgesTheOutcomeBriefOnceTheRunIsTerminal();
    testCancelledRunDoesNotRecordProjectMemorySegment();
    testMemoryRepositoryFailureDoesNotChangeCompletedRunResult();
    testCompactSucceedsAndReplacesSegments();
    testCompactionCostIsClassifiedPerResponseByTheModelsLongContextTier();
    testCompactRefusesWithFewerThanTwoSegments();
    testCompactRecordsProviderFailureWithoutTouchingSegments();
    testCompactRecordsFailureOnEmptySummary();
    testCompactRecordsCancellation();
    testCompactSurfacesCompactionRepositoryPersistenceFailure();
    testCompactStoresTheCompletenessOfTheSingleCostCalculation();
    testFailedAttemptsKeepTheFlagOnlyWhenThereWasAResponseToPrice();
    testTwoPaidFailuresOverTheSameSourcesStayTwoIndependentRecords();
    testACancelledCompactionKeepsTheUsageOfAResponseThatWasReceived();
    testACancelledCompactionTakesTheUsageFromExactlyOnePlace();
    testCompactionPriceStateFollowsTheNormativeOrder();
    testCompactRejectsBoundedInputBeforeProviderCall();
    testCompactRejectsOversizedSourceTextBeforeProviderCall();
    testCompactRecordsOversizedProviderSummaryWithoutMutation();
    testCompactRetryAfterFailureDoesNotDuplicateOrLoseSegments();
    testCompactRejectsUnknownProject();
    testCompactRejectsDisabledModel();
    return 0;
}
