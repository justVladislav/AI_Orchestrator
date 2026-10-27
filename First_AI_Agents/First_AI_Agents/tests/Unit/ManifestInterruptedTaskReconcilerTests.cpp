// Unit tests for AG-17.7D-1's ManifestInterruptedTaskReconciler over in-memory repositories: the
// rules that decide WHEN the Backend may write a `manifest.task_interrupted` row. The positions in
// these fixtures are computed BY HAND from the documented allocation (a roster member's first task
// keeps its frozen position, a repeated task takes maxMemberPosition + 1 + ordinal; a single
// assignment uses lead position + 1 + task ordinal), so the tests do not lean on the code they
// check. The end-to-end behaviour - a real SQLite crash state, the production startup order, a
// restart - is proved in ManifestRosterRestartTests.
#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/InMemoryExecutionManifestRepository.hpp"
#include "ai/application/InMemoryManifestTaskExecutionRepository.hpp"
#include "ai/application/InMemoryManifestWorkLedgerRepository.hpp"
#include "ai/application/InMemoryRepositories.hpp"
#include "ai/application/ManifestInterruptedTaskReconciler.hpp"
#include "ai/application/ManifestRouting.hpp"
#include "ai/application/verification/ManifestVerificationContinuationPreparationService.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ai::application;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

StageModelAssignment member(const std::string& stageType, bool lead, bool workspace, std::uint32_t position)
{
    StageModelAssignment value;
    value.stageType = stageType;
    value.providerId = "provider-" + stageType;
    value.modelId = "model-" + stageType;
    value.role = "role-" + stageType;
    value.leadArchitect = lead;
    value.workspaceAccess = workspace;
    value.position = position;
    value.timeout = std::chrono::milliseconds(60000);
    return value;
}

ExecutionManifestTask task(const std::string& id, WorkspaceCapability capability,
    std::optional<std::string> assignee)
{
    ExecutionManifestTask value;
    value.id = id;
    value.objective = "objective " + id;
    value.role = "task-role-" + id;
    value.requestedCapability = capability;
    value.assigneeMemberId = std::move(assignee);
    return value;
}

ModelExecution execution(const std::string& id, std::uint32_t position, std::uint32_t attempt,
    const std::string& role, const std::string& memberId, ModelExecutionStatus status,
    const std::string& errorCode = {})
{
    ModelExecution value;
    value.id = id;
    value.sourceId = "run-1";
    value.position = position;
    value.attempt = attempt;
    value.role = role;
    value.providerId = "provider-" + memberId;
    value.modelId = "model-" + memberId;
    value.status = status;
    value.errorCode = errorCode;
    return value;
}

constexpr ModelExecutionStatus kFailed = ModelExecutionStatus::Failed;
const std::string kInterrupted = kModelExecutionInterruptedErrorCode;

// A full set of in-memory repositories holding one Interrupted Manifest Run.
struct World {
    InMemoryRunRepository runs;
    InMemoryExecutionManifestRepository manifests;
    InMemoryManifestTaskExecutionRepository taskRows;
    InMemoryModelExecutionRepository executions;
    InMemoryManifestWorkLedgerRepository ledger;
    ManifestInterruptedTaskReconciler reconciler {runs, manifests, taskRows, executions, ledger};

    explicit World(RunStatus status = RunStatus::Interrupted,
        const std::string& workflowId = kExecutionManifestWorkflowId)
    {
        Run run;
        run.id = "run-1";
        run.projectId = "project-1";
        run.title = "Interrupted";
        run.task = "Interrupted";
        run.workflowId = workflowId;
        run.status = status;
        run.createdAt = std::chrono::system_clock::now();
        require(runs.insertRun(run).success(), "Could not insert the Run");
    }

