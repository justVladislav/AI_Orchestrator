#include "FollowUpPlanRequestRepositoryContract.hpp"
#include "FollowUpOldV66Schema.hpp"

#include "ai/application/agents/ModelExecutionTypes.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace fc = followup_contract;
using namespace ai::application;
using namespace ai::infrastructure;
using namespace followup_test;
namespace fs = std::filesystem;
using S = FollowUpPlanRequestState;
using E = FollowUpPlanEvent;

void require(bool condition, const std::string& message)
{
    check(condition, message);
}

struct Workspace {
    fs::path root {fs::temp_directory_path()
        / ("ai-orchestrator-follow-up-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
    fs::path database {root / "orchestrator.sqlite3"};
    ~Workspace()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

// A second, independent connection for everything that must bypass the repository: schema guards, restrict keys, damaged rows,
// the old-database fixture. Foreign keys are a per-connection setting, so it asks for them like the repository does.
class RawConnection {
public:
    explicit RawConnection(const fs::path& databasePath, bool foreignKeys = true)
    {
        const std::u8string path = databasePath.generic_u8string();
        const std::string utf8Path(reinterpret_cast<const char*>(path.data()), path.size());
        require(sqlite3_open_v2(utf8Path.c_str(), &handle_, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK, "Could not open the raw connection");
        sqlite3_busy_timeout(handle_, 5000);
        require(exec(foreignKeys ? "PRAGMA foreign_keys = ON;" : "PRAGMA foreign_keys = OFF;").empty(), "Could not set foreign keys");
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

    std::string text(const std::string& sql)
    {
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(handle_, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK, "Could not prepare: " + sql + " (" + sqlite3_errmsg(handle_) + ")");
        const int step = sqlite3_step(statement);
        std::string value;
        if (step == SQLITE_ROW && sqlite3_column_text(statement, 0) != nullptr) {
            value = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        }
        sqlite3_finalize(statement);
        require(step == SQLITE_ROW || step == SQLITE_DONE, "The query failed: " + sql);
        return value;
    }

    long long scalar(const std::string& sql)
    {
        const std::string value = text(sql);
        return value.empty() ? -1 : std::stoll(value);
    }

    // The first column of every row.
    std::vector<std::string> column(const std::string& sql)
    {
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(handle_, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK, "Could not prepare: " + sql);
        std::vector<std::string> values;
        while (sqlite3_step(statement) == SQLITE_ROW) {
            values.push_back(sqlite3_column_text(statement, 0) != nullptr ? reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)) : "");
        }
        sqlite3_finalize(statement);
        return values;
    }

    // The column names of a table, in order.
    std::vector<std::string> columnsOf(const std::string& table)
    {
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(handle_, ("PRAGMA table_info(" + table + ");").c_str(), -1, &statement, nullptr) == SQLITE_OK, "table_info");
        std::vector<std::string> names;
        while (sqlite3_step(statement) == SQLITE_ROW) names.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)));
        sqlite3_finalize(statement);
        return names;
    }

private:
    sqlite3* handle_ {nullptr};
};

bool contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

std::shared_ptr<SqliteDatabase> openDatabase(const fs::path& path)
{
    const auto opened = SqliteDatabase::open(path);
    require(opened.success(), "Could not open the follow-up test database: " + (opened.success() ? std::string() : opened.error()->message));
    return opened.value();
}

void seedProject(const std::shared_ptr<SqliteDatabase>& database, const ProjectId& projectId)
{
    SqliteProjectRepository projects(database);
    const auto existing = projects.findById(projectId);
    if (existing.success() && existing.value().has_value()) return;
    const auto now = std::chrono::system_clock::now();
    Project project {projectId, "Follow-up project " + projectId, "Context", database->path().parent_path() / projectId,
        ProjectStatus::Active, now, now};
    require(projects.insert(project).success(), "Could not seed the project");
}

void seedRun(const std::shared_ptr<SqliteDatabase>& database, const RunId& runId, const ProjectId& projectId)
{
    seedProject(database, projectId);
    SqliteRunRepository runs(database);
    const auto existing = runs.findRun(runId);
    if (existing.success() && existing.value().has_value()) return;
    Run run;
    run.id = runId;
    run.projectId = projectId;
    run.title = "Follow-up source";
    run.task = "Resolved outcome";
    run.workflowId = kExecutionManifestWorkflowId;
    run.status = RunStatus::Completed;
    run.createdAt = std::chrono::system_clock::now();
    require(runs.insertRun(run).success(), "Could not seed the Run");
}

