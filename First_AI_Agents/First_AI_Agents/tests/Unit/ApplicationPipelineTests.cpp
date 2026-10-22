#include "ai/application/InMemoryRepositories.hpp"
#include "ai/application/MockWorkflowRunner.hpp"
#include "ai/application/OrchestrationService.hpp"
#include "ai/application/ProjectService.hpp"
#include "ai/application/RunStageSettlement.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

struct TempProjectDirectory {
    fs::path path;

    TempProjectDirectory()
    {
        path = fs::temp_directory_path()
            / ("ai-orchestrator-application-test-"
                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }

    ~TempProjectDirectory()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

// Poll the repository rather than sleeping for a fixed duration. This keeps the
// asynchronous tests fast on normal machines and bounded on slow CI workers.
ai::application::Run waitForTerminalRun(
    ai::application::IRunRepository& repository,
    const ai::application::RunId& runId,
    std::chrono::milliseconds timeout = 3s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto found = repository.findRun(runId);
        assert(found.success());
        assert(found.value().has_value());
        if (ai::application::isTerminal(found.value()->status)) {
            return *found.value();
        }
        std::this_thread::sleep_for(5ms);
    }
    assert(false && "Run did not reach a terminal state before timeout");
    return {};
}

void waitUntilRunning(
    ai::application::IRunRepository& repository,
    const ai::application::RunId& runId)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto found = repository.findRun(runId);
        assert(found.success());
        assert(found.value().has_value());
        if (found.value()->status == ai::application::RunStatus::Running) {
            return;
        }
        std::this_thread::sleep_for(2ms);
    }
    assert(false && "Run did not start before timeout");
}

ai::application::Project createProject(
    ai::application::ProjectService& service,
    const fs::path& path)
{
    const auto created = service.createProject({"Application test", "Mock pipeline", path});
    assert(created.success());
    assert(!created.value().id.empty());
    assert(created.value().rootPath == fs::weakly_canonical(path));
    return created.value();
}

void testProjectValidationAndDuplicateDetection(){
    // ProjectService must reject invalid commands before creating repository rows.
    TempProjectDirectory directory;
    ai::application::InMemoryProjectRepository repository;
    ai::application::ProjectService service(repository);

    const auto emptyName = service.createProject({"   ", {}, directory.path});
    assert(!emptyName.success());
    assert(emptyName.error()->code == "project.empty_name");

    const auto missingPath = service.createProject({"Missing", {}, directory.path / "missing"});
    assert(!missingPath.success());
    assert(missingPath.error()->code == "project.path_not_found");

    const auto project = createProject(service, directory.path);
    const auto duplicate = service.createProject({"Duplicate", {}, directory.path});
    assert(!duplicate.success());
    assert(duplicate.error()->code == "project.duplicate_path");

    const auto projects = service.listProjects();
    assert(projects.success());
    assert(projects.value().size() == 1);
    assert(projects.value().front().id == project.id);
}

void testMockPipelineCompletesAndPublishesProgress()
{
    // Proves the vertical C++ pipeline from service command through worker events
    // and back to persisted run/stage read models.
    TempProjectDirectory directory;
    ai::application::InMemoryProjectRepository projects;
    ai::application::InMemoryRunRepository runs;
    ai::application::ProjectService projectService(projects);
    const auto project = createProject(projectService, directory.path);

    ai::application::MockWorkflowRunner workflow(2ms, 2);
    ai::application::SingleRunnerResolver resolver(workflow);
    ai::application::RunManager manager(projects, runs, resolver);
    ai::application::OrchestrationService service(projects, runs, manager);

    std::mutex eventsMutex;
    std::condition_variable eventsChanged;
    std::vector<ai::application::RunEvent> events;
    service.setEventHandler([&](const ai::application::RunEvent& event) {
        {
            std::lock_guard lock(eventsMutex);
            events.push_back(event);
        }
        eventsChanged.notify_all();
    });

    const auto started = service.startRun({project.id, "Complete mock run", "Execute deterministic stages"});
    assert(started.success());
    const auto finalRun = waitForTerminalRun(runs, started.value());
    assert(finalRun.status == ai::application::RunStatus::Completed);
    assert(finalRun.workspacePath.has_value());
    assert(*finalRun.workspacePath == std::filesystem::weakly_canonical(directory.path));
    assert(finalRun.progress == 1.0);
    assert(finalRun.usage.inputTokens == 40);
    assert(finalRun.usage.outputTokens == 20);

    const auto stages = service.listStages(finalRun.id);
    assert(stages.success());
    assert(stages.value().size() == 4);
    assert(std::all_of(stages.value().begin(), stages.value().end(), [](const auto& stage) {
        return stage.status == ai::application::StageStatus::Completed
            && stage.progress == 1.0;
    }));

    std::unique_lock lock(eventsMutex);
    eventsChanged.wait_for(lock, 1s, [&] {
        return std::any_of(events.begin(), events.end(), [](const auto& event) {
            return event.type == ai::application::RunEventType::RunCompleted;
        });
    });
    assert(std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.type == ai::application::RunEventType::RunStarted;
    }));
    assert(std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.type == ai::application::RunEventType::StageProgress
            && event.runProgress > 0.0;
    }));
    assert(std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.type == ai::application::RunEventType::RunCompleted;
    }));
}