    void freeze(const std::vector<StageModelAssignment>& assignments, std::vector<ExecutionManifestTask> tasks,
        bool withRouting = true)
    {
        ExecutionManifest manifest;
        manifest.runId = "run-1";
        if (withRouting) manifest.routing = ManifestRouting::fromAssignments(assignments);
        manifest.tasks = std::move(tasks);
        require(manifests.insert(manifest).success(), "Could not freeze the Manifest");
    }
    void addExecution(const ModelExecution& value)
    {
        require(executions.insertExecution(value).success(), "Could not insert an execution");
    }
    void addRow(const std::string& taskId, ManifestTaskStatus status, const std::string& errorCode = {})
    {
        ManifestTaskState state;
        state.status = status;
        state.errorCode = errorCode;
        state.attempts = 1;
        require(taskRows.upsert("run-1", taskId, state).success(), "Could not insert a task row");
    }
    std::optional<ManifestTaskState> rowOf(const std::string& taskId) const
    {
        const auto rows = taskRows.listForRun("run-1");
        require(rows.success(), "Could not list the task rows");
        for (const auto& row : rows.value()) {
            if (row.taskId == taskId) return row.state;
        }
        return std::nullopt;
    }
    std::size_t rowCount() const
    {
        const auto rows = taskRows.listForRun("run-1");
        require(rows.success(), "Could not list the task rows");
        return rows.value().size();
    }
};

// lead@0, w1@1, w2@2; t1 -> w1, t2 -> w2, t3 -> w1 (w1's SECOND task: 2 + 1 + 0 = position 3).
void freezeRoster(World& world)
{
    world.freeze({member("lead", true, false, 0), member("w1", false, true, 1), member("w2", false, true, 2)},
        {task("t1", WorkspaceCapability::Propose, "w1"), task("t2", WorkspaceCapability::Propose, "w2"),
         task("t3", WorkspaceCapability::Propose, "w1")});
}

// ---- the provable case ------------------------------------------------------------------------

void testARosterTaskIsProvenFromItsPositionAndItsMembersRoute()
{
    World world;
    freezeRoster(world);
    world.addExecution(execution("planner", 0, 1, "planner", "lead", ModelExecutionStatus::Completed));
    world.addExecution(execution("t1", 1, 1, "role-w1", "w1", ModelExecutionStatus::Completed));
    // w1's second task is t3, at position 3 - NOT t2, whose member is w2 at position 2.
    world.addExecution(execution("t3", 3, 1, "role-w1", "w1", kFailed, kInterrupted));
    world.addRow("t1", ManifestTaskStatus::Completed);

    const auto outcome = world.reconciler.reconcile();
    require(outcome.success() && outcome.value().materialized == 1 && outcome.value().unreconciled.empty(),
        "exactly the interrupted task must get a row");
    const auto row = world.rowOf("t3");
    require(row.has_value() && row->status == ManifestTaskStatus::Failed
                && row->errorCode == kManifestTaskInterruptedErrorCode
                && row->errorMessage == kManifestTaskInterruptedMessage && row->attempts == 1
                && !row->changeSetId.has_value(),
        "the interrupted task is a Failed manifest.task_interrupted row");
    require(!world.rowOf("t2").has_value(), "a task that never started must get no row");
    require(world.rowOf("t1")->status == ManifestTaskStatus::Completed, "a completed task is untouched");

    const auto again = world.reconciler.reconcile();
    require(again.success() && again.value().materialized == 0 && world.rowCount() == 2,
        "a second pass is a no-op");
}

void testASingleAssignmentUsesTheOrdinalAllocation()
{
    World world;
    // The lone lead plans AND executes: lead@0, tasks at 0 + 1 + ordinal.
    world.freeze({member("solo", true, true, 0)},
        {task("t1", WorkspaceCapability::Propose, std::nullopt),
         task("t2", WorkspaceCapability::Propose, std::nullopt),
         task("t3", WorkspaceCapability::Propose, std::nullopt)});
    world.addExecution(execution("planner", 0, 1, "planner", "solo", ModelExecutionStatus::Completed));
    world.addExecution(execution("t1", 1, 1, "r", "solo", ModelExecutionStatus::Completed));
    world.addExecution(execution("t2", 2, 1, "r", "solo", kFailed, kInterrupted));
    world.addRow("t1", ManifestTaskStatus::Completed);

    const auto outcome = world.reconciler.reconcile();
    require(outcome.success() && outcome.value().materialized == 1, "the second task is the interrupted one");
    require(world.rowOf("t2").has_value() && world.rowOf("t2")->errorCode == kManifestTaskInterruptedErrorCode
                && !world.rowOf("t3").has_value(),
        "t2 (position 2), never the planner at position 0 or the unstarted t3");
}