void seedCatalog(const std::shared_ptr<SqliteDatabase>& database, const Catalog& catalog)
{
    const auto now = std::chrono::system_clock::now();
    SqliteProviderRepository providers(database);
    Provider provider {catalog.providerId, "Provider " + catalog.providerId, catalog.providerKind, catalog.baseUrl, true,
        catalog.credentialReference, now, now};
    require(providers.insert(provider).success(), "Could not seed the provider");
    SqliteModelRepository models(database);
    Model model {catalog.modelId, catalog.providerId, "model-api", "Model " + catalog.modelId, 200'000, 64'000, 3'000'000,
        15'000'000, 3'750'000, 300'000, true, true, now, now};
    require(models.insert(model).success(), "Could not seed the model");
}

const std::string kProjectOne = "fu-project-1";
const std::string kProjectTwo = "fu-project-2";

struct FollowUpFixture {
    std::shared_ptr<Workspace> workspace;
    std::shared_ptr<SqliteDatabase> database;
    SqliteFollowUpPlanRepository repository;
    SqliteRunDraftRepository drafts;
    Catalog catalog;

    // A fixture may be given the workspace of another one: destroying it closes ITS connection and keeps the files.
    explicit FollowUpFixture(std::shared_ptr<Workspace> shared = std::make_shared<Workspace>())
        : workspace(std::move(shared))
        , database(openDatabase(workspace->database))
        , repository(database)
        , drafts(database)
    {
        seedProject(database, kProjectOne);
        seedProject(database, kProjectTwo);
        seedCatalog(database, catalog);
    }

    [[nodiscard]] fc::Env env(const std::shared_ptr<FollowUpFixture>& self)
    {
        fc::Env value;
        value.keepAlive = self;
        value.repository = &repository;
        value.confirm = &repository;
        value.drafts = &drafts;
        value.catalog = catalog;
        value.projectId = kProjectOne;
        value.otherProjectId = kProjectTwo;
        value.ensureRun = [self](const std::string& runId, const std::string& projectId) { seedRun(self->database, runId, projectId); };
        value.moveRun = [self](const std::string& runId, const std::string& projectId) {
            RawConnection raw(self->workspace->database);
            const std::string failure = raw.exec("UPDATE runs SET project_id = '" + projectId + "' WHERE id = '" + runId + "';");
            require(failure.empty(), "Could not move the Run: " + failure);
        };
        return value;
    }
};

std::shared_ptr<FollowUpFixture> newFixture() { return std::make_shared<FollowUpFixture>(); }

// The repositories over a database that already exists (the migrated one): nothing is seeded.
struct OpenedDatabase {
    fs::path path;
    std::shared_ptr<SqliteDatabase> database;
    SqliteFollowUpPlanRepository repository;
    SqliteRunDraftRepository drafts;
    Catalog catalog;

    OpenedDatabase(fs::path databasePath, std::shared_ptr<SqliteDatabase> opened)
        : path(std::move(databasePath))
        , database(std::move(opened))
        , repository(database)
        , drafts(database)
    {
    }

    [[nodiscard]] fc::Env env(const std::shared_ptr<OpenedDatabase>& self)
    {
        fc::Env value;
        value.keepAlive = self;
        value.repository = &repository;
        value.confirm = &repository;
        value.drafts = &drafts;
        value.catalog = catalog;
        value.projectId = kProjectOne;
        value.otherProjectId = kProjectTwo;
        value.ensureRun = [self](const std::string& runId, const std::string& projectId) { seedRun(self->database, runId, projectId); };
        value.moveRun = [](const std::string&, const std::string&) {};
        return value;
    }
};

// ---- the contract, and the facts it cannot see ------------------------------------------------------------------------------

void testTheContractOnSqlite()
{
    fc::runFollowUpPlanRequestRepositoryContract([] {
        auto fixture = newFixture();
        return fixture->env(fixture);
    });
}

// A raw copy of a stored request under another id and Run, with extra assignments: the way to make a row the repository would
// never write. A TEMP table has no constraints, triggers or keys, so the copy exercises the real table's guards on INSERT.
std::string copyRequest(RawConnection& raw, const std::string& fromId, const std::string& newId, const std::string& newRun,
    const std::string& assignments = {})
{
    return raw.exec("DROP TABLE IF EXISTS temp.tmp_fu; CREATE TEMP TABLE tmp_fu AS SELECT * FROM follow_up_plan_requests WHERE id = '"
        + fromId + "'; UPDATE tmp_fu SET id = '" + newId + "', source_run_id = '" + newRun + "'"
        + (assignments.empty() ? std::string() : ", " + assignments) + "; INSERT INTO follow_up_plan_requests SELECT * FROM tmp_fu; DROP TABLE tmp_fu;");
}

void testTheSchemaKeepsTheIdentityTheResultAndTheEdges()
{
    auto fx = newFixture();
    const auto env = fx->env(fx);
    // A second provider and model, so that an identity update to them is a legal foreign key.
    Catalog other;
    other.providerId = "provider-2";
    other.modelId = "model-2";
    seedCatalog(fx->database, other);
    fx->env(fx).ensureRun("run-spare", kProjectOne);
    fx->env(fx).ensureRun("run-spare-2", kProjectTwo);

    fc::mustInsert(env, fc::quoted(env, "fu-1", "run-1", 0));
    RawConnection raw(fx->workspace->database);

    // Identity and quote: immutable. The update also takes a LEGAL step (quoted -> authorized) so that only the identity guard can object.
    const std::string legalStep = "state = 'authorized', authorized_at_ms = 1700000000100, updated_at_ms = 1700000000100";
    struct Edit {
        const char* column;
        const char* value;
    };
    const std::vector<Edit> edits = {{"id", "'fu-x'"}, {"project_id", "'fu-project-2'"}, {"source_run_id", "'run-spare'"},
        {"kind", "'recovery'"}, {"goal", "'another goal'"}, {"max_drafts", "3"}, {"split_hint", "'one'"},
        {"outcome_source_state_fingerprint", "'2222222222222222222222222222222222222222222222222222222222222222'"},
        {"snapshot_fingerprint", "'2222222222222222222222222222222222222222222222222222222222222222'"},
        {"context_files_json", "'[]'"},
        {"context_fingerprint", "'2222222222222222222222222222222222222222222222222222222222222222'"},
        {"provider_id", "'provider-2'"}, {"model_id", "'model-2'"}, {"provider_kind", "'openai'"},
        {"provider_base_url", "'https://elsewhere.example.test'"}, {"provider_credential_reference", "NULL"},
        {"provider_identity_fingerprint", "'2222222222222222222222222222222222222222222222222222222222222222'"},
        {"prompt_cache_mode", "'ephemeral_5m'"}, {"pricing_identity_json", "'{}'"},
        {"pricing_identity_fingerprint", "'2222222222222222222222222222222222222222222222222222222222222222'"},
        {"estimated_input_tokens", "estimated_input_tokens + 1"}, {"cacheable_input_tokens", "cacheable_input_tokens + 1"},
        {"effective_max_output_tokens", "effective_max_output_tokens - 1"}, {"cap_micros", "cap_micros - 1"},
        {"estimated_cost_micros", "estimated_cost_micros - 1"}, {"created_at_ms", "created_at_ms - 1"},
        {"quote_expires_at_ms", "quote_expires_at_ms + 1"}};
    for (const auto& edit : edits) {
        const std::string failure = raw.exec(std::string("UPDATE follow_up_plan_requests SET ") + edit.column + " = " + edit.value
            + ", " + legalStep + " WHERE id = 'fu-1';");
        require(contains(failure, "follow_up_request_identity_immutable"),
            std::string("Updating ") + edit.column + " must be refused by the identity guard (got '" + failure + "')");
    }
    require(fc::stored(env, "fu-1").state == S::Quoted, "None of those updates changed the request");

    // The result: write once. Take the request to plan_ready, then try to rewrite what is set, each with a LEGAL step.
    fc::mustStep(env, "fu-1", authorizeStep(fc::stored(env, "fu-1")), 1000);
    fc::mustStep(env, "fu-1", plainStep(E::Claim, S::Authorized), 2000);
    fc::mustStep(env, "fu-1", planStep(answerOf()), 3000);
    const std::string discardStep = "state = 'discarded', finished_at_ms = 1700000004000, updated_at_ms = 1700000004000";
    for (const char* rewrite : {"authorized_at_ms = authorized_at_ms + 1", "claimed_at_ms = claimed_at_ms + 1", "reply_text = 'tampered'",
             "plan_fingerprint = '2222222222222222222222222222222222222222222222222222222222222222'", "advisories_json = '[1]'",
             "accounting_json = '{}'"}) {
        const std::string failure = raw.exec(std::string("UPDATE follow_up_plan_requests SET ") + rewrite + ", " + discardStep + " WHERE id = 'fu-1';");
        require(contains(failure, "follow_up_request_result_immutable"), std::string("Rewriting a set result field (") + rewrite
            + ") must be refused by the write-once guard (got '" + failure + "')");
    }
    require(fc::stored(env, "fu-1").state == S::PlanReady, "...and the plan is still ready");

    // A row is never deleted.
    require(contains(raw.exec("DELETE FROM follow_up_plan_requests WHERE id = 'fu-1';"), "follow_up_request_immutable"), "no delete");
    // A request is born quoted.
    require(contains(copyRequest(raw, "fu-1", "fu-born", "run-spare", "state = 'authorized'"), "follow_up_request_must_start_quoted"),
        "a row that does not start quoted is refused");
}

// The 5A-1 table, edge by edge, at the schema: every pair that is not an edge is refused even when the adapter is bypassed.
void testTheTransitionGuardIsTheAgreedTable()
{
    auto fx = newFixture();
    const auto env = fx->env(fx);
    RawConnection raw(fx->workspace->database);
    const std::vector<std::string> names = {"quoted", "authorized", "composing", "plan_ready", "drafts_saved", "stale",
        "expired", "cancelled", "discarded", "interrupted", "failed"};
    int index = 0;
    for (const S from : fc::allStates()) {
        const std::string id = "tg-" + std::to_string(index);
        fc::drive(env, id, "run-tg-" + std::to_string(index), from);
        ++index;
        for (const auto& to : names) {
            const auto toState = followUpPlanRequestStateFromString(to);
            bool isEdge = false;
            for (const E event : {E::Authorize, E::Claim, E::PlanSucceeded, E::CompositionFailed, E::MarkStale, E::Expire,
                     E::Cancel, E::Interrupt, E::ConfirmDrafts, E::Discard}) {
                const auto next = transitionFollowUpPlanRequest(from, event);
                if (next.success() && next.value() == *toState) isEdge = true;
            }
            // Only the state moves: the other BEFORE guards and the CHECKs may object to an edge for their own reasons, but the
            // transition guard must object to exactly the pairs that are not edges.
            const std::string failure = raw.exec("UPDATE follow_up_plan_requests SET state = '" + to + "' WHERE id = '" + id + "';");
            const std::string pair = std::string(toString(from)) + " -> " + to;
            require(!failure.empty(), pair + " is never a bare state update");
            if (isEdge) {
                require(!contains(failure, "follow_up_request_invalid_transition"), pair + " is an agreed edge, the guard must let it through");
            } else if (to != "drafts_saved") {
                require(contains(failure, "follow_up_request_invalid_transition"), pair + " is not an edge (got '" + failure + "')");
            } else {
                require(contains(failure, "follow_up_request_"), pair + " is not an edge (got '" + failure + "')");
            }
        }
        require(fc::stored(env, id).state == from, "The guards left the request alone");
    }
}

void testTheConstraintsRefuseARowTheRulesCouldNotHaveMade()
{
    auto fx = newFixture();
    const auto env = fx->env(fx);
    fx->env(fx).ensureRun("run-spare", kProjectOne);
    fx->env(fx).ensureRun("run-spare-2", kProjectOne);
    fx->env(fx).ensureRun("run-bad-rows", kProjectOne);
    fx->env(fx).ensureRun("run-acc", kProjectOne);
    fx->env(fx).ensureRun("run-control", kProjectOne);
    fc::mustInsert(env, fc::quoted(env, "fu-1", "run-1", 0));
    RawConnection raw(fx->workspace->database);

    // At most one open request per Run - whatever open state the first one is in.
    const std::string second = copyRequest(raw, "fu-1", "fu-2", "run-1");
    require(contains(second, "UNIQUE") && contains(second, "source_run_id"), "A second open request of one Run is refused by the index: " + second);
    require(copyRequest(raw, "fu-1", "fu-3", "run-spare").empty(), "...while another Run takes one");
    for (const S open : {S::Authorized, S::Composing, S::PlanReady}) {
        const std::string run = std::string("run-open-") + toString(open);
        fc::drive(env, std::string("fu-open-") + toString(open), run, open);
        const std::string clash = copyRequest(raw, "fu-1", std::string("fu-clash-") + toString(open), run);
        require(contains(clash, "UNIQUE") && contains(clash, "source_run_id"),
            std::string("A '") + toString(open) + "' request holds the Run's slot at the schema too: " + clash);
    }
    // The accounting of the paid call exists exactly when a response arrived: the CHECKs refuse it where it cannot be.
    {
        const std::string accounting = R"json({"inputTokens":1,"outputTokens":1,"cacheCreationInputTokens":0,"cacheReadInputTokens":0,"cacheCreationDurationKnown":false,"cacheCreationFiveMinuteTokens":0,"cacheCreationOneHourTokens":0,"costMicros":1,"priceKnown":true,"latencyMs":1})json";
        require(!copyRequest(raw, "fu-open-plan_ready", "fu-acc-1", "run-acc", "accounting_json = NULL").empty(),
            "A ready plan without its accounting is refused");
        require(!copyRequest(raw, "fu-1", "fu-acc-2", "run-acc", "accounting_json = '" + accounting + "'").empty(),
            "A quoted request with an accounting is refused");
        require(!copyRequest(raw, "fu-open-composing", "fu-acc-3", "run-acc", "accounting_json = '" + accounting + "'").empty(),
            "A composing request with an accounting is refused");
        require(!copyRequest(raw, "fu-1", "fu-acc-4", "run-acc",
                    "state = 'cancelled', finished_at_ms = created_at_ms, updated_at_ms = created_at_ms, accounting_json = '" + accounting + "'").empty(),
            "An accounting without a claim is refused");
        require(!copyRequest(raw, "fu-open-plan_ready", "fu-acc-5", "run-acc", "accounting_json = 'not an object'").empty(),
            "An accounting that is not a JSON object is refused");
        require(raw.scalar("SELECT count(*) FROM follow_up_plan_requests WHERE id LIKE 'fu-acc-%';") == 0, "...and none of them was stored");
    }
    // A finished request does not hold it.
    fc::drive(env, "fu-done", "run-done", S::Cancelled);
    require(copyRequest(raw, "fu-1", "fu-after-done", "run-done").empty(), "...but a finished one does not");
    // The lifecycle CHECKs below must be what refuses a row, so the start-quoted guard is taken out of the way on this connection.
    require(raw.exec("DROP TRIGGER follow_up_requests_insert_quoted;").empty(), "setup: the start-quoted guard is off");
    // Control: a LEGAL authorised row is accepted once that guard is gone, so the refusals below are the constraints'.
    require(copyRequest(raw, "fu-1", "fu-control", "run-control",
                "state = 'authorized', authorized_at_ms = 1700000000010, updated_at_ms = 1700000000010").empty(),
        "A legal authorised row is accepted by the constraints");
    require(copyRequest(raw, "fu-1", "fu-control-discarded", "run-spare-2",
                "state = 'discarded', updated_at_ms = 1700000000010, finished_at_ms = 1700000000010").empty(),
        "...and so is a discarded quote (never authorised)");

    // Lifecycle: free versus charged cannot be faked.
    struct Case {
        const char* what;
        const char* assignments;
    };
    for (const Case bad : {Case {"a claim on a quoted request", "claimed_at_ms = 1700000000050"},
             Case {"a finished quoted request", "finished_at_ms = 1700000000000"},
             Case {"an authorised request without its time", "state = 'authorized'"},
             Case {"a composing request that was never claimed", "state = 'composing', authorized_at_ms = 1700000000010, updated_at_ms = 1700000000010"},
             Case {"a plan_ready request without a plan", "state = 'plan_ready', authorized_at_ms = 1700000000010, claimed_at_ms = 1700000000010, updated_at_ms = 1700000000010"},
             Case {"a stale request that claims a call", "state = 'stale', authorized_at_ms = 1700000000010, claimed_at_ms = 1700000000010, updated_at_ms = 1700000000010, finished_at_ms = 1700000000010"},
             Case {"a quoted request updated after it was created", "updated_at_ms = created_at_ms + 5"},
             Case {"an authorised request updated after its authorisation",
                 "state = 'authorized', authorized_at_ms = 1700000000010, updated_at_ms = 1700000000020"},
             Case {"a composing request updated after its claim",
                 "state = 'composing', authorized_at_ms = 1700000000010, claimed_at_ms = 1700000000010, updated_at_ms = 1700000000020"},
             Case {"a discarded request that was authorised but never claimed",
                 "state = 'discarded', authorized_at_ms = 1700000000010, updated_at_ms = 1700000000010, finished_at_ms = 1700000000010"},
             Case {"a failure on a quoted request", "failure_code = 'x', failure_message = 'y'"},
             Case {"a quote that expires in 14 minutes", "quote_expires_at_ms = created_at_ms + 840000"},
             Case {"a cost above the cap", "estimated_cost_micros = cap_micros + 1"},
             Case {"an output below the minimum", "effective_max_output_tokens = 4095"},
             Case {"many drafts out of one", "split_hint = 'many', max_drafts = 1"},
             Case {"a cache policy with nothing cacheable", "cacheable_input_tokens = 0"},
             Case {"a context policy of another version", "context_policy_id = 'follow-up-composer.v2'"},
             Case {"a fingerprint that is not hex", "context_fingerprint = 'zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz'"}}) {
        const std::string failure = copyRequest(raw, "fu-1", "fu-bad", "run-bad-rows", bad.assignments);
        require(!failure.empty(), std::string("The schema must refuse ") + bad.what);
        // The temp copy is rebuilt by every call; a failed attempt leaves no row.
        require(raw.scalar("SELECT count(*) FROM follow_up_plan_requests WHERE id = 'fu-bad';") == 0, std::string("...and nothing of ") + bad.what + " was stored");
    }
}

// A request becomes drafts_saved only together with a group that names it, and a follow-up group is born only from a ready plan.
void testTheGroupAndTheRequestGuardEachOther()
{
    auto fx = newFixture();
    const auto env = fx->env(fx);
    RawConnection raw(fx->workspace->database);
    const auto quotedOnly = fc::drive(env, "g-quoted", "run-q", S::Quoted);
    const auto ready = fc::drive(env, "g-ready", "run-r", S::PlanReady);

    const auto groupInsert = [&](const FollowUpPlanRequest& request, const std::string& groupId, const std::string& planFingerprint,
                                 const std::string& contextFingerprint, const std::string& project) {
        return raw.exec("INSERT INTO run_draft_groups (id,project_id,origin_kind,idempotency_key,plan_title,plan_fingerprint,"
                        "context_fingerprint,source_run_ids_json,created_at_ms,source_request_id) VALUES ('" + groupId + "','" + project
            + "','follow_up_request','" + hex64('e') + "','Plan','" + planFingerprint + "','" + contextFingerprint + "','[\"" + request.sourceRunId
            + "\"]',1700000000050,'" + request.id + "');");
    };
    require(contains(groupInsert(quotedOnly, "g-1", hex64('c'), quotedOnly.quote.contextFingerprint, kProjectOne),
                "run_draft_group_follow_up_not_confirmable"),
        "A group of a request that is only quoted is refused");
    require(contains(groupInsert(ready, "g-2", hex64('9'), ready.quote.contextFingerprint, kProjectOne), "run_draft_group_follow_up_not_confirmable"),
        "...and one that names another plan");
    require(contains(groupInsert(ready, "g-3", ready.answer->planFingerprint, hex64('9'), kProjectOne), "run_draft_group_follow_up_not_confirmable"),
        "...and one that names another sealed context");
    require(contains(groupInsert(ready, "g-4", ready.answer->planFingerprint, ready.quote.contextFingerprint, kProjectTwo),
                "run_draft_group_follow_up_not_confirmable"),
        "...and one of another project");
    // The origin CHECK on its own (no guard fires for these origins): a request id belongs to the follow-up origin only, and the
    // follow-up origin needs a sealed context.
    require(contains(raw.exec("INSERT INTO run_draft_groups (id,project_id,origin_kind,idempotency_key,plan_title,plan_fingerprint,"
                              "source_run_ids_json,created_at_ms,source_request_id) VALUES ('g-5','" + kProjectOne + "','manual_import','"
                              + hex64('e') + "','Plan','" + hex64('c') + "','[]',1,'g-ready');"),
                "CHECK"),
        "A manual group that names a request breaks the origin CHECK");
    require(contains(raw.exec("INSERT INTO run_draft_groups (id,project_id,origin_kind,idempotency_key,plan_title,plan_fingerprint,"
                              "source_run_ids_json,created_at_ms) VALUES ('g-6','" + kProjectOne + "','follow_up_request','"
                              + hex64('e') + "','Plan','" + hex64('c') + "','[]',1);"),
                "run_draft_group_follow_up_not_confirmable"),
        "A follow-up group without its request is refused by the origin guard");
    require(raw.scalar("SELECT count(*) FROM run_draft_groups;") == 0, "None of those left a group");

    const std::string drafted = "state = 'drafts_saved', draft_group_id = 'g-ok', finished_at_ms = 1700000005000, updated_at_ms = 1700000005000";
    require(contains(raw.exec("UPDATE follow_up_plan_requests SET " + drafted + " WHERE id = 'g-ready';"), "follow_up_request_confirm_needs_group"),
        "A request cannot become drafts_saved without a group that names it");

    // The legal pair: group first, then the request - exactly what the Confirm does.
    require(groupInsert(ready, "g-ok", ready.answer->planFingerprint, ready.quote.contextFingerprint, kProjectOne).empty(),
        "A group of a ready plan is accepted");
    // One group per request.
    require(contains(raw.exec("INSERT INTO run_draft_groups (id,project_id,origin_kind,idempotency_key,plan_title,plan_fingerprint,"
                              "context_fingerprint,source_run_ids_json,created_at_ms,source_request_id) VALUES ('g-twice','" + kProjectOne
                              + "','follow_up_request','" + hex64('d') + "','Plan','" + ready.answer->planFingerprint + "','"
                              + ready.quote.contextFingerprint + "','[]',1,'g-ready');"),
                "UNIQUE"),
        "A second group of the same request is refused");
    require(raw.exec("UPDATE follow_up_plan_requests SET " + drafted + " WHERE id = 'g-ready';").empty(), "...and now the request may follow");
    require(fc::stored(env, "g-ready").state == S::DraftsSaved, "The repository reads it as saved");

    // The group is immutable, including the new column; a request cannot be deleted from under its group.
    require(contains(raw.exec("UPDATE run_draft_groups SET source_request_id = 'g-quoted' WHERE id = 'g-ok';"), "run_draft_group_immutable"),
        "The request id of a group never changes");
    require(contains(raw.exec("UPDATE run_draft_groups SET plan_title = 'x' WHERE id = 'g-ok';"), "run_draft_group_immutable"), "...nor the rest");
    require(!raw.exec("DELETE FROM follow_up_plan_requests WHERE id = 'g-ready';").empty(), "A request is never deleted");
}

void testTheKeysKeepTheEvidence()
{
    auto fx = newFixture();
    const auto env = fx->env(fx);
    // Rows that exist only for this request, so that the restrict key is the one that answers.
    Catalog own;
    own.providerId = "provider-own";
    own.modelId = "model-own";
    seedCatalog(fx->database, own);
    seedRun(fx->database, "run-own", kProjectOne);
    auto request = quotedRequest(own, "own-1", kProjectOne, "run-own", at(0));
    require(env.repository->insertQuoted(request).success(), "setup: a request on its own rows");
    RawConnection raw(fx->workspace->database);
    require(contains(raw.exec("DELETE FROM models WHERE id = 'model-own';"), "FOREIGN KEY"), "The model of a request cannot be deleted");
    require(contains(raw.exec("DELETE FROM runs WHERE id = 'run-own';"), "FOREIGN KEY"), "...nor its source Run");
    require(contains(raw.exec("UPDATE runs SET id = 'run-renamed' WHERE id = 'run-own';"), "FOREIGN KEY"), "...nor can that Run be renamed");
    require(contains(raw.exec("UPDATE projects SET id = 'renamed' WHERE id = '" + kProjectOne + "';"), "FOREIGN KEY"), "...nor its project");
    // A request refers to a provider/model that exist.
    Catalog ghost;
    ghost.providerId = "provider-ghost";
    ghost.modelId = "model-ghost";
    const auto refused = env.repository->insertQuoted(quotedRequest(ghost, "ghost-1", kProjectOne, "run-own", at(10)));
    require(!refused.success(), "A quote for a provider and model that do not exist is refused by the keys");
    require(env.repository->findOpenForSourceRun("run-own").value()->id == "own-1", "...and the open request is untouched (the replacement rolled back)");
}

// A stored row is read back like any consumer would: damage behind the repository's back is refused, never repaired and never shown.
void testADamagedRowIsNeverRead()
{
    auto fx = newFixture();
    const auto env = fx->env(fx);
    fc::mustInsert(env, fc::quoted(env, "dm-1", "run-1", 0));
    fc::mustInsert(env, fc::quoted(env, "dm-2", "run-2", 0));
    RawConnection raw(fx->workspace->database, /*foreignKeys=*/false);
    // Switch the guards off on this connection only, the way a hand-made edit would.
    require(raw.exec("DROP TRIGGER follow_up_requests_identity_immutable; DROP TRIGGER follow_up_requests_transition;").empty(),
        "setup: drop the identity and the transition guards");

    const auto isInvalid = [](const auto& result) { return !result.success() && result.error()->code == "persistence.invalid_data"; };
    struct Damage {
        const char* what;
        const char* update;
    };
    const std::vector<Damage> damages = {
        {"a recipient that no longer matches its fingerprint", "provider_kind = 'openai'"},
        {"a base URL changed after sealing", "provider_base_url = 'https://elsewhere.example.test'"},
        {"a cache mode that disagrees with the pricing identity", "prompt_cache_mode = 'ephemeral_5m'"},
        {"a provider column that disagrees with the pricing identity", "provider_id = 'provider-elsewhere'"},
        {"a model column that disagrees with the pricing identity", "model_id = 'model-elsewhere'"},
        {"unreadable pinned files", "context_files_json = '[1]'"},
        {"a malformed pricing identity", "pricing_identity_json = '{}'"},
        {"a tampered pricing fingerprint", "pricing_identity_fingerprint = '2222222222222222222222222222222222222222222222222222222222222222'"},
        {"a goal that is not a valid goal", "goal = 'a' || char(1) || 'b'"},
        {"a draft window that is not authorised", "max_drafts = 4, split_hint = 'many', goal = goal"},   // valid: control row
    };
    for (const auto& damage : damages) {
        const std::string failure = raw.exec(std::string("UPDATE follow_up_plan_requests SET ") + damage.update + " WHERE id = 'dm-1';");
        if (std::string(damage.what).find("not authorised") != std::string::npos) {
            require(failure.empty(), "setup: a valid edit is accepted");
            require(env.repository->findById("dm-1").success(), "A valid edit still reads");
            continue;
        }
        require(failure.empty(), std::string("setup: ") + damage.what + ": " + failure);
        require(isInvalid(env.repository->findById("dm-1")), std::string("findById refuses ") + damage.what);
        require(isInvalid(env.repository->findOpenForSourceRun("run-1")), std::string("findOpen refuses ") + damage.what);
        require(isInvalid(env.repository->listForProject(kProjectOne, 10)), std::string("a list that meets ") + damage.what + " fails whole");
        require(isInvalid(env.repository->listByState(S::Quoted, 10)), std::string("listByState refuses ") + damage.what);
        require(isInvalid(env.repository->transition("dm-1", plainStep(E::Cancel, S::Quoted), at(100))), std::string("no step is built on ") + damage.what);
        require(fc::stored(env, "dm-2").state == S::Quoted, "The other request is fine");
        // Put it back by copying the healthy sibling's value (the guard is off).
        require(raw.exec("UPDATE follow_up_plan_requests SET provider_kind = (SELECT provider_kind FROM follow_up_plan_requests WHERE id = 'dm-2'),"
                         "provider_base_url = (SELECT provider_base_url FROM follow_up_plan_requests WHERE id = 'dm-2'),"
                         "prompt_cache_mode = (SELECT prompt_cache_mode FROM follow_up_plan_requests WHERE id = 'dm-2'),"
                         "provider_id = (SELECT provider_id FROM follow_up_plan_requests WHERE id = 'dm-2'),"
                         "model_id = (SELECT model_id FROM follow_up_plan_requests WHERE id = 'dm-2'),"
                         "context_files_json = (SELECT context_files_json FROM follow_up_plan_requests WHERE id = 'dm-2'),"
                         "pricing_identity_json = (SELECT pricing_identity_json FROM follow_up_plan_requests WHERE id = 'dm-2'),"
                         "pricing_identity_fingerprint = (SELECT pricing_identity_fingerprint FROM follow_up_plan_requests WHERE id = 'dm-2'),"
                         "goal = (SELECT goal FROM follow_up_plan_requests WHERE id = 'dm-2') WHERE id = 'dm-1';").empty(),
            "setup: repair");
        require(env.repository->findById("dm-1").success(), std::string("Repaired after ") + damage.what + ": the row reads again");
    }
}

// P1 at the database: a plan_ready row whose reply was rewritten behind the repository's back (the write-once guard off) is never
// read back and never confirmed - the read gate parses the stored plan, so neither a list nor a Confirm nor a preview can meet it.
void testAStoredPlanThatIsNotThePlanIsNeverReadOrConfirmed()
{
    auto fx = newFixture();
    const auto env = fx->env(fx);
    const auto ready = fc::drive(env, "pr-1", "run-1", S::PlanReady);
    RawConnection raw(fx->workspace->database);
    // Every UPDATE must be a state step, and a set result is write-once: both guards are dropped from this test's own database.
    require(raw.exec("DROP TRIGGER follow_up_requests_result_write_once; DROP TRIGGER follow_up_requests_transition;").empty(),
        "setup: the write-once and the transition guards are dropped");

    const auto escaped = [](const std::string& text) {
        std::string out;
        for (const char ch : text) out += ch == '\'' ? std::string("''") : std::string(1, ch);
        return out;
    };
    const std::string groupId = "group-pr-1";
    const auto confirm = [&] {
        return env.confirm->confirmDrafts("pr-1", ready.answer->planFingerprint, fc::groupFor(ready, groupId, 'e'),
            fc::draftsFor(env, groupId, "pr", &ready), at(9000));
    };
    const auto isInvalid = [](const auto& result) { return !result.success() && result.error()->code == "persistence.invalid_data"; };
    const auto untouched = [&](const char* what) {
        require(!env.drafts->findGroupById(groupId).value().has_value() && fc::draftCount(env) == 0,
            std::string(what) + ": no group and no draft was written");
    };
    const auto setReply = [&](const std::string& text) {
        require(raw.exec("UPDATE follow_up_plan_requests SET reply_text = '" + escaped(text) + "' WHERE id = 'pr-1';").empty(), "setup: rewrite the reply");
    };

    // The reply is replaced by one that holds another VALID plan (its fingerprint is not the stored one).
    setReply(answerOf('d').rawReply);
    require(isInvalid(env.repository->findById("pr-1")), "findById refuses a stored reply that holds another plan");
    require(isInvalid(env.repository->listForProject(kProjectOne, 10)) && isInvalid(env.repository->listByState(S::PlanReady, 10)),
        "...lists meet it and fail whole");
    require(isInvalid(env.repository->findOpenForSourceRun("run-1")), "...so does the open-slot read");
    require(isInvalid(confirm()), "...the Confirm cannot even load it");
    untouched("another plan's reply");
    // ... or a reply that is no plan at all.
    setReply("no plan here");
    require(isInvalid(env.repository->findById("pr-1")) && isInvalid(confirm()), "A reply with no envelope is refused too");
    untouched("a reply without an envelope");
    // ... or advisories that do not read back.
    setReply(ready.answer->rawReply);
    require(raw.exec("UPDATE follow_up_plan_requests SET advisories_json = '[1]' WHERE id = 'pr-1';").empty(), "setup: damage the advisories");
    require(isInvalid(env.repository->findById("pr-1")), "Advisories that do not read back are refused");
    require(raw.exec("UPDATE follow_up_plan_requests SET advisories_json = '[]' WHERE id = 'pr-1';").empty(), "setup: restore the advisories");
    // ... or an accounting that is not what the log accepts.
    require(raw.exec("UPDATE follow_up_plan_requests SET accounting_json = replace(accounting_json, '\"cacheReadInputTokens\":0', '\"cacheReadInputTokens\":99999') WHERE id = 'pr-1';").empty(),
        "setup: damage the accounting");
    require(isInvalid(env.repository->findById("pr-1")), "An accounting whose cache tokens exceed its input is refused");
    require(raw.exec("UPDATE follow_up_plan_requests SET accounting_json = replace(accounting_json, '\"cacheReadInputTokens\":99999', '\"cacheReadInputTokens\":0') WHERE id = 'pr-1';").empty(),
        "setup: restore the accounting");
    // Restored: the same Confirm succeeds.
    require(env.repository->findById("pr-1").success(), "With everything restored the row reads again");
    require(confirm().success(), "...and the Confirm saves the plan");
}

// The Confirm and the replacing insert are each ONE transaction: a failure in the last statement undoes the first ones.
void testAHalfWrittenConfirmOrReplacementRollsBack()
{
    auto fx = newFixture();
    const auto env = fx->env(fx);
    RawConnection raw(fx->workspace->database);

    const auto ready = fc::drive(env, "tx-1", "run-1", S::PlanReady);
    require(raw.exec("CREATE TRIGGER test_fail_confirm BEFORE UPDATE OF state ON follow_up_plan_requests WHEN NEW.state = 'drafts_saved' "
                     "BEGIN SELECT RAISE(ABORT, 'injected_failure'); END;").empty(), "setup: inject a failure into the last statement");
    const std::string groupId = "group-tx-1";
    const auto failed = env.confirm->confirmDrafts("tx-1", ready.answer->planFingerprint, fc::groupFor(ready, groupId, 'e'),
        fc::draftsFor(env, groupId, "tx"), at(9000));
    require(!failed.success() && contains(failed.error()->message, "injected_failure"), "The injected failure is what the Confirm reports");
    require(!env.drafts->findGroupById(groupId).value().has_value() && env.drafts->listGroupDrafts(groupId).value().empty()
            && fc::draftCount(env) == 0,
        "The group and its drafts, written before the failure, are gone");
    require(sameRequest(fc::stored(env, "tx-1"), ready), "...and the request is still the ready plan");
    require(raw.exec("DROP TRIGGER test_fail_confirm;").empty(), "setup");
    require(env.confirm->confirmDrafts("tx-1", ready.answer->planFingerprint, fc::groupFor(ready, groupId, 'e'), fc::draftsFor(env, groupId, "tx"), at(9000)).success(),
        "Without the failure the same Confirm succeeds");

    // The replacing insert: the predecessor is discarded in the same transaction as the insert of the new quote.
    fc::mustInsert(env, fc::quoted(env, "tx-old", "run-2", 0));
    require(raw.exec("CREATE TRIGGER test_fail_insert BEFORE INSERT ON follow_up_plan_requests WHEN NEW.id = 'tx-new' "
                     "BEGIN SELECT RAISE(ABORT, 'injected_failure'); END;").empty(), "setup");
    const auto replaced = env.repository->insertQuoted(fc::quoted(env, "tx-new", "run-2", 5000));
    require(!replaced.success() && contains(replaced.error()->message, "injected_failure"), "The failure of the new insert is reported");
    const auto old = fc::stored(env, "tx-old");
    require(old.state == S::Quoted && !old.finishedAt && env.repository->findOpenForSourceRun("run-2").value()->id == "tx-old",
        "...and the predecessor was NOT discarded");
}

// The paid call is accounted under its own source, with no link to the source Run.
void testTheFollowUpSourceOfAnExecutionRoundTrips()
{
    auto fx = newFixture();
    const auto env = fx->env(fx);
    const auto request = fc::drive(env, "ex-1", "run-1", S::Composing);
    SqliteModelExecutionRepository executions(fx->database);
    ModelExecution execution;
    execution.id = "execution-follow-up";
    execution.source = ModelExecutionSource::FollowUp;
    execution.sourceId = request.id;
    execution.projectId = request.projectId;
    execution.providerId = fx->catalog.providerId;
    execution.modelId = fx->catalog.modelId;
    execution.role = "follow_up_composer";
    execution.position = 0;
    execution.attempt = 1;
    execution.status = ModelExecutionStatus::Completed;
    execution.requestHash = request.quote.contextFingerprint;
    execution.contextHash = request.snapshotFingerprint;
    execution.outputContent = "reply";
    execution.outputPreview = "reply";
    execution.inputTokens = 1000;
    execution.outputTokens = 200;
    execution.costMicros = 4321;
    execution.createdAt = at(3000);
    execution.startedAt = at(3000);
    execution.finishedAt = at(3500);
    require(executions.insertExecution(execution).success(), "A follow_up execution is accepted");
    const auto listed = executions.listExecutions(ModelExecutionSource::FollowUp, request.id);
    require(listed.success() && listed.value().size() == 1 && listed.value().front().source == ModelExecutionSource::FollowUp
            && listed.value().front().sourceId == request.id && listed.value().front().projectId == std::optional<std::string>(request.projectId)
            && listed.value().front().costMicros == 4321,
        "...lists under its source and reads back");
    require(executions.listExecutions(ModelExecutionSource::Run, request.id).value().empty()
            && executions.listExecutions(ModelExecutionSource::Run, request.sourceRunId).value().empty(),
        "...and is linked to neither the Run source type nor the source Run's id");
    const auto project = executions.listProjectExecutions(request.projectId);
    require(project.success() && project.value().size() == 1, "...but it is visible in the project's accounting");
    RawConnection raw(fx->workspace->database);
    require(contains(raw.exec("UPDATE model_executions SET source_type = 'bogus' WHERE id = 'execution-follow-up';"), "CHECK"),
        "An unknown source type is still refused");
    require(modelExecutionSourceFromString("follow_up") == std::optional<ModelExecutionSource>(ModelExecutionSource::FollowUp)
            && std::string(toString(ModelExecutionSource::FollowUp)) == "follow_up" && !modelExecutionSourceFromString("followup").has_value(),
        "The source spelling is exactly follow_up");
}

// ---- the migration ----------------------------------------------------------------------------------------------------------

// The REAL v65 definitions (dumped from a database the v65 code created), so that the "old database" below is byte for byte what
// the live database looked like before this step.
const char* kV65ModelExecutions = R"sql(CREATE TABLE "model_executions" (
    id TEXT PRIMARY KEY NOT NULL,
    source_type TEXT NOT NULL CHECK(source_type IN ('run','comparison','chat')),
    source_id TEXT NOT NULL,
    project_id TEXT REFERENCES projects(id) ON UPDATE CASCADE ON DELETE SET NULL,
    provider_id TEXT NOT NULL REFERENCES providers(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    model_id TEXT NOT NULL REFERENCES models(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    role TEXT NOT NULL,
    position INTEGER NOT NULL CHECK(position >= 0),
    attempt INTEGER NOT NULL DEFAULT 1 CHECK(attempt >= 1),
    status TEXT NOT NULL CHECK(status IN ('pending','running','completed','partial','failed','cancelled')),
    request_hash TEXT NOT NULL,
    context_hash TEXT NOT NULL,
    output_artifact_id TEXT REFERENCES artifacts(id) ON UPDATE CASCADE ON DELETE SET NULL,
    output_content TEXT NOT NULL DEFAULT '',
    output_preview TEXT NOT NULL,
    input_tokens INTEGER NOT NULL CHECK(input_tokens >= 0),
    output_tokens INTEGER NOT NULL CHECK(output_tokens >= 0),
    cost_micros INTEGER NOT NULL CHECK(cost_micros >= 0),
    latency_ms INTEGER NOT NULL CHECK(latency_ms >= 0),
    error_code TEXT NOT NULL,
    error_message TEXT NOT NULL,
    created_at_ms INTEGER NOT NULL,
    started_at_ms INTEGER,
    finished_at_ms INTEGER, cost_price_known INTEGER NOT NULL DEFAULT 1, cache_creation_input_tokens INTEGER NOT NULL DEFAULT 0 CHECK(cache_creation_input_tokens >= 0), cache_read_input_tokens INTEGER NOT NULL DEFAULT 0 CHECK(cache_read_input_tokens >= 0), cache_creation_duration_partition_known INTEGER NOT NULL DEFAULT 0 CHECK(cache_creation_duration_partition_known IN (0,1)), cache_creation_five_minute_input_tokens INTEGER NOT NULL DEFAULT 0 CHECK(cache_creation_five_minute_input_tokens >= 0), cache_creation_one_hour_input_tokens INTEGER NOT NULL DEFAULT 0 CHECK(cache_creation_one_hour_input_tokens >= 0),
    UNIQUE(source_type, source_id, position, attempt)
))sql";

const char* kV65RunDraftGroups = R"sql(CREATE TABLE run_draft_groups (
    id TEXT PRIMARY KEY NOT NULL CHECK(length(id) BETWEEN 1 AND 64),
    project_id TEXT NOT NULL REFERENCES projects(id) ON UPDATE CASCADE ON DELETE CASCADE,
    origin_kind TEXT NOT NULL CHECK(origin_kind IN ('chat_message','manual_import')),
    idempotency_key TEXT NOT NULL UNIQUE CHECK(length(idempotency_key) = 64),
    plan_title TEXT NOT NULL CHECK(length(CAST(plan_title AS BLOB)) BETWEEN 1 AND 512),
    plan_fingerprint TEXT NOT NULL CHECK(length(plan_fingerprint) = 64),
    source_conversation_id TEXT,
    source_message_id TEXT,
    source_message_fingerprint TEXT,
    context_fingerprint TEXT CHECK(context_fingerprint IS NULL OR length(context_fingerprint) = 64),
    source_run_ids_json TEXT NOT NULL DEFAULT '[]',
    created_at_ms INTEGER NOT NULL,
    CHECK((origin_kind = 'chat_message') = (source_message_id IS NOT NULL)),
    CHECK((source_message_id IS NULL) = (source_conversation_id IS NULL)),
    CHECK((source_message_id IS NULL) = (source_message_fingerprint IS NULL)),
    CHECK(source_message_fingerprint IS NULL OR length(source_message_fingerprint) = 64),
    CHECK(context_fingerprint IS NULL OR origin_kind = 'chat_message')
))sql";

