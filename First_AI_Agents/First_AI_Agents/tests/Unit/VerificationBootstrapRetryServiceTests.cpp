// AG-18.3 tests for VerificationBootstrapRetryService: the Backend-level
// operation that creates a new Run linked to a predecessor Run's durable
// ActionRequired disposition. Uses the trivial Mock workflow (no provider/
// session mocking needed) so the created Run can actually reach a terminal
// state cheaply.
#include "ai/application/InMemoryRepositories.hpp"
#include "ai/application/InMemoryVerificationBootstrapRepository.hpp"
#include "ai/application/execution/MockWorkflowRunner.hpp"
#include "ai/application/execution/OrchestrationService.hpp"
#include "ai/application/execution/RunManager.hpp"
#include "ai/application/verification/VerificationBootstrapRetryService.hpp"
#include "ai/application/execution/WorkflowRouter.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

namespace common = ai::common;
using namespace ai::application;
using namespace std::chrono_literals;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct TempWorkspace {
    std::filesystem::path path {std::filesystem::temp_directory_path()
        / ("ai-orchestrator-bootstrap-retry-test-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};

    TempWorkspace() { std::filesystem::create_directories(path); }
    ~TempWorkspace()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    void writePreset() const
    {
        std::ofstream stream(path / "CMakeUserPresets.json", std::ios::binary);
        stream << "{\"version\":6,\"configurePresets\":[],\"buildPresets\":[]}";
        require(static_cast<bool>(stream), "Could not write retry test preset");
    }
};

struct Harness {
    TempWorkspace workspace;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    InMemoryExecutionProfileRepository profiles;
    InMemoryVerificationBootstrapRepository bootstrapRepository;
    MockWorkflowRunner mockRunner {1ms, 1};
    WorkflowRouter router {profiles, providers, models};
    RunManager manager {projects, runs, router};
    OrchestrationService orchestration {projects, runs, manager};
    VerificationBootstrapRetryService retryService {
        bootstrapRepository, runs, projects, orchestration};

    Harness() { router.registerRunner(kMockWorkflowId, mockRunner); }

    ProjectId makeProject(bool withPreset = true)
    {
        if (withPreset) workspace.writePreset();
        Project project;
        project.name = "Bootstrap retry project";
        project.description = "Test project";
        project.rootPath = workspace.path;
        const auto created = projects.insert(project);
        require(created.success(), "Could not seed project");
        return created.value().id;
    }
};

Run waitForTerminalRun(InMemoryRunRepository& runs, const RunId& runId)
{
    for (int attempt = 0; attempt < 200; ++attempt) {
        const auto found = runs.findRun(runId);
        require(found.success() && found.value().has_value(), "Run disappeared while waiting");
        const auto status = found.value()->status;
        if (status == RunStatus::Completed || status == RunStatus::Failed
            || status == RunStatus::Cancelled) {
            return *found.value();
        }
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("Run never reached a terminal state");
}

// AG-18.3 hardening (blocker 4, required test #2): wraps a real InMemory
// repository so recordDecision/findDecisionForRun keep working normally
// (needed to seed a genuine ActionRequired predecessor) while
// recordRetryLink simulates a durable-storage outage - proving link
// persistence failure occurs, and is surfaced, BEFORE any new Run is ever
// created.
class RejectingRetryLinkVerificationBootstrapRepository final
    : public IVerificationBootstrapRepository {
public:
    explicit RejectingRetryLinkVerificationBootstrapRepository(
        InMemoryVerificationBootstrapRepository& inner)
        : inner_(inner)
    {
    }

    common::Result<void> recordDecision(VerificationBootstrapDecision decision) override
    {
        return inner_.recordDecision(std::move(decision));
    }
    common::Result<std::optional<VerificationBootstrapDecision>> findDecisionForRun(
        const RunId& runId) const override
    {
        return inner_.findDecisionForRun(runId);
    }
    common::Result<void> appendTransition(VerificationBootstrapTransition transition) override
    {
        return inner_.appendTransition(std::move(transition));
    }
    common::Result<std::vector<VerificationBootstrapTransition>> listTransitionsForRun(
        const RunId& runId) const override
    {
        return inner_.listTransitionsForRun(runId);
    }
    common::Result<void> recordRetryLink(VerificationBootstrapRetryLink) override
    {
        return common::Result<void>::failure({"storage.verification_bootstrap_rejected",
            "Simulated verification bootstrap retry-link persistence failure"});
    }
    common::Result<std::optional<VerificationBootstrapRetryLink>> findRetryLinkForPredecessor(
        const RunId& predecessorRunId) const override
    {
        return inner_.findRetryLinkForPredecessor(predecessorRunId);
    }

private:
    InMemoryVerificationBootstrapRepository& inner_;
};

struct RejectingLinkHarness {
    TempWorkspace workspace;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    InMemoryExecutionProfileRepository profiles;
    InMemoryVerificationBootstrapRepository innerBootstrapRepository;
    RejectingRetryLinkVerificationBootstrapRepository bootstrapRepository {innerBootstrapRepository};
    MockWorkflowRunner mockRunner {1ms, 1};
    WorkflowRouter router {profiles, providers, models};
    RunManager manager {projects, runs, router};
    OrchestrationService orchestration {projects, runs, manager};
    VerificationBootstrapRetryService retryService {
        bootstrapRepository, runs, projects, orchestration};

    RejectingLinkHarness() { router.registerRunner(kMockWorkflowId, mockRunner); }

    ProjectId makeProject()
    {
        workspace.writePreset();
        Project project;
        project.name = "Bootstrap retry project";
        project.description = "Test project";
        project.rootPath = workspace.path;
        const auto created = projects.insert(project);
        require(created.success(), "Could not seed project");
        return created.value().id;
    }
};

template <typename HarnessT>
RunId startActionRequiredPredecessor(HarnessT& harness, const ProjectId& projectId,
    VerificationRepairPolicy repairPolicy = VerificationRepairPolicy::OneBoundedWave)
{
    StartRunRequest request;
    request.projectId = projectId;
    request.title = "Original Run needing manual setup";
    request.task = "Build the project";
    request.workflowId = kMockWorkflowId;
    request.verificationRepairPolicy = repairPolicy;
    const auto started = harness.orchestration.startRun(request);
    require(started.success(), "Could not start predecessor Run");
    waitForTerminalRun(harness.runs, started.value());

    VerificationBootstrapDecision decision;
    decision.runId = started.value();
    decision.projectId = projectId;
    decision.baselineFingerprint = "baseline-fp-1";
    decision.verificationSnapshotSchemaVersion = "verification-environment-snapshot.v1";
    decision.disposition = VerificationBootstrapDisposition::ActionRequired;
    decision.reasonCode = "verification_environment.action_required";
    decision.message = "No usable profile and no safe bootstrap was declared";
    decision.decidedAt = std::chrono::system_clock::now();
    require(harness.bootstrapRepository.recordDecision(decision).success(),
        "Could not record ActionRequired decision for the predecessor Run");
    return started.value();
}

void testLinkedRetryCreatesNewRunWithPredecessorLink()
{
    Harness harness;
    const auto projectId = harness.makeProject();
    const auto predecessorId = startActionRequiredPredecessor(harness, projectId);

    const auto retried = harness.retryService.createLinkedRetry(predecessorId);
    require(retried.success(), "A durable ActionRequired predecessor did not admit a linked retry");
    require(retried.value() != predecessorId, "The linked retry reused the old runId");

    const auto newRun = harness.runs.findRun(retried.value());
    require(newRun.success() && newRun.value().has_value(), "The linked retry Run does not exist");
    require(newRun.value()->parentRunId.has_value() && *newRun.value()->parentRunId == predecessorId,
        "The linked retry Run did not record the predecessor as its parent");
    require(newRun.value()->projectId == projectId,
        "The linked retry Run did not preserve the original project identity");

    const auto link = harness.bootstrapRepository.findRetryLinkForPredecessor(predecessorId);
    require(link.success() && link.value().has_value()
            && link.value()->newRunId == retried.value(),
        "The durable retry link was not recorded");
}

void testDuplicateLinkedRetryIsIdempotent()
{
    Harness harness;
    const auto projectId = harness.makeProject();
    const auto predecessorId = startActionRequiredPredecessor(harness, projectId);

    const auto first = harness.retryService.createLinkedRetry(predecessorId);
    require(first.success(), "First linked retry request failed");
    const auto second = harness.retryService.createLinkedRetry(predecessorId);
    require(second.success() && second.value() == first.value(),
        "A duplicate linked retry request must deterministically return the same Run, not "
        "create a second one");
}

void testRetryFailsClosedWithoutActionRequiredDecision()
{
    Harness harness;
    const auto projectId = harness.makeProject();
    StartRunRequest request;
    request.projectId = projectId;
    request.title = "An ordinary completed Run";
    request.task = "Build the project";
    request.workflowId = kMockWorkflowId;
    const auto started = harness.orchestration.startRun(request);
    require(started.success(), "Could not start Run");
    waitForTerminalRun(harness.runs, started.value());
    // Deliberately never record a bootstrap decision for this Run.

    const auto retried = harness.retryService.createLinkedRetry(started.value());
    require(!retried.success()
            && retried.error()->code == "verification_environment.retry_source_invalid",
        "A linked retry was created from a Run with no durable ActionRequired decision");
}

void testRetryWithoutPresetCreatesNeitherReservationNorRun()
{
    Harness harness;
    const auto projectId = harness.makeProject(false);
    const auto predecessorId = startActionRequiredPredecessor(harness, projectId);
    const auto before = harness.runs.listRuns(projectId);
    require(before.success(), "Could not list Runs before missing-preset retry");

    const auto refused = harness.retryService.createLinkedRetry(predecessorId);
    require(!refused.success()
            && refused.error()->code == "verification_environment.setup_still_required",
        "Retry after setup did not reject an ActionRequired Run while setup was still absent");
    const auto link = harness.bootstrapRepository.findRetryLinkForPredecessor(predecessorId);
    require(link.success() && !link.value().has_value(),
        "Missing-preset retry persisted a reservation before its command-boundary preflight");
    const auto after = harness.runs.listRuns(projectId);
    require(after.success() && after.value().size() == before.value().size(),
        "Missing-preset retry created a new Run");

    // The action stays available: after setup appears, the same explicit
    // command succeeds without a page refresh or a different predecessor.
    harness.workspace.writePreset();
    const auto retried = harness.retryService.createLinkedRetry(predecessorId);
    require(retried.success(),
        "Retry after setup did not succeed after the preset file appeared");
}

void testRetryFailsClosedOnUnknownRunId()
{
    Harness harness;
    const auto retried = harness.retryService.createLinkedRetry("does-not-exist");
    require(!retried.success()
            && retried.error()->code == "verification_environment.retry_source_invalid",
        "A linked retry was created from a nonexistent predecessor Run id");
}

void testRetryNeverMutatesSourceRun()
{
    Harness harness;
    const auto projectId = harness.makeProject();
    const auto predecessorId = startActionRequiredPredecessor(harness, projectId);
    const auto before = harness.runs.findRun(predecessorId);
    require(before.success() && before.value().has_value(), "Predecessor Run missing before retry");
    const auto statusBefore = before.value()->status;
    const auto revisionBefore = before.value()->revision;

    require(harness.retryService.createLinkedRetry(predecessorId).success(),
        "Linked retry creation failed");

    const auto after = harness.runs.findRun(predecessorId);
    require(after.success() && after.value().has_value(), "Predecessor Run missing after retry");
    require(after.value()->status == statusBefore && after.value()->revision == revisionBefore,
        "Creating a linked retry mutated the predecessor Run");
}

// AG-18.3 hardening (blocker 4, required test #2): a recordRetryLink
// persistence failure must occur, and be surfaced, BEFORE any new Run is
// ever created - never an orphaned Run with no durable reservation.
void testRetryLinkPersistenceFailureCreatesNoRun()
{
    RejectingLinkHarness harness;
    const auto projectId = harness.makeProject();
    const auto predecessorId = startActionRequiredPredecessor(harness, projectId);
    const auto runCountBefore = harness.runs.listRuns(projectId);
    require(runCountBefore.success(), "Could not list Runs before retry");
    const auto countBefore = runCountBefore.value().size();

    const auto retried = harness.retryService.createLinkedRetry(predecessorId);
    require(!retried.success()
            && retried.error()->code == "storage.verification_bootstrap_rejected",
        "A retry-link persistence failure was not surfaced as-is");

    const auto runCountAfter = harness.runs.listRuns(projectId);
    require(runCountAfter.success() && runCountAfter.value().size() == countBefore,
        "A retry-link persistence failure must never create a new Run");
}

// AG-18.3 hardening (blocker 4, required test #4): a reservation that was
// durably recorded but whose Run was never created (interrupted between
// the two steps) must be recoverable on the next call - resuming with the
// exact same reserved id, never a fresh one.
void testInterruptedReservationIsRecoverableWithSameReservedId()
{
    Harness harness;
    const auto projectId = harness.makeProject();
    const auto predecessorId = startActionRequiredPredecessor(harness, projectId);

    const RunId reservedRunId = "reserved-run-id-never-created";
    VerificationBootstrapRetryLink link;
    link.predecessorRunId = predecessorId;
    link.newRunId = reservedRunId;
    link.projectId = projectId;
    link.reason = "action_required_linked_retry";
    link.createdAt = std::chrono::system_clock::now();
    require(harness.bootstrapRepository.recordRetryLink(link).success(),
        "Could not seed an interrupted reservation");
    // Deliberately never create a Run at reservedRunId - simulates a crash
    // between recordRetryLink succeeding and the Run actually being
    // created.
    require(!harness.runs.findRun(reservedRunId).value().has_value(),
        "Test setup invariant violated: the reserved Run must not yet exist");

    const auto resumed = harness.retryService.createLinkedRetry(predecessorId);
    require(resumed.success() && resumed.value() == reservedRunId,
        "An interrupted reservation was not resumed with its exact reserved id");
    const auto resumedRun = harness.runs.findRun(reservedRunId);
    require(resumedRun.success() && resumedRun.value().has_value()
            && resumedRun.value()->parentRunId.has_value()
            && *resumedRun.value()->parentRunId == predecessorId,
        "The resumed Run does not exist or lost its predecessor linkage");

    // A further call must now be a plain idempotent success against the
    // now-created Run.
    const auto again = harness.retryService.createLinkedRetry(predecessorId);
    require(again.success() && again.value() == reservedRunId,
        "A call after the reservation resumed was not idempotent");
}

// AG-18.3 hardening (blocker 4, required test #6): a retry link whose
// reserved Run exists but has drifted identity (different predecessor
// linkage) from the recorded link must fail closed rather than being
// handed back as if it were this retry's own Run.
void testConflictingReservedRunIdentityFailsClosed()
{
    Harness harness;
    const auto projectId = harness.makeProject();
    const auto predecessorId = startActionRequiredPredecessor(harness, projectId);

    // An unrelated, independently-started Run - never linked to
    // predecessorId as its parent.
    StartRunRequest unrelatedRequest;
    unrelatedRequest.projectId = projectId;
    unrelatedRequest.title = "An unrelated Run";
    unrelatedRequest.task = "Do something else entirely";
    unrelatedRequest.workflowId = kMockWorkflowId;
    const auto unrelated = harness.orchestration.startRun(unrelatedRequest);
    require(unrelated.success(), "Could not start the unrelated Run");
    waitForTerminalRun(harness.runs, unrelated.value());

    VerificationBootstrapRetryLink conflictingLink;
    conflictingLink.predecessorRunId = predecessorId;
    conflictingLink.newRunId = unrelated.value();
    conflictingLink.projectId = projectId;
    conflictingLink.reason = "action_required_linked_retry";
    conflictingLink.createdAt = std::chrono::system_clock::now();
    require(harness.bootstrapRepository.recordRetryLink(conflictingLink).success(),
        "Could not seed a conflicting retry link");

    const auto retried = harness.retryService.createLinkedRetry(predecessorId);
    require(!retried.success()
            && retried.error()->code == "verification_environment.retry_link_conflict",
        "A retry link pointing at a Run with mismatched predecessor identity was accepted");
}

// AG-18.3 hardening (blocker 4, required test #5): concurrent linked-retry
// requests for the same predecessor must never create more than one new
// Run - the reservation's own durable uniqueness serializes the race.
void testConcurrentLinkedRetriesCreateExactlyOneRun()
{
    Harness harness;
    const auto projectId = harness.makeProject();
    const auto predecessorId = startActionRequiredPredecessor(harness, projectId);

    constexpr int kConcurrentCallers = 8;
    std::vector<std::thread> threads;
    std::mutex resultsMutex;
    std::vector<RunId> successfulRunIds;
    std::atomic<int> failures {0};

    for (int i = 0; i < kConcurrentCallers; ++i) {
        threads.emplace_back([&] {
            const auto result = harness.retryService.createLinkedRetry(predecessorId);
            if (result.success()) {
                std::lock_guard lock(resultsMutex);
                successfulRunIds.push_back(result.value());
            } else {
                ++failures;
            }
        });
    }
    for (auto& thread : threads) thread.join();

    require(!successfulRunIds.empty(), "No concurrent caller ever succeeded");
    for (const auto& id : successfulRunIds) {
        require(id == successfulRunIds.front(),
            "Concurrent callers that succeeded disagreed on the linked retry's Run id");
    }
    const auto runsForProject = harness.runs.listRuns(projectId);
    require(runsForProject.success(), "Could not list Runs after concurrent retries");
    std::size_t linkedRunCount = 0;
    for (const auto& run : runsForProject.value()) {
        if (run.parentRunId.has_value() && *run.parentRunId == predecessorId) {
            ++linkedRunCount;
        }
    }
    require(linkedRunCount == 1,
        "Concurrent linked-retry requests created more than one new Run for the same "
        "predecessor");
}

// A linked retry is a new Run for the same work, so it must start with the predecessor's paid-repair decision:
// a Disabled verification repair stays Disabled (it used to come back as the default one bounded paid wave),
// and the default stays the default. Checked on the request the retry starts and end to end through the
// service; no provider is involved.
void testLinkedRetryPreservesTheVerificationRepairPolicy()
{
    for (const auto policy : {VerificationRepairPolicy::Disabled, VerificationRepairPolicy::OneBoundedWave}) {
        Run predecessor;
        predecessor.id = "predecessor-run";
        predecessor.projectId = "project-1";
        predecessor.title = "Original";
        predecessor.task = "Task";
        predecessor.workflowId = kMockWorkflowId;
        predecessor.verificationRepairPolicy = policy;
        require(buildVerificationBootstrapRetryRequest(predecessor, "reserved-run").verificationRepairPolicy == policy,
            "The linked retry request must carry the predecessor's repair policy exactly");
    }

    for (const auto policy : {VerificationRepairPolicy::Disabled, VerificationRepairPolicy::OneBoundedWave}) {
        Harness harness;
        const auto projectId = harness.makeProject();
        const auto predecessorId = startActionRequiredPredecessor(harness, projectId, policy);
        const auto predecessor = harness.runs.findRun(predecessorId);
        require(predecessor.success() && predecessor.value().has_value()
                && predecessor.value()->verificationRepairPolicy == policy,
            "The predecessor must have frozen the policy it was started with");

        const auto retried = harness.retryService.createLinkedRetry(predecessorId);
        require(retried.success(), "The linked retry could not be created");
        const auto child = harness.runs.findRun(retried.value());
        require(child.success() && child.value().has_value()
                && child.value()->verificationRepairPolicy == policy,
            "A linked retry must keep the predecessor's verification repair policy (disabled stays disabled)");
    }
}

// PC-2: a linked retry is a new Run for the same work, so it starts with the predecessor's frozen interface
// contracts (and its other immutable inputs) - never with the contracts silently gone.
void testLinkedRetryRequestInheritsTheInterfaceContractsAndImmutableInputs()
{
    Run predecessor;
    predecessor.id = "predecessor-run";
    predecessor.projectId = "project-1";
    predecessor.title = "Original";
    predecessor.task = "The task text";
    predecessor.workflowId = kExecutionManifestWorkflowId;
    predecessor.executionProfileId = "profile-1";
    predecessor.maxParallelism = 3;
    RunInterfaceContract shape;
    shape.id = "C1";
    shape.title = "Shape";
    shape.text = "property string metricId";
    shape.appliesToPaths = {"qml/models"};
    predecessor.interfaceContracts = {shape};

    const auto request = buildVerificationBootstrapRetryRequest(predecessor, "reserved-run");
    require(request.interfaceContracts.size() == 1 && request.interfaceContracts[0].id == "C1"
            && request.interfaceContracts[0].text == "property string metricId"
            && request.interfaceContracts[0].appliesToPaths == std::vector<std::string>({"qml/models"}),
        "The linked retry must carry the predecessor's interface contracts");
    require(request.task == "The task text" && request.maxParallelism == 3
            && request.preselectedRunId == std::optional<RunId>("reserved-run"),
        "The linked retry keeps the predecessor's task, parallelism and the reserved id");

    predecessor.interfaceContracts.clear();
    require(buildVerificationBootstrapRetryRequest(predecessor, "reserved-run").interfaceContracts.empty(),
        "A predecessor without contracts yields a retry without contracts");
}

} // namespace

int runVerificationBootstrapRetryServiceTests()
{
    testLinkedRetryCreatesNewRunWithPredecessorLink();
    testDuplicateLinkedRetryIsIdempotent();
    testRetryFailsClosedWithoutActionRequiredDecision();
    testRetryWithoutPresetCreatesNeitherReservationNorRun();
    testRetryFailsClosedOnUnknownRunId();
    testRetryNeverMutatesSourceRun();
    testRetryLinkPersistenceFailureCreatesNoRun();
    testInterruptedReservationIsRecoverableWithSameReservedId();
    testConflictingReservedRunIdentityFailsClosed();
    testConcurrentLinkedRetriesCreateExactlyOneRun();
    testLinkedRetryRequestInheritsTheInterfaceContractsAndImmutableInputs();
    testLinkedRetryPreservesTheVerificationRepairPolicy();
    return 0;
}
