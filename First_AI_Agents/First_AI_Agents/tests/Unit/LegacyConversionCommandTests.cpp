// AG-17.7C-1: the Backend commands that convert a legacy roster Run into a Manifest Run -
// Retry, the read-only preview, the explicit user-confirmed conversion and the generic Edit &
// rerun guards - proved through the real RunManager / OrchestrationService / WorkflowRouter with
// in-memory repositories and a mock workflow runner (provider-free). The Manifest workflow
// itself is exercised end to end in ManifestWorkflowRunnerTests.
#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/InMemoryRepositories.hpp"
#include "ai/application/LegacyConversionRecord.hpp"
#include "ai/application/LegacyRosterAdapter.hpp"
#include "ai/application/MockWorkflowRunner.hpp"
#include "ai/application/OrchestrationService.hpp"
#include "ai/application/ProjectService.hpp"
#include "ai/application/catalog/ProviderCatalogService.hpp"
#include "ai/application/WorkflowRouter.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace ai::application;
using namespace std::chrono_literals;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// A run repository that can store everything EXCEPT the conversion audit: it inherits the
// fail-closed default of insertRunWithStagesAndLegacyConversion. Proves that a repository
// without audit support can never produce an unaudited conversion.
class NoAuditRunRepository final : public IRunRepository {
public:
    ai::common::Result<Run> insertRun(Run run) override { return inner_.insertRun(std::move(run)); }
    ai::common::Result<Run> insertRunWithStages(Run run, std::vector<RunStage> stages) override
    {
        return inner_.insertRunWithStages(std::move(run), std::move(stages));
    }
    ai::common::Result<Run> updateRun(Run run) override { return inner_.updateRun(std::move(run)); }
    ai::common::Result<std::optional<Run>> findRun(const RunId& id) const override { return inner_.findRun(id); }
    ai::common::Result<std::vector<Run>> listRuns(const std::optional<ProjectId>& projectId) const override
    {
        return inner_.listRuns(projectId);
    }
    ai::common::Result<std::vector<Run>> listUnfinishedRuns() const override { return inner_.listUnfinishedRuns(); }
    ai::common::Result<void> replaceStages(const RunId& runId, std::vector<RunStage> stages) override
    {
        return inner_.replaceStages(runId, std::move(stages));
    }
    ai::common::Result<RunStage> updateStage(RunStage stage) override { return inner_.updateStage(std::move(stage)); }
    ai::common::Result<std::vector<RunStage>> listStages(const RunId& runId) const override
    {
        return inner_.listStages(runId);
    }

private:
    InMemoryRunRepository inner_;
};

// A run repository that stores everything and the audit like the in-memory one, but whose read of
// ONE chosen Run can be made to fail (a read fault) or to find nothing (a genuinely absent Run).
// The record's own integrity check is not affected: the inner repository reads the Run itself.
class FlakySourceRunRepository final : public IRunRepository {
public:
    enum class Mode { Healthy, FailRead, Missing };
    Mode mode {Mode::Healthy};
    RunId target;

    ai::common::Result<Run> insertRun(Run run) override { return inner_.insertRun(std::move(run)); }
    ai::common::Result<Run> insertRunWithStages(Run run, std::vector<RunStage> stages) override
    {
        return inner_.insertRunWithStages(std::move(run), std::move(stages));
    }
    ai::common::Result<Run> insertRunWithStagesAndLegacyConversion(Run run, std::vector<RunStage> stages,
        LegacyConversionRecord record, const std::optional<RunId>& taskLinkSourceRunId) override
    {
        return inner_.insertRunWithStagesAndLegacyConversion(
            std::move(run), std::move(stages), std::move(record), taskLinkSourceRunId);
    }
    ai::common::Result<std::optional<LegacyConversionRecord>> findLegacyConversion(
        const RunId& runId) const override
    {
        return inner_.findLegacyConversion(runId);
    }
    ai::common::Result<std::vector<LegacyConversionIndexEntry>> listLegacyConversionIndex() const override
    {
        return inner_.listLegacyConversionIndex();
    }
    ai::common::Result<Run> updateRun(Run run) override { return inner_.updateRun(std::move(run)); }
    ai::common::Result<std::optional<Run>> findRun(const RunId& id) const override
    {
        if (id == target && mode == Mode::FailRead) {
            return ai::common::Result<std::optional<Run>>::failure(
                {"persistence.read_failed", "The Run could not be read"});
        }
        if (id == target && mode == Mode::Missing) {
            return ai::common::Result<std::optional<Run>>::ok(std::nullopt);
        }
        return inner_.findRun(id);
    }
    ai::common::Result<std::vector<Run>> listRuns(const std::optional<ProjectId>& projectId) const override
    {
        return inner_.listRuns(projectId);
    }
    ai::common::Result<std::vector<Run>> listUnfinishedRuns() const override { return inner_.listUnfinishedRuns(); }
    ai::common::Result<void> replaceStages(const RunId& runId, std::vector<RunStage> stages) override
    {
        return inner_.replaceStages(runId, std::move(stages));
    }
    ai::common::Result<RunStage> updateStage(RunStage stage) override { return inner_.updateStage(std::move(stage)); }
    ai::common::Result<std::vector<RunStage>> listStages(const RunId& runId) const override
    {
        return inner_.listStages(runId);
    }

private:
    InMemoryRunRepository inner_;
};