const char* kV65GroupsRejectUpdate = R"sql(CREATE TRIGGER run_draft_groups_reject_update
BEFORE UPDATE OF id, project_id, origin_kind, idempotency_key, plan_title, plan_fingerprint, source_conversation_id,
    source_message_id, source_message_fingerprint, context_fingerprint, source_run_ids_json, created_at_ms
ON run_draft_groups
BEGIN
    SELECT RAISE(ABORT, 'run_draft_group_immutable');
END)sql";

const char* kV65ProjectMatch = R"sql(CREATE TRIGGER run_drafts_group_project_match
BEFORE INSERT ON run_drafts
WHEN NEW.group_id IS NOT NULL
 AND NEW.project_id IS NOT (SELECT project_id FROM run_draft_groups WHERE id = NEW.group_id)
BEGIN
    SELECT RAISE(ABORT, 'run_draft_group_project_mismatch');
END)sql";

const char* kV65ExecutionIndexProject = R"sql(CREATE INDEX model_executions_project_created_idx
    ON model_executions(project_id,created_at_ms,id))sql";

const char* kV65ExecutionIndexSource = R"sql(CREATE INDEX model_executions_source_idx
    ON model_executions(source_type,source_id,position,attempt))sql";

const char* kV65ExecutionTriggerInsert = R"sql(CREATE TRIGGER model_executions_cache_duration_validate_insert BEFORE INSERT ON model_executions
WHEN NEW.cache_creation_input_tokens > NEW.input_tokens
  OR NEW.cache_read_input_tokens > NEW.input_tokens - NEW.cache_creation_input_tokens
  OR NOT ((NEW.cache_creation_duration_partition_known = 0
           AND NEW.cache_creation_five_minute_input_tokens = 0
           AND NEW.cache_creation_one_hour_input_tokens = 0)
       OR (NEW.cache_creation_duration_partition_known = 1
           AND NEW.cache_creation_five_minute_input_tokens <= NEW.cache_creation_input_tokens
           AND NEW.cache_creation_one_hour_input_tokens
               = NEW.cache_creation_input_tokens - NEW.cache_creation_five_minute_input_tokens))