void testTheLatestInterruptedAttemptSetsTheAttemptCount()
{
    World world;
    freezeRoster(world);
    world.addExecution(execution("t2-a1", 2, 1, "role-w2", "w2", kFailed, "provider.request_failed"));
    world.addExecution(execution("t2-a2", 2, 2, "role-w2", "w2", kFailed, kInterrupted));
    const auto outcome = world.reconciler.reconcile();
    require(outcome.success() && outcome.value().materialized == 1, "the interrupted retry attempt");
    require(world.rowOf("t2").has_value() && world.rowOf("t2")->attempts == 2,
        "the row records the attempt that was interrupted");
}

// ---- what is NOT proven ------------------------------------------------------------------------

// "Nothing to reconcile" is told apart from an INCOMPLETE account: the latter holds the Run back from
// the immutable ledger, the former does not.
void testAnUnattributableInterruptionHoldsTheLedgerBackButNothingToDoDoesNot()
{
    const auto held = [](const ManifestInterruptedTaskReconciler::Outcome& outcome) {
        return outcome.unreconciled.contains("run-1");
    };
    {
        World world; // a Manifest frozen before routing records existed: reconciliation is inapplicable
        world.freeze({member("lead", true, false, 0), member("w1", false, true, 1)},
            {task("t1", WorkspaceCapability::Propose, "w1")}, /*withRouting=*/false);
        world.addExecution(execution("t1", 1, 1, "role-w1", "w1", kFailed, kInterrupted));
        const auto outcome = world.reconciler.reconcile();
        require(outcome.success() && outcome.value().materialized == 0 && !held(outcome.value())
                    && world.rowCount() == 0,
            "a legacy Manifest without a routing record is out of scope, not an incomplete account");
    }
    {
        World world; // never frozen: no tasks to account for
        world.addExecution(execution("planner", 0, 1, "planner", "lead", kFailed, kInterrupted));
        const auto outcome = world.reconciler.reconcile();
        require(outcome.success() && outcome.value().materialized == 0 && !held(outcome.value()),
            "a Run that never froze a Manifest has nothing to reconcile");
    }
    {
        World world; // a routing record that is PRESENT but fails its own integrity check
        ExecutionManifest manifest;
        manifest.runId = "run-1";
        manifest.routing = ManifestRouting::fromAssignments(
            {member("lead", true, false, 0), member("w1", false, true, 1), member("w2", false, true, 2)});
        manifest.routing->fingerprint = "not-the-fingerprint";
        manifest.tasks = {task("t1", WorkspaceCapability::Propose, "w1")};
        require(world.manifests.insert(manifest).success(), "Could not freeze the tampered Manifest");
        world.addExecution(execution("t1", 1, 1, "role-w1", "w1", kFailed, kInterrupted));
        const auto outcome = world.reconciler.reconcile();
        require(outcome.success() && outcome.value().materialized == 0 && held(outcome.value()) && world.rowCount() == 0,
            "a damaged routing record must hold the Run back: the interruption cannot be attributed");
    }
    {
        World world; // an emptied record
        ExecutionManifest manifest;
        manifest.runId = "run-1";
        manifest.routing = ManifestRouting::fromAssignments(
            {member("lead", true, false, 0), member("w1", false, true, 1)});
        manifest.routing->members.clear();
        manifest.tasks = {task("t1", WorkspaceCapability::Propose, "w1")};
        require(world.manifests.insert(manifest).success(), "Could not freeze the Manifest");
        world.addExecution(execution("t1", 1, 1, "role-w1", "w1", kFailed, kInterrupted));
        require(held(world.reconciler.reconcile().value()), "a record without members must hold the Run back");
    }
    {
        World world; // no lead Architect in the record
        world.freeze({member("a", false, true, 0), member("b", false, true, 1)},
            {task("t1", WorkspaceCapability::Propose, "b")});
        world.addExecution(execution("t1", 1, 1, "role-b", "b", kFailed, kInterrupted));
        const auto outcome = world.reconciler.reconcile();
        require(outcome.success() && outcome.value().materialized == 0 && held(outcome.value()),
            "a record without a lead must hold the Run back");
    }
    {
        World world; // the position allocation cannot be computed: a task names a member that is not in the record
        world.freeze({member("lead", true, false, 0), member("w1", false, true, 1)},
            {task("t1", WorkspaceCapability::Propose, "ghost")});
        world.addExecution(execution("t1", 1, 1, "role-w1", "w1", kFailed, kInterrupted));
        const auto outcome = world.reconciler.reconcile();
        require(outcome.success() && outcome.value().materialized == 0 && held(outcome.value()),
            "an allocation that cannot be computed must hold the Run back");
    }
    {
        World world; // ... but a damaged record with NOTHING interrupted has nothing to attribute
        ExecutionManifest manifest;
        manifest.runId = "run-1";
        manifest.routing = ManifestRouting::fromAssignments(
            {member("lead", true, false, 0), member("w1", false, true, 1), member("w2", false, true, 2)});
        manifest.routing->fingerprint = "not-the-fingerprint";
        manifest.tasks = {task("t1", WorkspaceCapability::Propose, "w1")};
        require(world.manifests.insert(manifest).success(), "Could not freeze the tampered Manifest");
        world.addExecution(execution("planner", 0, 1, "planner", "lead", ModelExecutionStatus::Completed));
        world.addExecution(execution("t1", 1, 1, "role-w1", "w1", ModelExecutionStatus::Cancelled, "manifest.task_cancelled"));
        require(!held(world.reconciler.reconcile().value()),
            "with no interrupted execution the account is complete whatever the record says");
    }
}

