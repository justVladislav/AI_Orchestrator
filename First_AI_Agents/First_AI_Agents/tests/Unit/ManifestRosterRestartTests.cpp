// AG-17.7D-1: a multi-member Manifest roster Run through a RESTART and an exact Retry, on a real
// SQLite file, with the real ManifestWorkflowRunner and scripted provider sessions - no network, no
// provider call, no paid Run, no schema change.
//
// Crash states are captured, not simulated by killing threads: from inside a scripted provider
// session (the runner's own worker thread, between two repository calls) a SEPARATE read-only
// connection runs `VACUUM INTO`, which writes a standalone copy of exactly the COMMITTED state - what
// a crash at that instant leaves on disk. "Process B" is then a fresh composition on that copy
// (new connection, repositories, RunManager and runner) that runs the production startup recovery
// sequence (ai::backend::runStartupRecovery - the very function BackendRuntime calls).
#include "ScriptedManifestSupport.hpp"

#include "ai/application/artifacts/ArtifactService.hpp"
#include "ai/application/proposals/ChangeProposalService.hpp"
#include "ai/application/execution/ExecutionManifestReadModelService.hpp"
#include "ai/application/analytics/ExecutionAnalyticsService.hpp"
#include "ai/application/execution/LegacyRosterAdapter.hpp"
#include "ai/application/execution/ManifestInterruptedTaskReconciler.hpp"
#include "ai/application/execution/ManifestMemberUsage.hpp"
#include "ai/application/execution/ManifestWorkLedgerService.hpp"
#include "ai/application/agents/ModelExecutionService.hpp"
#include "ai/application/execution/OrchestrationService.hpp"
#include "ai/application/projects/ProjectService.hpp"
#include "ai/application/catalog/ProviderCatalogService.hpp"
#include "ai/application/execution/RunManager.hpp"
#include "ai/application/workspace/ToolRegistry.hpp"
#include "ai/application/verification/VerificationEnvironmentDiscoveryService.hpp"
#include "ai/application/execution/WorkflowRouter.hpp"
#include "ai/backend/StartupRecovery.hpp"
#include "ai/infrastructure/FileArtifactStore.hpp"
#include "ai/infrastructure/FileChangeProposalStore.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

using namespace ai::tests::scripted;
using namespace ai::infrastructure;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

// ---- the catalog as a RUN sees it ---------------------------------------------------------------

// The catalog API cannot make a provider or model vanish under a Run (deleting a model that any
// Run's assignment snapshot references is refused with model.in_use by a RESTRICT constraint, and
// there is no provider delete at all). The runner still defends against a catalog entry that is
// gone after the plan was resolved (manifest.route_resolution_failed), and this is the honest way
// to reach that defence: the runner and the router read the catalog through these views, which
// forward everything to the real SQLite repositories but report the hidden ids as absent.
class HidingProviderRepository final : public IProviderRepository {
public:
    explicit HidingProviderRepository(IProviderRepository& inner) : inner_(inner) {}
    void hide(const ProviderId& id) { hidden_.insert(id); }

    common::Result<Provider> insert(Provider provider) override { return inner_.insert(std::move(provider)); }
    common::Result<Provider> update(Provider provider) override { return inner_.update(std::move(provider)); }
    common::Result<std::optional<Provider>> findById(const ProviderId& id) const override
    {
        if (hidden_.contains(id)) return common::Result<std::optional<Provider>>::ok(std::nullopt);
        return inner_.findById(id);
    }
    common::Result<std::vector<Provider>> listAll() const override { return inner_.listAll(); }

private:
    IProviderRepository& inner_;
    std::set<ProviderId> hidden_;
};

class HidingModelRepository final : public IModelRepository {
public:
    explicit HidingModelRepository(IModelRepository& inner) : inner_(inner) {}
    void hide(const ModelId& id) { hidden_.insert(id); }

    common::Result<Model> insert(Model model) override { return inner_.insert(std::move(model)); }
    common::Result<Model> update(Model model) override { return inner_.update(std::move(model)); }
    common::Result<std::optional<Model>> findById(const ModelId& id) const override
    {
        if (hidden_.contains(id)) return common::Result<std::optional<Model>>::ok(std::nullopt);
        return inner_.findById(id);
    }
    common::Result<std::vector<Model>> listAll(const std::optional<ProviderId>& providerId) const override
    {
        return inner_.listAll(providerId);
    }
    common::Result<void> setDefaultModel(const ProviderId& providerId, const ModelId& modelId) override
    {
        return inner_.setDefaultModel(providerId, modelId);
    }
    common::Result<void> remove(const ModelId& id) override { return inner_.remove(id); }

private:
    IModelRepository& inner_;
    std::set<ModelId> hidden_;
};

// ---- one "process": a full composition over one SQLite file ------------------------------------

struct Process {
    fs::path file;
    std::shared_ptr<SqliteDatabase> database;
    SqliteProjectRepository projects;
    SqliteProviderRepository providers;
    SqliteModelRepository models;
    SqliteExecutionProfileRepository profiles;
    SqliteRunRepository runs;
    SqliteExecutionManifestRepository manifestRepository;
    SqliteManifestTaskExecutionRepository taskExecutions;
    SqliteManifestWorkLedgerRepository workLedger;
    SqliteAgentToolSessionRepository toolSessions;
    SqliteAgentToolAuditRepository toolAudits;
    SqliteModelExecutionRepository modelExecutionRepository;
    SqliteChangeProposalRepository changeRepo;
    SqliteArtifactRepository artifactRepository;
    SqliteVerificationEnvironmentRepository verificationEnvironmentRepository;
    SqliteVerificationBootstrapRepository verificationBootstrapRepository;
    std::unique_ptr<FileArtifactStore> artifactStore;
    std::unique_ptr<FileChangeProposalStore> changeStore;

    // What the runner and the router read; see HidingProviderRepository.
    HidingProviderRepository runtimeProviders {providers};
    HidingModelRepository runtimeModels {models};

    ProviderCatalogService catalog {providers, models};
    ProjectService projectService {projects};
    ModelExecutionService modelExecutions {modelExecutionRepository};
    ManifestInterruptedTaskReconciler interruptedTasks {
        runs, manifestRepository, taskExecutions, modelExecutionRepository, workLedger};
    AlwaysSucceedExecutor readOnlyExecutor;
    ScriptedProviderAgentSessionFactory sessionFactory;
    FakeProviderClientFactory clientFactory;
    FakeVerificationCommandRunner verificationCommandRunner;
    ArtifactService artifactService;
    ExecutionManifestService manifests {manifestRepository, runs};
    AgentToolExecutionService toolExecution;
    ChangeProposalService changeProposals;
    ManifestWorkLedgerService workLedgerService;
    VerificationEnvironmentDiscoveryService discovery {verificationEnvironmentRepository};
    ManifestWorkflowRunner manifestRunner;
    WorkflowRouter router {profiles, runtimeProviders, runtimeModels};
    RunManager manager;
    OrchestrationService orchestration {projects, runs, manager};

    static std::shared_ptr<SqliteDatabase> open(const fs::path& databasePath)
    {
        const auto opened = SqliteDatabase::open(databasePath);
        require(opened.success(), "Could not open the SQLite database");
        return opened.value();
    }

    static std::unique_ptr<FileArtifactStore> openArtifacts(const fs::path& root)
    {
        auto opened = FileArtifactStore::open(root);
        require(opened.success(), "Could not open the artifact store");
        return std::move(opened.value());
    }

    static std::unique_ptr<FileChangeProposalStore> openProposals(const fs::path& root)
    {
        auto opened = FileChangeProposalStore::open(root);
        require(opened.success(), "Could not open the change-proposal store");
        return std::move(opened.value());
    }

    Process(const fs::path& databasePath, const fs::path& stateRoot)
        : file(databasePath)
        , database(open(databasePath))
        , projects(database), providers(database), models(database), profiles(database)
        , runs(database), manifestRepository(database), taskExecutions(database)
        , workLedger(database), toolSessions(database), toolAudits(database)
        , modelExecutionRepository(database), changeRepo(database), artifactRepository(database)
        , verificationEnvironmentRepository(database), verificationBootstrapRepository(database)
        , artifactStore(openArtifacts(stateRoot / "artifacts"))
        , changeStore(openProposals(stateRoot / "proposals"))
        , artifactService(artifactRepository, *artifactStore)
        , toolExecution(
              [] { auto r = ToolRegistry::createDefaultReadOnly(); require(r.success(), "registry"); return std::move(r.value()); }(),
              readOnlyExecutor, toolSessions, toolAudits, true)
        , changeProposals(changeRepo, *changeStore, nullptr)
        , workLedgerService(workLedger, runs, manifestRepository, taskExecutions, changeRepo, &artifactService)
        , manifestRunner(
              sessionFactory, toolExecution, changeProposals, manifests, clientFactory, runtimeProviders, runtimeModels,
              &artifactService, &modelExecutions, &taskExecutions, &verificationCommandRunner,
              &workLedgerService, /*runWorkspaceScopeService=*/nullptr, &discovery,
              &verificationBootstrapRepository)
        , manager(projects, runs, router, /*projectMemoryService=*/nullptr, /*pathClaimRegistry=*/nullptr,
              &modelExecutions, &workLedgerService)
    {
        router.registerRunner(kExecutionManifestWorkflowId, manifestRunner);
    }

    // The production startup recovery sequence - the same function BackendRuntime calls. The
    // narrower composition has no comparison / continuation / manual-handoff services, so those
    // passes are (explicitly) not run here.
    ai::backend::StartupRecoveryOutcome recover()
    {
        return ai::backend::runStartupRecovery(ai::backend::StartupRecoveryServices {
            orchestration, modelExecutions, interruptedTasks, toolSessions, toolAudits, workLedgerService});
    }

    Provider makeProvider(const char* name, const char* kind = "openai")
    {
        const auto created = catalog.createProvider({name, kind, "https://example.invalid", true, std::nullopt});
        require(created.success(), "provider");
        return *catalog.findProvider(created.value()).value();
    }