void testRunningAndQueuedCancellation()
{
    // Exercise both cancellation paths: queue removal and cooperative stop_token.
    TempProjectDirectory directory;
    ai::application::InMemoryProjectRepository projects;
    ai::application::InMemoryRunRepository runs;
    ai::application::ProjectService projectService(projects);
    const auto project = createProject(projectService, directory.path);

    ai::application::MockWorkflowRunner workflow(20ms, 10);
    ai::application::SingleRunnerResolver resolver(workflow);
    ai::application::RunManager manager(projects, runs, resolver);
    ai::application::OrchestrationService service(projects, runs, manager);

    const auto first = service.startRun({project.id, "Running", "Cancel while running"});
    const auto second = service.startRun({project.id, "Queued", "Cancel before execution"});
    assert(first.success());
    assert(second.success());

    waitUntilRunning(runs, first.value());
    assert(service.cancelRun(second.value()).success());
    assert(service.cancelRun(first.value()).success());

    const auto queuedFinal = waitForTerminalRun(runs, second.value());
    const auto runningFinal = waitForTerminalRun(runs, first.value());
    assert(queuedFinal.status == ai::application::RunStatus::Cancelled);
    assert(runningFinal.status == ai::application::RunStatus::Cancelled);
    assert(runningFinal.progress < 1.0);
}

void testRecoveryMarksUnfinishedRunsInterrupted()
{
    // Simulate a persisted row loaded after an unclean process termination.
    TempProjectDirectory directory;
    ai::application::InMemoryProjectRepository projects;
    ai::application::InMemoryRunRepository runs;
    ai::application::ProjectService projectService(projects);
    const auto project = createProject(projectService, directory.path);

    ai::application::Run unfinished;
    unfinished.id = "recovery-test-run";
    unfinished.projectId = project.id;
    unfinished.title = "Recover";
    unfinished.task = "Recover interrupted state";
    unfinished.workflowId = "mock-sequential-v1";
    unfinished.status = ai::application::RunStatus::Running;
    unfinished.createdAt = std::chrono::system_clock::now();
    assert(runs.insertRun(unfinished).success());

    ai::application::MockWorkflowRunner workflow(2ms, 1);
    ai::application::SingleRunnerResolver resolver(workflow);
    ai::application::RunManager manager(projects, runs, resolver);
    ai::application::OrchestrationService service(projects, runs, manager);
    assert(service.recoverInterruptedRuns().success());

    const auto recovered = runs.findRun(unfinished.id);
    assert(recovered.success());
    assert(recovered.value().has_value());
    assert(recovered.value()->status == ai::application::RunStatus::Interrupted);
    assert(recovered.value()->finishedAt.has_value());
}

// AG-17.7D-1: a run repository whose updateRun can be made to fail ONCE - the process "dying" after
// the stage rows were settled and before the Run row was written.
class FailingUpdateRunRepository final : public ai::application::IRunRepository {
public:
    std::atomic<bool> failNextRunUpdate {false};

    ai::common::Result<ai::application::Run> insertRun(ai::application::Run run) override
    {
        return inner_.insertRun(std::move(run));
    }
    ai::common::Result<ai::application::Run> insertRunWithStages(
        ai::application::Run run, std::vector<ai::application::RunStage> stages) override
    {
        return inner_.insertRunWithStages(std::move(run), std::move(stages));
    }
    ai::common::Result<ai::application::Run> updateRun(ai::application::Run run) override
    {
        if (failNextRunUpdate) {
            failNextRunUpdate = false;
            return ai::common::Result<ai::application::Run>::failure(
                {"persistence.write_failed", "Simulated crash before the Run row was written"});
        }
        return inner_.updateRun(std::move(run));
    }
    ai::common::Result<std::optional<ai::application::Run>> findRun(
        const ai::application::RunId& id) const override
    {
        return inner_.findRun(id);
    }
    ai::common::Result<std::vector<ai::application::Run>> listRuns(
        const std::optional<ai::application::ProjectId>& projectId) const override
    {
        return inner_.listRuns(projectId);
    }
    ai::common::Result<std::vector<ai::application::Run>> listUnfinishedRuns() const override
    {
        return inner_.listUnfinishedRuns();
    }
    ai::common::Result<void> replaceStages(
        const ai::application::RunId& runId, std::vector<ai::application::RunStage> stages) override
    {
        return inner_.replaceStages(runId, std::move(stages));
    }
    ai::common::Result<ai::application::RunStage> updateStage(ai::application::RunStage stage) override
    {
        return inner_.updateStage(std::move(stage));
    }
    ai::common::Result<std::vector<ai::application::RunStage>> listStages(
        const ai::application::RunId& runId) const override
    {
        return inner_.listStages(runId);
    }

private:
    ai::application::InMemoryRunRepository inner_;
};