BEGIN SELECT RAISE(ABORT, 'model_execution_cache_duration_invalid'); END)sql";

const char* kV65ExecutionTriggerUpdate = R"sql(CREATE TRIGGER model_executions_cache_duration_validate_update BEFORE UPDATE OF
    input_tokens,cache_creation_input_tokens,cache_read_input_tokens,
    cache_creation_duration_partition_known,cache_creation_five_minute_input_tokens,
    cache_creation_one_hour_input_tokens ON model_executions
WHEN NEW.cache_creation_input_tokens > NEW.input_tokens
  OR NEW.cache_read_input_tokens > NEW.input_tokens - NEW.cache_creation_input_tokens
  OR NOT ((NEW.cache_creation_duration_partition_known = 0
           AND NEW.cache_creation_five_minute_input_tokens = 0
           AND NEW.cache_creation_one_hour_input_tokens = 0)
       OR (NEW.cache_creation_duration_partition_known = 1
           AND NEW.cache_creation_five_minute_input_tokens <= NEW.cache_creation_input_tokens
           AND NEW.cache_creation_one_hour_input_tokens
               = NEW.cache_creation_input_tokens - NEW.cache_creation_five_minute_input_tokens))
BEGIN SELECT RAISE(ABORT, 'model_execution_cache_duration_invalid'); END)sql";