struct Harness {
    explicit Harness(bool withAudit = true, bool flakySource = false)
        : runsStorage(makeRuns(withAudit, flakySource, flaky))
        , runs(*runsStorage)
    {
        router.registerRunner(kMockWorkflowId, mock);
        router.registerRunner(kExecutionProfileWorkflowId, mock);
        router.registerRunner(kExecutionManifestWorkflowId, mock);

        const auto created = projectService.createProject({"Conversion", "Conversion commands", workspace()});
        require(created.success(), "test fixture: project");
        project = created.value();
        const auto provider = catalog.createProvider(
            {"Provider", "openai", "https://example.invalid", true, std::nullopt});
        require(provider.success(), "test fixture: provider");
        providerId = provider.value();
        const auto model = catalog.createModel(
            {providerId, "model-a", "Model A", 128000, 8192, 1000000, 3000000, 0, 0, true});
        require(model.success(), "test fixture: model");
        modelId = model.value();
    }

    static std::filesystem::path workspace()
    {
        static const auto path = [] {
            const auto value = std::filesystem::temp_directory_path()
                / ("ai-orchestrator-conversion-commands-"
                    + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(value);
            return value;
        }();
        return path;
    }

    StageModelAssignment member(const std::string& stageType, const std::string& role, bool lead,
        bool workspaceAccess, std::uint32_t position) const
    {
        StageModelAssignment value;
        value.stageType = stageType;
        value.providerId = providerId;
        value.modelId = modelId;
        value.role = role;
        value.leadArchitect = lead;
        value.workspaceAccess = workspaceAccess;
        value.position = position;
        value.timeout = 120000ms;
        return value;
    }

    // The retired read-only snapshot: no lead flag, workspaceAccess false (the column did not exist).
    std::vector<StageModelAssignment> readOnlySnapshot() const
    {
        auto analysis = member("analysis", "Read-only workspace analysis agent", false, false, 0);
        analysis.agentName = "Read-only analysis";
        return {analysis};
    }

    // A lone workspace lead of an execution-profile Run (the migrated shape).
    std::vector<StageModelAssignment> loneWorkspaceLead() const
    {
        return {member("architect", "Workspace analysis agent", true, true, 0)};
    }

    // The real S4 proposal roster: a lead without workspace access (attempts 2, instructions).
    std::vector<StageModelAssignment> proposalRoster() const
    {
        auto lead = member("architect", "Software Architect", true, false, 0);
        lead.maxAttempts = 2;
        lead.instructions = "Plan the work and keep tasks independent.";
        std::vector<StageModelAssignment> roster {lead};
        for (std::uint32_t index = 1; index <= 3; ++index) {
            roster.push_back(member("agent-" + std::to_string(index + 1), "Implementation specialist",
                false, true, index));
        }
        return roster;
    }

    std::vector<StageModelAssignment> textRoster() const
    {
        auto lead = member("architect", "Software Architect", true, false, 0);
        auto worker = member("agent-2", "Writer", false, false, 1);
        return {lead, worker};
    }

    Run seedRun(const std::string& id, const std::string& workflowId,
        std::vector<StageModelAssignment> snapshot, RunStatus status = RunStatus::Completed,
        std::optional<RunId> parent = std::nullopt)
    {
        Run run;
        run.id = id;
        run.projectId = project.id;
        run.parentRunId = std::move(parent);
        run.title = "Historical " + id;
        run.task = "Do the historical task";
        run.workflowId = workflowId;
        run.assignmentSnapshot = std::move(snapshot);
        run.maxParallelism = 2;
        run.workspacePath = workspace();
        run.status = status;
        run.createdAt = std::chrono::system_clock::now();
        if (isTerminal(status)) run.finishedAt = run.createdAt;
        require(runs.insertRun(run).success(), "test fixture: seed run");
        return run;
    }

    Run waitForTerminal(const RunId& id)
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto found = runs.findRun(id);
            require(found.success() && found.value().has_value(), "the Run disappeared");
            if (isTerminal(found.value()->status)) return *found.value();
            std::this_thread::sleep_for(5ms);
        }
        throw std::runtime_error("Run did not reach a terminal state before the timeout");
    }

    Run find(const RunId& id) const
    {
        const auto found = runs.findRun(id);
        require(found.success() && found.value().has_value(), "the Run does not exist");
        return *found.value();
    }

    std::size_t runCount() const
    {
        const auto all = runs.listRuns();
        require(all.success(), "listRuns failed");
        return all.value().size();
    }

    LegacyConversionRecord requireRecord(const RunId& id) const
    {
        const auto record = orchestration.findLegacyConversion(id);
        require(record.success() && record.value().has_value(), "the converted Run has no conversion record");
        return *record.value();
    }

    bool hasNoRecord(const RunId& id) const
    {
        const auto record = orchestration.findLegacyConversion(id);
        return record.success() && !record.value().has_value();
    }

    static std::unique_ptr<IRunRepository> makeRuns(
        bool withAudit, bool flakySource, FlakySourceRunRepository*& flaky)
    {
        if (flakySource) {
            auto repository = std::make_unique<FlakySourceRunRepository>();
            flaky = repository.get();
            return repository;
        }
        return withAudit ? std::unique_ptr<IRunRepository>(std::make_unique<InMemoryRunRepository>())
                         : std::unique_ptr<IRunRepository>(std::make_unique<NoAuditRunRepository>());
    }

    // Declared BEFORE runsStorage: makeRuns() fills it while runsStorage is being initialised.
    FlakySourceRunRepository* flaky {nullptr};
    std::unique_ptr<IRunRepository> runsStorage;
    IRunRepository& runs;
    InMemoryProjectRepository projects;
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    InMemoryExecutionProfileRepository profiles;
    ProviderCatalogService catalog {providers, models};
    ProjectService projectService {projects};
    MockWorkflowRunner mock {1ms, 1};
    WorkflowRouter router {profiles, providers, models};
    RunManager manager {projects, runs, router};
    OrchestrationService orchestration {projects, runs, manager};
    Project project;
    ProviderId providerId;
    ModelId modelId;
};