void testOnlyAnInterruptedManifestRunWithoutALedgerEntryIsReconciled()
{
    const auto interruptedExecution = execution("t1", 1, 1, "role-w1", "w1", kFailed, kInterrupted);
    {
        World world(RunStatus::Failed); // not Interrupted
        freezeRoster(world);
        world.addExecution(interruptedExecution);
        require(world.reconciler.reconcile().value().materialized == 0 && world.rowCount() == 0,
            "only an Interrupted Run is reconciled");
    }
    {
        World world(RunStatus::Interrupted, "mock-sequential-v1"); // not a ledger workflow
        freezeRoster(world);
        world.addExecution(interruptedExecution);
        require(world.reconciler.reconcile().value().materialized == 0 && world.rowCount() == 0,
            "only a Manifest workflow Run is reconciled");
    }
    {
        World world; // the continuation workflow records ledger entries too
        world.runs.insertRun([] {
            Run run;
            run.id = "run-2";
            run.projectId = "project-1";
            run.title = "continuation";
            run.task = "continuation";
            run.workflowId = std::string(kManifestVerificationContinuationWorkflowId);
            run.status = RunStatus::Interrupted;
            run.createdAt = std::chrono::system_clock::now();
            return run;
        }());
        freezeRoster(world);
        world.addExecution(interruptedExecution);
        require(world.reconciler.reconcile().value().materialized == 1, "the Manifest Run itself is reconciled");
    }
    {
        World world; // a ledger entry is immutable history: never second-guessed
        freezeRoster(world);
        world.addExecution(interruptedExecution);
        ManifestWorkLedgerEntry entry;
        entry.runId = "run-1";
        entry.projectId = "project-1";
        entry.terminalStatus = RunStatus::Interrupted;
        require(world.ledger.insertEntry(entry).success(), "Could not insert the ledger entry");
        const auto outcome = world.reconciler.reconcile();
        require(outcome.success() && outcome.value().materialized == 0 && world.rowCount() == 0
                    && outcome.value().unreconciled.empty(),
            "a Run that already has a ledger entry must not be touched or held back");
    }
}

void testAStillRunningExecutionHoldsTheRunBack()
{
    World world;
    freezeRoster(world);
    world.addExecution(execution("t1", 1, 1, "role-w1", "w1", ModelExecutionStatus::Running));
    const auto outcome = world.reconciler.reconcile();
    require(outcome.success() && outcome.value().materialized == 0
                && outcome.value().unreconciled.contains("run-1"),
        "an execution recovery has not settled means the account is incomplete: hold the Run back");

    // Once the execution is settled the Run is complete.
    ModelExecution settled = execution("t1", 1, 1, "role-w1", "w1", kFailed, kInterrupted);
    require(world.executions.updateExecution(settled).success(), "Could not settle the execution");
    const auto after = world.reconciler.reconcile();
    require(after.success() && after.value().materialized == 1 && after.value().unreconciled.empty(),
        "a settled execution completes the account");
}