const char* kExecutionColumns =
    "id,source_type,source_id,project_id,provider_id,model_id,role,position,attempt,status,request_hash,context_hash,"
    "output_artifact_id,output_content,output_preview,input_tokens,output_tokens,cost_micros,latency_ms,error_code,error_message,"
    "created_at_ms,started_at_ms,finished_at_ms,cost_price_known,cache_creation_input_tokens,cache_read_input_tokens,"
    "cache_creation_duration_partition_known,cache_creation_five_minute_input_tokens,cache_creation_one_hour_input_tokens";
const char* kGroupColumns =
    "id,project_id,origin_kind,idempotency_key,plan_title,plan_fingerprint,source_conversation_id,source_message_id,"
    "source_message_fingerprint,context_fingerprint,source_run_ids_json,created_at_ms";

// Turns a database the current code created into the database the v65 code would have: the two tables and their guards in their
// REAL v65 shape (rows and rowids kept), no follow-up table, version 65. The shape is asserted against the dumped definitions.
void downgradeToV65(const fs::path& database)
{
    RawConnection raw(database, /*foreignKeys=*/false);
    const std::string script = std::string("BEGIN;"
        "DROP TRIGGER IF EXISTS run_draft_groups_follow_up_origin;"
        "DROP TRIGGER IF EXISTS run_draft_groups_reject_update;"
        "DROP TRIGGER IF EXISTS run_drafts_group_project_match;"
        "CREATE TEMP TABLE groups_backup AS SELECT rowid AS rid, * FROM run_draft_groups;"
        "DROP TABLE run_draft_groups;")
        + kV65RunDraftGroups + ";"
        + "INSERT INTO run_draft_groups (rowid," + kGroupColumns + ") SELECT rid," + kGroupColumns + " FROM groups_backup;"
        + "DROP TABLE groups_backup;"
        + kV65GroupsRejectUpdate + ";" + kV65ProjectMatch + ";"
        + "CREATE TEMP TABLE exec_backup AS SELECT rowid AS rid, * FROM model_executions;"
        + "DROP TABLE model_executions;"
        + kV65ModelExecutions + ";"
        + "INSERT INTO model_executions (rowid," + kExecutionColumns + ") SELECT rid," + kExecutionColumns + " FROM exec_backup;"
        + "DROP TABLE exec_backup;"
        + kV65ExecutionIndexSource + ";" + kV65ExecutionIndexProject + ";" + kV65ExecutionTriggerInsert + ";" + kV65ExecutionTriggerUpdate + ";"
        + "DROP TABLE follow_up_plan_requests;"
        + "PRAGMA user_version = 65;"
          "COMMIT;";
    const std::string failure = raw.exec(script);
    require(failure.empty(), "Could not build the v65 fixture: " + failure);

    const std::vector<std::pair<const char*, const char*>> expected = {
        {"model_executions", kV65ModelExecutions}, {"run_draft_groups", kV65RunDraftGroups},
        {"run_draft_groups_reject_update", kV65GroupsRejectUpdate}, {"run_drafts_group_project_match", kV65ProjectMatch},
        {"model_executions_project_created_idx", kV65ExecutionIndexProject}, {"model_executions_source_idx", kV65ExecutionIndexSource},
        {"model_executions_cache_duration_validate_insert", kV65ExecutionTriggerInsert},
        {"model_executions_cache_duration_validate_update", kV65ExecutionTriggerUpdate}};
    for (const auto& [name, sql] : expected) {
        require(raw.text(std::string("SELECT sql FROM sqlite_master WHERE name = '") + name + "';") == sql,
            std::string("The fixture's '") + name + "' is not byte for byte the v65 definition");
    }
    require(raw.scalar("SELECT count(*) FROM sqlite_master WHERE name LIKE '%follow_up%';") == 0, "No follow-up object is left in the v65 fixture");
    require(raw.scalar("SELECT user_version FROM pragma_user_version;") == 65, "The fixture is at version 65");
}