void testRecoverySettlesTheStagesOfAnInterruptedRunBeforeTheRunItself()
{
    using namespace ai::application;
    TempProjectDirectory directory;
    InMemoryProjectRepository projects;
    FailingUpdateRunRepository runs;
    ProjectService projectService(projects);
    const auto project = createProject(projectService, directory.path);

    const auto longAgo = std::chrono::system_clock::now() - 1h;
    Run unfinished;
    unfinished.id = "stage-recovery-run";
    unfinished.projectId = project.id;
    unfinished.title = "Recover the stages";
    unfinished.task = "A Run that died mid-execution";
    unfinished.workflowId = "mock-sequential-v1";
    unfinished.status = RunStatus::Running;
    unfinished.createdAt = longAgo;
    unfinished.startedAt = longAgo;
    const auto stage = [&](const char* id, const char* type, StageStatus status) {
        RunStage value;
        value.id = id;
        value.runId = unfinished.id;
        value.type = type;
        value.title = type;
        value.status = status;
        return value;
    };
    auto completed = stage("s-completed", "planning", StageStatus::Completed);
    completed.finishedAt = longAgo;
    completed.errorMessage = "kept exactly";
    auto failed = stage("s-failed", "review", StageStatus::Failed);
    failed.finishedAt = longAgo;
    failed.errorMessage = "failed for its own reason";
    auto cancelled = stage("s-cancelled", "extra", StageStatus::Cancelled);
    cancelled.finishedAt = longAgo;
    assert(runs.insertRunWithStages(unfinished, {
        completed,
        stage("s-running", "execution", StageStatus::Running),
        stage("s-pending", "verification", StageStatus::Pending),
        failed,
        cancelled}).success());

    MockWorkflowRunner workflow(2ms, 1);
    SingleRunnerResolver resolver(workflow);
    RunManager manager(projects, runs, resolver);
    OrchestrationService service(projects, runs, manager);

    const auto stages = [&] {
        const auto listed = runs.listStages(unfinished.id);
        assert(listed.success());
        std::map<std::string, RunStage> byId;
        for (const auto& value : listed.value()) byId.emplace(value.id, value);
        return byId;
    };

    // The process "dies" after the stages are settled and before the Run row is written: the
    // recovery reports the failure, and the Run is STILL unfinished - so the next startup finds it.
    runs.failNextRunUpdate = true;
    assert(!service.recoverInterruptedRuns().success());
    const auto stillRunning = runs.findRun(unfinished.id);
    assert(stillRunning.success() && stillRunning.value()->status == RunStatus::Running
        && "A Run whose recovery did not finish must stay unfinished so it is recovered again");
    const auto afterCrash = stages();
    assert(afterCrash.at("s-running").status == StageStatus::Failed
        && afterCrash.at("s-pending").status == StageStatus::Skipped
        && "The stages are settled FIRST: a failure before the Run row must not leave them live");
    const auto firstFinishedAt = afterCrash.at("s-running").finishedAt;
    assert(firstFinishedAt.has_value());

    // The next startup repeats the recovery; the already-settled stages are terminal and untouched.
    assert(service.recoverInterruptedRuns().success());
    const auto recovered = runs.findRun(unfinished.id);
    assert(recovered.success() && recovered.value()->status == RunStatus::Interrupted
        && recovered.value()->finishedAt.has_value()
        && recovered.value()->errorMessage == kInterruptedRunRecoveryMessage);
    const auto settled = stages();
    assert(settled.at("s-running").status == StageStatus::Failed
        && settled.at("s-running").errorMessage == kInterruptedRunRecoveryMessage
        && settled.at("s-running").finishedAt == firstFinishedAt
        && "A stage that is already terminal is not touched again");
    assert(settled.at("s-pending").status == StageStatus::Skipped
        && settled.at("s-pending").errorMessage == kInterruptedRunRecoveryMessage
        && settled.at("s-pending").finishedAt == firstFinishedAt);

    // Stages that were ALREADY terminal are exactly as they were - not even their message.
    assert(settled.at("s-completed").status == StageStatus::Completed
        && settled.at("s-completed").errorMessage == "kept exactly"
        && settled.at("s-completed").finishedAt == std::optional(longAgo));
    assert(settled.at("s-failed").status == StageStatus::Failed
        && settled.at("s-failed").errorMessage == "failed for its own reason"
        && settled.at("s-failed").finishedAt == std::optional(longAgo));
    assert(settled.at("s-cancelled").status == StageStatus::Cancelled
        && settled.at("s-cancelled").finishedAt == std::optional(longAgo));

    // A third startup finds nothing unfinished and changes nothing.
    assert(service.recoverInterruptedRuns().success());
    assert(runs.findRun(unfinished.id).value()->finishedAt == recovered.value()->finishedAt);
}