void testAnExistingTerminalRowIsNeverOverwrittenButALiveOneIs()
{
    {
        World world;
        freezeRoster(world);
        world.addExecution(execution("t1", 1, 1, "role-w1", "w1", kFailed, kInterrupted));
        world.addRow("t1", ManifestTaskStatus::Failed, "manifest.worker_rejected");
        require(world.reconciler.reconcile().value().materialized == 0
                    && world.rowOf("t1")->errorCode == "manifest.worker_rejected",
            "a task with a terminal row keeps it");
    }
    {
        World world;
        freezeRoster(world);
        world.addExecution(execution("t1", 1, 1, "role-w1", "w1", kFailed, kInterrupted));
        world.addRow("t1", ManifestTaskStatus::Running);
        require(world.reconciler.reconcile().value().materialized == 1
                    && world.rowOf("t1")->status == ManifestTaskStatus::Failed
                    && world.rowOf("t1")->errorCode == kManifestTaskInterruptedErrorCode,
            "a row that still claims to be Running is replaced by the proven interruption");
    }
}

// What an execution is - and is not - taken to be. `held` = the Run's account is incomplete.
void testAnExecutionIsNeverMatchedByGuess()
{
    struct Shape {
        std::size_t written;
        bool held;
    };
    const auto shape = [](const std::string& id, std::uint32_t position, const std::string& role,
                           const std::string& memberId, ModelExecutionStatus status, const std::string& code) {
        World world;
        freezeRoster(world);
        world.addExecution(execution(id, position, 1, role, memberId, status, code));
        const auto outcome = world.reconciler.reconcile().value();
        return Shape {outcome.materialized, outcome.unreconciled.contains("run-1")};
    };
    const auto expectShape = [&](const Shape& actual, std::size_t written, bool held, const char* message) {
        require(actual.written == written && actual.held == held, message);
    };
    expectShape(shape("e", 1, "role-w1", "w1", kFailed, kInterrupted), 1, false, "control");

    // Not an interruption at all: no row, and nothing is unaccounted for.
    expectShape(shape("e", 1, "role-w1", "w1", kFailed, kModelExecutionOrphanedErrorCode), 0, false,
        "orphaned is not interrupted");
    expectShape(shape("e", 1, "role-w1", "w1", kFailed, "provider.request_failed"), 0, false,
        "a provider failure is not interrupted");
    expectShape(shape("e", 1, "role-w1", "w1", ModelExecutionStatus::Cancelled, "manifest.task_cancelled"), 0, false,
        "cancellation is not interruption");

    // An interruption the Backend cannot attribute with certainty: no row, and the Run is HELD BACK.
    expectShape(shape("e", 1, "role-w1", "w2", kFailed, kInterrupted), 0, true,
        "another member's provider/model is not this task's: an incomplete account");
    expectShape(shape("e", 9, "role-w1", "w1", kFailed, kInterrupted), 0, true,
        "a position that is no task's slot: an incomplete account");

    // A worker's role is user-chosen text: identity is the POSITION, never the role.
    expectShape(shape("e", 1, "planner", "w1", kFailed, kInterrupted), 1, false,
        "a worker whose role is 'planner' is still a task");
    expectShape(shape("e", 1, "integration", "w1", kFailed, kInterrupted), 1, false,
        "a worker whose role is 'integration' is still a task");

    // The lead's own rows are told apart by POSITION: the Planner at the lead's position and the
    // integration call one past the last task slot (t3 is at 3, so 4) are not tasks - and not an
    // incomplete account either.
    expectShape(shape("e", 0, "role-lead", "lead", kFailed, kInterrupted), 0, false,
        "the lead's position is the Planner's, not a task slot");
    expectShape(shape("e", 4, "integration", "lead", kFailed, kInterrupted), 0, false,
        "the integration position is not a task slot");
    expectShape(shape("e", 4, "role-w1", "w1", kFailed, kInterrupted), 0, false,
        "the integration position is excluded by position, whatever the role says");
}

} // namespace

int runManifestInterruptedTaskReconcilerTests()
{
    try {
        testARosterTaskIsProvenFromItsPositionAndItsMembersRoute();
        testASingleAssignmentUsesTheOrdinalAllocation();
        testTheLatestInterruptedAttemptSetsTheAttemptCount();
        testAnUnattributableInterruptionHoldsTheLedgerBackButNothingToDoDoesNot();
        testOnlyAnInterruptedManifestRunWithoutALedgerEntryIsReconciled();
        testAStillRunningExecutionHoldsTheRunBack();
        testAnExistingTerminalRowIsNeverOverwrittenButALiveOneIs();
        testAnExecutionIsNeverMatchedByGuess();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "ManifestInterruptedTaskReconciler tests failed: " << exception.what() << '\n';
        return 1;
    }
}