// A digest of every row of a table (rowid included), over the named columns.
std::string tableDigest(RawConnection& raw, const std::string& table, const std::vector<std::string>& columns)
{
    std::string line = "rowid";
    for (const auto& column : columns) line += " || '|' || quote(" + column + ")";
    const std::string text = raw.text("SELECT group_concat(line, char(10)) FROM (SELECT " + line + " AS line FROM " + table + " ORDER BY rowid);");
    return sha256Hex(text);
}

std::vector<std::string> splitColumns(const char* list)
{
    std::vector<std::string> out;
    std::string current;
    for (const char* p = list; *p != '\0'; ++p) {
        if (*p == ',') {
            out.push_back(current);
            current.clear();
        } else {
            current.push_back(*p);
        }
    }
    out.push_back(current);
    return out;
}

// Everything a database holds that the step must keep: the digest of each table over the OLD columns.
std::map<std::string, std::string> oldDataDigests(RawConnection& raw)
{
    std::map<std::string, std::string> digests;
    digests["model_executions"] = tableDigest(raw, "model_executions", splitColumns(kExecutionColumns));
    digests["run_draft_groups"] = tableDigest(raw, "run_draft_groups", splitColumns(kGroupColumns));
    for (const char* table : {"run_drafts", "run_draft_assignments", "quality_evaluations", "projects", "providers", "models", "runs"}) {
        digests[table] = tableDigest(raw, table, raw.columnsOf(table));
    }
    return digests;
}

std::string schemaText(RawConnection& raw)
{
    return raw.text("SELECT group_concat(type || ' ' || name || ': ' || COALESCE(sql, ''), char(10)) FROM "
                    "(SELECT type, name, sql FROM sqlite_master ORDER BY type, name);");
}

// Old rows of every kind, written by the repositories (so they are rows the product really makes).
void populateOldData(const std::shared_ptr<FollowUpFixture>& fx)
{
    const auto env = fx->env(fx);
    SqliteModelExecutionRepository executions(fx->database);
    const auto execution = [&](std::string id, ModelExecutionSource source, std::string sourceId, std::uint32_t position) {
        ModelExecution value;
        value.id = std::move(id);
        value.source = source;
        value.sourceId = std::move(sourceId);
        value.projectId = kProjectOne;
        value.providerId = fx->catalog.providerId;
        value.modelId = fx->catalog.modelId;
        value.role = "reviewer";
        value.position = position;
        value.attempt = position + 1;
        value.status = ModelExecutionStatus::Completed;
        value.requestHash = "request-" + value.id;
        value.contextHash = "context-" + value.id;
        value.outputContent = std::string(300, 'o') + value.id;
        value.outputPreview = "preview " + value.id;
        value.inputTokens = 1000 + position;
        value.outputTokens = 200 + position;
        value.cacheCreationInputTokens = 300;
        value.cacheReadInputTokens = 400;
        value.cacheCreationDuration.known = true;
        value.cacheCreationDuration.fiveMinuteTokens = 100;
        value.cacheCreationDuration.oneHourTokens = 200;
        value.costMicros = 777 + position;
        value.costPriceKnown = position != 1;
        value.latencyMs = 1234 + position;
        value.errorCode = position == 2 ? "provider.timeout" : "";
        value.errorMessage = position == 2 ? "timed out once" : "";
        value.createdAt = at(1000 + position);
        value.startedAt = at(1100 + position);
        value.finishedAt = at(1200 + position);
        require(executions.insertExecution(value).success(), "setup: an old execution");
    };
    execution("old-run-execution", ModelExecutionSource::Run, "old-run", 0);
    execution("old-comparison-execution", ModelExecutionSource::Comparison, "old-comparison", 1);
    execution("old-chat-execution", ModelExecutionSource::Chat, "old-conversation", 2);
    QualityEvaluation evaluation;
    evaluation.id = "old-evaluation";
    evaluation.executionId = "old-chat-execution";
    evaluation.evaluatorKind = QualityEvaluatorKind::Automated;
    evaluation.rubricVersion = "v1";
    evaluation.scoreBasisPoints = 8000;
    evaluation.criteriaJson = "{}";
    evaluation.explanation = "fine";
    evaluation.evidence = "evidence";
    evaluation.createdAt = at(2000);
    require(executions.insertEvaluation(evaluation).success(), "setup: an old evaluation");

    const auto draftEnv = fc::draftEnvOf(env);
    auto chatGroup = rundraft_contract::makeGroup(draftEnv, "old-chat-group", 'a', RunDraftGroupOrigin::ChatMessage);
    chatGroup.contextFingerprint = hex64('d');
    chatGroup.sourceRunIds = {"run-a", "run-b"};
    require(fx->drafts.insertGroup(chatGroup, {rundraft_contract::groupDraft(draftEnv, "old-chat-group", "old-draft-1", 0, "first"),
                rundraft_contract::groupDraft(draftEnv, "old-chat-group", "old-draft-2", 1, "second", {"first"})}).success(),
        "setup: an old chat-message group");
    auto manualGroup = rundraft_contract::makeGroup(draftEnv, "old-manual-group", 'b');
    require(fx->drafts.insertGroup(manualGroup, {rundraft_contract::groupDraft(draftEnv, "old-manual-group", "old-draft-3", 0, "only")}).success(),
        "setup: an old manual group");
    // A manual draft with a roster and contracts.
    auto manual = rundraft_contract::manualDraft(draftEnv, "old-manual-draft");
    manual.roster = {rundraft_contract::rosterEntry(draftEnv, "lead", 0)};
    manual.roster[0].modelId = fx->catalog.modelId;
    manual.roster[0].providerId = fx->catalog.providerId;
    require(fx->drafts.insert(manual).success(), "setup: an old manual draft");
}