// One recovery, one instant: the stages settled by a single pass carry exactly the Run's own
// finish time, so the recovered timeline reads as one event.
void testRecoveryStampsEverySettledStageAndTheRunWithOneInstant()
{
    using namespace ai::application;
    TempProjectDirectory directory;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    ProjectService projectService(projects);
    const auto project = createProject(projectService, directory.path);

    Run unfinished;
    unfinished.id = "one-instant-run";
    unfinished.projectId = project.id;
    unfinished.title = "One instant";
    unfinished.task = "Settle together";
    unfinished.workflowId = "mock-sequential-v1";
    unfinished.status = RunStatus::Running;
    unfinished.createdAt = std::chrono::system_clock::now() - 1h;
    const auto stage = [&](const char* id, StageStatus status) {
        RunStage value;
        value.id = id;
        value.runId = unfinished.id;
        value.type = id;
        value.title = id;
        value.status = status;
        return value;
    };
    assert(runs.insertRunWithStages(unfinished, {
        stage("a", StageStatus::Running), stage("b", StageStatus::Pending), stage("c", StageStatus::Pending)}).success());
    MockWorkflowRunner workflow(2ms, 1);
    SingleRunnerResolver resolver(workflow);
    RunManager manager(projects, runs, resolver);
    OrchestrationService service(projects, runs, manager);
    assert(service.recoverInterruptedRuns().success());

    const auto run = runs.findRun(unfinished.id);
    const auto listed = runs.listStages(unfinished.id);
    assert(run.success() && run.value()->finishedAt.has_value() && listed.success()
        && listed.value().size() == 3);
    for (const auto& value : listed.value()) {
        assert(value.finishedAt == run.value()->finishedAt
            && "Every stage and the Run share one recovery instant");
        assert(value.errorMessage == kInterruptedRunRecoveryMessage);
    }
}

void testRetryCreatesANewLinkedRun()
{
    // Retry preserves history by creating a different UUID linked to the source.
    TempProjectDirectory directory;
    ai::application::InMemoryProjectRepository projects;
    ai::application::InMemoryRunRepository runs;
    ai::application::ProjectService projectService(projects);
    const auto project = createProject(projectService, directory.path);

    ai::application::MockWorkflowRunner workflow(1ms, 1);
    ai::application::SingleRunnerResolver resolver(workflow);
    ai::application::RunManager manager(projects, runs, resolver);
    ai::application::OrchestrationService service(projects, runs, manager);

    ai::application::StartRunRequest originalRequest;
    originalRequest.projectId = project.id;
    originalRequest.title = "Original";
    originalRequest.task = "Complete before retry";
    originalRequest.workspacePath = directory.path / "override-workspace";
    std::filesystem::create_directories(*originalRequest.workspacePath);

    const auto original = service.startRun(originalRequest);
    assert(original.success());
    const auto originalRun = waitForTerminalRun(runs, original.value());
    assert(originalRun.status == ai::application::RunStatus::Completed);
    assert(originalRun.workspacePath.has_value());

    const auto retried = service.retryRun(original.value());
    assert(retried.success());
    assert(retried.value() != original.value());

    const auto retryRun = waitForTerminalRun(runs, retried.value());
    assert(retryRun.status == ai::application::RunStatus::Completed);
    assert(retryRun.parentRunId.has_value());
    assert(*retryRun.parentRunId == original.value());

    // Regression: retryRun() must carry the original run's per-run
    // workspace override forward, not silently fall back to the project's
    // own rootPath.
    assert(retryRun.workspacePath.has_value());
    assert(*retryRun.workspacePath == *originalRun.workspacePath);

    // Edit & rerun starts from a Backend-owned draft and Backend assigns the
    // lineage again when the edited command is submitted.
    const auto draft = service.prepareRerunDraft(original.value());
    assert(draft.success());
    assert(draft.value().sourceRunId == original.value());
    assert(draft.value().launchMode == ai::application::RunLaunchMode::Mock);
    assert(draft.value().request.workspacePath == originalRun.workspacePath);
    auto editedRequest = draft.value().request;
    editedRequest.title = "Edited rerun";
    const auto edited = service.startEditedRerun(
        draft.value().sourceRunId, std::move(editedRequest));
    assert(edited.success());
    const auto editedRun = waitForTerminalRun(runs, edited.value());
    assert(editedRun.parentRunId == original.value());
    assert(editedRun.title == "Edited rerun");
    assert(editedRun.workspacePath == originalRun.workspacePath);
}