    Model makeModel(const ProviderId& providerId, const char* apiModelId)
    {
        const auto created = catalog.createModel(
            {providerId, apiModelId, apiModelId, 128000, 8192, 1'000'000, 3'000'000,
                1'250'000, 100'000, true, {}, 2'000'000});
        require(created.success(), "model");
        return *catalog.findModel(created.value()).value();
    }
};

Run waitForTerminal(IRunRepository& repository, const RunId& runId, std::chrono::milliseconds timeout = 15s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto found = repository.findRun(runId);
        require(found.success() && found.value().has_value(), "Run not found while waiting");
        if (isTerminal(found.value()->status)) return *found.value();
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("Run did not reach a terminal state before the timeout");
}

// ---- crash-state capture -----------------------------------------------------------------------

// A standalone copy of the COMMITTED state of `source`, taken through a SEPARATE read-only
// connection: nothing the running process holds is touched, and nothing uncommitted is copied.
void captureCommittedState(const fs::path& source, const fs::path& target)
{
    std::error_code ignored;
    fs::remove(target, ignored);
    sqlite3* handle = nullptr;
    const std::string sourcePath = source.generic_string();
    require(sqlite3_open_v2(sourcePath.c_str(), &handle, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
        "Could not open a separate read-only connection for the crash-state capture");
    const std::string sql = "VACUUM INTO '" + target.generic_string() + "';";
    char* message = nullptr;
    const int result = sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, &message);
    const std::string failure = message != nullptr ? std::string(message) : std::string {};
    sqlite3_free(message);
    sqlite3_close_v2(handle);
    require(result == SQLITE_OK, ("VACUUM INTO failed: " + failure).c_str());
}

std::uint64_t fileFingerprint(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    require(stream.is_open(), "Could not read a file to fingerprint it");
    std::uint64_t hash = 1469598103934665603ULL;
    char buffer[4096];
    while (stream.read(buffer, sizeof(buffer)) || stream.gcount() > 0) {
        for (std::streamsize index = 0; index < stream.gcount(); ++index) {
            hash ^= static_cast<unsigned char>(buffer[index]);
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

// ---- the scenario ------------------------------------------------------------------------------

// A roster Run: the lead plans, member "implementation" owns TWO tasks and member "documentation"
// one. maxParallelism 1 serialises them in manifest order (impl-1, impl-2, doc).
struct Scenario {
    Provider leadProvider, implProvider, docProvider;
    Model leadModel, implModel, docModel;
    std::vector<StageModelAssignment> roster;
    Project project;
    StartRunRequest request;
};

void giveWorkspaceAnExistingCmakePreset(const fs::path& workspace, FakeVerificationCommandRunner& runner)
{
    {
        std::ofstream presets(workspace / "CMakePresets.json");
        presets << R"({"version":3,"configurePresets":[{"name":"local-qt-debug",)"
                   R"("binaryDir":"${sourceDir}/build/local-qt-debug"}],)"
                   R"("buildPresets":[{"name":"local-qt-debug",)"
                   R"("configurePreset":"local-qt-debug","targets":["all_qmllint"]}]})";
    }
    ManifestVerificationCommandOutcome configureOutcome;
    configureOutcome.started = true;
    configureOutcome.exitCode = 0;
    configureOutcome.boundedOutput =
        "Available configure presets:\n\n  \"local-qt-debug\"   - Desktop Qt 6.11.1 Debug\n";
    ManifestVerificationCommandOutcome buildOutcome;
    buildOutcome.started = true;
    buildOutcome.exitCode = 0;
    buildOutcome.boundedOutput =
        "Available build presets:\n\n  \"local-qt-debug\"   - Desktop Qt 6.11.1 Debug\n";
    // Discovery consumes the first pair; the post-freeze environment preflight re-lists them.
    runner.outcomes = {configureOutcome, buildOutcome, configureOutcome, buildOutcome};
}

Scenario makeScenario(Process& process, const fs::path& workspace)
{
    Scenario scenario;
    scenario.leadProvider = process.makeProvider("Lead Provider", "openai");
    scenario.implProvider = process.makeProvider("Impl Provider", "anthropic");
    scenario.docProvider = process.makeProvider("Docs Provider", "openai_compatible");
    scenario.leadModel = process.makeModel(scenario.leadProvider.id, "lead-model");
    scenario.implModel = process.makeModel(scenario.implProvider.id, "impl-model");
    scenario.docModel = process.makeModel(scenario.docProvider.id, "docs-model");

    StageModelAssignment lead {"architect", scenario.leadProvider.id, scenario.leadModel.id, 5000ms};
    lead.role = "Lead Architect";
    lead.instructions = "Plan and delegate only.";
    lead.leadArchitect = true;
    lead.position = 0;
    StageModelAssignment impl {"implementation", scenario.implProvider.id, scenario.implModel.id, 5000ms};
    impl.role = "Implementer";
    impl.instructions = "Own implementation tasks.";
    impl.workspaceAccess = true;
    impl.position = 1;
    StageModelAssignment docs {"documentation", scenario.docProvider.id, scenario.docModel.id, 5000ms};
    docs.role = "Documenter";
    docs.instructions = "Own documentation tasks.";
    docs.workspaceAccess = true;
    docs.position = 2;
    scenario.roster = {lead, impl, docs};

    fs::create_directories(workspace);
    const auto created = process.projectService.createProject(
        {"Roster restart project", "AG-17.7D-1", workspace});
    require(created.success(), "project");
    scenario.project = created.value();

    scenario.request.projectId = scenario.project.id;
    scenario.request.title = "Roster restart Run";
    scenario.request.task = "Create three files across two members";
    scenario.request.workflowId = kExecutionManifestWorkflowId;
    scenario.request.assignmentOverrides = scenario.roster;
    scenario.request.maxParallelism = 1;
    return scenario;
}

// Prepares the scripted sessions: one Planner, two Implementer tasks, one Documenter task.
struct PreparedSessions {
    ScriptedProviderAgentSession* planner {nullptr};
    ScriptedProviderAgentSession* implFirst {nullptr};
    ScriptedProviderAgentSession* implSecond {nullptr};
    ScriptedProviderAgentSession* docs {nullptr};
};

PreparedSessions prepareSessions(Process& process, const Scenario& scenario)
{
    PreparedSessions prepared;
    const auto make = [&](const ProviderId& provider, ProviderAgentTurnResponse response) {
        auto session = std::make_unique<ScriptedProviderAgentSession>();
        auto* raw = session.get();
        session->responses.push(common::Result<ProviderAgentTurnResponse>::ok(std::move(response)));
        process.sessionFactory.prepareFor(provider, std::move(session));
        return raw;
    };
    const std::vector<ScriptedTask> tasks {
        {"task-impl-1", "notes/impl-one.md", "implementation"},
        {"task-impl-2", "notes/impl-two.md", "implementation"},
        {"task-docs", "docs/guide.md", "documentation"},
    };
    prepared.planner = make(scenario.leadProvider.id,
        toolCallResponse("submit-roster", kSubmitExecutionManifestDraftToolName, rosterDraft(tasks, 1)));
    auto firstImplementation = stopResponse(proposalSubmission("notes/impl-one.md"));
    firstImplementation.usage.cacheCreationInputTokens = 4;
    firstImplementation.usage.cacheReadInputTokens = 2;
    firstImplementation.usage.cacheCreationDuration = {true, 1, 3};
    auto secondImplementation = stopResponse(proposalSubmission("notes/impl-two.md"));
    secondImplementation.usage.cacheCreationInputTokens = 4;
    secondImplementation.usage.cacheReadInputTokens = 2;
    secondImplementation.usage.cacheCreationDuration = {true, 1, 3};
    prepared.implFirst = make(scenario.implProvider.id, std::move(firstImplementation));
    prepared.implSecond = make(scenario.implProvider.id, std::move(secondImplementation));
    prepared.docs = make(scenario.docProvider.id, stopResponse(proposalSubmission("docs/guide.md")));
    return prepared;
}

// ---- a deterministic text of every durable row of one Run --------------------------------------

std::string millis(const std::chrono::system_clock::time_point& value)
{
    return std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count());
}

std::string millis(const std::optional<std::chrono::system_clock::time_point>& value)
{
    return value.has_value() ? millis(*value) : std::string("-");
}

// Everything durable that belongs to `runId`: the Run row, its stages, the frozen Manifest and its
// routing, the task rows, the model executions, the Work Ledger entry and the change sets. Two
// equal strings mean two identical durable states - the proof a Retry did not touch its parent.
std::string describe(Process& process, const RunId& runId)
{
    std::ostringstream out;
    const auto run = process.runs.findRun(runId);
    require(run.success() && run.value().has_value(), "Run not found");
    const Run& value = *run.value();
    out << "run " << value.id << " parent=" << value.parentRunId.value_or("-")
        << " status=" << toString(value.status) << " revision=" << value.revision
        << " finished=" << millis(value.finishedAt) << " error=" << value.errorMessage
        << " usage=" << value.providerBudgetUsage.turnsStarted << "/" << value.providerBudgetUsage.turnsCompleted
        << "/" << value.providerBudgetUsage.inputTokens << "/" << value.providerBudgetUsage.outputTokens
        << "/" << value.providerBudgetUsage.toolCalls
        << " cache=" << value.providerBudgetUsage.cacheCreationInputTokens
        << "/" << value.providerBudgetUsage.cacheReadInputTokens
        << "/" << value.providerBudgetUsage.cacheCreationDuration.known
        << "/" << value.providerBudgetUsage.cacheCreationDuration.fiveMinuteTokens
        << "/" << value.providerBudgetUsage.cacheCreationDuration.oneHourTokens << "\n";
    const auto stages = process.runs.listStages(runId);
    require(stages.success(), "stages");
    for (const auto& stage : stages.value()) {
        out << "stage " << stage.id << " " << stage.type << " " << toString(stage.status)
            << " finished=" << millis(stage.finishedAt) << " error=" << stage.errorMessage << "\n";
    }
    const auto manifest = process.manifests.findForRun(runId);
    require(manifest.success(), "manifest");
    if (manifest.value().has_value()) {
        out << "manifest tasks=" << manifest.value()->tasks.size()
            << " routing=" << (manifest.value()->routing.has_value() ? manifest.value()->routing->fingerprint : "-")
            << "\n";
        for (const auto& task : manifest.value()->tasks) {
            out << "task " << task.id << " assignee=" << task.assigneeMemberId.value_or("-") << "\n";
        }
    } else {
        out << "manifest none\n";
    }
    const auto taskRows = process.taskExecutions.listForRun(runId);
    require(taskRows.success(), "task executions");
    for (const auto& row : taskRows.value()) {
        out << "taskrow " << row.taskId << " " << toString(row.state.status) << " attempts=" << row.state.attempts
            << " error=" << row.state.errorCode << " changeset=" << row.state.changeSetId.value_or("-") << "\n";
    }
    const auto executions = process.modelExecutions.listExecutions(ModelExecutionSource::Run, runId);
    require(executions.success(), "model executions");
    for (const auto& execution : executions.value()) {
        out << "exec " << execution.id << " " << execution.role << " position=" << execution.position
            << " attempt=" << execution.attempt << " " << std::string(toString(execution.status))
            << " error=" << execution.errorCode << " in=" << execution.inputTokens << " out=" << execution.outputTokens
            << " cache=" << execution.cacheCreationInputTokens << "/" << execution.cacheReadInputTokens
            << "/" << execution.cacheCreationDuration.known
            << "/" << execution.cacheCreationDuration.fiveMinuteTokens
            << "/" << execution.cacheCreationDuration.oneHourTokens
            << " cost=" << execution.costMicros << "/" << execution.costPriceKnown
            << " finished=" << millis(execution.finishedAt) << "\n";
    }
    const auto ledger = process.workLedger.findEntry(runId);
    require(ledger.success(), "ledger");
    if (ledger.value().has_value()) {
        out << "ledger " << toString(ledger.value()->terminalStatus) << " tasks=" << ledger.value()->taskDispositions.size()
            << " proposals=" << ledger.value()->proposalIds.size() << "\n";
    } else {
        out << "ledger none\n";
    }
    const auto changeSets = process.changeRepo.listForRun(runId);
    require(changeSets.success(), "change sets");
    for (const auto& set : changeSets.value()) {
        out << "changeset " << set.changeSetId << " " << toString(set.state) << "\n";
    }
    return out.str();
}

// What a restart and a Retry must leave unchanged about a Run: its input, its frozen roster, its
// budgets, its policies and (once frozen) the Manifest's routing identity and task assignments.
std::string identityOf(Process& process, const RunId& runId, bool includeManifest)
{
    std::ostringstream out;
    const auto run = process.runs.findRun(runId);
    require(run.success() && run.value().has_value(), "Run not found");
    const Run& value = *run.value();
    out << "workflow=" << value.workflowId << " profile=" << value.executionProfileId.value_or("-")
        << " title=" << value.title << " task=" << value.task
        << " roster=" << ai::application::LegacyRosterAdapter::rosterFingerprint(value.assignmentSnapshot)
        << " members=" << value.assignmentSnapshot.size() << " parallel=" << value.maxParallelism
        << " workspace=" << (value.workspacePath.has_value() ? value.workspacePath->generic_string() : "-")
        << " planning=" << static_cast<int>(value.planningCompletionPolicy)
        << " repair=" << static_cast<int>(value.verificationRepairPolicy)
        << " budget=" << value.providerBudget.maxInputTokens << "/" << value.providerBudget.maxOutputTokens << "/"
        << value.providerBudget.maxTurns << "/" << value.providerBudget.maxToolCalls << "/"
        << value.providerBudget.maxToolResultBytes << "/" << value.providerBudget.maxWallTimeMs << "/"
        << value.providerBudget.maxRequestTimeMs << "\n";
    if (includeManifest) {
        const auto manifest = process.manifests.findForRun(runId);
        require(manifest.success() && manifest.value().has_value() && manifest.value()->routing.has_value(),
            "A frozen Manifest with its routing record was expected");
        out << "routing=" << manifest.value()->routing->fingerprint
            << " mode=" << toString(manifest.value()->routing->mode)
            << " members=" << manifest.value()->routing->members.size() << "\n";
        for (const auto& member : manifest.value()->routing->members) {
            out << "member " << member.memberId << " " << member.providerId << " " << member.modelId
                << " position=" << member.position << " lead=" << member.leadArchitect << "\n";
        }
        for (const auto& task : manifest.value()->tasks) {
            out << "task " << task.id << " assignee=" << task.assigneeMemberId.value_or("-") << "\n";
        }
    }
    return out.str();
}

void dump(const std::string& label, Process& process, const RunId& runId)
{
    std::istringstream lines(describe(process, runId));
    for (std::string line; std::getline(lines, line);) std::cout << label << " " << line << "\n";
}

// ---- the captured crash states -------------------------------------------------------------------

struct Captured {
    TempDir root;
    fs::path workspace;
    fs::path state;
    std::map<std::string, fs::path> crashStates;
    fs::path completedState;
    Scenario scenario;
    RunId runId;
    // The Run's own durable input and the frozen Manifest identity, read from process A after its
    // Run completed normally: what every later reading must still show.
    std::string referenceRunIdentity;
    std::string referenceManifestIdentity;
    std::vector<ModelExecution> referenceExecutions;
    ExecutionManifest referenceManifest;
    ProviderExecutionBudgetUsage referenceUsage;
    mutable int restarts {0};
};

constexpr const char* kBeforeFreeze = "P0-before-freeze";
constexpr const char* kAfterFreeze = "P1-after-freeze-before-dispatch";
constexpr const char* kDuringExecution = "P2-during-execution";

std::unique_ptr<Captured> captureCrashStates()
{
    auto captured = std::make_unique<Captured>();
    captured->workspace = captured->root.path / "workspace";
    captured->state = captured->root.path / "state";
    const fs::path databaseA = captured->root.path / "process-a.sqlite";
    std::map<std::string, std::uint64_t> fingerprints;
    {
        Process a(databaseA, captured->state);
        captured->scenario = makeScenario(a, captured->workspace);
        giveWorkspaceAnExistingCmakePreset(captured->workspace, a.verificationCommandRunner);
        const auto prepared = prepareSessions(a, captured->scenario);
        const auto capture = [&](const std::string& name) {
            const fs::path target = captured->root.path / ("crash-" + name + ".sqlite");
            captureCommittedState(databaseA, target);
            captured->crashStates[name] = target;
        };
        // P0: inside the Planner's first response - the Planner execution is in flight, nothing frozen.
        prepared.planner->beforeNextResponse = [&] { capture(kBeforeFreeze); };
        // P1: the first WORKER session is about to be handed out - the Manifest is frozen.
        bool firstWorker = true;
        a.sessionFactory.onCreateSession = [&](const ProviderId& provider) {
            if (provider == captured->scenario.leadProvider.id || !firstWorker) return;
            firstWorker = false;
            capture(kAfterFreeze);
        };
        // P2: inside the SECOND implementation task - the first is Completed, this one in flight.
        prepared.implSecond->beforeNextResponse = [&] { capture(kDuringExecution); };

        const auto started = a.orchestration.startRun(captured->scenario.request);
        require(started.success(), "Could not start the roster Run");
        captured->runId = started.value();
        const auto finished = waitForTerminal(a.runs, captured->runId);
        require(finished.status == RunStatus::Completed, "Process A must complete its own Run normally");
        require(captured->crashStates.size() == 3, "Not every crash state was captured");

        captured->referenceUsage = finished.providerBudgetUsage;
        captured->referenceRunIdentity = identityOf(a, captured->runId, /*includeManifest=*/false);
        captured->referenceManifestIdentity = identityOf(a, captured->runId, /*includeManifest=*/true);
        const auto manifest = a.manifests.findForRun(captured->runId);
        require(manifest.success() && manifest.value().has_value(), "The reference Manifest is missing");
        captured->referenceManifest = *manifest.value();
        const auto executions = a.modelExecutions.listExecutions(ModelExecutionSource::Run, captured->runId);
        require(executions.success(), "reference executions");
        captured->referenceExecutions = executions.value();
        captured->completedState = captured->root.path / "completed.sqlite";
        captureCommittedState(databaseA, captured->completedState);
        for (const auto& [name, path] : captured->crashStates) fingerprints[name] = fileFingerprint(path);
        fingerprints["completed"] = fileFingerprint(captured->completedState);
    }
    // Process A is gone (its connection closed, its worker joined): the captured states must not have
    // changed - they were taken from a SEPARATE connection and are standalone files.
    for (const auto& [name, path] : captured->crashStates) {
        require(fileFingerprint(path) == fingerprints[name], "Destroying process A changed a crash-state file");
    }
    require(fileFingerprint(captured->completedState) == fingerprints["completed"],
        "Destroying process A changed the completed-state file");
    return captured;
}

// "Process B": a fresh composition on a COPY of one crash state (the captured file itself is never
// opened, so every test starts from the same bytes).
std::unique_ptr<Process> restartFrom(const Captured& captured, const std::string& name)
{
    const fs::path working = captured.root.path / ("restart-" + std::to_string(++captured.restarts) + ".sqlite");
    fs::copy_file(captured.crashStates.at(name), working, fs::copy_options::overwrite_existing);
    return std::make_unique<Process>(working, captured.state);
}

// ---- row readers and assertions -----------------------------------------------------------------

void expect(bool condition, const std::string& what)
{
    if (!condition) throw std::runtime_error(what);
}

Run runOf(Process& process, const RunId& runId)
{
    const auto found = process.runs.findRun(runId);
    expect(found.success() && found.value().has_value(), "Run not found: " + runId);
    return *found.value();
}

std::map<std::string, RunStage> stagesByType(Process& process, const RunId& runId)
{
    const auto stages = process.runs.listStages(runId);
    expect(stages.success(), "Could not list the stages of " + runId);
    std::map<std::string, RunStage> byType;
    for (const auto& stage : stages.value()) byType.emplace(stage.type, stage);
    return byType;
}

std::map<std::string, ModelExecution> executionsById(Process& process, const RunId& runId)
{
    const auto executions = process.modelExecutions.listExecutions(ModelExecutionSource::Run, runId);
    expect(executions.success(), "Could not list the model executions of " + runId);
    std::map<std::string, ModelExecution> byId;
    for (const auto& execution : executions.value()) byId.emplace(execution.id, execution);
    return byId;
}

std::vector<ModelExecution> valuesOf(const std::map<std::string, ModelExecution>& byId)
{
    std::vector<ModelExecution> values;
    for (const auto& [id, execution] : byId) values.push_back(execution);
    return values;
}

std::vector<ManifestTaskExecutionRecord> taskRowsOf(Process& process, const RunId& runId)
{
    const auto rows = process.taskExecutions.listForRun(runId);
    expect(rows.success(), "Could not list the task rows of " + runId);
    return rows.value();
}

std::vector<ProposedChangeSet> changeSetsOf(Process& process, const RunId& runId)
{
    const auto sets = process.changeRepo.listForRun(runId);
    expect(sets.success(), "Could not list the change sets of " + runId);
    return sets.value();
}

std::optional<ExecutionManifest> manifestOf(Process& process, const RunId& runId)
{
    const auto manifest = process.manifests.findForRun(runId);
    expect(manifest.success(), "Could not read the manifest of " + runId);
    return manifest.value();
}

std::optional<ManifestWorkLedgerEntry> ledgerOf(Process& process, const RunId& runId)
{
    const auto entry = process.workLedger.findEntry(runId);
    expect(entry.success(), "Could not read the ledger entry of " + runId);
    return entry.value();
}

// A Run becomes terminal BEFORE RunManager writes its Work Ledger entry (the entry is the last step
// of finishRun), so a Run still executing in this process is waited for until its entry exists.
std::optional<ManifestWorkLedgerEntry> waitForLedger(Process& process, const RunId& runId)
{
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (const auto entry = ledgerOf(process, runId); entry.has_value()) return entry;
        std::this_thread::sleep_for(5ms);
    }
    return std::nullopt;
}

bool recoveryReportedNothing(const ai::backend::StartupRecoveryOutcome& outcome)
{
    return !outcome.recoveryError.has_value() && !outcome.modelExecutionRecoveryError.has_value()
        && !outcome.followUpRecoveryError.has_value() && outcome.followUpToProcess.empty()
        && !outcome.comparisonRecoveryError.has_value() && !outcome.agentAuditRecoveryError.has_value();
}

bool sameExecution(const ModelExecution& left, const ModelExecution& right)
{
    return left.id == right.id && left.position == right.position && left.attempt == right.attempt
        && left.status == right.status && left.errorCode == right.errorCode
        && left.inputTokens == right.inputTokens && left.outputTokens == right.outputTokens
        && left.finishedAt == right.finishedAt && left.providerId == right.providerId
        && left.modelId == right.modelId;
}

// ---- T1: every crash state recovers to an honest Interrupted Run ---------------------------------

struct CrashExpectation {
    const char* state;
    bool frozen;                      // a Manifest and its routing record exist
    const char* planningBefore;
    const char* planningAfter;
    const char* executionBefore;
    const char* executionAfter;
    std::size_t executions;           // model_executions rows (each of them survives the restart)
    std::size_t taskRowsBefore;       // manifest_task_executions rows a crash leaves
    std::size_t taskRowsAfter;        // ... and after recovery: the interrupted task's row is added
    std::size_t ledgerTasks;          // task dispositions the Work Ledger entry carries
    std::size_t ledgerProposals;
    const char* interruptedTask;      // the task recovery proves interrupted (nullptr: none)
};

const std::vector<CrashExpectation>& crashExpectations()
{
    static const std::vector<CrashExpectation> expectations {
        // The Planner execution is in flight; nothing is frozen yet.
        {kBeforeFreeze, false, "running", "failed", "pending", "skipped", 1, 0, 0, 0, 0, nullptr},
        // Frozen, the first worker's execution row exists (running, no tokens) but its session was
        // never handed out: the first task is provably the interrupted one (its row did not exist).
        {kAfterFreeze, true, "completed", "completed", "running", "failed", 2, 0, 1, 1, 0, "task-impl-1"},
        // The first implementation task is Completed (row + proposal); the second is in flight and
        // gets its `manifest.task_interrupted` row; the third never started and gets none.
        {kDuringExecution, true, "completed", "completed", "running", "failed", 3, 1, 2, 2, 1, "task-impl-2"},
    };
    return expectations;
}

void testEveryCrashStateRecoversToAnInterruptedRun(const Captured& captured)
{
    for (const auto& expected : crashExpectations()) {
        const std::string label = std::string("[") + expected.state + "] ";
        auto b = restartFrom(captured, expected.state);
        const RunId runId = captured.runId;

        const Run before = runOf(*b, runId);
        const auto stagesBefore = stagesByType(*b, runId);
        const auto executionsBefore = executionsById(*b, runId);
        const auto taskRowsBefore = taskRowsOf(*b, runId);
        const auto changeSetsBefore = changeSetsOf(*b, runId);
        expect(before.status == RunStatus::Running, label + "the captured Run must still be Running");
        expect(!ledgerOf(*b, runId).has_value(), label + "a crash writes no Work Ledger entry");
        expect(std::string(toString(stagesBefore.at("planning").status)) == expected.planningBefore,
            label + "planning stage before recovery");
        expect(std::string(toString(stagesBefore.at("execution").status)) == expected.executionBefore,
            label + "execution stage before recovery");
        expect(executionsBefore.size() == expected.executions, label + "model execution rows before recovery");
        expect(manifestOf(*b, runId).has_value() == expected.frozen, label + "Manifest presence before recovery");

        expect(recoveryReportedNothing(b->recover()), label + "startup recovery reported an error");

        // The Run: Interrupted, stable message, one instant.
        const Run after = runOf(*b, runId);
        expect(after.status == RunStatus::Interrupted, label + "the Run must be Interrupted");
        expect(after.errorMessage == kInterruptedRunRecoveryMessage, label + "the Run's recovery message");
        expect(after.finishedAt.has_value(), label + "the Run needs a finish instant");

        // The stages: settled, never live, stamped with the SAME instant and message as the Run - and
        // an already-terminal stage is not changed at all.
        const auto stagesAfter = stagesByType(*b, runId);
        expect(stagesAfter.size() == 2, label + "the Run must keep its two stages");
        for (const auto& [type, stage] : stagesAfter) {
            const RunStage& earlier = stagesBefore.at(type);
            if (earlier.status == StageStatus::Completed) {
                expect(stage.status == earlier.status && stage.finishedAt == earlier.finishedAt
                           && stage.errorMessage == earlier.errorMessage,
                    label + "the terminal '" + type + "' stage must not be touched");
            } else {
                expect(stage.finishedAt == after.finishedAt, label + "'" + type + "' must share the Run's instant");
                expect(stage.errorMessage == kInterruptedRunRecoveryMessage,
                    label + "'" + type + "' must carry the recovery message");
            }
            expect(stage.status != StageStatus::Pending && stage.status != StageStatus::Running,
                label + "'" + type + "' is still live under a terminal Run");
        }
        expect(std::string(toString(stagesAfter.at("planning").status)) == expected.planningAfter,
            label + "planning stage after recovery");
        expect(std::string(toString(stagesAfter.at("execution").status)) == expected.executionAfter,
            label + "execution stage after recovery");

        // The model executions: every unfinished one is an honest interrupted failure; every finished
        // one is byte-for-byte what it was.
        const auto executionsAfter = executionsById(*b, runId);
        expect(executionsAfter.size() == executionsBefore.size(), label + "recovery must not add or drop an execution");
        std::size_t interrupted = 0;
        for (const auto& [id, earlier] : executionsBefore) {
            const ModelExecution& now = executionsAfter.at(id);
            if (earlier.status == ModelExecutionStatus::Running) {
                ++interrupted;
                expect(now.status == ModelExecutionStatus::Failed && now.errorCode == kModelExecutionInterruptedErrorCode
                           && now.finishedAt.has_value(),
                    label + "an unfinished execution must become failed/model_execution.interrupted");
            } else {
                expect(sameExecution(earlier, now), label + "a finished execution must not be touched");
            }
        }
        expect(interrupted == 1, label + "exactly one execution was in flight");

        // Task rows: a row that existed is untouched; the ONE interrupted task gets a Failed
        // `manifest.task_interrupted` row (never for a task that did not start); none claims to be live.
        const auto taskRowsAfter = taskRowsOf(*b, runId);
        expect(taskRowsBefore.size() == expected.taskRowsBefore && taskRowsAfter.size() == expected.taskRowsAfter,
            label + "task row count");
        for (const auto& row : taskRowsAfter) {
            expect(row.state.status != ManifestTaskStatus::Running && row.state.status != ManifestTaskStatus::Pending,
                label + "a task row claims to be live");
            const bool existed = std::any_of(taskRowsBefore.begin(), taskRowsBefore.end(),
                [&](const ManifestTaskExecutionRecord& earlier) { return earlier.taskId == row.taskId; });
            if (existed) {
                const auto earlier = std::find_if(taskRowsBefore.begin(), taskRowsBefore.end(),
                    [&](const ManifestTaskExecutionRecord& candidate) { return candidate.taskId == row.taskId; });
                expect(earlier->state.status == row.state.status && earlier->state.changeSetId == row.state.changeSetId
                           && earlier->state.errorCode == row.state.errorCode,
                    label + "an existing task row must not be touched");
            } else {
                expect(expected.interruptedTask != nullptr && row.taskId == expected.interruptedTask
                           && row.state.status == ManifestTaskStatus::Failed
                           && row.state.errorCode == kManifestTaskInterruptedErrorCode
                           && row.state.errorMessage == kManifestTaskInterruptedMessage
                           && row.state.attempts == 1 && !row.state.changeSetId.has_value(),
                    label + "the only new row is the interrupted task's manifest.task_interrupted");
            }
        }
        const auto changeSetsAfter = changeSetsOf(*b, runId);
        expect(changeSetsAfter.size() == changeSetsBefore.size() && changeSetsAfter.size() == expected.ledgerProposals,
            label + "the Run's change sets");

        // The Work Ledger entry recovery recorded.
        const auto ledger = ledgerOf(*b, runId);
        expect(ledger.has_value() && ledger->terminalStatus == RunStatus::Interrupted,
            label + "recovery must record an Interrupted Work Ledger entry");
        expect(ledger->taskDispositions.size() == expected.ledgerTasks, label + "ledger task dispositions");
        expect(ledger->proposalIds.size() == expected.ledgerProposals, label + "ledger proposals");
        if (expected.interruptedTask != nullptr) {
            // The ledger was written AFTER the interrupted task, so its account is complete.
            const auto found = std::find_if(ledger->taskDispositions.begin(), ledger->taskDispositions.end(),
                [&](const ManifestWorkLedgerTaskDisposition& disposition) {
                    return disposition.taskId == expected.interruptedTask;
                });
            expect(found != ledger->taskDispositions.end() && found->status == "failed",
                label + "the Work Ledger must record the interrupted task as failed");
        }

        // The frozen identity survives: Run input, roster, budgets, policies - and, once frozen, the
        // Manifest's routing record and task assignments.
        const auto manifest = manifestOf(*b, runId);
        expect(manifest.has_value() == expected.frozen, label + "Manifest presence after recovery");
        expect(identityOf(*b, runId, expected.frozen)
                   == (expected.frozen ? captured.referenceManifestIdentity : captured.referenceRunIdentity),
            label + "the frozen identity changed across the restart");
        if (expected.frozen) {
            const auto usage = projectManifestMemberUsage(*manifest, valuesOf(executionsAfter));
            expect(usage.available && usage.partitionExact && usage.unattributed.executions == 0,
                label + "the member usage partition must stay exact");
            expect(usage.runTotal.interrupted == 1 && usage.runTotal.inFlight == 0,
                label + "exactly one interrupted execution and none in flight");
        }

        // Nothing was resumed: no provider session in process B, and no second Run.
        expect(b->sessionFactory.invocationSnapshot().empty(), label + "recovery opened a provider session");
        const auto runs = b->orchestration.listRuns();
        expect(runs.success() && runs.value().size() == 1, label + "recovery created a Run");
    }
}

// Closing the SQLite file and reopening it keeps exactly what recovery wrote, and a second recovery
// over it is a no-op.
void testRecoveryIsDurableAndIdempotentAcrossAReopen(const Captured& captured)
{
    for (const auto& expected : crashExpectations()) {
        const std::string label = std::string("[") + expected.state + "] ";
        fs::path file;
        std::string recovered;
        {
            auto b = restartFrom(captured, expected.state);
            expect(recoveryReportedNothing(b->recover()), label + "first recovery");
            file = b->file;
            recovered = describe(*b, captured.runId);
        }
        Process c(file, captured.state);
        expect(describe(c, captured.runId) == recovered, label + "the reopened file differs from what recovery wrote");
        expect(recoveryReportedNothing(c.recover()), label + "second recovery");
        expect(describe(c, captured.runId) == recovered, label + "a second recovery changed the durable state");
        expect(c.sessionFactory.invocationSnapshot().empty(), label + "the second recovery opened a provider session");
    }
}

// ---- T4: an exact Retry of a crashed Run ---------------------------------------------------------

struct ExecutionShape {
    std::string role;
    std::uint32_t position;
    std::uint32_t attempt;
    bool operator<(const ExecutionShape& other) const
    {
        return std::tie(position, attempt, role) < std::tie(other.position, other.attempt, other.role);
    }
    bool operator==(const ExecutionShape& other) const
    {
        return position == other.position && attempt == other.attempt && role == other.role;
    }
};

std::set<ExecutionShape> shapesOf(const std::vector<ModelExecution>& executions)
{
    std::set<ExecutionShape> shapes;
    for (const auto& execution : executions) {
        const bool inserted = shapes.insert({execution.role, execution.position, execution.attempt}).second;
        expect(inserted, "Two model executions share (position, attempt) " + std::to_string(execution.position));
    }
    return shapes;
}

void testExactRetryOfACrashedRunKeepsTheParentAndTheRoster(const Captured& captured)
{
    for (const auto& expected : crashExpectations()) {
        const std::string label = std::string("[") + expected.state + "] ";
        auto b = restartFrom(captured, expected.state);
        expect(recoveryReportedNothing(b->recover()), label + "recovery");
        const RunId parentId = captured.runId;
        const std::string parentBefore = describe(*b, parentId);
        const auto parentExecutions = executionsById(*b, parentId);
        std::set<std::string> parentChangeSetIds;
        for (const auto& set : changeSetsOf(*b, parentId)) parentChangeSetIds.insert(set.changeSetId);

        giveWorkspaceAnExistingCmakePreset(captured.workspace, b->verificationCommandRunner);
        prepareSessions(*b, captured.scenario);
        const auto retried = b->orchestration.retryRun(parentId);
        expect(retried.success(), label + "retryRun refused a recovered Run");
        const Run child = waitForTerminal(b->runs, retried.value());
        expect(child.status == RunStatus::Completed, label + "the Retry must complete: " + child.errorMessage);
        expect(child.id != parentId && child.parentRunId == std::optional<RunId>(parentId),
            label + "the Retry must be a NEW Run linked to its parent");

        // The parent - Run, stages, Manifest, task rows, executions, Work Ledger, proposals - is
        // byte-for-byte what recovery left.
        expect(describe(*b, parentId) == parentBefore, label + "the Retry changed its parent");

        // The roster, budgets, policies and (frozen again by the child's own Planner) the routing
        // identity are the source Run's.
        expect(identityOf(*b, child.id, true) == captured.referenceManifestIdentity,
            label + "the Retry did not reuse the frozen roster");

        // Its own rows: nothing is shared with, or carried from, the parent.
        const auto childExecutions = executionsById(*b, child.id);
        for (const auto& [id, execution] : childExecutions) {
            expect(!parentExecutions.contains(id), label + "the child reuses a parent execution row");
        }
        expect(shapesOf(valuesOf(childExecutions)) == shapesOf(captured.referenceExecutions),
            label + "the child's (role, position, attempt) rows differ from a normal Run's");
        const auto childSets = changeSetsOf(*b, child.id);
        expect(childSets.size() == 3, label + "the child must propose its own three change sets");
        for (const auto& set : childSets) {
            expect(set.runId == child.id && !parentChangeSetIds.contains(set.changeSetId),
                label + "a child proposal is shared with, or carried from, the parent");
        }
        const auto childRows = taskRowsOf(*b, child.id);
        expect(childRows.size() == 3, label + "the child must have its own three task rows");
        for (const auto& row : childRows) {
            expect(row.state.status == ManifestTaskStatus::Completed && row.state.changeSetId.has_value()
                       && !parentChangeSetIds.contains(*row.state.changeSetId),
                label + "a child task row is not its own completed work");
        }
        const auto childLedger = waitForLedger(*b, child.id);
        expect(childLedger.has_value() && childLedger->terminalStatus == RunStatus::Completed
                   && childLedger->taskDispositions.size() == 3 && childLedger->proposalIds.size() == 3,
            label + "the child's own Work Ledger entry: "
                + (childLedger.has_value()
                          ? std::string(toString(childLedger->terminalStatus)) + " tasks="
                              + std::to_string(childLedger->taskDispositions.size()) + " proposals="
                              + std::to_string(childLedger->proposalIds.size())
                          : std::string("none")));
        // Nothing was carried: a normal Run's four provider sessions - the Planner and every task,
        // including the one the parent had already completed.
        expect(b->sessionFactory.invocationSnapshot().size() == 4,
            label + "the Retry must re-run the Planner and every task");

        // Usage is the child's own: it equals a normal Run's, with none of the parent's partial work.
        const auto& usage = child.providerBudgetUsage;
        expect(usage.turnsStarted == captured.referenceUsage.turnsStarted
                   && usage.turnsCompleted == captured.referenceUsage.turnsCompleted
                   && usage.inputTokens == captured.referenceUsage.inputTokens
                   && usage.outputTokens == captured.referenceUsage.outputTokens
                   && usage.toolCalls == captured.referenceUsage.toolCalls,
            label + "the child's budget usage is not its own");
        const auto childManifest = manifestOf(*b, child.id);
        expect(childManifest.has_value(), label + "the child has no Manifest");
        const auto partition = projectManifestMemberUsage(*childManifest, valuesOf(childExecutions));
        expect(partition.available && partition.partitionExact && partition.unattributed.executions == 0,
            label + "the child's member usage partition is not exact");
        expect(partition.runTotal.interrupted == 0 && partition.runTotal.inFlight == 0
                   && partition.totalFor("implementation").executions == 2
                   && partition.totalFor("documentation").executions == 1 && partition.planning.executions == 1,
            label + "the child's member usage attribution");

        const auto runs = b->orchestration.listRuns();
        expect(runs.success() && runs.value().size() == 2, label + "exactly the parent and the child");
    }
}

// ---- T5: a vanished or DISABLED provider/model fails the Retry before any provider work -----------

struct CatalogDriftCase {
    const char* name;
    std::function<void(Process&, const Scenario&)> apply;
    // The full failure message: ONE umbrella code, a message that tells "gone" from "disabled" and
    // names the member.
    std::string expectedError;
};

std::string drift(const char* what, const char* member)
{
    const std::string prefix = "manifest.route_resolution_failed: ";
    if (std::string(what) == "gone") {
        return prefix + "The provider or model of routing member '" + member + "' disappeared after plan resolution";
    }
    return prefix + "The " + what + " of routing member '" + member + "' is disabled";
}

void testACatalogDriftFailsBeforeAnyProviderWork(const Captured& captured)
{
    const auto disable = [](Process& p, const Scenario&, const ProviderId& id) {
        require(p.catalog.setProviderEnabled(id, false).success(), "Could not disable the provider");
    };
    const auto disableModel = [](Process& p, const Scenario&, const ModelId& id) {
        require(p.catalog.setModelEnabled(id, false).success(), "Could not disable the model");
    };
    const std::vector<CatalogDriftCase> cases {
        // The entry is GONE (the catalog API refuses to delete one, so the runner's defence is
        // reached through the hiding views).
        {"lead model gone", [](Process& p, const Scenario& s) { p.runtimeModels.hide(s.leadModel.id); },
            drift("gone", "architect")},
        {"lead provider gone", [](Process& p, const Scenario& s) { p.runtimeProviders.hide(s.leadProvider.id); },
            drift("gone", "architect")},
        {"worker model gone", [](Process& p, const Scenario& s) { p.runtimeModels.hide(s.implModel.id); },
            drift("gone", "implementation")},
        {"worker provider gone", [](Process& p, const Scenario& s) { p.runtimeProviders.hide(s.docProvider.id); },
            drift("gone", "documentation")},
        // The entry is DISABLED: `enabled=false` means no new provider session - a Retry replaying a
        // frozen snapshot does not get to bypass the switch.
        {"lead provider disabled",
            [&](Process& p, const Scenario& s) { disable(p, s, s.leadProvider.id); }, drift("provider", "architect")},
        {"lead model disabled",
            [&](Process& p, const Scenario& s) { disableModel(p, s, s.leadModel.id); }, drift("model", "architect")},
        {"worker provider disabled",
            [&](Process& p, const Scenario& s) { disable(p, s, s.implProvider.id); }, drift("provider", "implementation")},
        {"worker model disabled",
            [&](Process& p, const Scenario& s) { disableModel(p, s, s.docModel.id); }, drift("model", "documentation")},
    };
    for (const auto& scenarioCase : cases) {
        const std::string label = std::string("[") + scenarioCase.name + "] ";
        auto b = restartFrom(captured, kDuringExecution);
        expect(recoveryReportedNothing(b->recover()), label + "recovery");
        const std::string parentBefore = describe(*b, captured.runId);
        scenarioCase.apply(*b, captured.scenario);

        const auto retried = b->orchestration.retryRun(captured.runId);
        expect(retried.success(), label + "retryRun itself must accept the immutable snapshot");
        const Run child = waitForTerminal(b->runs, retried.value());
        expect(child.status == RunStatus::Failed, label + "the Retry must fail");
        // One stable code at one boundary - the runner's entry, before the Planner.
        expect(child.errorMessage == scenarioCase.expectedError,
            label + "unexpected failure: " + child.errorMessage);
        // Zero provider work of any kind.
        expect(b->sessionFactory.invocationSnapshot().empty(), label + "a provider session was opened");
        expect(executionsById(*b, child.id).empty(), label + "a model execution was created");
        expect(child.providerBudgetUsage.turnsStarted == 0, label + "provider budget was consumed");
        expect(!manifestOf(*b, child.id).has_value(), label + "a Manifest was frozen");
        expect(taskRowsOf(*b, child.id).empty() && changeSetsOf(*b, child.id).empty(),
            label + "the child produced work");
        const auto ledger = waitForLedger(*b, child.id);
        expect(ledger.has_value() && ledger->terminalStatus == RunStatus::Failed && ledger->taskDispositions.empty(),
            label + "the failed child needs its own empty Work Ledger entry");
        // A refusal before the first StageStarted still leaves no live stage under the Failed Run.
        const auto stages = stagesByType(*b, child.id);
        expect(stages.size() == 2, label + "the child must have its two stages");
        for (const auto& [type, stage] : stages) {
            expect(stage.status == StageStatus::Skipped && stage.errorMessage == child.errorMessage
                       && stage.finishedAt == child.finishedAt,
                label + "'" + type + "' must be Skipped with the Run's message and instant");
        }
        expect(describe(*b, captured.runId) == parentBefore, label + "the failed Retry changed its parent");
    }
}

// The catalog API cannot make such a catalog: any model a Run's snapshot references is protected.
void testTheCatalogRefusesToDeleteAModelARunReferences(const Captured& captured)
{
    auto b = restartFrom(captured, kDuringExecution);
    expect(recoveryReportedNothing(b->recover()), "recovery");
    const std::string before = describe(*b, captured.runId);
    for (const auto& modelId : {captured.scenario.leadModel.id, captured.scenario.implModel.id,
             captured.scenario.docModel.id}) {
        const auto result = b->catalog.deleteModel(modelId);
        expect(!result.success() && result.error()->code == "model.in_use",
            "deleting a roster model must be refused with model.in_use");
    }
    expect(describe(*b, captured.runId) == before, "a refused deletion changed the Run");
    expect(identityOf(*b, captured.runId, true) == captured.referenceManifestIdentity,
        "a refused deletion changed the frozen identity");
}

// ---- T6: several tasks of one member -------------------------------------------------------------

void testSeveralTasksOfOneMemberGetUniquePositions(const Captured& captured)
{
    const auto& executions = captured.referenceExecutions;
    expect(executions.size() == 4, "the reference Run must have the Planner and three task executions");
    const auto shapes = shapesOf(executions); // also proves (position, attempt) is unique
    const std::set<ExecutionShape> expected {
        {"planner", 0, 1}, {"Implementer", 1, 1}, {"Documenter", 2, 1}, {"Implementer", 3, 1}};
    expect(shapes == expected, "the roster's positions: Planner 0, each member's first task keeps its frozen "
                               "position, the second task of a member follows the last member position");
    for (const auto& execution : executions) {
        const bool implementation = execution.role == "Implementer";
        expect(execution.providerId == (implementation ? captured.scenario.implProvider.id
                                           : execution.role == "Documenter" ? captured.scenario.docProvider.id
                                                                            : captured.scenario.leadProvider.id),
            "an execution must carry its member's provider");
        expect(execution.modelId == (implementation ? captured.scenario.implModel.id
                                        : execution.role == "Documenter" ? captured.scenario.docModel.id
                                                                         : captured.scenario.leadModel.id),
            "an execution must carry its member's model");
    }
    const auto usage = projectManifestMemberUsage(captured.referenceManifest, executions);
    expect(usage.available && usage.partitionExact && usage.unattributed.executions == 0,
        "the reference member usage partition");
    expect(usage.totalFor("implementation").executions == 2 && usage.totalFor("documentation").executions == 1
               && usage.planning.executions == 1,
        "one member's two tasks must be attributed to that member");
}

// ---- ACD-5: duration-aware cache accounting through a completed restart ------------------------

void testDurationAwareManifestUsageSurvivesRestartAndAnalytics(const Captured& captured)
{
    const auto& usage = captured.referenceUsage;
    expect(usage.cacheCreationInputTokens == 8 && usage.cacheReadInputTokens == 4
            && usage.cacheCreationDuration.known
            && usage.cacheCreationDuration.fiveMinuteTokens == 2
            && usage.cacheCreationDuration.oneHourTokens == 6
            && usage.inputTokens == 34 && usage.outputTokens == 19,
        "the completed Manifest Run must aggregate the two Anthropic duration partitions");

    std::uint64_t expectedInputTokens {0};
    std::uint64_t expectedOutputTokens {0};
    std::uint64_t expectedCostMicros {0};
    std::size_t durationAwareExecutions {0};
    for (const auto& execution : captured.referenceExecutions) {
        expectedInputTokens += execution.inputTokens;
        expectedOutputTokens += execution.outputTokens;
        expectedCostMicros += execution.costMicros;
        if (execution.role != "Implementer") continue;
        ++durationAwareExecutions;
        expect(execution.cacheCreationInputTokens == 4 && execution.cacheReadInputTokens == 2
                && execution.cacheCreationDuration.known
                && execution.cacheCreationDuration.fiveMinuteTokens == 1
                && execution.cacheCreationDuration.oneHourTokens == 3
                && execution.costMicros == 26 && execution.costPriceKnown,
            "an Anthropic member execution must retain its exact duration-aware usage and price");
    }
    expect(durationAwareExecutions == 2, "both Anthropic member executions must carry duration detail");
    // Planner: 8 input * $1/M + 4 output * $3/M = 20.
    // Each Anthropic worker: 4 fresh * $1/M + 1 five-minute write * $1.25/M
    // + 3 one-hour writes * $2/M + 2 reads * $0.1/M + 5 output * $3/M
    // = 26.45, whose whole-micro actual cost is 26. Documentation worker: 10 + 15 = 25.
    expect(expectedInputTokens == 38 && expectedOutputTokens == 19 && expectedCostMicros == 97,
        "the completed Manifest execution ledger must use the independent duration-aware price");

    const fs::path reopenedFile = captured.root.path
        / ("completed-restart-" + std::to_string(++captured.restarts) + ".sqlite");
    fs::copy_file(captured.completedState, reopenedFile, fs::copy_options::overwrite_existing);
    Process reopened(reopenedFile, captured.state);

    const Run run = runOf(reopened, captured.runId);
    expect(run.status == RunStatus::Completed
            && run.providerBudgetUsage.cacheCreationInputTokens == 8
            && run.providerBudgetUsage.cacheReadInputTokens == 4
            && run.providerBudgetUsage.cacheCreationDuration.known
            && run.providerBudgetUsage.cacheCreationDuration.fiveMinuteTokens == 2
            && run.providerBudgetUsage.cacheCreationDuration.oneHourTokens == 6
            && run.usage.cacheCreationDuration.known
            && run.usage.cacheCreationDuration.fiveMinuteTokens
                == run.providerBudgetUsage.cacheCreationDuration.fiveMinuteTokens
            && run.usage.cacheCreationDuration.oneHourTokens
                == run.providerBudgetUsage.cacheCreationDuration.oneHourTokens,
        "the restarted Run must preserve its duration-aware provider budget and final usage");

    const auto executions = reopened.modelExecutions.listExecutions(ModelExecutionSource::Run, captured.runId);
    expect(executions.success() && executions.value().size() == captured.referenceExecutions.size(),
        "the restarted Run must preserve every execution row");
    for (const auto& execution : executions.value()) {
        const auto reference = std::find_if(captured.referenceExecutions.begin(), captured.referenceExecutions.end(),
            [&](const ModelExecution& candidate) { return candidate.id == execution.id; });
        expect(reference != captured.referenceExecutions.end()
                && execution.cacheCreationInputTokens == reference->cacheCreationInputTokens
                && execution.cacheReadInputTokens == reference->cacheReadInputTokens
                && execution.cacheCreationDuration.known == reference->cacheCreationDuration.known
                && execution.cacheCreationDuration.fiveMinuteTokens
                    == reference->cacheCreationDuration.fiveMinuteTokens
                && execution.cacheCreationDuration.oneHourTokens
                    == reference->cacheCreationDuration.oneHourTokens
                && execution.costMicros == reference->costMicros
                && execution.costPriceKnown == reference->costPriceKnown,
            "a restarted execution changed its cache audit or durable price");
    }

    ExecutionAnalyticsService analytics(reopened.modelExecutionRepository, reopened.models);
    const auto beforeEdit = analytics.sourceSnapshot(ModelExecutionSource::Run, captured.runId);
    expect(beforeEdit.success()
            && beforeEdit.value().totals.executionCount == captured.referenceExecutions.size()
            && beforeEdit.value().totals.inputTokens == expectedInputTokens
            && beforeEdit.value().totals.outputTokens == expectedOutputTokens
            && beforeEdit.value().totals.costMicros == expectedCostMicros
            && beforeEdit.value().totals.priceState() == ExecutionPriceState::Known,
        "analytics must reproduce the completed Manifest execution ledger after restart");

    auto currentModel = reopened.models.findById(captured.scenario.implModel.id);
    expect(currentModel.success() && currentModel.value().has_value(),
        "the restarted Anthropic model must exist");
    currentModel.value()->inputPriceMicrosPerMillionTokens = 99'000'000;
    currentModel.value()->cacheWritePriceMicrosPerMillionTokens = 99'000'000;
    currentModel.value()->cacheWriteOneHourPriceMicrosPerMillionTokens = 99'000'000;
    expect(reopened.models.update(*currentModel.value()).success(),
        "the current Anthropic catalog price must be editable");
    const auto afterEdit = analytics.sourceSnapshot(ModelExecutionSource::Run, captured.runId);
    expect(afterEdit.success() && afterEdit.value().totals.costMicros == expectedCostMicros
            && afterEdit.value().totals.priceState() == ExecutionPriceState::Known,
        "analytics must not re-price durable Manifest history from the edited catalog");
}

// ---- T7: the interrupted-task materialization is durable at every crash point --------------------

// A crash between ANY two startup steps is finished by the next startup, and only then - never
// before the interrupted task is on record - does the immutable Work Ledger entry get written.
void testStartupRecoveryIsResumableAfterACrashAtEveryStep(const Captured& captured)
{
    enum class StoppedAfter { Runs, Executions, InterruptedTasks };
    for (const auto& expected : crashExpectations()) {
        if (expected.interruptedTask == nullptr) continue;
        for (const auto stopped : {StoppedAfter::Runs, StoppedAfter::Executions, StoppedAfter::InterruptedTasks}) {
            const std::string label = std::string("[") + expected.state + " crash after step "
                + std::to_string(static_cast<int>(stopped) + 1) + "] ";
            fs::path file;
            {
                auto b = restartFrom(captured, expected.state);
                expect(b->orchestration.recoverInterruptedRuns().success(), label + "step 1");
                if (stopped != StoppedAfter::Runs) {
                    expect(b->modelExecutions.recoverInterruptedExecutions().success(), label + "step 2");
                }
                if (stopped == StoppedAfter::InterruptedTasks) {
                    const auto outcome = b->interruptedTasks.reconcile();
                    expect(outcome.success() && outcome.value().materialized == 1
                               && outcome.value().unreconciled.empty(),
                        label + "step 3 must write the one provable row");
                }
                // Nothing before the ledger pass writes the immutable entry.
                expect(!ledgerOf(*b, captured.runId).has_value(), label + "an entry was written too early");
                expect(runOf(*b, captured.runId).status == RunStatus::Interrupted, label + "the Run");
                file = b->file;
            }
            // The next startup finds the unfinished Run from persisted state and completes it.
            Process c(file, captured.state);
            expect(recoveryReportedNothing(c.recover()), label + "the next startup");
            const auto rows = taskRowsOf(c, captured.runId);
            expect(rows.size() == expected.taskRowsAfter, label + "task rows after the next startup");
            const auto found = std::find_if(rows.begin(), rows.end(), [&](const ManifestTaskExecutionRecord& row) {
                return row.taskId == expected.interruptedTask;
            });
            expect(found != rows.end() && found->state.errorCode == kManifestTaskInterruptedErrorCode,
                label + "the interrupted task's row is missing");
            const auto ledger = ledgerOf(c, captured.runId);
            expect(ledger.has_value() && ledger->taskDispositions.size() == expected.ledgerTasks,
                label + "the ledger must be written from the COMPLETE account");
            for (const auto& [id, execution] : executionsById(c, captured.runId)) {
                expect(execution.status != ModelExecutionStatus::Running && execution.status != ModelExecutionStatus::Pending,
                    label + "an execution is still unfinished");
            }
            // A further startup changes nothing.
            const std::string settled = describe(c, captured.runId);
            expect(recoveryReportedNothing(c.recover()) && describe(c, captured.runId) == settled,
                label + "a further startup must be a no-op");
        }
    }
}

// The ledger entry is immutable history: a Run that already has one - written by an earlier policy,
// before its interrupted task was ever recorded - is never second-guessed or rewritten.
void testAnExistingWorkLedgerEntryIsNeverRewritten(const Captured& captured)
{
    auto b = restartFrom(captured, kDuringExecution);
    expect(b->orchestration.recoverInterruptedRuns().success(), "step 1");
    expect(b->modelExecutions.recoverInterruptedExecutions().success(), "step 2");
    // The earlier policy: the entry was written right after step 1/2, before any task row existed.
    expect(b->workLedgerService.recordTerminalRun(captured.runId).success(), "the historical entry");
    const auto historical = ledgerOf(*b, captured.runId);
    expect(historical.has_value() && historical->taskDispositions.size() == 1, "the historical entry's account");
    const std::string before = describe(*b, captured.runId);

    const auto outcome = b->interruptedTasks.reconcile();
    expect(outcome.success() && outcome.value().materialized == 0 && outcome.value().unreconciled.empty(),
        "a Run with a ledger entry must not be touched");
    expect(recoveryReportedNothing(b->recover()), "recovery");
    expect(describe(*b, captured.runId) == before, "recovery rewrote a Run whose ledger entry already exists");
    expect(taskRowsOf(*b, captured.runId).size() == 1, "a task row was added behind an immutable ledger entry");
}

// Only a task the Backend can PROVE was interrupted gets a row.
void testOnlyAProvablyInterruptedTaskGetsARow(const Captured& captured)
{
    struct Case {
        const char* name;
        std::function<void(Process&, ModelExecution&)> change;
        std::size_t expectedRows;
        bool heldBack; // the Run's account is incomplete: its ledger entry must wait
    };
    const std::vector<Case> cases {
        {"control: the execution is exactly the interrupted one", [](Process&, ModelExecution&) {}, 1, false},
        // Not an interruption at all: no row, and nothing is unaccounted for.
        {"an orphaned execution (the Run ended under it) is not a task interruption",
            [](Process&, ModelExecution& e) { e.errorCode = kModelExecutionOrphanedErrorCode; }, 0, false},
        {"a cancelled execution is not an interruption",
            [](Process&, ModelExecution& e) {
                e.status = ModelExecutionStatus::Cancelled;
                e.errorCode = "manifest.task_cancelled";
            }, 0, false},
        {"an ordinary provider failure is not an interruption",
            [](Process&, ModelExecution& e) { e.errorCode = "provider.request_failed"; }, 0, false},
        // An interruption that cannot be attributed with certainty: no row, and the Run is HELD BACK.
        {"an execution carrying another member's provider is not provably this task's",
            [&](Process&, ModelExecution& e) {
                e.providerId = captured.scenario.docProvider.id;
                e.modelId = captured.scenario.docModel.id;
            }, 0, true},
        {"a position that matches no task slot proves nothing",
            [](Process&, ModelExecution& e) { e.position = 9; }, 0, true},
        // A worker's role is user-chosen text: identity is the POSITION, never the role.
        {"a worker whose role is 'planner' is still a task",
            [](Process&, ModelExecution& e) { e.role = "planner"; }, 1, false},
        {"a worker whose role is 'integration' is still a task",
            [](Process&, ModelExecution& e) { e.role = "integration"; }, 1, false},
    };
    for (const auto& scenarioCase : cases) {
        const std::string label = std::string("[") + scenarioCase.name + "] ";
        auto b = restartFrom(captured, kDuringExecution);
        expect(b->orchestration.recoverInterruptedRuns().success(), label + "step 1");
        expect(b->modelExecutions.recoverInterruptedExecutions().success(), label + "step 2");
        // The in-flight execution (position 3) as recovery left it; change it as the case says.
        std::optional<ModelExecution> inFlight;
        for (const auto& [id, execution] : executionsById(*b, captured.runId)) {
            if (execution.errorCode == kModelExecutionInterruptedErrorCode) inFlight = execution;
        }
        expect(inFlight.has_value() && inFlight->position == 3, label + "the in-flight execution");
        ModelExecution changed = *inFlight;
        scenarioCase.change(*b, changed);
        expect(b->modelExecutionRepository.updateExecution(changed).success(), label + "could not alter the execution");

        const auto outcome = b->interruptedTasks.reconcile();
        expect(outcome.success() && outcome.value().unreconciled.contains(captured.runId) == scenarioCase.heldBack,
            label + "the Run is held back from the ledger exactly when its account is incomplete");
        expect(outcome.value().materialized == scenarioCase.expectedRows, label + "rows written");
        const auto rows = taskRowsOf(*b, captured.runId);
        expect(rows.size() == 1 + scenarioCase.expectedRows, label + "task rows");
    }

    // A task that ALREADY has a terminal row is left as it is.
    {
        auto b = restartFrom(captured, kDuringExecution);
        expect(b->orchestration.recoverInterruptedRuns().success(), "step 1");
        expect(b->modelExecutions.recoverInterruptedExecutions().success(), "step 2");
        ManifestTaskState existing;
        existing.status = ManifestTaskStatus::Failed;
        existing.errorCode = "manifest.worker_rejected";
        existing.attempts = 2;
        expect(b->taskExecutions.upsert(captured.runId, "task-impl-2", existing).success(), "the existing row");
        const auto outcome = b->interruptedTasks.reconcile();
        expect(outcome.success() && outcome.value().materialized == 0, "an existing terminal row must stand");
        for (const auto& row : taskRowsOf(*b, captured.runId)) {
            if (row.taskId == "task-impl-2") {
                expect(row.state.errorCode == "manifest.worker_rejected" && row.state.attempts == 2,
                    "an existing terminal row was overwritten");
            }
        }
    }
    // A Run that never froze a Manifest has no task to account for.
    {
        auto b = restartFrom(captured, kBeforeFreeze);
        expect(b->orchestration.recoverInterruptedRuns().success(), "step 1");
        expect(b->modelExecutions.recoverInterruptedExecutions().success(), "step 2");
        const auto outcome = b->interruptedTasks.reconcile();
        expect(outcome.success() && outcome.value().materialized == 0 && outcome.value().unreconciled.empty()
                   && taskRowsOf(*b, captured.runId).empty(),
            "a Run without a Manifest must get no task row");
    }
}

// A Run whose account is not complete is held back from the ledger pass - an execution recovery
// failed, so an interrupted task cannot be proven yet - and the next startup finishes it.
class ExecutionsThatCannotBeRecovered final : public IModelExecutionRepository {
public:
    explicit ExecutionsThatCannotBeRecovered(IModelExecutionRepository& inner) : inner_(inner) {}
    common::Result<ModelExecution> insertExecution(ModelExecution value) override
    { return inner_.insertExecution(std::move(value)); }
    common::Result<ModelExecution> updateExecution(ModelExecution value) override
    { return inner_.updateExecution(std::move(value)); }
    common::Result<std::optional<ModelExecution>> findExecution(const ModelExecutionId& id) const override
    { return inner_.findExecution(id); }
    common::Result<std::vector<ModelExecution>> listExecutions(
        ModelExecutionSource source, const std::string& sourceId) const override
    { return inner_.listExecutions(source, sourceId); }
    common::Result<std::vector<ModelExecution>> listProjectExecutions(const ProjectId& projectId) const override
    { return inner_.listProjectExecutions(projectId); }
    common::Result<std::vector<ModelExecution>> listUnfinishedExecutions() const override
    { return common::Result<std::vector<ModelExecution>>::failure({"persistence.read_failed", "Simulated failure"}); }
    common::Result<QualityEvaluation> insertEvaluation(QualityEvaluation value) override
    { return inner_.insertEvaluation(std::move(value)); }
    common::Result<std::optional<QualityEvaluation>> findEvaluation(const QualityEvaluationId& id) const override
    { return inner_.findEvaluation(id); }
    common::Result<std::vector<QualityEvaluation>> listEvaluations(const ModelExecutionId& id) const override
    { return inner_.listEvaluations(id); }

private:
    IModelExecutionRepository& inner_;
};

void testARunWithAnIncompleteAccountIsHeldBackFromTheLedger(const Captured& captured)
{
    fs::path file;
    {
        auto b = restartFrom(captured, kDuringExecution);
        file = b->file;
        ExecutionsThatCannotBeRecovered broken(b->modelExecutionRepository);
        ModelExecutionService brokenService(broken);
        const auto outcome = ai::backend::runStartupRecovery(ai::backend::StartupRecoveryServices {
            b->orchestration, brokenService, b->interruptedTasks, b->toolSessions, b->toolAudits,
            b->workLedgerService});
        expect(outcome.modelExecutionRecoveryError.has_value(), "the execution recovery failure is surfaced");
        expect(outcome.recoveryError.has_value()
                   && (outcome.recoveryError->code == "manifest_work_ledger.recovery_incomplete"
                       || outcome.recoveryError->message.find("manifest_work_ledger.recovery_incomplete")
                              != std::string::npos),
            "a Run held back from the ledger must be visible in the recovery error");
        expect(runOf(*b, captured.runId).status == RunStatus::Interrupted, "the Run itself is Interrupted");
        expect(!ledgerOf(*b, captured.runId).has_value(),
            "an immutable entry must not be written from an incomplete account");
        expect(taskRowsOf(*b, captured.runId).size() == 1, "an interrupted task cannot be proven yet");
    }
    Process c(file, captured.state);
    expect(recoveryReportedNothing(c.recover()), "the next, healthy startup");
    const auto ledger = ledgerOf(c, captured.runId);
    expect(ledger.has_value() && ledger->taskDispositions.size() == 2 && taskRowsOf(c, captured.runId).size() == 2,
        "the next startup completes the account and then writes the ledger");
}

// ---- T8: an interruption the Backend cannot attribute never freezes the immutable ledger ---------

bool mentionsLedgerRecoveryIncomplete(const ai::backend::StartupRecoveryOutcome& outcome)
{
    return outcome.recoveryError.has_value()
        && (outcome.recoveryError->code == "manifest_work_ledger.recovery_incomplete"
            || outcome.recoveryError->message.find("manifest_work_ledger.recovery_incomplete") != std::string::npos);
}

// On-disk corruption of the frozen Manifest, made on a COPY. The table is immutable (a trigger rejects
// any UPDATE); real corruption bypasses it, so the test drops the trigger first.
void rewriteTextInStoredManifest(const fs::path& file, const std::string& from, const std::string& to)
{
    sqlite3* handle = nullptr;
    const std::string path = file.generic_string();
    require(sqlite3_open_v2(path.c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
        "Could not open the copy to corrupt its Manifest");
    const std::string sql = "DROP TRIGGER IF EXISTS execution_manifests_reject_update; "
                            "UPDATE execution_manifests SET manifest_json = REPLACE(manifest_json, '"
        + from + "', '" + to + "') WHERE instr(manifest_json, '" + from + "') > 0;";
    char* message = nullptr;
    const int result = sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, &message);
    const int changed = sqlite3_changes(handle);
    const std::string failure = message != nullptr ? std::string(message) : std::string {};
    sqlite3_free(message);
    sqlite3_close_v2(handle);
    require(result == SQLITE_OK, ("Could not corrupt the stored Manifest: " + failure).c_str());
    require(changed == 1, "The stored Manifest did not contain the text to rewrite");
}

// A routing record that is PRESENT but damaged cannot attribute the interrupted task, so the account
// is incomplete: the Run is held back from the ledger (and the startup says so) until the record is
// readable again - it is never frozen as "no task was interrupted".
void testACorruptedRoutingRecordHoldsTheLedgerBackUntilItIsRepaired(const Captured& captured)
{
    const std::string fingerprint = captured.referenceManifest.routing->fingerprint;
    const std::string damaged(fingerprint.size(), '0');
    const fs::path file = captured.root.path / "corrupted-routing.sqlite";
    fs::copy_file(captured.crashStates.at(kDuringExecution), file, fs::copy_options::overwrite_existing);
    rewriteTextInStoredManifest(file, fingerprint, damaged);
    {
        Process b(file, captured.state);
        const auto first = b.recover();
        expect(mentionsLedgerRecoveryIncomplete(first), "a damaged routing record must be visible as an incomplete recovery");
        expect(runOf(b, captured.runId).status == RunStatus::Interrupted, "the Run itself is Interrupted");
        expect(!ledgerOf(b, captured.runId).has_value(), "the immutable ledger entry must not be frozen on a damaged record");
        expect(taskRowsOf(b, captured.runId).size() == 1, "the interrupted task cannot be proven from a damaged record");
        // Held, not forgotten: every startup finds it again.
        expect(mentionsLedgerRecoveryIncomplete(b.recover()) && !ledgerOf(b, captured.runId).has_value(),
            "a further startup must still hold the Run back");
    }
    rewriteTextInStoredManifest(file, damaged, fingerprint); // the record is readable again
    Process c(file, captured.state);
    expect(recoveryReportedNothing(c.recover()), "the startup after the repair");
    const auto ledger = ledgerOf(c, captured.runId);
    expect(ledger.has_value() && ledger->taskDispositions.size() == 2 && taskRowsOf(c, captured.runId).size() == 2,
        "once attributable, the account is completed and only then the ledger is written");
}

// An interrupted execution whose provider/model disagree with the frozen member is not provably that
// task's: the account is incomplete and the ledger waits.
void testAnInconsistentExecutionIdentityHoldsTheLedgerBackUntilItIsConsistent(const Captured& captured)
{
    auto b = restartFrom(captured, kDuringExecution);
    expect(b->orchestration.recoverInterruptedRuns().success(), "step 1");
    expect(b->modelExecutions.recoverInterruptedExecutions().success(), "step 2");
    std::optional<ModelExecution> inFlight;
    for (const auto& [id, execution] : executionsById(*b, captured.runId)) {
        if (execution.errorCode == kModelExecutionInterruptedErrorCode) inFlight = execution;
    }
    expect(inFlight.has_value(), "the interrupted execution");
    ModelExecution inconsistent = *inFlight;
    inconsistent.providerId = captured.scenario.docProvider.id; // not the implementation member's route
    inconsistent.modelId = captured.scenario.docModel.id;
    expect(b->modelExecutionRepository.updateExecution(inconsistent).success(), "could not alter the execution");

    const auto held = b->recover();
    expect(mentionsLedgerRecoveryIncomplete(held), "an inconsistent identity must be visible as an incomplete recovery");
    expect(!ledgerOf(*b, captured.runId).has_value(), "the immutable ledger entry must not be frozen on a guess");
    expect(taskRowsOf(*b, captured.runId).size() == 1, "no row is written for an execution that is not provably the task's");

    expect(b->modelExecutionRepository.updateExecution(*inFlight).success(), "could not restore the execution");
    expect(recoveryReportedNothing(b->recover()), "the startup once the identity agrees");
    const auto ledger = ledgerOf(*b, captured.runId);
    expect(ledger.has_value() && ledger->taskDispositions.size() == 2 && taskRowsOf(*b, captured.runId).size() == 2,
        "a consistent identity completes the account, and only then the ledger is written");
}

// ---- T3: a CLEAN shutdown, characterised on its own ---------------------------------------------

void testCleanShutdownIsReconciledByTheRunnerNotByRecovery()
{
    TempDir root;
    const fs::path workspace = root.path / "workspace";
    const fs::path state = root.path / "state";
    const fs::path databaseA = root.path / "process-a.sqlite";
    RunId activeId;
    RunId queuedId;
    std::string activeDescription;
    std::thread shutdownThread;
    {
        Process a(databaseA, state);
        const auto scenario = makeScenario(a, workspace);
        giveWorkspaceAnExistingCmakePreset(workspace, a.verificationCommandRunner);
        const auto prepared = prepareSessions(a, scenario);

        // Inside the SECOND implementation task: queue a second Run (it cannot start - one worker),
        // then shut the manager down from another thread and hold this session until the stop arrives.
        prepared.implSecond->beforeNextResponseWithStop = [&](std::stop_token token) {
            const auto queued = a.orchestration.startRun(scenario.request);
            require(queued.success(), "Could not queue the second Run");
            queuedId = queued.value();
            shutdownThread = std::thread([&a] { a.manager.shutdown(); });
            const auto deadline = std::chrono::steady_clock::now() + 10s;
            while (!token.stop_requested() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(1ms);
            }
            require(token.stop_requested(), "The shutdown never requested the stop");
        };
        const auto started = a.orchestration.startRun(scenario.request);
        require(started.success(), "Could not start the roster Run");
        activeId = started.value();
        waitForTerminal(a.runs, activeId);
        if (shutdownThread.joinable()) shutdownThread.join();

        // The active Run: the RUNNER settles everything it owns before the process exits.
        const Run active = runOf(a, activeId);
        expect(active.status == RunStatus::Interrupted && active.errorMessage == "Application shutdown interrupted the run",
            "the active Run must end Interrupted by the shutdown");
        const auto stages = stagesByType(a, activeId);
        expect(std::string(toString(stages.at("planning").status)) == "completed"
                   && std::string(toString(stages.at("execution").status)) == "cancelled",
            "the planning stage is Completed and the execution stage Cancelled by the runner");
        // The in-flight execution is CANCELLED by the runner (manifest.task_cancelled), not left for
        // the orphaned safety net: nothing is unfinished.
        const auto executions = executionsById(a, activeId);
        expect(executions.size() == 3, "the Planner and the two started tasks");
        std::size_t cancelled = 0;
        for (const auto& [id, execution] : executions) {
            expect(execution.status != ModelExecutionStatus::Running && execution.finishedAt.has_value(),
                "a clean shutdown must leave no unfinished model execution");
            if (execution.status == ModelExecutionStatus::Cancelled) {
                ++cancelled;
                expect(execution.errorCode == "manifest.task_cancelled", "the cancelled execution's code");
            }
        }
        expect(cancelled == 1, "exactly the in-flight execution is cancelled");
        // Every task has an explicit row: completed, cancelled-as-failed, and the never-started one.
        std::map<std::string, ManifestTaskExecutionRecord> rows;
        for (const auto& row : taskRowsOf(a, activeId)) rows.emplace(row.taskId, row);
        expect(rows.size() == 3, "a clean shutdown must leave an explicit row for every task");
        expect(rows.at("task-impl-1").state.status == ManifestTaskStatus::Completed, "task-impl-1");
        expect(rows.at("task-impl-2").state.status == ManifestTaskStatus::Failed
                   && rows.at("task-impl-2").state.errorCode == "manifest.task_cancelled"
                   && rows.at("task-impl-2").state.attempts == 1,
            "the interrupted task is a Failed row with manifest.task_cancelled");
        expect(rows.at("task-docs").state.status == ManifestTaskStatus::Pending
                   && rows.at("task-docs").state.attempts == 0,
            "the never-started task is an explicit Pending row");
        const auto ledger = ledgerOf(a, activeId);
        expect(ledger.has_value() && ledger->terminalStatus == RunStatus::Interrupted
                   && ledger->taskDispositions.size() == 3 && ledger->proposalIds.size() == 1,
            "the Work Ledger covers every task");
        activeDescription = describe(a, activeId);

        // The Run that was only QUEUED never started anything.
        const Run queued = runOf(a, queuedId);
        expect(queued.status == RunStatus::Interrupted && queued.errorMessage == "Application shutdown before execution",
            "the queued Run must end Interrupted before execution");
        expect(executionsById(a, queuedId).empty() && !manifestOf(a, queuedId).has_value(),
            "the queued Run must have started nothing");
        // Its stages are settled by the shutdown too - never left Pending under the terminal Run.
        for (const auto& [type, stage] : stagesByType(a, queuedId)) {
            expect(stage.status == StageStatus::Skipped && stage.errorMessage == "Application shutdown before execution"
                       && stage.finishedAt == queued.finishedAt,
                "the queued Run's '" + type + "' stage must be Skipped by the shutdown");
        }
        expect(a.sessionFactory.invocationSnapshot().size() == 3,
            "the shutdown opened no provider session for the queued Run");
    }
    // Restart: the file is closed and reopened; recovery has nothing to do for the active Run.
    Process b(databaseA, state);
    expect(recoveryReportedNothing(b.recover()), "recovery after a clean shutdown");
    expect(describe(b, activeId) == activeDescription, "recovery must not touch a Run a clean shutdown settled");
    const Run queued = runOf(b, queuedId);
    expect(queued.status == RunStatus::Interrupted && queued.errorMessage == "Application shutdown before execution",
        "the queued Run stays as the shutdown left it");
    expect(executionsById(b, queuedId).empty(), "recovery created executions for a Run that never started");
    for (const auto& [type, stage] : stagesByType(b, queuedId)) {
        expect(stage.status == StageStatus::Skipped, "recovery changed a stage the shutdown had already settled");
    }
    expect(b.sessionFactory.invocationSnapshot().empty(), "recovery opened a provider session");
}

} // namespace

int main()
{
    try {
        const auto captured = captureCrashStates();
        testEveryCrashStateRecoversToAnInterruptedRun(*captured);
        testRecoveryIsDurableAndIdempotentAcrossAReopen(*captured);
        testExactRetryOfACrashedRunKeepsTheParentAndTheRoster(*captured);
        testACatalogDriftFailsBeforeAnyProviderWork(*captured);
        testTheCatalogRefusesToDeleteAModelARunReferences(*captured);
        testSeveralTasksOfOneMemberGetUniquePositions(*captured);
        testDurationAwareManifestUsageSurvivesRestartAndAnalytics(*captured);
        testStartupRecoveryIsResumableAfterACrashAtEveryStep(*captured);
        testAnExistingWorkLedgerEntryIsNeverRewritten(*captured);
        testOnlyAProvablyInterruptedTaskGetsARow(*captured);
        testARunWithAnIncompleteAccountIsHeldBackFromTheLedger(*captured);
        testACorruptedRoutingRecordHoldsTheLedgerBackUntilItIsRepaired(*captured);
        testAnInconsistentExecutionIdentityHoldsTheLedgerBackUntilItIsConsistent(*captured);
        testCleanShutdownIsReconciledByTheRunnerNotByRecovery();
        std::cout << "ManifestRosterRestart tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "ManifestRosterRestart tests failed: " << exception.what() << '\n';
        return 1;
    }
}