void requireOldDataStillWorks(const std::shared_ptr<SqliteDatabase>& database, const fc::Env& env)
{
    SqliteModelExecutionRepository executions(database);
    SqliteRunDraftRepository drafts(database);
    const auto chat = executions.findExecution("old-chat-execution");
    require(chat.success() && chat.value().has_value() && chat.value()->source == ModelExecutionSource::Chat
            && chat.value()->sourceId == "old-conversation" && chat.value()->cacheCreationDuration.known
            && chat.value()->costMicros == 779 && chat.value()->errorCode == "provider.timeout",
        "An old chat execution still opens, with its cache detail and error");
    require(executions.listExecutions(ModelExecutionSource::Chat, "old-conversation").value().size() == 1
            && executions.listExecutions(ModelExecutionSource::Run, "old-run").value().size() == 1
            && executions.listExecutions(ModelExecutionSource::Comparison, "old-comparison").value().size() == 1,
        "...and every old source still lists");
    const auto group = drafts.findGroupById("old-chat-group");
    require(group.success() && group.value().has_value() && group.value()->origin == RunDraftGroupOrigin::ChatMessage
            && !group.value()->sourceRequestId.has_value() && group.value()->contextFingerprint == std::optional<std::string>(hex64('d'))
            && group.value()->sourceRunIds.size() == 2,
        "An old chat-message group still reads, with no request id");
    const auto groupDrafts = drafts.listGroupDrafts("old-chat-group");
    require(groupDrafts.success() && groupDrafts.value().size() == 2 && groupDrafts.value()[1].provenance->dependsOn == std::vector<std::string> {"first"},
        "...and the drafts of an old group still resolve their group");
    require(drafts.findGroupById("old-manual-group").value().has_value() && drafts.findById("old-manual-draft").value()->roster.size() == 1,
        "...as do the manual group and the manual draft");
    require(drafts.findGroupByIdempotencyKey(hex64('a')).value().has_value(), "...and the idempotency key still finds its group");
    require(executions.listEvaluations("old-chat-execution").success(), "An old evaluation still reaches its execution");
    static_cast<void>(env);
}

void requireTheNewShapeWorks(const fs::path& databasePath, const std::shared_ptr<SqliteDatabase>& database, const fc::Env& env)
{
    RawConnection raw(databasePath);
    require(raw.scalar("SELECT user_version FROM pragma_user_version;") == 70, "The database is at version 70");
    require(contains(raw.text("SELECT sql FROM sqlite_master WHERE name = 'model_executions';"), "'follow_up'"), "model_executions admits follow_up");
    require(contains(raw.text("SELECT sql FROM sqlite_master WHERE name = 'run_draft_groups';"), "'follow_up_request'"), "run_draft_groups admits follow_up_request");
    require(raw.scalar("SELECT count(*) FROM follow_up_plan_requests;") == 0, "The new table exists and is empty");
    require(raw.text("PRAGMA integrity_check;") == "ok", "integrity_check is ok");
    require(raw.text("SELECT count(*) FROM pragma_foreign_key_check;") == "0", "The whole database passes foreign_key_check");
    for (const char* name : {"follow_up_requests_one_open_per_run", "follow_up_requests_transition", "follow_up_requests_identity_immutable",
             "follow_up_requests_result_write_once", "follow_up_requests_insert_quoted", "follow_up_requests_reject_delete",
             "follow_up_requests_confirm_needs_group", "run_draft_groups_follow_up_origin", "run_draft_groups_reject_update",
             "run_drafts_group_project_match", "model_executions_source_idx", "model_executions_project_created_idx",
             "model_executions_cache_duration_validate_insert", "model_executions_cache_duration_validate_update"}) {
        require(raw.scalar(std::string("SELECT count(*) FROM sqlite_master WHERE name = '") + name + "';") == 1, std::string("The object '") + name + "' exists once");
    }
    // The old guards still guard.
    require(contains(raw.exec("UPDATE run_draft_groups SET plan_title = 'x' WHERE id = 'old-chat-group';"), "run_draft_group_immutable"),
        "The group stays immutable");
    require(contains(raw.exec("UPDATE model_executions SET cache_read_input_tokens = input_tokens + 1 WHERE id = 'old-chat-execution';"),
                "model_execution_cache_duration_invalid"),
        "The cache-usage trigger of v65 survived the rebuild");
    // And the new capabilities work on the rebuilt tables.
    SqliteModelExecutionRepository executions(database);
    ModelExecution execution;
    execution.id = "new-follow-up-execution";
    execution.source = ModelExecutionSource::FollowUp;
    execution.sourceId = "request-x";
    execution.projectId = kProjectOne;
    execution.providerId = env.catalog.providerId;
    execution.modelId = env.catalog.modelId;
    execution.role = "follow_up_composer";
    execution.status = ModelExecutionStatus::Completed;
    execution.requestHash = "r";
    execution.contextHash = "c";
    execution.createdAt = at(5000);
    require(executions.insertExecution(execution).success(), "A follow_up execution can be written after the migration");
    SqliteFollowUpPlanRepository repository(database);
    const auto request = fc::mustInsert(env, fc::quoted(env, "after-1", "run-after", 0));
    require(repository.findById("after-1").value().has_value() && request.state == S::Quoted, "A follow-up request can be stored after the migration");
}

// A v65 database whose schema is byte for byte the real one, with old rows of every kind in the two tables that are rebuilt.
struct OldDatabase {
    std::shared_ptr<Workspace> workspace {std::make_shared<Workspace>()};
    std::map<std::string, std::string> digests;   // of every table over its OLD columns
    OldDatabase()
    {
        {
            auto seeded = std::make_shared<FollowUpFixture>(workspace);
            populateOldData(seeded);
        }   // the seeding connection is closed here; the files stay with the workspace
        downgradeToV65(workspace->database);
        RawConnection raw(workspace->database);
        digests = oldDataDigests(raw);
        require(raw.scalar("SELECT count(*) FROM model_executions;") == 3 && raw.scalar("SELECT count(*) FROM run_draft_groups;") == 2
                && raw.scalar("SELECT count(*) FROM run_drafts;") == 4 && raw.scalar("SELECT count(*) FROM quality_evaluations;") == 1,
            "The old data is there before the migration");
    }
};

void requireTheOldDataIsUnchanged(const fs::path& databasePath, const std::map<std::string, std::string>& before, const char* when)
{
    RawConnection raw(databasePath);
    const auto after = oldDataDigests(raw);
    for (const auto& [table, digest] : before) {
        require(after.at(table) == digest, std::string("Every column of every row of '") + table
            + "' is byte for byte what it was " + when + " (rowids included)");
    }
    const auto groupColumns = raw.columnsOf("run_draft_groups");
    if (std::find(groupColumns.begin(), groupColumns.end(), "source_request_id") != groupColumns.end()) {
        require(raw.scalar("SELECT count(*) FROM run_draft_groups WHERE source_request_id IS NOT NULL;") == 0, "No old group names a request");
    }
}

void testAV65DatabaseMigratesWithoutLosingAnything()
{
    OldDatabase old;
    const fs::path databasePath = old.workspace->database;

    // The migration, by the production entry point.
    const auto migrated = SqliteDatabase::open(databasePath);
    require(migrated.success(), "A v65 database migrates: " + (migrated.success() ? std::string() : migrated.error()->message));
    requireTheOldDataIsUnchanged(databasePath, old.digests, "before the migration");
    auto opened = std::make_shared<OpenedDatabase>(databasePath, migrated.value());
    const auto env = opened->env(opened);
    requireOldDataStillWorks(migrated.value(), env);
    requireTheNewShapeWorks(databasePath, migrated.value(), env);
}

// Running the step again over a v66 schema (a rewound version) changes nothing: the rebuilds are skipped, the guards come back as they were.
void testRunningTheStepAgainIsANoOp()
{
    OldDatabase old;
    const fs::path databasePath = old.workspace->database;
    std::string schemaAfterFirst;
    {
        const auto migrated = SqliteDatabase::open(databasePath);
        require(migrated.success(), "setup: the first migration");
    }
    {
        RawConnection raw(databasePath);
        schemaAfterFirst = schemaText(raw);
        require(raw.exec("PRAGMA user_version = 65;").empty(), "setup: rewind the version");
    }
    const auto again = SqliteDatabase::open(databasePath);
    require(again.success(), "Steps 66 and 67 run again over a v70 schema: " + (again.success() ? std::string() : again.error()->message));
    RawConnection raw(databasePath);
    require(raw.scalar("SELECT user_version FROM pragma_user_version;") == 70, "...and ends at 70");
    require(schemaText(raw) == schemaAfterFirst, "...with exactly the schema it had: no object was added, lost or rewritten");
    requireTheOldDataIsUnchanged(databasePath, old.digests, "after the step ran twice");
}