void requireCode(const ai::common::Result<RunId>& result, const char* code, const char* message)
{
    require(!result.success() && result.error()->code == code, message);
}

bool sameRoster(const std::vector<StageModelAssignment>& left, const std::vector<StageModelAssignment>& right)
{
    return LegacyRosterAdapter::rosterFingerprint(left) == LegacyRosterAdapter::rosterFingerprint(right);
}

// ---- Retry ------------------------------------------------------------------------------

void testRetryOfARetiredReadOnlyRunConvertsAutomaticallyWithASafeRetryRecord()
{
    Harness h;
    const auto source = h.seedRun("read-only-1", kRetiredReadOnlyAnalysisWorkflowId, h.readOnlySnapshot());
    const auto before = h.runCount();

    const auto retried = h.orchestration.retryRun(source.id);
    require(retried.success(), "A retired read-only Run must be retryable");
    require(h.runCount() == before + 1, "The retry creates exactly one Run");
    const auto child = h.find(retried.value());
    require(child.workflowId == kExecutionManifestWorkflowId && !child.executionProfileId.has_value()
            && child.parentRunId == std::optional<RunId>(source.id),
        "The retry must be a per-Run Manifest Run whose parent is the source");
    require(child.assignmentSnapshot.size() == 1 && child.assignmentSnapshot.front().leadArchitect
            && child.assignmentSnapshot.front().workspaceAccess
            && child.assignmentSnapshot.front().maxAttempts == 1
            && child.assignmentSnapshot.front().stageType == "analysis"
            && child.assignmentSnapshot.front().providerId == h.providerId
            && child.maxParallelism == source.maxParallelism && child.task == source.task
            && child.title == source.title,
        "The converted Run must keep the source's provider, model, task and parallelism");

    const auto record = h.requireRecord(child.id);
    require(record.confirmationMode == LegacyConversionConfirmationMode::NotRequiredSafeRetry
            && !record.confirmedAt.has_value() && record.dropped.empty()
            && record.sourceRunId == source.id && record.runId == child.id
            && record.sourceWorkflowId == kRetiredReadOnlyAnalysisWorkflowId
            && record.policyId == "legacy-roster-adapter.v1"
            && record.reasonCode == "legacy_roster.converted_lone_workspace_lead",
        "A safe automatic Retry records the decision and NEVER claims a confirmation");
    require(record.targetRosterFingerprint == LegacyRosterAdapter::rosterFingerprint(child.assignmentSnapshot),
        "The record certifies exactly the roster that was frozen on the new Run");

    // The immutable source is untouched.
    const auto after = h.find(source.id);
    require(after.workflowId == kRetiredReadOnlyAnalysisWorkflowId && after.status == RunStatus::Completed
            && !after.assignmentSnapshot.front().workspaceAccess,
        "A historical Run and its workflow id must never be rewritten");
    static_cast<void>(h.waitForTerminal(child.id));
}

void testRetryOfALoneWorkspaceLeadFromAProfileRunConvertsOnlyWhenNothingIsDropped()
{
    Harness h;
    const auto clean = h.seedRun("lone-clean", kExecutionProfileWorkflowId, h.loneWorkspaceLead());
    const auto safe = h.orchestration.retryRun(clean.id);
    require(safe.success() && h.find(safe.value()).workflowId == kExecutionManifestWorkflowId
            && h.requireRecord(safe.value()).confirmationMode
                == LegacyConversionConfirmationMode::NotRequiredSafeRetry,
        "A lone workspace lead with nothing dropped converts automatically");

    const auto before = h.runCount();
    auto withInstructions = h.loneWorkspaceLead();
    withInstructions.front().instructions = "Be careful with the build.";
    auto withAttempts = h.loneWorkspaceLead();
    withAttempts.front().maxAttempts = 3;
    auto withFallback = h.loneWorkspaceLead();
    withFallback.front().fallbackProviderId = h.providerId;
    withFallback.front().fallbackModelId = h.modelId;
    const std::vector<std::pair<const char*, std::vector<StageModelAssignment>>> needConfirmation {
        {"lone-instructions", withInstructions}, {"lone-attempts", withAttempts}, {"lone-fallback", withFallback}};
    for (const auto& [id, snapshot] : needConfirmation) {
        const auto source = h.seedRun(id, kExecutionProfileWorkflowId, snapshot);
        const auto retried = h.orchestration.retryRun(source.id);
        requireCode(retried, kLegacyConversionConfirmationRequiredCode,
            "A conversion that drops anything must ask for a confirmation instead of running");
    }
    require(h.runCount() == before + 3, "A refused Retry must create no Run (only the three seeded sources exist)");
}