void testShutdownInterruptsAnActiveRun()
{
    // RunManager destruction must not leave a stale Running status behind.
    TempProjectDirectory directory;
    ai::application::InMemoryProjectRepository projects;
    ai::application::InMemoryRunRepository runs;
    ai::application::ProjectService projectService(projects);
    const auto project = createProject(projectService, directory.path);

    ai::application::MockWorkflowRunner workflow(20ms, 10);
    ai::application::RunId runId;
    {
        ai::application::SingleRunnerResolver resolver(workflow);
    ai::application::RunManager manager(projects, runs, resolver);
        ai::application::OrchestrationService service(projects, runs, manager);
        const auto started = service.startRun({project.id, "Shutdown", "Interrupt during shutdown"});
        assert(started.success());
        runId = started.value();
        waitUntilRunning(runs, runId);
        manager.shutdown();
    }

    const auto interrupted = runs.findRun(runId);
    assert(interrupted.success());
    assert(interrupted.value().has_value());
    assert(interrupted.value()->status == ai::application::RunStatus::Interrupted);
    assert(interrupted.value()->finishedAt.has_value());
}

// ---- AG-17.7D-1: a terminal Run never owns a stage that claims to be live ------------------------------

// A workflow runner whose behaviour a test scripts: it declares two Pending stages ("planning",
// "execution") and `behaviour` plays the runner's side of the contract - emit some stage events (or
// none at all) and return a result.
class ScriptedWorkflowRunner final : public ai::application::IWorkflowRunner {
public:
    using Behaviour = std::function<ai::application::WorkflowResult(
        const ai::application::WorkflowRunContext&, std::stop_token, ai::application::IRunEventSink&)>;
    Behaviour behaviour;

    [[nodiscard]] std::vector<ai::application::RunStage> createStages(
        const ai::application::RunId& runId, const ai::application::ResolvedExecutionPlan&) const override
    {
        std::vector<ai::application::RunStage> stages;
        for (const char* type : {"planning", "execution"}) {
            ai::application::RunStage stage;
            stage.id = runId + ":" + type;
            stage.runId = runId;
            stage.type = type;
            stage.title = type;
            stages.push_back(std::move(stage));
        }
        return stages;
    }
    ai::application::WorkflowResult run(
        const ai::application::WorkflowRunContext& context,
        std::stop_token stopToken,
        ai::application::IRunEventSink& sink) override
    {
        return behaviour(context, stopToken, sink);
    }
};

ai::application::RunEvent stageEvent(
    ai::application::RunEventType type,
    const ai::application::WorkflowRunContext& context,
    const std::string& stageType,
    const std::string& message = {})
{
    ai::application::RunEvent event;
    event.type = type;
    event.runId = context.run.id;
    event.stageId = context.run.id + ":" + stageType;
    event.message = message;
    return event;
}

template <typename Repository = ai::application::InMemoryRunRepository>
struct ScriptedFixture {
    TempProjectDirectory directory;
    ai::application::InMemoryProjectRepository projects;
    Repository runs;
    ai::application::ProjectService projectService {projects};
    ai::application::Project project {createProject(projectService, directory.path)};
    ScriptedWorkflowRunner runner;
    ai::application::SingleRunnerResolver resolver {runner};
    ai::application::RunManager manager {projects, runs, resolver};
    ai::application::OrchestrationService service {projects, runs, manager};

    std::map<std::string, ai::application::RunStage> stagesOf(const ai::application::RunId& runId) const
    {
        const auto listed = runs.listStages(runId);
        assert(listed.success());
        std::map<std::string, ai::application::RunStage> byType;
        for (const auto& stage : listed.value()) byType.emplace(stage.type, stage);
        return byType;
    }
};

void waitUntil(const std::function<bool()>& condition)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return;
        std::this_thread::sleep_for(2ms);
    }
    assert(false && "The condition did not become true before the timeout");
}