// The whole chain from the "v42" shape of the existing fixtures, re-run over the v66 schema, leaves the data and the schema alone.
void testTheChainFromV42ReRunsOverTheNewSchema()
{
    OldDatabase old;
    const fs::path databasePath = old.workspace->database;
    std::string schemaAfterFirst;
    {
        const auto migrated = SqliteDatabase::open(databasePath);
        require(migrated.success(), "setup: the first migration");
    }
    {
        RawConnection raw(databasePath);
        schemaAfterFirst = schemaText(raw);
        require(raw.exec("PRAGMA user_version = 42;").empty(), "setup: rewind to v42");
    }
    const auto chain = SqliteDatabase::open(databasePath);
    require(chain.success(), "Every step from v43 re-runs over the v70 schema: " + (chain.success() ? std::string() : chain.error()->message));
    RawConnection raw(databasePath);
    require(raw.scalar("SELECT user_version FROM pragma_user_version;") == 70, "...and ends at 70");
    requireTheOldDataIsUnchanged(databasePath, old.digests, "after the chain re-ran");
    require(contains(raw.text("SELECT sql FROM sqlite_master WHERE name = 'model_executions';"), "'follow_up'")
            && contains(raw.text("SELECT sql FROM sqlite_master WHERE name = 'run_draft_groups';"), "'follow_up_request'"),
        "...and the rebuilt tables kept their new shape");
    require(schemaText(raw) == schemaAfterFirst, "...and the schema is unchanged");
}

// A failure in the middle of the step leaves a v65 database a v65 database.
void testAFailedStepLeavesTheDatabaseAtV65()
{
    OldDatabase old;
    const fs::path databasePath = old.workspace->database;
    {
        // A table named like the rebuild's temporary one makes the step fail halfway through.
        RawConnection raw(databasePath);
        require(raw.exec("CREATE TABLE run_draft_groups_v66 (x INTEGER);").empty(), "setup: block the rebuild");
    }
    const auto failed = SqliteDatabase::open(databasePath);
    require(!failed.success() && contains(failed.error()->code, "migration.v66_failed"), "The step fails with its own code");
    RawConnection raw(databasePath);
    require(raw.scalar("SELECT user_version FROM pragma_user_version;") == 65, "...and the version is still 65");
    require(raw.scalar("SELECT count(*) FROM sqlite_master WHERE name = 'follow_up_plan_requests';") == 0,
        "...and the new table was rolled back with the rest");
    require(raw.text("SELECT sql FROM sqlite_master WHERE name = 'run_draft_groups';") == kV65RunDraftGroups
            && raw.text("SELECT sql FROM sqlite_master WHERE name = 'model_executions';") == kV65ModelExecutions,
        "...and both tables are as they were");
    requireTheOldDataIsUnchanged(databasePath, old.digests, "after the failed step");
    require(raw.exec("DROP TABLE run_draft_groups_v66;").empty(), "setup: unblock");
    require(SqliteDatabase::open(databasePath).success(), "Once the cause is gone the same database migrates");
}

// ---- step 67: the repair of a v66 table made before the table was amended in place ---------------------------------------------------
//
// A database that had already run the FIRST spelling of step 66 stays at user_version 66, so the amended table never reached it and
// the amended code failed there ("no such column: r.accounting_json"). The old shape is rebuilt EXACTLY as the live database holds it
// (FollowUpOldV66Schema.hpp, dumped from it); a table that holds requests is never dropped.

// Replaces the follow-up table and everything on it of a (closed) current database with the old v66 objects.
void makeTheOldV66Shape(const fs::path& databasePath)
{
    RawConnection raw(databasePath, false);
    require(raw.exec("PRAGMA user_version = 66; DROP TABLE follow_up_plan_requests;").empty(), "setup: drop the amended table");
    for (const char* sql : followup_old_v66::kObjects) {
        const std::string failure = raw.exec(sql);
        require(failure.empty(), "setup: an old v66 object could not be created: " + failure);
    }
    require(raw.exec("SELECT accounting_json FROM follow_up_plan_requests;").find("no such column") != std::string::npos,
        "setup: the old table really has no accounting_json - the failure of the live database");
    require(raw.scalar("SELECT count(*) FROM sqlite_master WHERE tbl_name = 'follow_up_plan_requests' AND sql IS NOT NULL;") == 10,
        "setup: the table, 3 indexes and 6 triggers of the old shape");
}

void testAnOldV66TableIsRepairedToExactlyTheFreshSchema()
{
    std::string freshSchema;
    {
        Workspace workspace;
        {
            const auto fresh = openDatabase(workspace.database);
            RawConnection raw(workspace.database);
            freshSchema = schemaText(raw);
            require(raw.scalar("SELECT user_version FROM pragma_user_version;") == 70, "a new database is at version 70");
        }
    }
    auto workspace = std::make_shared<Workspace>();
    {
        FollowUpFixture seed(workspace);   // a current database with a project, a catalog and runs, then closed again
    }
    makeTheOldV66Shape(workspace->database);

    const auto repaired = SqliteDatabase::open(workspace->database);
    require(repaired.success(), "Step 67 repairs the old table: " + (repaired.success() ? std::string() : repaired.error()->message));
    {
        RawConnection raw(workspace->database);
        require(raw.scalar("SELECT user_version FROM pragma_user_version;") == 70, "...and the database is at version 70");
        require(raw.exec("SELECT accounting_json FROM follow_up_plan_requests;").empty(), "...with the column the amended code reads");
        require(schemaText(raw) == freshSchema, "...and the schema is byte for byte the one of a database created today (every table, index and trigger)");
    }
}

void testAV66DatabaseThatAlreadyHasTheAmendedTableOnlyGetsTheVersion()
{
    // A database made by the AMENDED step 66 and holding a request: the repair must leave table and request alone.
    auto workspace = std::make_shared<Workspace>();
    std::string schemaBefore;
    {
        auto fx = std::make_shared<FollowUpFixture>(workspace);
        const auto env = fx->env(fx);
        fc::mustInsert(env, fc::quoted(env, "keep-1", "run-1", 0));
    }
    {
        RawConnection raw(workspace->database);
        schemaBefore = schemaText(raw);
        require(raw.scalar("SELECT count(*) FROM follow_up_plan_requests;") == 1, "setup: the request is stored");
        require(raw.exec("PRAGMA user_version = 66;").empty(), "setup: the version of a database made by the amended step 66");
    }
    const auto reopened = SqliteDatabase::open(workspace->database);
    require(reopened.success(), "it opens: " + (reopened.success() ? std::string() : reopened.error()->message));
    RawConnection raw(workspace->database);
    require(raw.scalar("SELECT user_version FROM pragma_user_version;") == 70, "it is at 70 now");
    require(schemaText(raw) == schemaBefore, "nothing of its schema was touched");
    require(raw.scalar("SELECT count(*) FROM follow_up_plan_requests WHERE id = 'keep-1';") == 1, "and its request is still there");
}

void testAnOldV66TableThatHoldsRequestsIsNeverDropped()
{
    Workspace workspace;
    {
        const auto first = openDatabase(workspace.database);
    }
    {
        // Only what the step looks at: an old-shaped table (no accounting_json) with one request in it.
        RawConnection raw(workspace.database, false);
        require(raw.exec("PRAGMA user_version = 66; DROP TABLE follow_up_plan_requests; "
                         "CREATE TABLE follow_up_plan_requests (id TEXT PRIMARY KEY); INSERT INTO follow_up_plan_requests VALUES ('evidence');").empty(),
            "setup: an old-shaped table that holds a request");
    }
    const auto refused = SqliteDatabase::open(workspace.database);
    require(!refused.success() && contains(refused.error()->code, "migration.v67_failed"), "the step refuses with its own code");
    require(contains(refused.error()->message, "evidence of what was sent and paid for") && contains(refused.error()->message, "1 request"),
        "...saying why and how many: " + (refused.success() ? std::string() : refused.error()->message));
    RawConnection raw(workspace.database);
    require(raw.scalar("SELECT user_version FROM pragma_user_version;") == 66, "the version is still 66");
    require(raw.scalar("SELECT count(*) FROM follow_up_plan_requests WHERE id = 'evidence';") == 1, "the request is still there");
}

// The whole repository contract (every transition, the atomic Confirm, the read gate) on a database that was REPAIRED, not created.
void testTheContractRunsOnARepairedV66Database()
{
    struct Holder {
        std::shared_ptr<Workspace> workspace;
        std::shared_ptr<OpenedDatabase> database;
    };
    fc::runFollowUpPlanRequestRepositoryContract([] {
        auto holder = std::make_shared<Holder>();
        holder->workspace = std::make_shared<Workspace>();
        {
            FollowUpFixture seed(holder->workspace);
        }
        makeTheOldV66Shape(holder->workspace->database);
        holder->database = std::make_shared<OpenedDatabase>(holder->workspace->database, openDatabase(holder->workspace->database));
        fc::Env env = holder->database->env(holder->database);
        env.keepAlive = holder;
        env.moveRun = [holder](const std::string& runId, const std::string& projectId) {
            RawConnection raw(holder->workspace->database);
            const std::string failure = raw.exec("UPDATE runs SET project_id = '" + projectId + "' WHERE id = '" + runId + "';");
            require(failure.empty(), "Could not move the Run: " + failure);
        };
        return env;
    });
}
} // namespace

int runFollowUpPlanRequestRepositoryTests()
{
    testTheContractOnSqlite();
    testTheSchemaKeepsTheIdentityTheResultAndTheEdges();
    testTheTransitionGuardIsTheAgreedTable();
    testTheConstraintsRefuseARowTheRulesCouldNotHaveMade();
    testTheGroupAndTheRequestGuardEachOther();
    testTheKeysKeepTheEvidence();
    testADamagedRowIsNeverRead();
    testAStoredPlanThatIsNotThePlanIsNeverReadOrConfirmed();
    testAHalfWrittenConfirmOrReplacementRollsBack();
    testTheFollowUpSourceOfAnExecutionRoundTrips();
    testAV65DatabaseMigratesWithoutLosingAnything();
    testRunningTheStepAgainIsANoOp();
    testTheChainFromV42ReRunsOverTheNewSchema();
    testAFailedStepLeavesTheDatabaseAtV65();
    testAnOldV66TableIsRepairedToExactlyTheFreshSchema();
    testAV66DatabaseThatAlreadyHasTheAmendedTableOnlyGetsTheVersion();
    testAnOldV66TableThatHoldsRequestsIsNeverDropped();
    testTheContractRunsOnARepairedV66Database();
    return 0;
}
