#include "RunDraftRepositoryContract.hpp"

#include "ai/application/planning/RunDraftPlanService.hpp"
#include "ai/application/planning/RunDraftService.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace ai::application;
using namespace ai::infrastructure;
namespace fs = std::filesystem;

void requireDraft(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct RunDraftWorkspace {
    fs::path root {fs::temp_directory_path()
        / ("ai-orchestrator-rundraft-test-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
    fs::path database {root / "orchestrator.sqlite3"};
    ~RunDraftWorkspace() { std::error_code ignored; fs::remove_all(root, ignored); }
};

StageModelAssignment makeRosterEntry(
    std::string stageType, std::string providerId, std::string modelId, std::uint32_t position)
{
    StageModelAssignment entry;
    entry.stageType = std::move(stageType);
    entry.providerId = std::move(providerId);
    entry.modelId = std::move(modelId);
    entry.timeout = std::chrono::milliseconds(60'000);
    entry.agentName = "Agent " + std::to_string(position);
    entry.role = "implementation";
    entry.instructions = "Do the thing.";
    entry.position = position;
    entry.leadArchitect = position == 0;
    entry.maxAttempts = 1;
    entry.workspaceAccess = false;
    return entry;
}

// Fixture shared by every test: one project + provider + two models, so
// roster entries have real foreign keys to point at.
struct RunDraftFixture {
    std::shared_ptr<SqliteDatabase> database;
    Project project;
    Provider provider;
    Model modelA;
    Model modelB;

    explicit RunDraftFixture(const fs::path& databasePath)
    {
        const auto opened = SqliteDatabase::open(databasePath);
        requireDraft(opened.success(), "Could not open run draft test database");
        database = opened.value();

        const auto now = std::chrono::system_clock::now();
        SqliteProjectRepository projects(database);
        project = Project {"draft-project-1", "Draft project", "Context",
            databasePath.parent_path() / "project", ProjectStatus::Active, now, now};
        requireDraft(projects.insert(project).success(), "Could not insert draft test project");

        SqliteProviderRepository providers(database);
        provider = Provider {"draft-provider-1", "OpenAI", "openai",
            "https://api.openai.com", true, std::nullopt, now, now};
        requireDraft(providers.insert(provider).success(), "Could not insert draft test provider");

        SqliteModelRepository models(database);
        modelA = Model {"draft-model-a", provider.id, "gpt-a", "GPT A",
            128000, 4096, 100, 200, 0, 0, true, true, now, now};
        requireDraft(models.insert(modelA).success(), "Could not insert draft test model A");
        modelB = Model {"draft-model-b", provider.id, "gpt-b", "GPT B",
            128000, 4096, 100, 200, 0, 0, true, false, now, now};
        requireDraft(models.insert(modelB).success(), "Could not insert draft test model B");
    }
};

void testRunDraftRoundTrip()
{
    RunDraftWorkspace workspace;
    RunDraftFixture fixture(workspace.database);
    SqliteRunDraftRepository drafts(fixture.database);
    RunDraftService service(drafts);

    CreateRunDraftRequest request;
    request.name = "My draft";
    request.projectId = fixture.project.id;
    request.title = "Refactor the widget layer";
    request.task = "Refactor Widget/* to the new layout engine";
    request.workflowId = "execution-profile-workflow-v1";
    request.launchMode = RunLaunchMode::CustomRoster;
    request.maxParallelism = 2;
    request.verificationRepairPolicy = VerificationRepairPolicy::Disabled;
    request.workspacePath = workspace.root / "override-workspace";
    request.roster = {
        makeRosterEntry("agent-0", fixture.provider.id, fixture.modelA.id, 0),
        makeRosterEntry("agent-1", fixture.provider.id, fixture.modelB.id, 1)};

    const auto created = service.createDraft(request);
    requireDraft(created.success(), "Could not create run draft");
    const auto draftId = created.value();

    const auto found = service.findDraft(draftId);
    requireDraft(found.success() && found.value().has_value(), "Could not find created run draft");
    const RunDraft& draft = *found.value();
    requireDraft(draft.name == "My draft", "Draft name did not round-trip");
    requireDraft(draft.projectId == fixture.project.id, "Draft projectId did not round-trip");
    requireDraft(draft.title == request.title, "Draft title did not round-trip");
    requireDraft(draft.task == request.task, "Draft task did not round-trip");
    requireDraft(draft.launchMode == RunLaunchMode::CustomRoster, "Draft launchMode did not round-trip");
    requireDraft(draft.maxParallelism == 2, "Draft maxParallelism did not round-trip");
    requireDraft(draft.verificationRepairPolicy == VerificationRepairPolicy::Disabled,
        "Draft verification repair policy did not round-trip");
    requireDraft(draft.workspacePath.has_value()
        && *draft.workspacePath == request.workspacePath, "Draft workspacePath did not round-trip");
    requireDraft(draft.roster.size() == 2, "Draft roster size did not round-trip");
    requireDraft(draft.roster[0].agentName == "Agent 0", "Draft roster order/content did not round-trip");
    requireDraft(draft.roster[1].modelId == fixture.modelB.id, "Draft roster model id did not round-trip");
}

void testRunDraftUpdateReplacesRoster()
{
    RunDraftWorkspace workspace;
    RunDraftFixture fixture(workspace.database);
    SqliteRunDraftRepository drafts(fixture.database);
    RunDraftService service(drafts);

    CreateRunDraftRequest request;
    request.name = "Draft to edit";
    request.projectId = fixture.project.id;
    request.title = "Title";
    request.task = "Task";
    request.workflowId = "execution-profile-workflow-v1";
    request.launchMode = RunLaunchMode::CustomRoster;
    request.roster = {makeRosterEntry("agent-0", fixture.provider.id, fixture.modelA.id, 0)};
    const auto created = service.createDraft(request);
    requireDraft(created.success(), "Could not create draft to edit");
    const auto draftId = created.value();

    UpdateRunDraftRequest update;
    update.id = draftId;
    update.name = "Renamed draft";
    update.projectId = fixture.project.id;
    update.title = "New title";
    update.task = "New task";
    update.workflowId = request.workflowId;
    update.launchMode = RunLaunchMode::CustomRoster;
    update.verificationRepairPolicy = VerificationRepairPolicy::Disabled;
    update.roster = {
        makeRosterEntry("agent-0", fixture.provider.id, fixture.modelA.id, 0),
        makeRosterEntry("agent-1", fixture.provider.id, fixture.modelB.id, 1),
        makeRosterEntry("agent-2", fixture.provider.id, fixture.modelB.id, 2)};
    const auto updated = service.updateDraft(update);
    requireDraft(updated.success(), "Could not update run draft");

    const auto found = service.findDraft(draftId);
    requireDraft(found.success() && found.value().has_value(), "Could not find updated run draft");
    requireDraft(found.value()->name == "Renamed draft", "Updated draft name did not persist");
    requireDraft(found.value()->roster.size() == 3,
        "Updated draft roster did not fully replace the old roster");
    requireDraft(found.value()->verificationRepairPolicy == VerificationRepairPolicy::Disabled,
        "Updated draft verification repair policy did not persist");
}

void testRunDraftInterfaceContractsAreValidatedNormalizedAndReplacedByUpdate()
{
    RunDraftWorkspace workspace;
    RunDraftFixture fixture(workspace.database);
    SqliteRunDraftRepository drafts(fixture.database);
    RunDraftService service(drafts);

    RunInterfaceContract shape;
    shape.id = "C1";
    shape.title = " Loss entry ";
    shape.text = "property string metricId\r\nproperty real share";
    shape.appliesToPaths = {"qml\\models"};
    RunInterfaceContract rule;
    rule.id = "K1";
    rule.kind = RunContractKind::Constraint;
    rule.title = "File size rule";
    rule.text = "No file may exceed 300 lines.";

    CreateRunDraftRequest request;
    request.name = "Contracts draft";
    request.projectId = fixture.project.id;
    request.title = "T";
    request.task = "Task";
    request.workflowId = "execution-manifest-v1";
    request.launchMode = RunLaunchMode::CustomRoster;
    request.roster = {makeRosterEntry("agent-0", fixture.provider.id, fixture.modelA.id, 0)};

    // An invalid list is refused with its own typed code and nothing is stored.
    auto invalid = request;
    invalid.interfaceContracts = {shape, shape};
    const auto refused = service.createDraft(invalid);
    requireDraft(!refused.success() && refused.error()->code == "run_contracts.duplicate_id",
        "A draft with duplicate contract ids must be refused");
    const auto none = service.listDrafts();
    requireDraft(none.success() && none.value().empty(), "A refused draft must not be stored");

    request.interfaceContracts = {shape, rule};
    const auto created = service.createDraft(request);
    requireDraft(created.success(), "Could not create a draft with contracts");
    const auto found = service.findDraft(created.value());
    requireDraft(found.success() && found.value().has_value() && found.value()->interfaceContracts.size() == 2,
        "The draft must keep both contracts");
    const auto& first = found.value()->interfaceContracts[0];
    requireDraft(first.title == "Loss entry" && first.text == "property string metricId\nproperty real share"
            && first.appliesToPaths == std::vector<std::string>({"qml/models"})
            && found.value()->interfaceContracts[1].kind == RunContractKind::Constraint,
        "The draft stores the canonical form of its contracts");

    UpdateRunDraftRequest update;
    update.id = created.value();
    update.name = request.name;
    update.projectId = fixture.project.id;
    update.title = request.title;
    update.task = request.task;
    update.workflowId = request.workflowId;
    update.launchMode = request.launchMode;
    update.roster = request.roster;
    update.interfaceContracts = {rule, rule};
    const auto refusedUpdate = service.updateDraft(update);
    requireDraft(!refusedUpdate.success() && refusedUpdate.error()->code == "run_contracts.duplicate_id",
        "An update with duplicate contract ids must be refused");
    const auto unchanged = service.findDraft(created.value());
    requireDraft(unchanged.success() && unchanged.value().has_value()
            && unchanged.value()->interfaceContracts.size() == 2,
        "A refused update leaves the stored contracts alone");

    update.interfaceContracts = {rule};
    requireDraft(service.updateDraft(update).success(), "Could not update the draft contracts");
    const auto replaced = service.findDraft(created.value());
    requireDraft(replaced.success() && replaced.value().has_value()
            && replaced.value()->interfaceContracts.size() == 1
            && replaced.value()->interfaceContracts[0].id == "K1",
        "An update replaces the contracts");
    update.interfaceContracts.clear();
    requireDraft(service.updateDraft(update).success(), "Could not clear the draft contracts");
    const auto cleared = service.findDraft(created.value());
    requireDraft(cleared.success() && cleared.value().has_value() && cleared.value()->interfaceContracts.empty(),
        "An update without contracts removes them");
}

// A draft applies a stricter authoring-time rule than RunManager, which checks the effective workflow it has
// already resolved: a draft cannot resolve a saved profile's workflow, so contracts are saved only with an explicit
// Manifest workflow AND a per-Run roster (CustomRoster) - the combination the New Run dialog shows contracts for.
// Otherwise a draft saved through the Application API could fail to start, or lose its contracts in the dialog.
void testRunDraftInterfaceContractsRequireAManifestRoster()
{
    RunDraftWorkspace workspace;
    RunDraftFixture fixture(workspace.database);
    SqliteRunDraftRepository drafts(fixture.database);
    RunDraftService service(drafts);

    RunInterfaceContract rule;
    rule.id = "K1";
    rule.kind = RunContractKind::Constraint;
    rule.title = "File size rule";
    rule.text = "No file may exceed 300 lines.";

    CreateRunDraftRequest request;
    request.name = "Manifest draft";
    request.projectId = fixture.project.id;
    request.title = "T";
    request.task = "Task";
    request.workflowId = "execution-manifest-v1";
    request.launchMode = RunLaunchMode::CustomRoster;
    request.roster = {makeRosterEntry("agent-0", fixture.provider.id, fixture.modelA.id, 0)};

    struct Refused { const char* what; std::string workflowId; RunLaunchMode launchMode; };
    const std::vector<Refused> refusedShapes {
        {"no workflow", "", RunLaunchMode::CustomRoster},
        {"the mock workflow", "mock-sequential-v1", RunLaunchMode::CustomRoster},
        {"the profile workflow", "execution-profile-v1", RunLaunchMode::CustomRoster},
        {"a saved profile", "execution-manifest-v1", RunLaunchMode::SavedExecutionProfile},
        {"a mock launch", "execution-manifest-v1", RunLaunchMode::Mock},
    };
    for (const auto& shape : refusedShapes) {
        auto attempt = request;
        attempt.workflowId = shape.workflowId;
        attempt.launchMode = shape.launchMode;
        attempt.interfaceContracts = {rule};
        const auto refused = service.createDraft(attempt);
        requireDraft(!refused.success() && refused.error()->code == "run_contracts.requires_manifest_workflow",
            (std::string("A draft with contracts on ") + shape.what + " must be refused").c_str());
        // The same draft without contracts is an ordinary draft and stays allowed.
        attempt.interfaceContracts.clear();
        requireDraft(service.createDraft(attempt).success(),
            (std::string("A draft without contracts on ") + shape.what + " must still be saved").c_str());
    }
    const auto onlyPlain = service.listDrafts();
    requireDraft(onlyPlain.success() && onlyPlain.value().size() == refusedShapes.size(),
        "A refused draft must not be stored");

    // The accepted combination, then an update that would move it off the Manifest roster.
    request.interfaceContracts = {rule};
    const auto created = service.createDraft(request);
    requireDraft(created.success(), "A Manifest roster draft keeps its contracts");

    UpdateRunDraftRequest update;
    update.id = created.value();
    update.name = request.name;
    update.projectId = fixture.project.id;
    update.title = request.title;
    update.task = request.task;
    update.workflowId = "";
    update.launchMode = RunLaunchMode::CustomRoster;
    update.roster = request.roster;
    update.interfaceContracts = {rule};
    const auto refusedUpdate = service.updateDraft(update);
    requireDraft(!refusedUpdate.success()
            && refusedUpdate.error()->code == "run_contracts.requires_manifest_workflow",
        "An update that leaves the Manifest workflow with contracts must be refused");
    const auto unchanged = service.findDraft(created.value());
    requireDraft(unchanged.success() && unchanged.value().has_value()
            && unchanged.value()->workflowId == "execution-manifest-v1"
            && unchanged.value()->interfaceContracts.size() == 1,
        "A refused update leaves the stored draft alone");

    update.launchMode = RunLaunchMode::SavedExecutionProfile;
    update.workflowId = "execution-manifest-v1";
    requireDraft(!service.updateDraft(update).success(), "An update to a saved profile with contracts must be refused");

    // Dropping the contracts is what makes the move legal.
    update.workflowId = "";
    update.launchMode = RunLaunchMode::CustomRoster;
    update.interfaceContracts.clear();
    requireDraft(service.updateDraft(update).success(), "An update off the Manifest workflow without contracts is allowed");
    const auto moved = service.findDraft(created.value());
    requireDraft(moved.success() && moved.value().has_value() && moved.value()->workflowId.empty()
            && moved.value()->interfaceContracts.empty(),
        "The moved draft has no contracts");
}

void testRunDraftListOrderedByUpdatedDescending()
{
    RunDraftWorkspace workspace;
    RunDraftFixture fixture(workspace.database);
    SqliteRunDraftRepository drafts(fixture.database);
    RunDraftService service(drafts);

    CreateRunDraftRequest first;
    first.name = "First draft";
    first.projectId = fixture.project.id;
    first.title = "T1";
    first.task = "Task 1";
    first.workflowId = "wf";
    const auto firstId = service.createDraft(first);
    requireDraft(firstId.success(), "Could not create first draft");

    CreateRunDraftRequest second;
    second.name = "Second draft";
    second.projectId = fixture.project.id;
    second.title = "T2";
    second.task = "Task 2";
    second.workflowId = "wf";
    const auto secondId = service.createDraft(second);
    requireDraft(secondId.success(), "Could not create second draft");

    // updated_at_ms has millisecond resolution; without this, "touch" below
    // can land in the same millisecond as second's creation on a fast
    // machine, making the DESC-ordered list's tie outcome depend on the
    // random UUID id column instead of genuine recency (the same class of
    // flake documented for QualityEvaluation's ORDER BY created_at_ms,id).
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // Touch the first draft again so it becomes the most recently updated.
    UpdateRunDraftRequest touch;
    touch.id = firstId.value();
    touch.name = "First draft";
    touch.projectId = fixture.project.id;
    touch.title = "T1 edited";
    touch.task = "Task 1";
    touch.workflowId = "wf";
    requireDraft(service.updateDraft(touch).success(), "Could not touch first draft");

    const auto listed = service.listDrafts();
    requireDraft(listed.success(), "Could not list run drafts");
    requireDraft(listed.value().size() == 2, "Expected exactly two listed run drafts");
    requireDraft(listed.value()[0].id == firstId.value(),
        "Most recently updated draft should list first");
}

void testRunDraftRemove()
{
    RunDraftWorkspace workspace;
    RunDraftFixture fixture(workspace.database);
    SqliteRunDraftRepository drafts(fixture.database);
    RunDraftService service(drafts);

    CreateRunDraftRequest request;
    request.name = "Disposable draft";
    request.projectId = fixture.project.id;
    request.title = "T";
    request.task = "Task";
    request.workflowId = "wf";
    request.roster = {makeRosterEntry("agent-0", fixture.provider.id, fixture.modelA.id, 0)};
    const auto created = service.createDraft(request);
    requireDraft(created.success(), "Could not create disposable draft");

    requireDraft(service.removeDraft(created.value()).success(), "Could not remove run draft");
    const auto found = service.findDraft(created.value());
    requireDraft(found.success() && !found.value().has_value(),
        "Removed run draft should no longer be found");

    const auto removedAgain = service.removeDraft(created.value());
    requireDraft(!removedAgain.success(), "Removing an already-removed draft should fail");
}

void testRunDraftServiceRejectsEmptyName()
{
    RunDraftWorkspace workspace;
    RunDraftFixture fixture(workspace.database);
    SqliteRunDraftRepository drafts(fixture.database);
    RunDraftService service(drafts);

    CreateRunDraftRequest request;
    request.name = "";
    request.projectId = fixture.project.id;
    request.title = "T";
    request.task = "Task";
    request.workflowId = "wf";
    const auto created = service.createDraft(request);
    requireDraft(!created.success(), "Creating a draft with an empty name should fail");
    requireDraft(created.error()->code == "run_draft.empty_name",
        "Empty-name failure should use the run_draft.empty_name code");
}

// Explicit per-Run provider limits are saved with the draft (custom roster only) and come back as
// exactly the limits that were set; an unset draft keeps meaning "the Backend defaults".
void testRunDraftProviderLimitsRoundTripAndAreValidated()
{
    RunDraftWorkspace workspace;
    RunDraftFixture fixture(workspace.database);
    SqliteRunDraftRepository drafts(fixture.database);
    RunDraftService service(drafts);

    CreateRunDraftRequest request;
    request.name = "Draft with limits";
    request.projectId = fixture.project.id;
    request.title = "T";
    request.task = "Task";
    request.workflowId = "execution-manifest-v1";
    request.launchMode = RunLaunchMode::CustomRoster;
    request.roster = {makeRosterEntry("agent-0", fixture.provider.id, fixture.modelA.id, 0)};
    request.providerBudget.maxTurns = 40;
    request.providerBudget.maxInputTokens = 250'000;
    const auto created = service.createDraft(request);
    requireDraft(created.success(), "Could not create a draft with provider limits");

    auto found = service.findDraft(created.value());
    requireDraft(found.success() && found.value().has_value()
            && found.value()->providerBudget.maxTurns == 40
            && found.value()->providerBudget.maxInputTokens == 250'000
            && !found.value()->providerBudget.maxToolCalls.has_value()
            && !found.value()->providerBudget.maxOutputTokens.has_value(),
        "Draft provider limits did not round-trip as exactly the limits that were set");

    // An update replaces them, and clearing them returns to "no explicit limit".
    UpdateRunDraftRequest update;
    update.id = created.value();
    update.name = request.name;
    update.projectId = request.projectId;
    update.title = request.title;
    update.task = request.task;
    update.workflowId = request.workflowId;
    update.launchMode = RunLaunchMode::CustomRoster;
    update.roster = request.roster;
    update.providerBudget.maxToolCalls = 64;
    requireDraft(service.updateDraft(update).success(), "Could not update draft provider limits");
    found = service.findDraft(created.value());
    requireDraft(found.success() && found.value().has_value()
            && found.value()->providerBudget.maxToolCalls == 64
            && !found.value()->providerBudget.maxTurns.has_value(),
        "Updated draft provider limits must replace the old ones");
    update.providerBudget = {};
    requireDraft(service.updateDraft(update).success(), "Could not clear draft provider limits");
    found = service.findDraft(created.value());
    requireDraft(found.success() && found.value().has_value()
            && !hasExplicitProviderBudgetLimits(found.value()->providerBudget),
        "A draft saved without limits must read back as the Backend defaults");

    // A value above the Backend ceiling can never start: refused when saved.
    auto tooHigh = request;
    tooHigh.providerBudget = {};
    tooHigh.providerBudget.maxTurns = kProviderBudgetHardMaxTurns + 1;
    const auto refusedValue = service.createDraft(tooHigh);
    requireDraft(!refusedValue.success()
            && refusedValue.error()->code == "provider_budget.invalid_policy",
        "A draft with an out-of-range limit must be refused");

    // Limits belong to a per-Run roster, not to a saved-profile or Mock draft.
    auto wrongMode = request;
    wrongMode.launchMode = RunLaunchMode::Mock;
    wrongMode.roster.clear();
    const auto refusedMode = service.createDraft(wrongMode);
    requireDraft(!refusedMode.success()
            && refusedMode.error()->code == "run_draft.provider_budget_requires_roster",
        "Provider limits on a non-roster draft must be refused");
    UpdateRunDraftRequest wrongUpdate = update;
    wrongUpdate.launchMode = RunLaunchMode::SavedExecutionProfile;
    wrongUpdate.providerBudget.maxTurns = 10;
    const auto refusedUpdate = service.updateDraft(wrongUpdate);
    requireDraft(!refusedUpdate.success()
            && refusedUpdate.error()->code == "run_draft.provider_budget_requires_roster",
        "Provider limits on a non-roster draft update must be refused");
}

// ---------------------------------------------------------------------------------------------------------------
// PC-3B: draft groups on SQLite (schema v62)
// ---------------------------------------------------------------------------------------------------------------

void requireDraft(bool condition, const std::string& message)
{
    requireDraft(condition, message.c_str());
}

// A second, independent connection: the checks that must bypass the repository (schema guards, triggers, cascades,
// the migration fixture). Foreign keys are a per-connection setting, so it asks for them like the repository does.
class RawConnection {
public:
    explicit RawConnection(const fs::path& databasePath, bool foreignKeys = true)
    {
        const std::u8string path = databasePath.generic_u8string();
        const std::string utf8Path(reinterpret_cast<const char*>(path.data()), path.size());
        requireDraft(sqlite3_open_v2(utf8Path.c_str(), &handle_, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
            "Could not open the raw SQLite connection");
        sqlite3_busy_timeout(handle_, 5000);
        if (foreignKeys) requireDraft(exec("PRAGMA foreign_keys = ON;").empty(), "Could not enable foreign keys");
    }
    ~RawConnection() { sqlite3_close_v2(handle_); }
    RawConnection(const RawConnection&) = delete;
    RawConnection& operator=(const RawConnection&) = delete;

    // Empty on success, otherwise SQLite's message.
    std::string exec(const std::string& sql)
    {
        char* message = nullptr;
        const int result = sqlite3_exec(handle_, sql.c_str(), nullptr, nullptr, &message);
        std::string text = result == SQLITE_OK ? std::string {} : std::string(message != nullptr ? message : "error");
        sqlite3_free(message);
        return text;
    }


    long long scalar(const std::string& sql)
    {
        sqlite3_stmt* statement = nullptr;
        requireDraft(sqlite3_prepare_v2(handle_, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK,
            "Could not prepare: " + sql);
        const int step = sqlite3_step(statement);
        const long long value = step == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
        sqlite3_finalize(statement);
        requireDraft(step == SQLITE_ROW, "The query returned no row: " + sql);
        return value;
    }

private:
    sqlite3* handle_ {nullptr};
};

struct GroupFixture {
    RunDraftWorkspace workspace;
    RunDraftFixture fixture {workspace.database};
    SqliteRunDraftRepository repository {fixture.database};

    GroupFixture()
    {
        SqliteProjectRepository projects(fixture.database);
        const auto now = std::chrono::system_clock::now();
        Project other {"draft-project-2", "Other project", "Context", workspace.root / "project2",
            ProjectStatus::Active, now, now};
        requireDraft(projects.insert(other).success(), "Could not insert the second draft test project");
    }
};

rundraft_contract::Env groupEnv(const std::shared_ptr<GroupFixture>& fx)
{
    rundraft_contract::Env env;
    env.keepAlive = fx;
    env.repository = &fx->repository;
    env.projectId = fx->fixture.project.id;
    env.otherProjectId = "draft-project-2";
    env.providerId = fx->fixture.provider.id;
    env.modelA = fx->fixture.modelA.id;
    env.modelB = fx->fixture.modelB.id;
    return env;
}

void expectFailureContaining(const std::string& message, const std::string& needle, const std::string& what)
{
    requireDraft(!message.empty() && message.find(needle) != std::string::npos,
        what + ": expected a failure containing '" + needle + "' but got '" + message + "'");
}

// Raw INSERT of a draft with the given provenance cells (already SQL literals).
std::string rawDraftInsert(const std::string& id, const std::string& projectId, const std::string& provenance)
{
    return "INSERT INTO run_drafts (id,name,project_id,title,task,workflow_id,launch_mode,max_parallelism,"
           "created_at_ms,updated_at_ms,group_id,group_ordinal,plan_key,depends_on_json,"
           "created_content_fingerprint) VALUES ('" + id + "','n','" + projectId + "','t','k',"
           "'execution-manifest-v1',2,1,1,1," + provenance + ");";
}

const std::string kHex = "'" + std::string(64, 'a') + "'";

void testGroupContractOnSqlite()
{
    rundraft_contract::runRunDraftRepositoryContract([] {
        return groupEnv(std::make_shared<GroupFixture>());
    });
}

void testAGroupInsertRollsBackWhenALaterRosterRowFails()
{
    auto fx = std::make_shared<GroupFixture>();
    const auto env = groupEnv(fx);
    RawConnection raw(fx->workspace.database);
    const auto counts = [&] {
        return std::vector<long long> {raw.scalar("SELECT COUNT(*) FROM run_draft_groups"),
            raw.scalar("SELECT COUNT(*) FROM run_drafts"), raw.scalar("SELECT COUNT(*) FROM run_draft_assignments")};
    };
    requireDraft(counts() == (std::vector<long long> {0, 0, 0}), "setup: the tables start empty");

    auto d0 = rundraft_contract::groupDraft(env, "g1", "d0", 0, "model");
    auto d1 = rundraft_contract::groupDraft(env, "g1", "d1", 1, "view");
    auto d2 = rundraft_contract::groupDraft(env, "g1", "d2", 2, "docs");
    d0.roster = {rundraft_contract::rosterEntry(env, "lead", 0)};
    d1.roster = {rundraft_contract::rosterEntry(env, "lead", 0), rundraft_contract::rosterEntry(env, "worker", 1)};
    d2.roster = {rundraft_contract::rosterEntry(env, "lead", 0)};
    d2.roster[0].providerId = "provider-that-does-not-exist";   // the foreign key fails on the LAST roster row
    const auto refused = fx->repository.insertGroup(rundraft_contract::makeGroup(env, "g1", 'a'), {d0, d1, d2});
    requireDraft(!refused.success() && refused.error()->code.rfind("persistence.", 0) == 0,
        "A roster row that violates a foreign key must fail the whole group");
    requireDraft(counts() == (std::vector<long long> {0, 0, 0}),
        "...and leave no group row, no draft and no roster row behind");
    requireDraft(!fx->repository.findGroupById("g1").value().has_value()
            && !fx->repository.findGroupByIdempotencyKey(std::string(64, 'a')).value().has_value(),
        "...and the idempotency key stays free");

    d2.roster[0].providerId = env.providerId;
    requireDraft(fx->repository.insertGroup(rundraft_contract::makeGroup(env, "g1", 'a'), {d0, d1, d2}).success(),
        "The same group with a valid roster inserts afterwards");
    requireDraft(counts() == (std::vector<long long> {1, 3, 4}), "...with every row in place");
}

void testRemovingTheLastDraftIsAtomicWithItsGroup()
{
    auto fx = std::make_shared<GroupFixture>();
    const auto env = groupEnv(fx);
    RawConnection raw(fx->workspace.database);
    auto draft = rundraft_contract::groupDraft(env, "g1", "only", 0, "model");
    draft.roster = {rundraft_contract::rosterEntry(env, "lead", 0)};
    requireDraft(fx->repository.insertGroup(rundraft_contract::makeGroup(env, "g1", 'a'), {draft}).success(),
        "setup: a group with one draft");

    // Something makes the group row undeletable: the draft must stay as well.
    requireDraft(raw.exec("CREATE TRIGGER test_block_group_delete BEFORE DELETE ON run_draft_groups "
                          "BEGIN SELECT RAISE(ABORT, 'blocked by the test'); END;").empty(),
        "Could not install the blocking trigger");
    const auto refused = fx->repository.remove("only");
    requireDraft(!refused.success(), "Removal must fail when the group cannot be removed");
    requireDraft(raw.scalar("SELECT COUNT(*) FROM run_drafts") == 1
            && raw.scalar("SELECT COUNT(*) FROM run_draft_groups") == 1
            && raw.scalar("SELECT COUNT(*) FROM run_draft_assignments") == 1,
        "...and the draft, its roster row and the group are all still there");
    requireDraft(fx->repository.findById("only").value().has_value(), "...and the draft is readable");

    requireDraft(raw.exec("DROP TRIGGER test_block_group_delete;").empty(), "Could not drop the blocking trigger");
    requireDraft(fx->repository.remove("only").success(), "Removal works once nothing blocks it");
    requireDraft(raw.scalar("SELECT COUNT(*) FROM run_drafts") == 0 && raw.scalar("SELECT COUNT(*) FROM run_draft_groups") == 0
            && raw.scalar("SELECT COUNT(*) FROM run_draft_assignments") == 0,
        "...and takes the roster rows and the group with it");
}

void testTheSchemaRefusesWhatTheRepositoryNeverWrites()
{
    auto fx = std::make_shared<GroupFixture>();
    const auto env = groupEnv(fx);
    RawConnection raw(fx->workspace.database);
    requireDraft(fx->repository.insertGroup(rundraft_contract::makeGroup(env, "group-1", 'a', RunDraftGroupOrigin::ChatMessage),
                     {rundraft_contract::groupDraft(env, "group-1", "d0", 0, "model"),
                      rundraft_contract::groupDraft(env, "group-1", "d1", 1, "view", {"model"})})
                     .success(),
        "setup: a group of two");
    const std::string project = env.projectId;
    const std::string other = env.otherProjectId;

    // Provenance and group rows cannot be rewritten by anyone.
    expectFailureContaining(raw.exec("UPDATE run_drafts SET plan_key='x' WHERE id='d0';"), "run_draft_provenance_immutable", "plan_key");
    expectFailureContaining(raw.exec("UPDATE run_drafts SET group_id=NULL WHERE id='d0';"), "run_draft_provenance_immutable", "group_id");
    expectFailureContaining(raw.exec("UPDATE run_drafts SET group_ordinal=5 WHERE id='d0';"), "run_draft_provenance_immutable", "ordinal");
    expectFailureContaining(raw.exec("UPDATE run_drafts SET depends_on_json='[]' WHERE id='d1';"), "run_draft_provenance_immutable", "dependencies");
    expectFailureContaining(raw.exec("UPDATE run_drafts SET created_content_fingerprint='" + std::string(64, 'f') + "' WHERE id='d0';"),
        "run_draft_provenance_immutable", "fingerprint");
    expectFailureContaining(raw.exec("UPDATE run_draft_groups SET plan_title='x';"), "run_draft_group_immutable", "plan title");
    expectFailureContaining(raw.exec("UPDATE run_draft_groups SET idempotency_key='" + std::string(64, 'e') + "';"),
        "run_draft_group_immutable", "idempotency key");
    expectFailureContaining(raw.exec("UPDATE run_draft_groups SET id='other';"), "run_draft_group_immutable", "group id");
    // A draft and its group can never be moved into different projects, whichever of the two is updated.
    expectFailureContaining(raw.exec("UPDATE run_drafts SET project_id='" + other + "' WHERE id='d0';"),
        "run_draft_group_project_immutable", "moving a grouped draft to another project");
    expectFailureContaining(raw.exec("UPDATE run_draft_groups SET project_id='" + other + "' WHERE id='group-1';"),
        "run_draft_group_immutable", "moving a group to another project");
    requireDraft(raw.scalar("SELECT COUNT(*) FROM run_drafts WHERE project_id='" + project + "'") == 2
            && raw.scalar("SELECT COUNT(*) FROM run_draft_groups WHERE project_id='" + project + "'") == 1,
        "...and nothing moved");
    // A project id is never renamed: with a group around, the cascade would update the draft and the group one after
    // the other, which the guards refuse - so the rename fails as a whole and leaves the project as it was.
    expectFailureContaining(raw.exec("UPDATE projects SET id='renamed-project' WHERE id='" + project + "';"),
        "immutable", "renaming a project that owns a group");
    requireDraft(raw.scalar("SELECT COUNT(*) FROM projects WHERE id='" + project + "'") == 1,
        "...the project keeps its id");
    // An editable column of a grouped draft is still editable.
    requireDraft(raw.exec("UPDATE run_drafts SET title='edited' WHERE id='d0';").empty(), "title is editable");
    requireDraft(raw.exec("UPDATE run_drafts SET group_id=group_id WHERE id='d0';").empty(),
        "an update that does not change a provenance value is not refused");

    // A group with drafts cannot be deleted from under them.
    expectFailureContaining(raw.exec("DELETE FROM run_draft_groups WHERE id='group-1';"), "FOREIGN KEY constraint failed", "group delete");

    // Incomplete, mismatching or colliding provenance is refused on insert.
    expectFailureContaining(raw.exec(rawDraftInsert("x1", project, "'group-1',NULL,NULL,NULL,NULL")),
        "run_draft_provenance_incomplete", "group id only");
    expectFailureContaining(raw.exec(rawDraftInsert("x2", project, "NULL,2,'k',NULL,NULL")),
        "run_draft_provenance_incomplete", "ordinal and key without a group");
    expectFailureContaining(raw.exec(rawDraftInsert("x3", other, "'group-1',2,'docs','[]'," + kHex)),
        "run_draft_group_project_mismatch", "another project than the group's");
    expectFailureContaining(raw.exec(rawDraftInsert("x4", project, "'no-such-group',2,'docs','[]'," + kHex)),
        "run_draft_group_project_mismatch", "a missing group");
    expectFailureContaining(raw.exec(rawDraftInsert("x5", project, "'group-1',0,'docs','[]'," + kHex)),
        "UNIQUE constraint failed", "a repeated ordinal");
    expectFailureContaining(raw.exec(rawDraftInsert("x6", project, "'group-1',2,'MODEL','[]'," + kHex)),
        "UNIQUE constraint failed", "a plan key equal ignoring case");
    expectFailureContaining(raw.exec(rawDraftInsert("x7", project, "'group-1',-1,'docs','[]'," + kHex)),
        "CHECK constraint failed", "a negative ordinal");
    expectFailureContaining(raw.exec(rawDraftInsert("x8", project, "'group-1',2,'docs','[]','short'")),
        "CHECK constraint failed", "a short fingerprint");
    expectFailureContaining(raw.exec(rawDraftInsert("x9", project, "'group-1',2,'','[]'," + kHex)),
        "CHECK constraint failed", "an empty plan key");
    // The same ordinal in ANOTHER group, and a manual draft, are fine.
    requireDraft(raw.exec("INSERT INTO run_draft_groups (id,project_id,origin_kind,idempotency_key,plan_title,"
                          "plan_fingerprint,source_run_ids_json,created_at_ms) VALUES ('group-2','" + project
                          + "','manual_import','" + std::string(64, 'b') + "','t','" + std::string(64, 'c') + "','[]',1);").empty(),
        "A valid raw group row inserts");
    requireDraft(raw.exec(rawDraftInsert("y1", project, "'group-2',0,'model','[]'," + kHex)).empty(),
        "The same ordinal and key in another group is fine");
    requireDraft(raw.exec(rawDraftInsert("y2", project, "NULL,NULL,NULL,NULL,NULL")).empty(), "A manual draft is fine");
    requireDraft(raw.exec("UPDATE run_drafts SET project_id='" + other + "' WHERE id='y2';").empty(),
        "A manual draft may move to another project: only grouped drafts are pinned");
    requireDraft(raw.exec("UPDATE run_drafts SET project_id='" + project + "' WHERE id='y2';").empty(), "...and back");

    // Group rows are constrained too.
    const auto groupInsert = [&](const std::string& values) {
        return raw.exec("INSERT INTO run_draft_groups (id,project_id,origin_kind,idempotency_key,plan_title,plan_fingerprint,"
                        "source_conversation_id,source_message_id,source_message_fingerprint,context_fingerprint,"
                        "source_run_ids_json,created_at_ms) VALUES (" + values + ");");
    };
    const std::string k = "'" + std::string(64, 'd') + "'";
    const std::string fp = "'" + std::string(64, 'c') + "'";
    expectFailureContaining(groupInsert("'g','" + project + "','bogus'," + k + ",'t'," + fp + ",NULL,NULL,NULL,NULL,'[]',1"),
        "CHECK constraint failed", "an unknown origin");
    expectFailureContaining(groupInsert("'g','" + project + "','chat_message'," + k + ",'t'," + fp + ",NULL,NULL,NULL,NULL,'[]',1"),
        "CHECK constraint failed", "a chat group without a message");
    expectFailureContaining(groupInsert("'g','" + project + "','manual_import'," + k + ",'t'," + fp + ",'c','m'," + fp + ",NULL,'[]',1"),
        "CHECK constraint failed", "a manual group naming a message");
    expectFailureContaining(groupInsert("'g','" + project + "','chat_message'," + k + ",'t'," + fp + ",NULL,'m'," + fp + ",NULL,'[]',1"),
        "CHECK constraint failed", "a message without a conversation");
    expectFailureContaining(groupInsert("'g','" + project + "','manual_import','short','t'," + fp + ",NULL,NULL,NULL,NULL,'[]',1"),
        "CHECK constraint failed", "a short idempotency key");
    expectFailureContaining(groupInsert("'g','" + project + "','manual_import'," + k + ",''," + fp + ",NULL,NULL,NULL,NULL,'[]',1"),
        "CHECK constraint failed", "an empty plan title");
    expectFailureContaining(groupInsert("'g','" + project + "','manual_import'," + k + ",'t'," + fp + ",NULL,NULL,NULL,'x','[]',1"),
        "CHECK constraint failed", "a short context fingerprint");
    expectFailureContaining(groupInsert("'g','no-such-project','manual_import'," + k + ",'t'," + fp + ",NULL,NULL,NULL,NULL,'[]',1"),
        "FOREIGN KEY constraint failed", "an unknown project");
    // The context fingerprint belongs to a chat group only; a manual paste has no Chat request to attach it to.
    expectFailureContaining(groupInsert("'g','" + project + "','manual_import'," + k + ",'t'," + fp + ",NULL,NULL,NULL," + fp + ",'[]',1"),
        "CHECK constraint failed", "a manual group with a context fingerprint");
    requireDraft(groupInsert("'g-chat','" + project + "','chat_message'," + k + ",'t'," + fp + ",'c','m'," + fp + "," + fp + ",'[]',1").empty(),
        "A chat group may carry the context fingerprint of its request");
}

void testDeletingAProjectRemovesItsGroupsAndDrafts()
{
    auto fx = std::make_shared<GroupFixture>();
    const auto env = groupEnv(fx);
    RawConnection raw(fx->workspace.database);   // foreign keys on, like the repository's connection
    requireDraft(fx->repository.insertGroup(rundraft_contract::makeGroup(env, "group-1", 'a'),
                     {rundraft_contract::groupDraft(env, "group-1", "d0", 0, "model"),
                      rundraft_contract::groupDraft(env, "group-1", "d1", 1, "view", {"model"})})
                     .success(),
        "setup: a group");
    requireDraft(fx->repository.insert(rundraft_contract::manualDraft(env, "manual-1")).success(), "setup: a manual draft");
    const auto empty = raw.exec("DELETE FROM projects WHERE id='" + env.projectId + "';");
    requireDraft(empty.empty(), "Deleting a project that owns groups must not trip a foreign key: " + empty);
    requireDraft(raw.scalar("SELECT COUNT(*) FROM run_drafts") == 0 && raw.scalar("SELECT COUNT(*) FROM run_draft_groups") == 0,
        "The project's drafts and groups are gone with it");
    requireDraft(fx->repository.listAll().value().empty() && !fx->repository.findGroupById("group-1").value().has_value(),
        "...as the repository sees it");
}

void testDamagedProvenanceIsReportedNotRepaired()
{
    auto fx = std::make_shared<GroupFixture>();
    const auto env = groupEnv(fx);
    RawConnection raw(fx->workspace.database);
    requireDraft(fx->repository.insertGroup(rundraft_contract::makeGroup(env, "group-1", 'a'),
                     {rundraft_contract::groupDraft(env, "group-1", "d0", 0, "model"),
                      rundraft_contract::groupDraft(env, "group-1", "d1", 1, "view", {"model"})})
                     .success(),
        "setup: a group");
    // Only someone who removes the guards can damage the provenance; the repository must then say so.
    requireDraft(raw.exec("DROP TRIGGER run_drafts_provenance_immutable;").empty(), "Could not drop the guard");
    requireDraft(raw.exec("UPDATE run_drafts SET depends_on_json='not json' WHERE id='d1';").empty(), "Could not damage dependencies");
    const auto single = fx->repository.findById("d1");
    requireDraft(!single.success() && single.error()->code == "persistence.invalid_data",
        "findById must report damaged dependencies as invalid data");
    const auto everything = fx->repository.listAll();
    requireDraft(!everything.success() && everything.error()->code == "persistence.invalid_data",
        "listAll must report damaged dependencies as invalid data");
    const auto members = fx->repository.listGroupDrafts("group-1");
    requireDraft(!members.success() && members.error()->code == "persistence.invalid_data",
        "listGroupDrafts must report damaged dependencies as invalid data");
    // The update path only needs the provenance of the row it replaces: unrelated damage does not matter, damaged
    // provenance does.
    RunDraft attempt = rundraft_contract::manualDraft(env, "d1");
    requireDraft(!fx->repository.update(attempt).success(), "A draft with damaged provenance is not rewritten");

    requireDraft(raw.exec("UPDATE run_drafts SET depends_on_json='[\"model\"]' WHERE id='d1';").empty(), "repair the dependencies");
    requireDraft(raw.exec("UPDATE run_drafts SET group_ordinal=NULL WHERE id='d0';").empty(), "Could not damage the ordinal");
    const auto incomplete = fx->repository.findById("d0");
    requireDraft(!incomplete.success() && incomplete.error()->code == "persistence.invalid_data",
        "A row with only part of its provenance is invalid data");

    requireDraft(raw.exec("DROP TRIGGER run_draft_groups_reject_update;").empty(), "Could not drop the group guard");
    requireDraft(raw.exec("UPDATE run_draft_groups SET source_run_ids_json='nope';").empty(), "Could not damage the group");
    const auto damagedGroup = fx->repository.findGroupById("group-1");
    requireDraft(!damagedGroup.success() && damagedGroup.error()->code == "persistence.invalid_data",
        "Damaged source runs are invalid data");
}

// v62 adds run_draft_groups and the five provenance columns on run_drafts (PC-3B). Every existing draft stays a
// manual one with its roster and contracts; the migration is idempotent; the triggers and indexes survive a restart.
void testV61ToV62RunDraftGroupsMigration()
{
    RunDraftWorkspace workspace;
    std::string projectId;
    std::string providerId;
    std::string modelA;
    std::string modelB;
    {
        RunDraftFixture fixture(workspace.database);
        projectId = fixture.project.id;
        providerId = fixture.provider.id;
        modelA = fixture.modelA.id;
        modelB = fixture.modelB.id;
        SqliteRunDraftRepository drafts(fixture.database);
        rundraft_contract::Env env;
        env.projectId = projectId;
        env.providerId = providerId;
        env.modelA = modelA;
        env.modelB = modelB;
        auto manual = rundraft_contract::manualDraft(env, "manual-1");
        requireDraft(drafts.insert(manual).success(), "setup: a manual draft with a roster and contracts");
        auto plain = rundraft_contract::manualDraft(env, "manual-2");
        plain.roster.clear();
        plain.interfaceContracts.clear();
        plain.launchMode = RunLaunchMode::Mock;
        plain.workflowId.clear();
        requireDraft(drafts.insert(plain).success(), "setup: a plain draft");
    }

    // Rewind to a genuine v61 shape: run_drafts without the provenance columns, no groups table.
    {
        RawConnection raw(workspace.database, /*foreignKeys=*/false);
        const std::string rewind = R"sql(
PRAGMA foreign_keys = OFF;
BEGIN IMMEDIATE;
CREATE TABLE run_drafts_v61 (
    id TEXT PRIMARY KEY NOT NULL,
    name TEXT NOT NULL,
    project_id TEXT NOT NULL REFERENCES projects(id) ON UPDATE CASCADE ON DELETE CASCADE,
    title TEXT NOT NULL,
    task TEXT NOT NULL,
    workflow_id TEXT NOT NULL,
    launch_mode INTEGER NOT NULL CHECK(launch_mode IN (0,1,2)),
    execution_profile_id TEXT REFERENCES execution_profiles(id) ON UPDATE CASCADE ON DELETE SET NULL,
    max_parallelism INTEGER NOT NULL CHECK(max_parallelism > 0),
    workspace_path TEXT,
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL,
    verification_repair_policy TEXT NOT NULL DEFAULT 'one_bounded_wave'
        CHECK(verification_repair_policy IN ('one_bounded_wave','disabled')),
    provider_budget_json TEXT NOT NULL DEFAULT '{}',
    run_interface_contracts_json TEXT NOT NULL DEFAULT '[]'
);
INSERT INTO run_drafts_v61 (id,name,project_id,title,task,workflow_id,launch_mode,execution_profile_id,max_parallelism,
    workspace_path,created_at_ms,updated_at_ms,verification_repair_policy,provider_budget_json,run_interface_contracts_json)
SELECT id,name,project_id,title,task,workflow_id,launch_mode,execution_profile_id,max_parallelism,
    workspace_path,created_at_ms,updated_at_ms,verification_repair_policy,provider_budget_json,run_interface_contracts_json
FROM run_drafts;
DROP TABLE run_drafts;
ALTER TABLE run_drafts_v61 RENAME TO run_drafts;
CREATE INDEX run_drafts_updated_idx ON run_drafts(updated_at_ms, id);
CREATE INDEX run_drafts_project_idx ON run_drafts(project_id);
DROP TABLE run_draft_groups;
PRAGMA user_version = 61;
COMMIT;
)sql";
        const auto rewound = raw.exec(rewind);
        requireDraft(rewound.empty(), "Could not rewind the fixture to v61: " + rewound);
        requireDraft(raw.scalar("PRAGMA user_version") == 61, "The fixture is at v61");
        requireDraft(raw.scalar("SELECT COUNT(*) FROM pragma_table_info('run_drafts') WHERE name='group_id'") == 0
                && raw.scalar("SELECT COUNT(*) FROM sqlite_master WHERE name='run_draft_groups'") == 0,
            "The v61 fixture has neither the provenance columns nor the groups table");
    }

    rundraft_contract::Env env;
    env.projectId = projectId;
    env.providerId = providerId;
    env.modelA = modelA;
    env.modelB = modelB;
    {
        const auto migrated = SqliteDatabase::open(workspace.database);
        requireDraft(migrated.success(), "The v61 database must migrate through v62 to the latest schema");
        SqliteRunDraftRepository drafts(migrated.value());
        const auto manual = drafts.findById("manual-1");
        requireDraft(manual.success() && manual.value().has_value() && !manual.value()->provenance.has_value()
                && rundraft_contract::sameRoster(manual.value()->roster, rundraft_contract::manualDraft(env, "x").roster)
                && manual.value()->interfaceContracts.size() == 1,
            "An existing manual draft keeps its roster and contracts and has no provenance");
        const auto plain = drafts.findById("manual-2");
        requireDraft(plain.success() && plain.value().has_value() && !plain.value()->provenance.has_value()
                && plain.value()->launchMode == RunLaunchMode::Mock,
            "A plain draft is unchanged");
        requireDraft(drafts.listAll().value().size() == 2, "Both drafts survive");
        requireDraft(drafts.insertGroup(rundraft_contract::makeGroup(env, "group-1", 'a'),
                         {rundraft_contract::groupDraft(env, "group-1", "g0", 0, "model"),
                          rundraft_contract::groupDraft(env, "group-1", "g1", 1, "view", {"model"})})
                         .success(),
            "A group inserts into the migrated database");
        requireDraft(drafts.update(*manual.value()).success(), "A pre-existing draft still updates");
        RawConnection raw(workspace.database);
        requireDraft(raw.scalar("PRAGMA user_version") == 71, "The migrated database is at the latest schema");
        requireDraft(raw.scalar("SELECT COUNT(*) FROM pragma_foreign_key_check") == 0, "No foreign key is broken");
        requireDraft(raw.scalar("SELECT COUNT(*) FROM sqlite_master WHERE type='trigger' AND name LIKE 'run_draft%'") == 6
                && raw.scalar("SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name LIKE 'run_drafts_group_%'") == 2,
            "The six guards (the five of v62 and the follow-up origin guard of v66) and the two unique indexes exist");
    }
    {
        const auto reopened = SqliteDatabase::open(workspace.database);
        requireDraft(reopened.success(), "The post-v62 database reopens");
        SqliteRunDraftRepository drafts(reopened.value());
        requireDraft(drafts.listGroupDrafts("group-1").value().size() == 2, "The group survives a restart");
    }

    // Re-run the migration over a database that already has everything: it must neither fail nor lose the group.
    {
        RawConnection raw(workspace.database, /*foreignKeys=*/false);
        requireDraft(raw.exec("PRAGMA user_version = 61;").empty(), "Could not rewind only the version");
    }
    {
        const auto again = SqliteDatabase::open(workspace.database);
        requireDraft(again.success(), "Re-running the v62 migration over existing structures must succeed");
        SqliteRunDraftRepository drafts(again.value());
        const auto group = drafts.findGroupById("group-1");
        const auto members = drafts.listGroupDrafts("group-1");
        requireDraft(group.success() && group.value().has_value() && members.success() && members.value().size() == 2
                && members.value()[1].provenance.has_value() && members.value()[1].provenance->dependsOn.size() == 1,
            "A re-run migration keeps the group and the provenance of its drafts");
        RawConnection raw(workspace.database);
        requireDraft(raw.scalar("PRAGMA user_version") == 71, "...and ends at the latest schema again");
    }
}

// A write is always the confirm of a preview.
ai::common::Result<RunDraftPlanCreation> confirmMessage(RunDraftPlanService& service, const std::string& conversationId,
    const std::string& messageId, const std::optional<RunDraftId>& rosterTemplate = std::nullopt)
{
    const auto preview = service.previewFromMessage(conversationId, messageId);
    if (!preview.success()) return ai::common::Result<RunDraftPlanCreation>::failure(*preview.error());
    return service.createFromPreview(preview.value().previewToken, rosterTemplate);
}

// PC-3C on the real SQLite adapters: the service's group, drafts and roster copy survive the foreign keys, CHECKs and
// triggers of v62, the created-content fingerprint it computes is accepted by the repository, and a repeat or an
// edited message behaves as designed.
void testThePlanServiceWritesRealGroupsOnSqlite()
{
    auto fx = std::make_shared<GroupFixture>();
    const auto env = groupEnv(fx);
    RawConnection raw(fx->workspace.database);

    SqliteProjectRepository projects(fx->fixture.database);
    SqliteChatRepository chats(fx->fixture.database);
    ChatConversation conversation;
    conversation.id = "conversation-1";
    conversation.projectId = env.projectId;
    conversation.providerId = env.providerId;
    conversation.modelId = env.modelA;
    conversation.title = "Plan";
    conversation.createdAt = std::chrono::system_clock::now();
    conversation.updatedAt = conversation.createdAt;
    requireDraft(chats.insertConversation(conversation).success(), "setup: the conversation");

    const std::string plan = R"json({
      "schemaVersion": "next-run-plan.v1", "title": "History page",
      "sharedContracts": [{"id": "S1", "kind": "interface_contract", "title": "Loss entry",
                           "text": "property real share", "appliesToPaths": ["qml/models"]}],
      "drafts": [
        {"key": "model", "title": "Model task", "task": "Create the model.", "sharedContractIds": ["S1"]},
        {"key": "view", "title": "View task", "task": "Create the view.", "dependsOn": ["model"], "sharedContractIds": ["S1"]}]
    })json";
    ChatMessage message;
    message.id = "message-1";
    message.conversationId = conversation.id;
    message.role = ChatRole::Assistant;
    message.status = ChatMessageStatus::Sent;
    message.content = "Plan:\n\n```next-run-plan.v1\n" + plan + "\n```\n";
    message.createdAt = conversation.createdAt;
    requireDraft(chats.insertMessage(message).success(), "setup: the message");

    RunDraft tpl = rundraft_contract::groupDraft(env, "unused", "template-1", 0, "unused");
    tpl.provenance.reset();
    tpl.name = "Roster template";
    tpl.maxParallelism = 5;
    tpl.verificationRepairPolicy = VerificationRepairPolicy::Disabled;
    tpl.providerBudget.maxTurns = 21;
    tpl.roster = {rundraft_contract::rosterEntry(env, "lead", 0), rundraft_contract::rosterEntry(env, "worker", 1)};
    requireDraft(fx->repository.insert(tpl).success(), "setup: the template draft");

    // The production adapter: Run ids are looked up through the real SQLite Run repository (none exist here).
    SqliteRunRepository runRepository(fx->fixture.database);
    RunRepositorySourceLookup runLookup(runRepository);
    RunDraftPlanService service(fx->repository, projects, chats, runLookup);
    const auto created = confirmMessage(service, conversation.id, message.id, std::string("template-1"));
    requireDraft(created.success(), "A plan creates on SQLite: " + (created.error() ? created.error()->message : ""));
    requireDraft(!created.value().alreadyExisted && created.value().drafts.size() == 2
            && created.value().group.sourceMessageFingerprint.has_value()
            && created.value().group.origin == RunDraftGroupOrigin::ChatMessage,
        "Two drafts in a chat-message group");
    requireDraft(raw.scalar("SELECT COUNT(*) FROM run_draft_groups") == 1
            && raw.scalar("SELECT COUNT(*) FROM run_drafts") == 3
            && raw.scalar("SELECT COUNT(*) FROM run_draft_assignments") == 6,
        "...one group, the template and the two new drafts, and a copied roster row for each");

    const auto stored = fx->repository.listGroupDrafts(created.value().group.id);
    requireDraft(stored.success() && stored.value().size() == 2, "The group's drafts read back");
    for (const RunDraft& draft : stored.value()) {
        requireDraft(draft.roster.size() == 2 && draft.maxParallelism == 5
                && draft.verificationRepairPolicy == VerificationRepairPolicy::Disabled
                && draft.providerBudget.maxTurns == std::optional<std::uint64_t>(21)
                && draft.interfaceContracts.size() == 1 && draft.interfaceContracts[0].id == "S1"
                && draft.provenance.has_value()
                && draft.provenance->createdContentFingerprint
                    == nextRunPlanDraftContentFingerprint(draft.title, draft.task, draft.interfaceContracts,
                        draft.provenance->dependsOn),
            "A stored draft carries the template's roster and choices, its contracts and a sealed fingerprint");
    }
    requireDraft(stored.value()[1].provenance->dependsOn == std::vector<std::string> {"model"},
        "...and the dependency");

    // Source Runs go through the real Run repository: an unknown id is refused before anything is written.
    {
        const auto previewForRuns = service.previewFromPaste(conversation.id, plan);
        requireDraft(previewForRuns.success(), "setup: a paste preview for the source-run check");
        const long long groupsBefore = raw.scalar("SELECT COUNT(*) FROM run_draft_groups");
        const auto unknownRun = service.createFromPreview(
            previewForRuns.value().previewToken, std::nullopt, std::vector<RunId> {"no-such-run"});
        requireDraft(!unknownRun.success() && unknownRun.error()->code == "run_draft_plan.source_run_not_found"
                && raw.scalar("SELECT COUNT(*) FROM run_draft_groups") == groupsBefore,
            "An unknown source Run is refused by the real adapter and nothing is written");
    }

    // The same message again: the same group, nothing written.
    const auto again = confirmMessage(service, conversation.id, message.id);
    requireDraft(again.success() && again.value().alreadyExisted && again.value().group.id == created.value().group.id
            && raw.scalar("SELECT COUNT(*) FROM run_drafts") == 3 && raw.scalar("SELECT COUNT(*) FROM run_draft_assignments") == 6,
        "A repeat returns the stored group and touches nothing");

    // An edited message is another content fingerprint: a second group.
    message.content += "\nEdited.";
    requireDraft(chats.updateMessage(message).success(), "setup: edit the message");
    const auto edited = confirmMessage(service, conversation.id, message.id);
    requireDraft(edited.success() && !edited.value().alreadyExisted && edited.value().group.id != created.value().group.id
            && raw.scalar("SELECT COUNT(*) FROM run_draft_groups") == 2 && raw.scalar("SELECT COUNT(*) FROM run_drafts") == 5,
        "An edited message creates a second group (with empty rosters)");

    // A manual import: its own origin and key, with no message columns.
    const auto preview = service.previewFromPaste(conversation.id, plan);
    requireDraft(preview.success(), "setup: a paste preview");
    const auto imported = service.createFromPreview(preview.value().previewToken);
    requireDraft(imported.success() && imported.value().group.origin == RunDraftGroupOrigin::ManualImport
            && raw.scalar("SELECT COUNT(*) FROM run_draft_groups WHERE origin_kind='manual_import' AND source_message_id IS NULL") == 1,
        "A manual import is stored without message provenance");

    // Removing every draft of a group removes the group; the same source can then be imported afresh.
    for (const RunDraft& draft : stored.value()) requireDraft(fx->repository.remove(draft.id).success(), "setup: remove");
    requireDraft(!fx->repository.findGroupById(created.value().group.id).value().has_value(), "The emptied group is gone");

    // An archived project takes no new drafts, and nothing is written.
    auto project = projects.findById(env.projectId).value().value();
    project.status = ProjectStatus::Archived;
    requireDraft(projects.update(project).success(), "setup: archive the project");
    const long long draftsBefore = raw.scalar("SELECT COUNT(*) FROM run_drafts");
    const auto refused = confirmMessage(service, conversation.id, message.id);
    requireDraft(!refused.success() && refused.error()->code == "run_draft_plan.project_inactive"
            && raw.scalar("SELECT COUNT(*) FROM run_drafts") == draftsBefore,
        "An archived project is refused before anything is written");
}

} // namespace

int runRunDraftRepositoryTests()
{
    testGroupContractOnSqlite();
    testAGroupInsertRollsBackWhenALaterRosterRowFails();
    testRemovingTheLastDraftIsAtomicWithItsGroup();
    testTheSchemaRefusesWhatTheRepositoryNeverWrites();
    testDeletingAProjectRemovesItsGroupsAndDrafts();
    testDamagedProvenanceIsReportedNotRepaired();
    testV61ToV62RunDraftGroupsMigration();
    testThePlanServiceWritesRealGroupsOnSqlite();
    testRunDraftProviderLimitsRoundTripAndAreValidated();
    testRunDraftRoundTrip();
    testRunDraftUpdateReplacesRoster();
    testRunDraftInterfaceContractsAreValidatedNormalizedAndReplacedByUpdate();
    testRunDraftInterfaceContractsRequireAManifestRoster();
    testRunDraftListOrderedByUpdatedDescending();
    testRunDraftRemove();
    testRunDraftServiceRejectsEmptyName();
    return 0;
}