void testATerminalTransitionSettlesTheStagesBeforeTheRun()
{
    using namespace ai::application;
    using Fixture = ScriptedFixture<>;

    // (1) A runner that fails BEFORE it emits a single stage event (a route or budget refusal): both
    // stages never started, so both are Skipped, with the Run's own instant and message.
    {
        Fixture f;
        f.runner.behaviour = [](const WorkflowRunContext&, std::stop_token, IRunEventSink&) {
            return WorkflowResult {false, RunStatus::Failed, "route refused: the member's provider is gone", {}};
        };
        const auto started = f.service.startRun({f.project.id, "Pre-stage failure", "Refused before any stage"});
        assert(started.success());
        const Run run = waitForTerminalRun(f.runs, started.value());
        assert(run.status == RunStatus::Failed);
        const auto stages = f.stagesOf(run.id);
        for (const char* type : {"planning", "execution"}) {
            assert(stages.at(type).status == StageStatus::Skipped
                && "a stage that never started must not stay Pending under a terminal Run");
            assert(stages.at(type).errorMessage == "route refused: the member's provider is gone");
            assert(stages.at(type).finishedAt == run.finishedAt && "one instant for the stages and the Run");
        }
    }

    // (2) A stage the runner left Running when it failed -> Failed; a stage it had already
    // completed is not touched; the stage after it is Skipped.
    {
        Fixture f;
        f.runner.behaviour = [](const WorkflowRunContext& context, std::stop_token, IRunEventSink& sink) {
            sink.publish(stageEvent(RunEventType::StageStarted, context, "planning"));
            sink.publish(stageEvent(RunEventType::StageCompleted, context, "planning"));
            sink.publish(stageEvent(RunEventType::StageStarted, context, "execution"));
            return WorkflowResult {false, RunStatus::Failed, "the provider exploded", {}};
        };
        const auto started = f.service.startRun({f.project.id, "Mid-stage failure", "Fails while executing"});
        const Run run = waitForTerminalRun(f.runs, started.value());
        assert(run.status == RunStatus::Failed);
        const auto stages = f.stagesOf(run.id);
        assert(stages.at("planning").status == StageStatus::Completed
            && stages.at("planning").errorMessage.empty()
            && stages.at("planning").finishedAt != run.finishedAt
            && "an already-terminal stage is not touched");
        assert(stages.at("execution").status == StageStatus::Failed
            && stages.at("execution").errorMessage == "the provider exploded"
            && stages.at("execution").finishedAt == run.finishedAt);
    }

    // (3) A Run that ends WITHOUT a runner-emitted terminal event for its running stage after a
    // cancellation: the stage is Cancelled, the one after it Skipped.
    {
        Fixture f;
        std::atomic<bool> stageRunning {false};
        f.runner.behaviour = [&](const WorkflowRunContext& context, std::stop_token token, IRunEventSink& sink) {
            sink.publish(stageEvent(RunEventType::StageStarted, context, "planning"));
            stageRunning = true;
            while (!token.stop_requested()) std::this_thread::sleep_for(1ms);
            return WorkflowResult {false, RunStatus::Cancelled, "Run cancelled by the user", {}};
        };
        const auto started = f.service.startRun({f.project.id, "Cancelled", "Cancelled while planning"});
        waitUntil([&] { return stageRunning.load(); });
        assert(f.service.cancelRun(started.value()).success());
        const Run run = waitForTerminalRun(f.runs, started.value());
        assert(run.status == RunStatus::Cancelled);
        const auto stages = f.stagesOf(run.id);
        assert(stages.at("planning").status == StageStatus::Cancelled
            && stages.at("planning").errorMessage == "Run cancelled by the user"
            && stages.at("planning").finishedAt == run.finishedAt);
        assert(stages.at("execution").status == StageStatus::Skipped);
    }

    // (4) A Completed Run: its stages are exactly what the runner made them - and a stage the
    // runner left Pending is Skipped, while one it left RUNNING stays Running (a runner defect that
    // must stay visible, never be rewritten into a success or a failure).
    {
        Fixture f;
        f.runner.behaviour = [](const WorkflowRunContext& context, std::stop_token, IRunEventSink& sink) {
            sink.publish(stageEvent(RunEventType::StageStarted, context, "planning"));
            sink.publish(stageEvent(RunEventType::StageCompleted, context, "planning"));
            return WorkflowResult {true, RunStatus::Completed, "done", {}};
        };
        const auto started = f.service.startRun({f.project.id, "Completed", "Never starts its second stage"});
        const Run run = waitForTerminalRun(f.runs, started.value());
        assert(run.status == RunStatus::Completed);
        const auto stages = f.stagesOf(run.id);
        assert(stages.at("planning").status == StageStatus::Completed);
        assert(stages.at("execution").status == StageStatus::Skipped
            && stages.at("execution").errorMessage == "The run finished before this stage started");
    }
    {
        Fixture f;
        f.runner.behaviour = [](const WorkflowRunContext& context, std::stop_token, IRunEventSink& sink) {
            sink.publish(stageEvent(RunEventType::StageStarted, context, "planning"));
            return WorkflowResult {true, RunStatus::Completed, "done", {}};
        };
        const auto started = f.service.startRun({f.project.id, "Completed", "Leaves a stage running"});
        const Run run = waitForTerminalRun(f.runs, started.value());
        assert(f.stagesOf(run.id).at("planning").status == StageStatus::Running
            && "a running stage under a Completed Run is a runner defect and stays visible");
    }
}