void testRetryOfOtherLegacyShapesStaysExactlyAsBefore()
{
    Harness h;
    // S4: a proposal roster stays on the legacy runner whatever it would drop (the lead has
    // maxAttempts 2 and instructions).
    const auto proposal = h.seedRun("s4", kExecutionProfileWorkflowId, h.proposalRoster());
    const auto retried = h.orchestration.retryRun(proposal.id);
    require(retried.success(), "A proposal roster must stay retryable");
    const auto child = h.find(retried.value());
    require(child.workflowId == kExecutionProfileWorkflowId && sameRoster(child.assignmentSnapshot, proposal.assignmentSnapshot)
            && child.parentRunId == std::optional<RunId>(proposal.id),
        "A proposal roster's exact Retry must stay on the legacy runner with its exact snapshot");
    require(h.hasNoRecord(child.id), "An exact legacy Retry has no conversion record");

    const auto text = h.seedRun("s3", kExecutionProfileWorkflowId, h.textRoster());
    const auto textRetry = h.orchestration.retryRun(text.id);
    require(textRetry.success() && h.find(textRetry.value()).workflowId == kExecutionProfileWorkflowId
            && h.hasNoRecord(textRetry.value()),
        "A text roster must retry exactly as before");

    const auto lone = h.seedRun("s1", kExecutionProfileWorkflowId, {h.member("architect", "Architect", true, false, 0)});
    const auto loneRetry = h.orchestration.retryRun(lone.id);
    require(loneRetry.success() && h.find(loneRetry.value()).workflowId == kExecutionProfileWorkflowId
            && h.hasNoRecord(loneRetry.value()),
        "A lone text architect must retry exactly as before");
    for (const auto& id : {child.id, textRetry.value(), loneRetry.value()}) {
        static_cast<void>(h.waitForTerminal(id));
    }
}

void testRetryOfALoneWorkspaceLeadThatCannotBeConvertedFailsTypedAndCreatesNothing()
{
    Harness h;
    auto lone = h.loneWorkspaceLead();
    lone.front().stageType = std::string(81, 'a');   // a member id a routing record cannot hold
    const auto source = h.seedRun("lone-unfreezable", kExecutionProfileWorkflowId, lone);
    const auto before = h.runCount();
    requireCode(h.orchestration.retryRun(source.id), kLegacyConversionNotConvertibleCode,
        "A lone workspace lead that cannot convert must not be sent to the legacy runner that cannot run it");
    require(h.runCount() == before, "No Run may be created");
}

void testRetryOfAManifestRunIsExactAndCarriesNoNewConversionRecord()
{
    Harness h;
    const auto source = h.seedRun("read-only-2", kRetiredReadOnlyAnalysisWorkflowId, h.readOnlySnapshot());
    const auto converted = h.orchestration.retryRun(source.id);
    require(converted.success(), "test fixture: the conversion");
    const auto convertedRun = h.waitForTerminal(converted.value());

    // A converted Run that ended Interrupted (the application restarted under it) is only ever
    // resumed by a new Run: an exact Manifest Retry of its own snapshot.
    auto interrupted = convertedRun;
    interrupted.status = RunStatus::Interrupted;
    require(h.runs.updateRun(interrupted).success(), "test fixture: interrupt the Run");
    const auto retried = h.orchestration.retryRun(convertedRun.id);
    require(retried.success(), "An interrupted converted Run must be retryable");
    const auto child = h.find(retried.value());
    require(child.workflowId == kExecutionManifestWorkflowId && child.parentRunId == std::optional<RunId>(convertedRun.id)
            && sameRoster(child.assignmentSnapshot, convertedRun.assignmentSnapshot),
        "A Manifest Retry is exact: same workflow, same frozen roster, parent = the converted Run");
    require(h.hasNoRecord(child.id), "An exact Manifest Retry gets no new conversion record");
    // The first record is intact; lineage runs through parentRunId.
    require(h.requireRecord(convertedRun.id).sourceRunId == source.id,
        "The original conversion record must be untouched");
    static_cast<void>(h.waitForTerminal(child.id));
}

// ---- preview ----------------------------------------------------------------------------

void testThePreviewIsReadOnlyBoundToTheSourceRunAndHidesInstructionText()
{
    Harness h;
    const auto proposal = h.seedRun("preview-s4", kExecutionProfileWorkflowId, h.proposalRoster());
    const auto lone = h.seedRun("preview-s2", kExecutionProfileWorkflowId, h.loneWorkspaceLead());
    const auto text = h.seedRun("preview-s3", kExecutionProfileWorkflowId, h.textRoster());
    const auto before = h.runCount();

    const auto preview = h.orchestration.previewLegacyConversion(proposal.id);
    require(preview.success() && preview.value().plan.converts() && !preview.value().safeAutomaticRetry
            && preview.value().legacyRerunAvailable && preview.value().sourceRunId == proposal.id
            && preview.value().confirmationFingerprint
                == LegacyRosterAdapter::confirmationFingerprint(proposal.id, preview.value().plan),
        "A proposal roster previews as an explicit conversion, with its legacy variant still available");
    require(preview.value().defaults.title == proposal.title && preview.value().defaults.task == proposal.task
            && preview.value().defaults.maxParallelism == proposal.maxParallelism,
        "The dialog defaults are the source Run's own editable fields");
    // Only the instruction LENGTH is ever exposed.
    for (const auto& dropped : preview.value().plan.dropped) {
        require(dropped.legacyValue.find("keep tasks independent") == std::string::npos
                && dropped.reason.find("keep tasks independent") == std::string::npos,
            "The preview must never expose instruction text");
    }

    const auto s2 = h.orchestration.previewLegacyConversion(lone.id);
    require(s2.success() && s2.value().plan.converts() && s2.value().safeAutomaticRetry
            && !s2.value().legacyRerunAvailable,
        "A lone workspace lead previews as safe and without a legacy variant");
    const auto s3 = h.orchestration.previewLegacyConversion(text.id);
    require(s3.success() && !s3.value().plan.converts() && s3.value().confirmationFingerprint.empty()
            && s3.value().legacyRerunAvailable,
        "A text roster has nothing to confirm and keeps its legacy variant");
    require(h.runCount() == before, "A preview creates nothing");

    // The same snapshot in two different Runs previews with two different fingerprints.
    const auto twin = h.seedRun("preview-s4-twin", kExecutionProfileWorkflowId, h.proposalRoster());
    require(h.orchestration.previewLegacyConversion(twin.id).value().confirmationFingerprint
            != preview.value().confirmationFingerprint,
        "A confirmation fingerprint is bound to ONE source Run");
    requireCode(ai::common::Result<RunId>::failure(
                    *h.orchestration.previewLegacyConversion("no-such-run").error()),
        "run.not_found", "A preview of an unknown Run is a typed error");
    const auto running = h.seedRun("preview-running", kExecutionProfileWorkflowId, h.proposalRoster(), RunStatus::Running);
    require(!h.orchestration.previewLegacyConversion(running.id).success(),
        "Only a finished Run can be previewed");
}

