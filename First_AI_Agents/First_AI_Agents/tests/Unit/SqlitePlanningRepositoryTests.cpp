#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Contracts for the run draft repository that the shared repository contract does not reach: the typed errors of a
// single insert and its rollback, the roster read order, and rows that were damaged outside the repository.
namespace {
using namespace ai::application;
using namespace ai::infrastructure;
namespace fs = std::filesystem;
using Time = std::chrono::system_clock::time_point;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

Time at(std::int64_t milliseconds)
{
    return Time(std::chrono::milliseconds(milliseconds));
}

struct Workspace {
    fs::path root = fs::temp_directory_path() / ("aio-sqlite-planning-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path databasePath = root / "state.sqlite3";

    Workspace() { fs::create_directories(root); }
    ~Workspace() { std::error_code ignored; fs::remove_all(root, ignored); }

    std::shared_ptr<SqliteDatabase> open() const
    {
        auto result = SqliteDatabase::open(databasePath);
        require(result.success(), "Could not open test database");
        return std::move(result.value());
    }

    // Raw access for corrupting rows the public API would never write.
    void sql(const char* statement) const
    {
        sqlite3* handle = nullptr;
        const auto path = databasePath.generic_u8string();
        const auto opened = sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()),
            &handle, SQLITE_OPEN_READWRITE, nullptr);
        if (opened != SQLITE_OK) {
            if (handle) sqlite3_close(handle);
            throw std::runtime_error("Could not open test SQL connection");
        }
        const int result = sqlite3_exec(handle, statement, nullptr, nullptr, nullptr);
        sqlite3_close(handle);
        require(result == SQLITE_OK, "Test SQL statement failed");
    }
};

// One project, one provider and one model: the rows a draft and its roster may reference.
void addProjectAndCatalog(const std::shared_ptr<SqliteDatabase>& database, const Workspace& workspace)
{
    SqliteProjectRepository projects(database);
    Project project;
    project.id = "project-1";
    project.name = "Project";
    project.rootPath = workspace.root / "project";
    project.status = ProjectStatus::Active;
    project.createdAt = at(500);
    project.updatedAt = at(500);
    require(projects.insert(project).success(), "Could not insert a project");
    SqliteProviderRepository providers(database);
    Provider provider;
    provider.id = "provider-1";
    provider.name = "Provider";
    provider.kind = "openai_compatible";
    provider.baseUrl = "https://example.invalid/v1";
    provider.enabled = true;
    provider.createdAt = at(500);
    provider.updatedAt = at(500);
    require(providers.insert(provider).success(), "Could not insert a provider");
    SqliteModelRepository models(database);
    Model model;
    model.id = "model-1";
    model.providerId = "provider-1";
    model.apiModelId = "api-1";
    model.displayName = "Model";
    model.contextWindowTokens = 200'000;
    model.maxOutputTokens = 16'384;
    model.inputPriceMicrosPerMillionTokens = 3'000'000;
    model.outputPriceMicrosPerMillionTokens = 15'000'000;
    model.enabled = true;
    model.createdAt = at(500);
    model.updatedAt = at(500);
    require(models.insert(model).success(), "Could not insert a model");
}