void testShutdownSettlesTheStagesOfAnActiveAndAQueuedRun()
{
    using namespace ai::application;
    ScriptedFixture<> f;
    std::atomic<bool> stageRunning {false};
    f.runner.behaviour = [&](const WorkflowRunContext& context, std::stop_token token, IRunEventSink& sink) {
        sink.publish(stageEvent(RunEventType::StageStarted, context, "planning"));
        stageRunning = true;
        while (!token.stop_requested()) std::this_thread::sleep_for(1ms);
        // Returns without any terminal stage event, as a runner interrupted mid-stage may.
        return WorkflowResult {false, RunStatus::Cancelled, "stopped", {}};
    };
    const auto active = f.service.startRun({f.project.id, "Active", "Running at shutdown"});
    const auto queued = f.service.startRun({f.project.id, "Queued", "Only queued at shutdown"});
    assert(active.success() && queued.success());
    waitUntil([&] { return stageRunning.load(); });

    f.manager.shutdown();

    // A controlled shutdown CANCELS the stage that was running, with the shutdown message.
    const Run activeRun = *f.runs.findRun(active.value()).value();
    assert(activeRun.status == RunStatus::Interrupted
        && activeRun.errorMessage == "Application shutdown interrupted the run");
    const auto activeStages = f.stagesOf(active.value());
    assert(activeStages.at("planning").status == StageStatus::Cancelled
        && activeStages.at("planning").errorMessage == "Application shutdown interrupted the run"
        && activeStages.at("planning").finishedAt == activeRun.finishedAt);
    assert(activeStages.at("execution").status == StageStatus::Skipped);

    // A Run that was only QUEUED never started a stage: both are Skipped, not left Pending forever.
    const Run queuedRun = *f.runs.findRun(queued.value()).value();
    assert(queuedRun.status == RunStatus::Interrupted
        && queuedRun.errorMessage == "Application shutdown before execution");
    const auto queuedStages = f.stagesOf(queued.value());
    for (const char* type : {"planning", "execution"}) {
        assert(queuedStages.at(type).status == StageStatus::Skipped
            && queuedStages.at(type).errorMessage == "Application shutdown before execution"
            && queuedStages.at(type).finishedAt == queuedRun.finishedAt);
    }
}

void testCancellingAQueuedRunSettlesItsStages()
{
    using namespace ai::application;
    ScriptedFixture<> f;
    std::atomic<bool> release {false};
    std::atomic<bool> running {false};
    f.runner.behaviour = [&](const WorkflowRunContext&, std::stop_token token, IRunEventSink&) {
        running = true;
        while (!release.load() && !token.stop_requested()) std::this_thread::sleep_for(1ms);
        return WorkflowResult {true, RunStatus::Completed, "done", {}};
    };
    const auto first = f.service.startRun({f.project.id, "First", "Holds the single worker"});
    const auto second = f.service.startRun({f.project.id, "Second", "Cancelled while queued"});
    waitUntil([&] { return running.load(); });
    assert(f.service.cancelRun(second.value()).success());

    const Run cancelled = *f.runs.findRun(second.value()).value();
    assert(cancelled.status == RunStatus::Cancelled && cancelled.errorMessage == "Run cancelled before execution");
    const auto stages = f.stagesOf(second.value());
    for (const char* type : {"planning", "execution"}) {
        assert(stages.at(type).status == StageStatus::Skipped
            && stages.at(type).errorMessage == "Run cancelled before execution"
            && stages.at(type).finishedAt == cancelled.finishedAt);
    }
    release = true;
    waitForTerminalRun(f.runs, first.value());
}