// ---- the explicit conversion -------------------------------------------------------------

void testAnExplicitConversionCreatesTheRunAndAnExplicitUserRecordAtomically()
{
    Harness h;
    const auto source = h.seedRun("explicit-s4", kExecutionProfileWorkflowId, h.proposalRoster());
    const auto preview = h.orchestration.previewLegacyConversion(source.id);
    require(preview.success() && preview.value().plan.converts(), "test fixture: preview");

    ConvertedRerunOptions options;
    options.title = "Converted: edited title";
    options.task = "An edited task";
    options.workspacePath = Harness::workspace();
    options.maxParallelism = 3;
    options.planningCompletionPolicy = PlanningCompletionPolicy::AllowEvidenceBackedNoChanges;
    options.verificationRepairPolicy = VerificationRepairPolicy::Disabled;

    const auto before = std::chrono::system_clock::now();
    const auto converted = h.orchestration.startConvertedRerun(
        source.id, options, preview.value().confirmationFingerprint);
    const auto after = std::chrono::system_clock::now();
    require(converted.success(), "A confirmed conversion must create the Run");

    const auto run = h.find(converted.value());
    require(run.workflowId == kExecutionManifestWorkflowId && !run.executionProfileId.has_value()
            && run.parentRunId == std::optional<RunId>(source.id)
            && run.title == options.title && run.task == options.task && run.maxParallelism == 3
            && run.planningCompletionPolicy == PlanningCompletionPolicy::AllowEvidenceBackedNoChanges
            && run.verificationRepairPolicy == VerificationRepairPolicy::Disabled,
        "The editable fields come from the user, everything else from the Backend");
    require(sameRoster(run.assignmentSnapshot, preview.value().plan.roster)
            && run.assignmentSnapshot.size() == 4 && run.assignmentSnapshot.front().leadArchitect
            && run.assignmentSnapshot.front().maxAttempts == 1,
        "The roster is exactly the one the Backend built and the user was shown");

    const auto record = h.requireRecord(run.id);
    require(record.confirmationMode == LegacyConversionConfirmationMode::ExplicitUser
            && record.confirmedAt.has_value() && *record.confirmedAt >= before - 1ms && *record.confirmedAt <= after + 1ms
            && record.confirmationFingerprint == preview.value().confirmationFingerprint
            && record.reasonCode == "legacy_roster.converted_proposal_roster"
            && record.sourceRunId == source.id && record.dropped.size() == preview.value().plan.dropped.size(),
        "An explicit conversion records the confirmation, stamped by the Backend clock");
    require(std::any_of(record.dropped.begin(), record.dropped.end(),
                [](const LegacyRosterDroppedSetting& entry) {
                    return entry.setting == "instructions" && entry.legacyValue == "41 characters";
                }),
        "The audit holds the instruction LENGTH, never its text");
    const auto json = legacyConversionDroppedToJson(record.dropped);
    require(json.success() && json.value().find("keep tasks independent") == std::string::npos,
        "No instruction text may reach the audit");

    const auto sourceAfter = h.find(source.id);
    require(sourceAfter.workflowId == kExecutionProfileWorkflowId && sameRoster(sourceAfter.assignmentSnapshot, source.assignmentSnapshot),
        "The historical Run must stay exactly as it was");

    // The history marker: the service lists exactly the converted Run, with its mode, and not the
    // source it came from.
    const auto index = h.orchestration.listLegacyConversionIndex();
    require(index.success() && index.value().size() == 1 && index.value().front().runId == run.id
            && index.value().front().mode == std::optional<LegacyConversionConfirmationMode>(
                   LegacyConversionConfirmationMode::ExplicitUser),
        "The conversion index must list the converted Run with its confirmation mode");
    static_cast<void>(h.waitForTerminal(run.id));
}