StageModelAssignment makeEntry(std::string stage, std::uint32_t position)
{
    StageModelAssignment value;
    value.stageType = std::move(stage);
    value.providerId = "provider-1";
    value.modelId = "model-1";
    value.timeout = std::chrono::milliseconds(60'000);
    value.agentName = "Agent";
    value.role = "implementation";
    value.instructions = "Do the thing.";
    value.position = position;
    value.maxAttempts = 1;
    return value;
}

RunDraft makeDraft(std::string id, std::int64_t updatedMs, std::vector<StageModelAssignment> roster = {})
{
    RunDraft value;
    value.id = std::move(id);
    value.name = "Draft";
    value.projectId = "project-1";
    value.title = "Title";
    value.task = "Task";
    value.workflowId = "planning-test-workflow";
    value.roster = std::move(roster);
    value.createdAt = at(1000);
    value.updatedAt = at(updatedMs);
    return value;
}

std::vector<std::string> draftIds(const SqliteRunDraftRepository& drafts)
{
    const auto listed = drafts.listAll();
    require(listed.success(), "Could not list run drafts");
    std::vector<std::string> result;
    for (const auto& value : listed.value()) result.push_back(value.id);
    return result;
}

void testRunDraftInsertReportsTypedErrorsAndRollsBack()
{
    Workspace workspace;
    const auto database = workspace.open();
    addProjectAndCatalog(database, workspace);
    SqliteRunDraftRepository drafts(database);
    const RunDraft first = makeDraft("draft-1", 2000, {makeEntry("architect", 0)});
    const auto firstSaved = drafts.insert(first);
    require(firstSaved.success(), ("Could not insert a draft: " + (firstSaved.success() ? std::string() : firstSaved.error()->code + " " + firstSaved.error()->message)).c_str());

    const auto repeated = drafts.insert(makeDraft("draft-1", 3000));
    require(!repeated.success() && repeated.error()->code == "run_draft.duplicate_id",
        "A repeated draft id must report run_draft.duplicate_id");
    const auto stillFirst = drafts.findById("draft-1");
    require(stillFirst.success() && stillFirst.value().has_value() && stillFirst.value()->roster.size() == 1
            && stillFirst.value()->updatedAt == at(2000),
        "A refused insert altered the existing draft");

    // A failure after the draft row was written must take the row back out.
    const auto twice = drafts.insert(
        makeDraft("draft-2", 3000, {makeEntry("architect", 0), makeEntry("architect", 1)}));
    require(!twice.success() && twice.error()->code == "run_draft.duplicate_stage_type",
        "A repeated stage type in the roster must report run_draft.duplicate_stage_type");
    const auto absent = drafts.findById("draft-2");
    require(absent.success() && !absent.value().has_value(), "A failed roster insert must not leave the draft row behind");

    StageModelAssignment unknownModel = makeEntry("architect", 0);
    unknownModel.modelId = "model-unknown";
    const auto orphanModel = drafts.insert(makeDraft("draft-3", 3000, {unknownModel}));
    require(!orphanModel.success() && orphanModel.error()->code == "persistence.write_failed",
        "A roster entry with an unknown model must be refused as a failed write");
    RunDraft orphanProject = makeDraft("draft-4", 3000);
    orphanProject.projectId = "project-unknown";
    const auto refusedProject = drafts.insert(orphanProject);
    require(!refusedProject.success() && refusedProject.error()->code == "persistence.write_failed",
        "A draft of an unknown project must be refused as a failed write");

    RunDraft grouped = makeDraft("draft-5", 3000);
    grouped.provenance = RunDraftProvenance {};
    const auto refusedGroup = drafts.insert(grouped);
    require(!refusedGroup.success() && refusedGroup.error()->code == "run_draft.group_insert_required",
        "A draft that belongs to a group must report run_draft.group_insert_required");

    require(draftIds(drafts) == std::vector<std::string> {"draft-1"}, "Refused inserts must not add rows");
    // The same ids are usable once the cause is gone: nothing was left half-written and no transaction stayed open.
    require(drafts.insert(makeDraft("draft-2", 3000, {makeEntry("architect", 0), makeEntry("documentation", 1)})).success(),
        "The connection must be usable after refused inserts");
    const auto second = drafts.findById("draft-2");
    require(second.success() && second.value().has_value() && second.value()->roster.size() == 2,
        "A draft inserted after a refusal must store its whole roster");
}

void testRunDraftRosterIsReadInPositionOrder()
{
    Workspace workspace;
    const auto database = workspace.open();
    addProjectAndCatalog(database, workspace);
    SqliteRunDraftRepository drafts(database);
    // Positions are unique per draft and run against the alphabetical order of the stage types.
    const RunDraft draft = makeDraft("draft-1", 2000, {
        makeEntry("architect", 2), makeEntry("zeta", 5), makeEntry("documentation", 1),
        makeEntry("alpha", 4), makeEntry("implementation", 0), makeEntry("beta", 3)});
    const auto saved = drafts.insert(draft);
    require(saved.success(), ("Could not insert a draft: " + (saved.success() ? std::string() : saved.error()->code + " " + saved.error()->message)).c_str());
    const auto stored = drafts.findById("draft-1");
    require(stored.success() && stored.value().has_value(), "Could not read the draft");
    std::vector<std::string> order;
    for (const auto& entry : stored.value()->roster) order.push_back(entry.stageType);
    require(order == std::vector<std::string> {"implementation", "documentation", "architect", "beta", "alpha", "zeta"},
        "A roster must be read in position order");
    const auto listed = drafts.listAll();
    require(listed.success() && listed.value().size() == 1 && listed.value()[0].roster.size() == 6
            && listed.value()[0].roster[0].stageType == "implementation",
        "A listing must attach the roster in the same order");
}

void testCorruptRunDraftRowsFailClosed()
{
    Workspace workspace;
    {
        const auto database = workspace.open();
        addProjectAndCatalog(database, workspace);
        SqliteRunDraftRepository drafts(database);
        for (const char* id : {"draft-good", "draft-bad-launch", "draft-bad-parallelism", "draft-bad-policy",
                 "draft-bad-budget"}) {
            require(drafts.insert(makeDraft(id, 2000, {makeEntry("architect", 0)})).success(), "Could not insert drafts");
        }
    }
    // The schema refuses these values, so the checks are switched off on the corrupting connection only.
    workspace.sql("PRAGMA ignore_check_constraints=1; UPDATE run_drafts SET launch_mode=3 WHERE id='draft-bad-launch';");
    workspace.sql("PRAGMA ignore_check_constraints=1; UPDATE run_drafts SET max_parallelism=0 WHERE id='draft-bad-parallelism';");
    workspace.sql("PRAGMA ignore_check_constraints=1; UPDATE run_drafts SET verification_repair_policy='sometimes' "
                  "WHERE id='draft-bad-policy';");
    workspace.sql("UPDATE run_drafts SET provider_budget_json='{broken' WHERE id='draft-bad-budget';");

    const auto database = workspace.open();
    SqliteRunDraftRepository drafts(database);
    const auto good = drafts.findById("draft-good");
    require(good.success() && good.value().has_value() && good.value()->roster.size() == 1,
        "A valid draft must stay readable");
    for (const char* id : {"draft-bad-launch", "draft-bad-parallelism", "draft-bad-policy", "draft-bad-budget"}) {
        const auto bad = drafts.findById(id);
        require(!bad.success() && bad.error()->code == "persistence.invalid_data",
            "A damaged draft row must fail closed on findById");
    }
    const auto everything = drafts.listAll();
    require(!everything.success(), "A listing that contains a damaged draft must fail rather than skip it");
}
} // namespace

int runSqlitePlanningRepositoryTests()
{
    testRunDraftInsertReportsTypedErrorsAndRollsBack();
    testRunDraftRosterIsReadInPositionOrder();
    testCorruptRunDraftRowsFailClosed();
    return 0;
}