void testASettlementIsIdempotentAndNeverTouchesATerminalStage()
{
    using namespace ai::application;
    InMemoryRunRepository runs;
    Run run;
    run.id = "settle-run";
    run.projectId = "p";
    run.title = "Settle";
    run.task = "Settle";
    run.workflowId = "mock-sequential-v1";
    run.status = RunStatus::Running;
    run.createdAt = std::chrono::system_clock::now();
    const auto stage = [&](const char* id, StageStatus status) {
        RunStage value;
        value.id = id;
        value.runId = run.id;
        value.type = id;
        value.title = id;
        value.status = status;
        return value;
    };
    auto done = stage("done", StageStatus::Completed);
    done.errorMessage = "kept";
    assert(runs.insertRunWithStages(run, {done, stage("live", StageStatus::Running), stage("waiting", StageStatus::Pending)})
               .success());

    const auto first = std::chrono::system_clock::now();
    assert(settleLiveStages(runs, run.id, StageStatus::Cancelled, first, "first").success());
    const auto byId = [&] {
        const auto listed = runs.listStages(run.id);
        assert(listed.success());
        std::map<std::string, RunStage> result;
        for (const auto& value : listed.value()) result.emplace(value.id, value);
        return result;
    };
    auto stages = byId();
    assert(stages.at("done").status == StageStatus::Completed && stages.at("done").errorMessage == "kept");
    assert(stages.at("live").status == StageStatus::Cancelled && stages.at("live").errorMessage == "first"
        && stages.at("live").finishedAt == std::optional(first));
    assert(stages.at("waiting").status == StageStatus::Skipped && stages.at("waiting").finishedAt == std::optional(first));

    // A second settlement - later instant, other message, other outcome - finds nothing live.
    assert(settleLiveStages(runs, run.id, StageStatus::Failed, first + 1h, "second").success());
    const auto again = byId();
    for (const auto& [id, value] : again) {
        assert(value.status == stages.at(id).status && value.errorMessage == stages.at(id).errorMessage
            && value.finishedAt == stages.at(id).finishedAt && "a second terminalization changes nothing");
    }

    // nullopt leaves a Running stage alone; a non-terminal outcome is refused.
    InMemoryRunRepository other;
    assert(other.insertRunWithStages(run, {stage("live", StageStatus::Running), stage("waiting", StageStatus::Pending)})
               .success());
    assert(settleLiveStages(other, run.id, std::nullopt, first, "m").success());
    assert(other.listStages(run.id).value().front().status == StageStatus::Running
        && other.listStages(run.id).value().back().status == StageStatus::Skipped);
    assert(!settleLiveStages(other, run.id, StageStatus::Completed, first, "m").success());
}

// The process dying between "the stages were settled" and "the Run was written" on the NORMAL
// terminal path: the Run is still unfinished, so startup recovery finishes it - and the stages
// stay exactly as the first settlement left them.
void testACrashBetweenTheStagesAndTheRunIsFinishedByRecovery()
{
    using namespace ai::application;
    ScriptedFixture<FailingUpdateRunRepository> f;
    f.runner.behaviour = [&](const WorkflowRunContext&, std::stop_token, IRunEventSink&) {
        f.runs.failNextRunUpdate = true; // consumed by finishRun's terminal write
        return WorkflowResult {false, RunStatus::Failed, "failed just before the process died", {}};
    };
    const auto started = f.service.startRun({f.project.id, "Dies at the end", "Stages settle, the Run write fails"});
    assert(started.success());
    waitUntil([&] { return !f.runs.failNextRunUpdate.load(); });
    waitUntil([&] { return f.stagesOf(started.value()).at("planning").status == StageStatus::Skipped; });

    const auto stranded = f.runs.findRun(started.value());
    assert(stranded.success() && !isTerminal(stranded.value()->status)
        && "the stages are settled FIRST: the Run write that failed leaves the Run unfinished");
    const auto firstInstant = f.stagesOf(started.value()).at("planning").finishedAt;

    assert(f.service.recoverInterruptedRuns().success());
    const Run recovered = *f.runs.findRun(started.value()).value();
    assert(recovered.status == RunStatus::Interrupted);
    const auto stages = f.stagesOf(started.value());
    for (const char* type : {"planning", "execution"}) {
        assert(stages.at(type).status == StageStatus::Skipped
            && stages.at(type).errorMessage == "failed just before the process died"
            && stages.at(type).finishedAt == firstInstant
            && "recovery must not rewrite the stages the first settlement already closed");
    }
}

} // namespace

int runApplicationPipelineTests()
{
    testProjectValidationAndDuplicateDetection();
    testMockPipelineCompletesAndPublishesProgress();
    testRunningAndQueuedCancellation();
    testRecoveryMarksUnfinishedRunsInterrupted();
    testRecoverySettlesTheStagesOfAnInterruptedRunBeforeTheRunItself();
    testRecoveryStampsEverySettledStageAndTheRunWithOneInstant();
    testRetryCreatesANewLinkedRun();
    testShutdownInterruptsAnActiveRun();
    testATerminalTransitionSettlesTheStagesBeforeTheRun();
    testShutdownSettlesTheStagesOfAnActiveAndAQueuedRun();
    testCancellingAQueuedRunSettlesItsStages();
    testASettlementIsIdempotentAndNeverTouchesATerminalStage();
    testACrashBetweenTheStagesAndTheRunIsFinishedByRecovery();
    return 0;
}