void testAConfirmationIsRefusedUnlessItIsExactlyWhatWasShownForThisSourceRun()
{
    Harness h;
    const auto first = h.seedRun("twin-a", kExecutionProfileWorkflowId, h.proposalRoster());
    const auto second = h.seedRun("twin-b", kExecutionProfileWorkflowId, h.proposalRoster());
    const auto lone = h.seedRun("confirm-lone", kExecutionProfileWorkflowId, h.loneWorkspaceLead());
    const auto before = h.runCount();

    ConvertedRerunOptions options;
    options.title = "t";
    options.task = "t";
    const auto shownForFirst = h.orchestration.previewLegacyConversion(first.id).value().confirmationFingerprint;

    // Never confirmed.
    requireCode(h.orchestration.startConvertedRerun(first.id, options, ""),
        kLegacyConversionConfirmationRequiredCode, "A missing confirmation must be refused");
    // The headline requirement: a confirmation is not transferable to another Run with the same snapshot.
    requireCode(h.orchestration.startConvertedRerun(second.id, options, shownForFirst),
        kLegacyConversionConfirmationStaleCode, "A confirmation shown for one source Run must not convert another");
    // Not another decision's fingerprint either.
    requireCode(h.orchestration.startConvertedRerun(first.id, options,
                    h.orchestration.previewLegacyConversion(lone.id).value().confirmationFingerprint),
        kLegacyConversionConfirmationStaleCode, "A fingerprint of another decision must be refused");
    requireCode(h.orchestration.startConvertedRerun(first.id, options, std::string(64, '0')),
        kLegacyConversionConfirmationStaleCode, "An unknown fingerprint must be refused");
    require(h.runCount() == before, "A refused confirmation must create no Run and no record");
    require(h.hasNoRecord(first.id) && h.hasNoRecord(second.id), "Nothing may be recorded for a refused confirmation");
}

void testOnlyAConvertibleFinishedLegacyRunCanBeConvertedExplicitly()
{
    Harness h;
    ConvertedRerunOptions options;
    options.title = "t";
    options.task = "t";
    const auto text = h.seedRun("text-source", kExecutionProfileWorkflowId, h.textRoster());
    requireCode(h.orchestration.startConvertedRerun(text.id, options, std::string(64, 'a')),
        kLegacyConversionStaysLegacyCode, "A text roster stays on the legacy runner");

    const auto manifest = h.seedRun("manifest-source", kExecutionManifestWorkflowId, h.proposalRoster());
    requireCode(h.orchestration.startConvertedRerun(manifest.id, options, std::string(64, 'a')),
        kLegacyConversionNotConvertibleCode, "A Manifest Run is not a legacy roster");

    auto workspaceLead = h.proposalRoster();
    workspaceLead.front().workspaceAccess = true;
    const auto ambiguous = h.seedRun("workspace-lead-source", kExecutionProfileWorkflowId, workspaceLead);
    requireCode(h.orchestration.startConvertedRerun(ambiguous.id, options, std::string(64, 'a')),
        kLegacyConversionNotConvertibleCode, "A roster whose lead has workspace access is never converted");

    const auto running = h.seedRun("running-source", kExecutionProfileWorkflowId, h.proposalRoster(), RunStatus::Running);
    require(!h.orchestration.startConvertedRerun(running.id, options, std::string(64, 'a')).success(),
        "Only a finished Run can be converted");
    require(!h.orchestration.startConvertedRerun("no-such-run", options, std::string(64, 'a')).success(),
        "An unknown Run cannot be converted");
}

// ---- the generic Edit & rerun is not a way around the rules --------------------------------

void testEditAndRerunCannotBypassTheConversion()
{
    Harness h;
    const auto proposal = h.seedRun("bypass-s4", kExecutionProfileWorkflowId, h.proposalRoster());
    const auto lone = h.seedRun("bypass-s2", kExecutionProfileWorkflowId, h.loneWorkspaceLead());
    const auto text = h.seedRun("bypass-s3", kExecutionProfileWorkflowId, h.textRoster());
    const auto before = h.runCount();

    const auto request = [&](const Run& source, const std::string& workflowId,
                             std::vector<StageModelAssignment> roster) {
        StartRunRequest value;
        value.projectId = source.projectId;
        value.title = "Edited";
        value.task = "Edited task";
        value.workflowId = workflowId;
        value.assignmentOverrides = std::move(roster);
        value.maxParallelism = 2;
        return value;
    };
    // Switching a legacy roster Run to the Manifest workflow is a conversion.
    requireCode(h.orchestration.startEditedRerun(proposal.id,
                    request(proposal, kExecutionManifestWorkflowId, h.proposalRoster())),
        kLegacyConversionConfirmationRequiredCode, "A proposal roster cannot be switched to Manifest through Edit & rerun");
    requireCode(h.orchestration.startEditedRerun(lone.id,
                    request(lone, kExecutionManifestWorkflowId, h.loneWorkspaceLead())),
        kLegacyConversionConfirmationRequiredCode, "A lone workspace lead cannot be switched to Manifest through Edit & rerun");
    requireCode(h.orchestration.startEditedRerun(text.id,
                    request(text, kExecutionManifestWorkflowId, h.textRoster())),
        kLegacyConversionStaysLegacyCode, "A text roster cannot be switched to Manifest at all");
    // A lone workspace lead cannot be rerun on the legacy runner either: it is known to fail closed.
    requireCode(h.orchestration.startEditedRerun(lone.id,
                    request(lone, kExecutionProfileWorkflowId, h.loneWorkspaceLead())),
        kLegacyConversionConfirmationRequiredCode, "A lone workspace lead must not be rerun on the legacy runner");
    require(h.runCount() == before, "A refused Edit & rerun must create no Run");

    // What stays allowed: editing a proposal roster as a legacy roster, and editing a lone
    // workspace lead into a real roster.
    const auto legacyEdit = h.orchestration.startEditedRerun(proposal.id,
        request(proposal, kExecutionProfileWorkflowId, h.proposalRoster()));
    require(legacyEdit.success() && h.find(legacyEdit.value()).workflowId == kExecutionProfileWorkflowId
            && h.hasNoRecord(legacyEdit.value()),
        "A legacy Edit & rerun of a proposal roster must keep working, with no conversion record");
    const auto widened = h.orchestration.startEditedRerun(lone.id,
        request(lone, kExecutionProfileWorkflowId, h.proposalRoster()));
    require(widened.success() && h.hasNoRecord(widened.value()),
        "Editing a lone workspace lead into a real roster is a different Run, not the broken shape");

    // A Manifest source is unaffected: its Edit & rerun to Manifest is the ordinary one.
    const auto manifest = h.seedRun("bypass-manifest", kExecutionManifestWorkflowId, h.proposalRoster());
    const auto manifestEdit = h.orchestration.startEditedRerun(manifest.id,
        request(manifest, kExecutionManifestWorkflowId, h.proposalRoster()));
    require(manifestEdit.success() && h.hasNoRecord(manifestEdit.value()),
        "Editing a Manifest Run stays an ordinary Edit & rerun");
    for (const auto& id : {legacyEdit.value(), widened.value(), manifestEdit.value()}) {
        static_cast<void>(h.waitForTerminal(id));
    }
}

// ---- the record's source: a fact, or a fault - never one mistaken for the other ----------------

void testAnUnreadableSourceIsAFaultOfTheViewNeverADeletedSource()
{
    Harness h(/*withAudit=*/true, /*flakySource=*/true);
    require(h.flaky != nullptr, "test fixture: the flaky repository");
    const auto retired = h.seedRun("view-ro", kRetiredReadOnlyAnalysisWorkflowId, h.readOnlySnapshot());
    const auto child = h.orchestration.retryRun(retired.id);
    require(child.success(), "test fixture: a safe Retry creates the converted Run and its record");
    static_cast<void>(h.waitForTerminal(child.value()));

    // Healthy: the record comes with the source it was converted from.
    const auto healthy = h.orchestration.findLegacyConversionView(child.value());
    require(healthy.success() && healthy.value().has_value()
            && healthy.value()->record.runId == child.value()
            && healthy.value()->source.has_value() && healthy.value()->source->id == retired.id,
        "A converted Run's view must carry its record and its source Run");
    // An ordinary Run has no view at all.
    const auto ordinary = h.orchestration.findLegacyConversionView(retired.id);
    require(ordinary.success() && !ordinary.value().has_value(), "A Run that was not converted has no view");

    // The source row cannot be READ although the record is perfectly valid: the whole view is a
    // typed FAULT. It must not degrade into "the source no longer exists" with a good-looking record.
    h.flaky->target = retired.id;
    h.flaky->mode = FlakySourceRunRepository::Mode::FailRead;
    const auto fault = h.orchestration.findLegacyConversionView(child.value());
    require(!fault.success() && fault.error()->code == "persistence.read_failed",
        "A source that cannot be read is a fault of the view, not a deleted source");
    const auto record = h.orchestration.findLegacyConversion(child.value());
    require(record.success() && record.value().has_value(),
        "The record itself is still valid - the fault is only the unreadable source");

    // A source that is genuinely gone (the read succeeds and finds nothing) is a FACT: the view
    // stays, with no source, and the audit outlives it.
    h.flaky->mode = FlakySourceRunRepository::Mode::Missing;
    const auto gone = h.orchestration.findLegacyConversionView(child.value());
    require(gone.success() && gone.value().has_value() && !gone.value()->source.has_value()
            && gone.value()->record.sourceRunId == retired.id,
        "A source that no longer exists leaves the view with its record and no source");
}

// ---- the generic start can never forge lineage ------------------------------------------------

void testTheGenericStartNeverCreatesAParentedRunSoNoOneCanForgeAnUnauditedConversion()
{
    Harness h;
    const auto lone = h.seedRun("forge-s2", kExecutionProfileWorkflowId, h.loneWorkspaceLead());
    const auto proposal = h.seedRun("forge-s4", kExecutionProfileWorkflowId, h.proposalRoster());
    const auto before = h.runCount();

    // The attack: a caller builds the very Manifest-child-of-a-legacy-Run a conversion creates,
    // directly through the generic start, so that no conversion record would ever be written.
    const auto forged = [&](const Run& source, const std::string& workflowId,
                            std::vector<StageModelAssignment> roster) {
        StartRunRequest value;
        value.projectId = source.projectId;
        value.title = "Forged conversion";
        value.task = "Pretend to be a converted Run";
        value.workflowId = workflowId;
        value.parentRunId = source.id;
        value.assignmentOverrides = std::move(roster);
        value.maxParallelism = 2;
        return value;
    };

    const std::vector<StartRunRequest> attempts {
        forged(proposal, kExecutionManifestWorkflowId, h.proposalRoster()),
        forged(lone, kExecutionManifestWorkflowId, h.loneWorkspaceLead()),
        // Any parent is refused, not only a legacy one or a Manifest workflow: lineage belongs to
        // the commands that own it.
        forged(proposal, kExecutionProfileWorkflowId, h.proposalRoster()),
        forged(proposal, kMockWorkflowId, {}),
    };
    for (const auto& attempt : attempts) {
        requireCode(h.orchestration.startRun(attempt), kRunParentNotAllowedCode,
            "The generic start must refuse a Run that names a parent");
        // The layer below is closed as well, for any caller that holds a RunManager.
        requireCode(h.manager.enqueue(attempt), kRunParentNotAllowedCode,
            "RunManager::enqueue must refuse a Run that names a parent");
    }
    // A parent that does not even exist is refused for the same reason, before anything else.
    auto dangling = attempts.front();
    dangling.parentRunId = "no-such-run";
    requireCode(h.orchestration.startRun(dangling), kRunParentNotAllowedCode,
        "A made-up parent is refused by the lineage rule, not by a lookup");
    // The lineage rule is the FIRST thing checked, ahead of the Project lookup: with the guard
    // of startRun missing, this request would fail as project.not_found instead (RunManager's
    // own guard would not even be reached).
    auto unknownProject = attempts.front();
    unknownProject.projectId = "no-such-project";
    requireCode(h.orchestration.startRun(unknownProject), kRunParentNotAllowedCode,
        "startRun must refuse a parent before it looks anything up");

    require(h.runCount() == before, "A refused start must create no Run");
    require(h.hasNoRecord("forge-s2") && h.hasNoRecord("forge-s4"), "No conversion record may appear");
    const auto all = h.runs.listRuns();
    require(all.success()
            && std::none_of(all.value().begin(), all.value().end(),
                [](const Run& run) { return run.title == "Forged conversion"; }),
        "No forged Run may exist in any state");

    // The rule is about the parent only: an ordinary fresh Run still starts, with no lineage.
    StartRunRequest fresh;
    fresh.projectId = lone.projectId;
    fresh.title = "Fresh";
    fresh.task = "An ordinary Run";
    fresh.workflowId = kMockWorkflowId;
    const auto started = h.orchestration.startRun(fresh);
    require(started.success() && !h.find(started.value()).parentRunId.has_value()
            && h.hasNoRecord(started.value()),
        "An ordinary fresh Run must keep starting, without a parent or a conversion record");
    static_cast<void>(h.waitForTerminal(started.value()));

    // The commands that DO own lineage keep working: a safe Retry still creates its audited child.
    const auto retired = h.seedRun("forge-ro", kRetiredReadOnlyAnalysisWorkflowId, h.readOnlySnapshot());
    const auto retry = h.orchestration.retryRun(retired.id);
    require(retry.success() && h.find(retry.value()).parentRunId == std::optional<RunId>(retired.id),
        "Retry must still create its parented child");
    require(h.requireRecord(retry.value()).sourceRunId == retired.id,
        "The Retry child must still carry its conversion record");
    static_cast<void>(h.waitForTerminal(retry.value()));
}

// ---- atomicity: a repository without the audit never produces an unaudited conversion -------

void testARepositoryThatCannotStoreTheAuditRefusesTheConversionAndCreatesNoRun()
{
    Harness h(/*withAudit=*/false);
    const auto readOnly = h.seedRun("noaudit-ro", kRetiredReadOnlyAnalysisWorkflowId, h.readOnlySnapshot());
    const auto proposal = h.seedRun("noaudit-s4", kExecutionProfileWorkflowId, h.proposalRoster());
    const auto before = h.runCount();

    requireCode(h.orchestration.retryRun(readOnly.id), kLegacyConversionUnsupportedCode,
        "A safe Retry must be refused when the audit cannot be stored");
    ConvertedRerunOptions options;
    options.title = "t";
    options.task = "t";
    const auto fingerprint = h.orchestration.previewLegacyConversion(proposal.id).value().confirmationFingerprint;
    requireCode(h.orchestration.startConvertedRerun(proposal.id, options, fingerprint),
        kLegacyConversionUnsupportedCode, "An explicit conversion must be refused when the audit cannot be stored");
    require(h.runCount() == before, "No unaudited Run may exist");
    const auto noIndex = h.orchestration.listLegacyConversionIndex();
    require(noIndex.success() && noIndex.value().empty(),
        "A repository without audit support has no converted Runs, so its index is empty");

    // The ordinary paths are unaffected by the missing audit support.
    const auto exact = h.orchestration.retryRun(proposal.id);
    require(exact.success() && h.hasNoRecord(exact.value()), "An exact legacy Retry needs no audit");
    static_cast<void>(h.waitForTerminal(exact.value()));
}

} // namespace

int runLegacyConversionCommandTests()
{
    try {
        testRetryOfARetiredReadOnlyRunConvertsAutomaticallyWithASafeRetryRecord();
        testRetryOfALoneWorkspaceLeadFromAProfileRunConvertsOnlyWhenNothingIsDropped();
        testRetryOfOtherLegacyShapesStaysExactlyAsBefore();
        testRetryOfALoneWorkspaceLeadThatCannotBeConvertedFailsTypedAndCreatesNothing();
        testRetryOfAManifestRunIsExactAndCarriesNoNewConversionRecord();
        testThePreviewIsReadOnlyBoundToTheSourceRunAndHidesInstructionText();
        testAnExplicitConversionCreatesTheRunAndAnExplicitUserRecordAtomically();
        testAConfirmationIsRefusedUnlessItIsExactlyWhatWasShownForThisSourceRun();
        testOnlyAConvertibleFinishedLegacyRunCanBeConvertedExplicitly();
        testEditAndRerunCannotBypassTheConversion();
        testAnUnreadableSourceIsAFaultOfTheViewNeverADeletedSource();
        testTheGenericStartNeverCreatesAParentedRunSoNoOneCanForgeAnUnauditedConversion();
        testARepositoryThatCannotStoreTheAuditRefusesTheConversionAndCreatesNoRun();
        std::error_code ignored;
        std::filesystem::remove_all(Harness::workspace(), ignored);
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "LegacyConversionCommand tests failed: " << exception.what() << '\n';
        return 1;
    }
}
