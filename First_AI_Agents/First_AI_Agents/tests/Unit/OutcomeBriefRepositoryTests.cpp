#include "OutcomeBriefRepositoryContract.hpp"
#include "RunPagingContract.hpp"

#include "ai/application/verification/ManifestVerificationContinuationPreparationService.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace oc = outcomebrief_contract;
using namespace ai::application;
using namespace ai::infrastructure;
namespace fs = std::filesystem;

void require(bool condition, const std::string& message)
{
    oc::check(condition, message);
}

struct BriefWorkspace {
    fs::path root {fs::temp_directory_path()
        / ("ai-orchestrator-outcome-brief-test-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
    fs::path database {root / "orchestrator.sqlite3"};
    ~BriefWorkspace()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

// A second, independent connection: the checks that must bypass the repository (schema guards, triggers, restrict keys, the
// migration fixture). Foreign keys are a per-connection setting, so it asks for them like the repository does.
class RawConnection {
public:
    explicit RawConnection(const fs::path& databasePath, bool foreignKeys = true)
    {
        const std::u8string path = databasePath.generic_u8string();
        const std::string utf8Path(reinterpret_cast<const char*>(path.data()), path.size());
        require(sqlite3_open_v2(utf8Path.c_str(), &handle_, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
            "Could not open the raw SQLite connection");
        sqlite3_busy_timeout(handle_, 5000);
        if (foreignKeys) require(exec("PRAGMA foreign_keys = ON;").empty(), "Could not enable foreign keys");
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
        require(sqlite3_prepare_v2(handle_, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK, "Could not prepare: " + sql);
        const int step = sqlite3_step(statement);
        const long long value = step == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
        sqlite3_finalize(statement);
        require(step == SQLITE_ROW, "The query returned no row: " + sql);
        return value;
    }

    // A raw INSERT of a whole brief as one revision row, bypassing the repository (and so its authority checks).
    std::string insertBrief(const std::string& revisionId, long long ordinal, const OutcomeBrief& brief)
    {
        const auto reasons = [&] {
            std::string out = "[";
            for (std::size_t index = 0; index < brief.reasons.size(); ++index) {
                out += std::string(index == 0 ? "" : ",") + "\"" + toString(brief.reasons[index]) + "\"";
            }
            return out + "]";
        }();
        const auto counts = [&] {
            const auto& c = brief.counts;
            const std::pair<const char*, std::uint32_t> fields[] = {{"members", c.members}, {"reviewable", c.reviewable},
                {"applied", c.applied}, {"humanRejected", c.humanRejected}, {"approved", c.approved}, {"undecided", c.undecided},
                {"applyFailed", c.applyFailed}, {"noChanges", c.noChanges}, {"validatorRejected", c.validatorRejected},
                {"superseded", c.superseded}, {"unsettled", c.unsettled}, {"inconsistent", c.inconsistent}};
            std::string out = "{";
            for (std::size_t index = 0; index < std::size(fields); ++index) {
                out += std::string(index == 0 ? "" : ",") + "\"" + fields[index].first + "\":" + std::to_string(fields[index].second);
            }
            return out + "}";
        }();
        const auto members = [&] {
            std::string out = "[";
            for (std::size_t index = 0; index < brief.memberDecisions.size(); ++index) {
                out += std::string(index == 0 ? "" : ",") + "{\"changeSetId\":\"" + brief.memberDecisions[index].changeSetId
                    + "\",\"decision\":\"" + toString(brief.memberDecisions[index].decision) + "\"}";
            }
            return out + "]";
        }();

        sqlite3_stmt* statement = nullptr;
        const char* sql = "INSERT INTO outcome_brief_revisions (id,run_id,project_id,ordinal,schema_version,policy_id,workflow_id,"
                          "terminal_status,state,reasons_json,counts_json,member_decisions_json,recovery_eligible,requires_inspection,"
                          "continuation_source_run_id,continuation_child_run_id,manual_handoff_id,source_state_json,"
                          "source_state_fingerprint,rendered_text,content_fingerprint,created_at_ms) "
                          "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
        require(sqlite3_prepare_v2(handle_, sql, -1, &statement, nullptr) == SQLITE_OK, "Could not prepare the raw brief insert");
        int index = 0;
        const auto text = [&](const std::string& value) {
            sqlite3_bind_text(statement, ++index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
        };
        const auto optionalText = [&](const std::optional<std::string>& value) {
            if (value.has_value()) text(*value); else sqlite3_bind_null(statement, ++index);
        };
        text(revisionId);
        text(brief.runId);
        text(brief.projectId);
        sqlite3_bind_int64(statement, ++index, ordinal);
        text(brief.schemaVersion);
        text(brief.policyId);
        text(brief.workflowId);
        text(toString(brief.terminalStatus));
        text(toString(brief.state));
        text(reasons);
        text(counts);
        text(members);
        sqlite3_bind_int(statement, ++index, brief.recoveryEligible ? 1 : 0);
        sqlite3_bind_int(statement, ++index, brief.requiresInspection ? 1 : 0);
        optionalText(brief.continuationSourceRunId);
        optionalText(brief.continuationChildRunId);
        optionalText(brief.manualHandoffId);
        text(brief.sourceStateJson);
        text(brief.sourceStateFingerprint);
        text(brief.renderedText);
        text(brief.contentFingerprint);
        sqlite3_bind_int64(statement, ++index, 1'700'000'000'000LL + ordinal);
        const int step = sqlite3_step(statement);
        std::string failure = step == SQLITE_DONE ? std::string {} : std::string(sqlite3_errmsg(handle_));
        sqlite3_finalize(statement);
        return failure;
    }

    std::string text(const std::string& sql)
    {
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(handle_, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK, "Could not prepare: " + sql);
        const int step = sqlite3_step(statement);
        std::string value;
        if (step == SQLITE_ROW && sqlite3_column_text(statement, 0) != nullptr) {
            value = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        }
        sqlite3_finalize(statement);
        require(step == SQLITE_ROW, "The query returned no row: " + sql);
        return value;
    }

private:
    sqlite3* handle_ {nullptr};
};

std::shared_ptr<SqliteDatabase> openDatabase(const fs::path& path)
{
    const auto opened = SqliteDatabase::open(path);
    require(opened.success(), "Could not open the outcome brief test database");
    return opened.value();
}

void seedProject(const std::shared_ptr<SqliteDatabase>& database, const ProjectId& projectId)
{
    SqliteProjectRepository projects(database);
    const auto existing = projects.findById(projectId);
    if (existing.success() && existing.value().has_value()) return;
    const auto now = std::chrono::system_clock::now();
    Project project {projectId, "Outcome brief project", "Context", database->path().parent_path() / projectId,
        ProjectStatus::Active, now, now};
    require(projects.insert(project).success(), "Could not seed the project");
}

void seedRun(const std::shared_ptr<SqliteDatabase>& database, const RunId& runId, const ProjectId& projectId,
    RunStatus status = RunStatus::Completed)
{
    seedProject(database, projectId);
    SqliteRunRepository runs(database);
    const auto existing = runs.findRun(runId);
    if (existing.success() && existing.value().has_value()) return;
    Run run;
    run.id = runId;
    run.projectId = projectId;
    run.title = "Outcome brief persistence";
    run.task = "Round-trip a brief";
    run.workflowId = kExecutionManifestWorkflowId;
    run.status = status;
    run.createdAt = std::chrono::system_clock::now();
    require(runs.insertRun(run).success(), "Could not seed the Run");
}

const std::string kProjectOne = "outcome-project-1";
const std::string kProjectTwo = "outcome-project-2";

struct BriefFixture {
    BriefWorkspace workspace;
    std::shared_ptr<SqliteDatabase> database {openDatabase(workspace.database)};
    SqliteOutcomeBriefRepository repository {database};

    BriefFixture()
    {
        seedProject(database, kProjectOne);
        seedProject(database, kProjectTwo);
    }

    void ensureRun(const RunId& runId, const ProjectId& projectId, RunStatus status = RunStatus::Completed)
    {
        seedRun(database, runId, projectId, status);
    }

    void setRunStatus(const RunId& runId, RunStatus status)
    {
        RawConnection raw(workspace.database);
        const std::string failure = raw.exec(std::string("UPDATE runs SET status = '") + toString(status) + "' WHERE id = '" + runId + "';");
        require(failure.empty(), "Could not change the Run status: " + failure);
    }

    [[nodiscard]] oc::Env env(const std::shared_ptr<BriefFixture>& self)
    {
        oc::Env value;
        value.keepAlive = self;
        value.repository = &repository;
        value.projectId = kProjectOne;
        value.otherProjectId = kProjectTwo;
        value.ensureRun = [self](const std::string& runId, const std::string& projectId) { self->ensureRun(runId, projectId); };
        value.setRunStatus = [self](const std::string& runId, RunStatus status) { self->setRunStatus(runId, status); };
        return value;
    }
};

std::shared_ptr<BriefFixture> newFixture() { return std::make_shared<BriefFixture>(); }

void expectFailureContaining(const std::string& message, const std::string& needle, const std::string& what)
{
    require(!message.empty() && message.find(needle) != std::string::npos,
        what + ": expected a failure containing '" + needle + "' but got '" + message + "'");
}

// ---- the contract, and the facts it cannot see -----------------------------------------------------------------------------

void testTheContractOnSqlite()
{
    oc::runOutcomeBriefRepositoryContract([] {
        auto fixture = newFixture();
        return fixture->env(fixture);
    });
}

// Every kind of brief the resolver can produce is stored and read back field for field.
void testEveryKindOfBriefRoundTrips()
{
    auto fx = newFixture();
    const auto env = fx->env(fx);
    auto& repository = fx->repository;
    using Resolution = ManifestVerificationManualHandoffResolution;

    struct Case {
        std::string name;
        RunStatus status;
        OutcomeBriefFacts facts;
        OutcomeBriefState state;
    };
    std::vector<Case> cases;
    const auto add = [&](std::string name, RunStatus status, OutcomeBriefFacts facts, OutcomeBriefState state) {
        cases.push_back({std::move(name), status, std::move(facts), state});
    };
    const auto facts = [&](const std::string& run, RunStatus status, std::vector<OutcomeBriefProposalFacts> proposals,
                           ManifestWorkLedgerDisposition disposition = ManifestWorkLedgerDisposition::ManifestReady) {
        return oc::terminalFacts(run, kProjectOne, status, proposals, disposition);
    };

    add("mixed", RunStatus::Completed, facts("rt-mixed", RunStatus::Completed, {oc::applied("cs-1"), oc::rejected("cs-2")}),
        OutcomeBriefState::ResolvedMixed);
    auto handoff = facts("rt-handoff", RunStatus::Failed, {oc::applied("cs-1")});
    handoff.manualHandoff = OutcomeBriefHandoffFacts {"handoff-1", Resolution::Unresolved};
    handoff.continuationChildRunId = "child-run";
    add("handoff and child", RunStatus::Failed, handoff, OutcomeBriefState::AwaitingManualResolution);
    auto superseded = facts("rt-superseded", RunStatus::Failed, {}, ManifestWorkLedgerDisposition::NotProduced);
    superseded.continuationChildRunId = "child-run";
    add("superseded", RunStatus::Failed, superseded, OutcomeBriefState::SupersededByContinuation);
    add("interrupted", RunStatus::Interrupted,
        facts("rt-interrupted", RunStatus::Interrupted, {}, ManifestWorkLedgerDisposition::NotProduced), OutcomeBriefState::RecoveryOnly);
    add("cancelled", RunStatus::Cancelled,
        facts("rt-cancelled", RunStatus::Cancelled, {}, ManifestWorkLedgerDisposition::NotProduced), OutcomeBriefState::RecoveryOnly);
    auto noLedger = facts("rt-no-ledger", RunStatus::Completed, {oc::applied("cs-1")});
    noLedger.ledgerEntry.reset();
    add("missing ledger", RunStatus::Completed, noLedger, OutcomeBriefState::AttentionRequired);
    add("apply failed", RunStatus::Completed,
        facts("rt-apply-failed", RunStatus::Completed, {oc::proposal("cs-1", ChangeSetState::ApplyFailed), oc::applied("cs-2")}),
        OutcomeBriefState::AttentionRequired);
    auto broken = facts("rt-broken", RunStatus::Failed, {});
    broken.manualHandoff = OutcomeBriefHandoffFacts {"handoff-1", Resolution::Unresolved};
    broken.ledgerEntry.reset();
    broken.continuationChildRunId = "rt-broken";
    add("two reasons", RunStatus::Failed, broken, OutcomeBriefState::AttentionRequired);
    auto child = facts("rt-child", RunStatus::Completed, {oc::applied("cs-1")});
    child.workflowId = std::string(kManifestVerificationContinuationWorkflowId);
    child.continuationSourceRunId = "source-run";
    add("continuation child", RunStatus::Completed, child, OutcomeBriefState::ResolvedApplied);
    std::vector<OutcomeBriefProposalFacts> many;
    for (int index = 1; index <= 64; ++index) many.push_back(oc::ready("cs-" + std::to_string(index)));
    add("64 members", RunStatus::Completed, facts("rt-big", RunStatus::Completed, many), OutcomeBriefState::AwaitingReview);

    for (const Case& item : cases) {
        fx->ensureRun(item.facts.runId, kProjectOne, item.status);
        const auto derived = deriveOutcomeBrief(item.facts);
        require(derived.success(), item.name + ": the brief must be derived");
        require(derived.value().state == item.state, item.name + ": the state of the fixture");
        const auto stored = oc::mustAppend(repository, derived.value(), OutcomeBriefExpectedHead::none(), item.name);
        const auto head = oc::headOf(repository, item.facts.runId);
        require(head.has_value() && oc::sameRevision(*head, stored.head), item.name + ": the head reads back as it was stored");
        require(oc::sameBrief(head->brief, derived.value()), item.name + ": every field of the brief survives");
    }
    const auto heads = repository.listHeads(kProjectOne, 100);
    require(heads.success() && heads.value().size() == cases.size(), "All of them are heads of the project");
}

// ---- the writes -----------------------------------------------------------------------------------------------------------

void testRevisionAndHeadAreWrittenInOneTransaction()
{
    auto fx = newFixture();
    const auto env = fx->env(fx);
    RawConnection raw(fx->workspace.database);
    const auto counts = [&] {
        return std::vector<long long> {raw.scalar("SELECT COUNT(*) FROM outcome_brief_revisions"),
            raw.scalar("SELECT COUNT(*) FROM outcome_brief_heads")};
    };
    require(counts() == (std::vector<long long> {0, 0}), "setup: the tables start empty");

    // The head row cannot be written (a trigger of this test refuses it): the revision must not stay behind.
    require(raw.exec("CREATE TRIGGER refuse_head BEFORE INSERT ON outcome_brief_heads BEGIN "
                     "SELECT RAISE(ABORT, 'head_refused_by_the_test'); END;").empty(),
        "setup: a trigger that refuses the head row");
    const auto refused = fx->repository.append(oc::briefA(env, "run-1", kProjectOne), OutcomeBriefExpectedHead::none());
    require(!refused.success() && refused.error()->code == "persistence.write_failed", "A head row that cannot be written fails the append");
    expectFailureContaining(refused.error()->message, "head_refused_by_the_test", "the failure names the cause");
    require(counts() == (std::vector<long long> {0, 0}), "...and rolls the revision back too");
    require(!oc::headOf(fx->repository, "run-1").has_value(), "...so the Run still has no head");

    require(raw.exec("DROP TRIGGER refuse_head;").empty(), "setup: the trigger is gone");
    const auto stored = oc::mustAppend(fx->repository, oc::briefA(env, "run-1", kProjectOne), OutcomeBriefExpectedHead::none(), "after the refusal");
    require(stored.appended && stored.head.ordinal == 1, "The next append is the FIRST revision: no ordinal was burnt");
    require(counts() == (std::vector<long long> {1, 1}), "One revision and one head row");
    require(raw.text("SELECT revision_id FROM outcome_brief_heads WHERE run_id = 'run-1'") == stored.head.id,
        "The head row points at the revision that was appended");

    const auto second = oc::mustAppend(fx->repository, oc::briefB(env, "run-1", kProjectOne), OutcomeBriefExpectedHead::revision(stored.head.id), "B");
    require(counts() == (std::vector<long long> {2, 1}), "A new revision moves the one head row");
    require(raw.text("SELECT revision_id FROM outcome_brief_heads WHERE run_id = 'run-1'") == second.head.id,
        "...to the newest revision");
}

// The READ authority, project: a brief that is internally perfect but names another (existing) project than its Run must never be
// read, from any side. Only a hand-made row can be like that: the repository refuses to write it, and the schema (two independent
// foreign keys, no trigger that reads `runs` - see the v64 migration) does not.
void testABriefOfAnotherProjectIsNeverRead()
{
    auto fx = newFixture();
    const auto env = fx->env(fx);
    RawConnection raw(fx->workspace.database);
    const auto first = oc::mustAppend(fx->repository, oc::briefA(env, "run-1", kProjectOne), OutcomeBriefExpectedHead::none(), "A");

    // A brief the resolver could have produced - for run-1 AS A RUN OF PROJECT 2.
    const auto foreignDerived = deriveOutcomeBrief(oc::completedFacts("run-1", kProjectTwo, {oc::ready("cs-1"), oc::ready("cs-2")}));
    require(foreignDerived.success() && validateOutcomeBrief(foreignDerived.value()).success(), "setup: a valid brief about project 2");
    require(raw.insertBrief("foreign-row", 2, foreignDerived.value()).empty(), "setup: the schema alone lets the foreign row in");
    require(raw.scalar("SELECT COUNT(*) FROM runs WHERE id = 'run-1' AND project_id = '" + kProjectOne + "'") == 1, "setup: run-1 is project 1's");

    oc::requireInvalidData(fx->repository.findRevision("foreign-row"), "findRevision of the foreign row");
    oc::requireInvalidData(fx->repository.findHead("run-1"), "findHead when the head is foreign");
    oc::requireInvalidData(fx->repository.listRevisions("run-1", 10), "listRevisions that contains the foreign row");
    oc::requireInvalidData(fx->repository.listHeads(kProjectOne, 10), "listHeads of the project the Run belongs to");
    oc::requireInvalidData(fx->repository.listHeads(kProjectTwo, 10), "listHeads of the project the brief names");
    oc::requireInvalidData(fx->repository.findHeadVersionForAppend("run-1"), "the writer's read of a foreign head");
    const auto old = fx->repository.findRevision(first.head.id);
    require(old.success() && old.value().has_value() && oc::sameRevision(*old.value(), first.head),
        "The revision before it is intact and readable");

    // Nothing is built on it: a new brief for the Run is refused and writes nothing.
    const auto refused = fx->repository.append(oc::briefB(env, "run-1", kProjectOne), OutcomeBriefExpectedHead::any());
    require(!refused.success() && refused.error()->code == "persistence.invalid_data", "append on a foreign head is refused as invalid data");
    require(raw.scalar("SELECT COUNT(*) FROM outcome_brief_revisions WHERE run_id = 'run-1'") == 2, "...and writes nothing");

    // A foreign row in the middle of the history is met by every read that spans it, even when the head itself is fine.
    require(raw.insertBrief("good-row", 3, oc::briefB(env, "run-1", kProjectOne)).empty(), "setup: a good revision after the foreign one");
    oc::requireInvalidData(fx->repository.findRevision("foreign-row"), "findRevision of a foreign row below the head");
    oc::requireInvalidData(fx->repository.listRevisions("run-1", 10), "listRevisions across a foreign row below the head");
    const auto head = fx->repository.findHead("run-1");
    require(head.success() && head.value().has_value() && head.value()->id == "good-row", "...while the good head itself is readable");
}

// PC-4C: the Run pages of the history pass are a keyset query on the real table - the same scenario as the default implementation.
void testRunPagingOnSqlite()
{
    auto fx = newFixture();
    runpaging_contract::Env env;
    env.repository = nullptr;
    SqliteRunRepository runs(fx->database);
    env.repository = &runs;
    env.ensureProject = [&](const std::string& id) { seedProject(fx->database, id); };
    env.insert = [&](const Run& run) { require(runs.insertRun(run).success(), "setup: a Run"); };
    runpaging_contract::runRunPagingContract(env);
}

// ---- the schema --------------------------------------------------------------------------------------------------------

const std::vector<std::string> kRevisionColumns = {"id", "run_id", "project_id", "ordinal", "schema_version", "policy_id",
    "workflow_id", "terminal_status", "state", "reasons_json", "counts_json", "member_decisions_json", "recovery_eligible",
    "requires_inspection", "continuation_source_run_id", "continuation_child_run_id", "manual_handoff_id", "source_state_json",
    "source_state_fingerprint", "rendered_text", "content_fingerprint", "created_at_ms"};

// A raw INSERT of a copy of the revision `fromId` where some columns are replaced by SQL expressions.
std::string cloneRevisionSql(const std::string& fromId, const std::map<std::string, std::string>& overrides)
{
    std::string columns;
    std::string values;
    for (const auto& column : kRevisionColumns) {
        if (!columns.empty()) {
            columns += ",";
            values += ",";
        }
        columns += column;
        const auto found = overrides.find(column);
        values += found != overrides.end() ? found->second : column;
    }
    return "INSERT INTO outcome_brief_revisions (" + columns + ") SELECT " + values
        + " FROM outcome_brief_revisions WHERE id = '" + fromId + "';";
}

std::string quoted(const std::string& value) { return "'" + value + "'"; }

void testTheSchemaRefusesWhatTheRepositoryNeverWrites()
{
    auto fx = newFixture();
    const auto env = fx->env(fx);
    RawConnection raw(fx->workspace.database);
    const auto first = oc::mustAppend(fx->repository, oc::briefA(env, "run-1", kProjectOne), OutcomeBriefExpectedHead::none(), "A");
    const auto second = oc::mustAppend(fx->repository, oc::briefB(env, "run-1", kProjectOne), OutcomeBriefExpectedHead::any(), "B");
    const auto other = oc::mustAppend(fx->repository, oc::briefA(env, "run-2", kProjectOne), OutcomeBriefExpectedHead::none(), "another Run");

    // Immutable: no UPDATE and no DELETE, not even a no-op one.
    expectFailureContaining(raw.exec("UPDATE outcome_brief_revisions SET state = state;"), "outcome_brief_revision_immutable", "update");
    expectFailureContaining(raw.exec("UPDATE outcome_brief_revisions SET rendered_text = 'x' WHERE id = '" + first.head.id + "';"),
        "outcome_brief_revision_immutable", "update of the rendering");
    expectFailureContaining(raw.exec("DELETE FROM outcome_brief_revisions WHERE id = '" + first.head.id + "';"),
        "outcome_brief_revision_immutable", "delete");
    expectFailureContaining(raw.exec("DELETE FROM outcome_brief_revisions;"), "outcome_brief_revision_immutable", "delete all");

    // Ordinals are 1, 2, 3 ... without gaps and without a second 2.
    expectFailureContaining(raw.exec(cloneRevisionSql(second.head.id, {{"id", quoted("x1")}, {"ordinal", "ordinal + 5"},
                                          {"source_state_fingerprint", quoted(std::string(64, 'e'))}})),
        "outcome_brief_ordinal_gap", "an ordinal that skips");
    expectFailureContaining(raw.exec(cloneRevisionSql(second.head.id, {{"id", quoted("x2")}, {"ordinal", "ordinal"},
                                          {"source_state_fingerprint", quoted(std::string(64, 'e'))}})),
        "outcome_brief_ordinal_gap", "an ordinal that repeats");
    expectFailureContaining(raw.exec(cloneRevisionSql(first.head.id, {{"id", quoted("x3")}, {"ordinal", "3"}, {"source_state_fingerprint", quoted(std::string(64, 'e'))},
                                          {"run_id", quoted("run-2")}})),
        "outcome_brief_ordinal_gap", "run-2 has one revision, so its next ordinal is 2");

    // A revision never repeats the source state of the one before it (the head), but a return to an OLDER one is fine.
    expectFailureContaining(raw.exec(cloneRevisionSql(second.head.id, {{"id", quoted("x4")}, {"ordinal", "ordinal + 1"}})),
        "outcome_brief_repeats_head", "a revision that repeats the head");
    // A Run and a project that do not exist: the foreign keys refuse. (That the project is the Run's own project is
    // checked by the repository, see testTheRunMustExistAndMatch.)
    expectFailureContaining(raw.exec(cloneRevisionSql(second.head.id, {{"id", quoted("x5")}, {"ordinal", "ordinal + 1"},
                                          {"project_id", quoted("no-such-project")}, {"source_state_fingerprint", quoted(std::string(64, 'e'))}})),
        "FOREIGN KEY constraint failed", "a revision of a project that does not exist");
    expectFailureContaining(raw.exec(cloneRevisionSql(second.head.id, {{"id", quoted("x6")}, {"run_id", quoted("no-such-run")}, {"ordinal", "1"}})),
        "FOREIGN KEY constraint failed", "a revision of a Run that does not exist");
    require(raw.scalar("SELECT COUNT(*) FROM outcome_brief_revisions") == 3, "None of the refused rows is there");

    // Plain column checks (the other triggers are satisfied, so only the CHECK can refuse).
    const std::map<std::string, std::string> fresh = {{"ordinal", "ordinal + 1"}, {"source_state_fingerprint", quoted(std::string(64, 'e'))}};
    const auto refusedWith = [&](std::map<std::string, std::string> overrides, const std::string& what) {
        auto columns = fresh;
        for (const auto& [name, value] : overrides) columns[name] = value;
        columns["id"] = quoted("check-row");
        expectFailureContaining(raw.exec(cloneRevisionSql(second.head.id, columns)), "CHECK constraint failed", what);
    };
    refusedWith({{"state", quoted("bogus")}}, "an unknown state");
    refusedWith({{"terminal_status", quoted("running")}}, "a status that is not terminal");
    refusedWith({{"source_state_fingerprint", quoted(std::string(64, 'E'))}}, "an upper-case fingerprint");
    refusedWith({{"source_state_fingerprint", quoted(std::string(63, 'e'))}}, "a short fingerprint");
    refusedWith({{"content_fingerprint", quoted("zz")}}, "a content fingerprint that is no digest");
    refusedWith({{"counts_json", quoted("[]")}}, "counts that are not an object");
    refusedWith({{"reasons_json", quoted("{}")}}, "reasons that are not an array");
    refusedWith({{"member_decisions_json", quoted("")}}, "no member decisions at all");
    refusedWith({{"reasons_json", quoted("[\"source_ledger_missing\"]")}}, "reasons next to a state that needs none");
    refusedWith({{"state", quoted("attention_required")}}, "attention_required without reasons");
    refusedWith({{"recovery_eligible", "2"}}, "a flag that is not 0 or 1");
    refusedWith({{"rendered_text", "hex(zeroblob(40000))"}}, "a rendering above 64 KiB");
    refusedWith({{"rendered_text", quoted("")}}, "an empty rendering");
    refusedWith({{"source_state_json", quoted("")}}, "an empty source state");
    refusedWith({{"policy_id", quoted("")}}, "an empty policy id");
    require(raw.scalar("SELECT COUNT(*) FROM outcome_brief_revisions") == 3, "None of the refused rows is there");

    // The head row can only point at the NEWEST revision of its own Run.
    require(raw.exec("DELETE FROM outcome_brief_heads WHERE run_id = 'run-1';").empty(), "A lost head row may be removed (it is derived)");
    expectFailureContaining(raw.exec("INSERT INTO outcome_brief_heads (run_id, revision_id, updated_at_ms) VALUES ('run-1', '" + first.head.id + "', 1);"),
        "outcome_brief_head_not_latest", "a head row at an older revision");
    expectFailureContaining(raw.exec("INSERT INTO outcome_brief_heads (run_id, revision_id, updated_at_ms) VALUES ('run-2', '" + second.head.id + "', 1);"),
        "outcome_brief_head_not_latest", "a head row at the revision of another Run");
    // The first revision of run-1 has ordinal 1, which is also the newest ordinal of run-2: only the Run itself can refuse it.
    expectFailureContaining(raw.exec("INSERT INTO outcome_brief_heads (run_id, revision_id, updated_at_ms) VALUES ('run-2', '" + first.head.id + "', 1);"),
        "outcome_brief_head_not_latest", "a head row at a revision of another Run with the same ordinal");
    require(raw.exec("INSERT INTO outcome_brief_heads (run_id, revision_id, updated_at_ms) VALUES ('run-1', '" + second.head.id + "', 1);").empty(),
        "...while a head row at the newest revision is fine");
    expectFailureContaining(raw.exec("UPDATE outcome_brief_heads SET revision_id = '" + first.head.id + "' WHERE run_id = 'run-1';"),
        "outcome_brief_head_not_latest", "moving the head back");
    expectFailureContaining(raw.exec("UPDATE outcome_brief_heads SET run_id = 'run-2' WHERE run_id = 'run-1';"),
        "outcome_brief_head_", "moving a head to another Run");
    // Even together with a revision that is the newest of the other Run, a head row never changes its Run.
    require(raw.exec("DELETE FROM outcome_brief_heads WHERE run_id = 'run-2';").empty(), "setup: run-2 loses its head row");
    expectFailureContaining(raw.exec("UPDATE outcome_brief_heads SET run_id = 'run-2', revision_id = '" + other.head.id + "' WHERE run_id = 'run-1';"),
        "outcome_brief_head_identity_immutable", "re-pointing a head row at another Run and its revision");

    // Restrict: neither the Run nor the project can go while a brief is about them. The head rows hold the Run too, so they
    // are removed first: it is the revisions alone that must refuse.
    require(raw.exec("DELETE FROM outcome_brief_heads;").empty(), "setup: no head row is left");
    expectFailureContaining(raw.exec("DELETE FROM runs WHERE id = 'run-1';"), "FOREIGN KEY constraint failed", "deleting a Run with briefs");
    expectFailureContaining(raw.exec("DELETE FROM projects WHERE id = '" + kProjectOne + "';"), "FOREIGN KEY constraint failed",
        "deleting a project with briefs");
    expectFailureContaining(raw.exec("UPDATE runs SET id = 'renamed' WHERE id = 'run-1';"), "FOREIGN KEY constraint failed", "renaming a Run with briefs");
    expectFailureContaining(raw.exec("INSERT INTO outcome_brief_heads (run_id, revision_id, updated_at_ms) VALUES ('no-run', 'no-revision', 1);"),
        "outcome_brief_head_not_latest", "a head row of nothing");
}

// Defense in depth: the unique key refuses a second revision with the same ordinal even when the ordinal guard is gone.
void testTheOrdinalIsUniqueEvenWithoutItsTrigger()
{
    auto fx = newFixture();
    const auto env = fx->env(fx);
    RawConnection raw(fx->workspace.database);
    oc::mustAppend(fx->repository, oc::briefA(env, "run-1", kProjectOne), OutcomeBriefExpectedHead::none(), "A");
    const auto second = oc::mustAppend(fx->repository, oc::briefB(env, "run-1", kProjectOne), OutcomeBriefExpectedHead::any(), "B");
    require(raw.exec("DROP TRIGGER outcome_brief_revisions_validate_ordinal;").empty(), "setup: the ordinal guard is removed");
    expectFailureContaining(raw.exec(cloneRevisionSql(second.head.id, {{"id", quoted("dup")}, {"source_state_fingerprint", quoted(std::string(64, 'e'))}})),
        "UNIQUE constraint failed", "a second revision with ordinal 2");
    require(raw.scalar("SELECT COUNT(*) FROM outcome_brief_revisions") == 2, "...and nothing was added");
}

// A pointer that is lost or stale is repaired; it never decides what the head is.
void testTheHeadRowIsOnlyADerivedPointer()
{
    auto fx = newFixture();
    const auto env = fx->env(fx);
    RawConnection raw(fx->workspace.database);
    const auto a = oc::mustAppend(fx->repository, oc::briefA(env, "run-1", kProjectOne), OutcomeBriefExpectedHead::none(), "A");
    const auto b = oc::mustAppend(fx->repository, oc::briefB(env, "run-1", kProjectOne), OutcomeBriefExpectedHead::any(), "B");
    const auto pointer = [&] { return raw.text("SELECT revision_id FROM outcome_brief_heads WHERE run_id = 'run-1'"); };
    require(pointer() == b.head.id, "setup: the pointer is the head");

    // Lost.
    require(raw.exec("DELETE FROM outcome_brief_heads;").empty(), "setup: the pointer is lost");
    const auto head = oc::headOf(fx->repository, "run-1");
    require(head.has_value() && head->id == b.head.id, "A lost pointer does not change the head");
    const auto listed = fx->repository.listHeads(kProjectOne, 10);
    require(listed.success() && listed.value().size() == 1 && listed.value()[0].id == b.head.id, "...nor the list of heads");
    require(raw.scalar("SELECT COUNT(*) FROM outcome_brief_heads") == 0, "Reading does not write");
    const auto repeat = oc::mustAppend(fx->repository, oc::briefB(env, "run-1", kProjectOne), OutcomeBriefExpectedHead::revision(b.head.id), "B again");
    require(!repeat.appended && repeat.head.id == b.head.id, "A repeat of the head is not a revision");
    require(raw.scalar("SELECT COUNT(*) FROM outcome_brief_heads") == 1 && pointer() == b.head.id, "...but it puts the pointer back");

    // Stale (only possible by taking the guard away first).
    require(raw.exec("DROP TRIGGER outcome_brief_heads_validate_move;").empty(), "setup: the guard of the pointer is removed");
    require(raw.exec("UPDATE outcome_brief_heads SET revision_id = '" + a.head.id + "' WHERE run_id = 'run-1';").empty(), "setup: a stale pointer");
    require(oc::headOf(fx->repository, "run-1")->id == b.head.id, "A stale pointer does not change the head");
    require(fx->repository.listHeads(kProjectOne, 10).value()[0].id == b.head.id, "...nor the list of heads");
    oc::mustAppend(fx->repository, oc::briefB(env, "run-1", kProjectOne), OutcomeBriefExpectedHead::any(), "B once more");
    require(pointer() == b.head.id, "A repeat of the head repairs a stale pointer");
    require(raw.exec("UPDATE outcome_brief_heads SET revision_id = '" + a.head.id + "' WHERE run_id = 'run-1';").empty(), "setup: stale again");
    const auto c = oc::mustAppend(fx->repository, oc::briefC(env, "run-1", kProjectOne), OutcomeBriefExpectedHead::any(), "C");
    require(c.appended && c.head.ordinal == 3 && pointer() == c.head.id, "A new revision moves a stale pointer to itself");
}

// A row damaged behind the repository's back is refused when it is read, never shown and never built upon.
void testDamagedRowsAreRefusedOnRead()
{
    struct Damage {
        std::string name;
        std::map<std::string, std::string> overrides;
    };
    const std::vector<Damage> damages = {
        {"a state the content does not lead to", {{"state", quoted("awaiting_apply")}}},
        {"a rendering that is not the canonical one", {{"rendered_text", "rendered_text || 'x'"}}},
        {"a content fingerprint that is not the hash", {{"content_fingerprint", quoted(std::string(64, 'd'))}}},
        {"counts that are missing keys", {{"counts_json", quoted("{}")}}},
        {"counts with a wrong type", {{"counts_json", "replace(counts_json, '\"members\":1', '\"members\":\"1\"')"}}},
        {"a reason nobody knows", {{"state", quoted("attention_required")}, {"reasons_json", quoted("[\"bogus\"]")}}},
        {"a reason without a proof", {{"state", quoted("attention_required")}, {"reasons_json", quoted("[\"apply_failed\"]")}}},
        {"member decisions of the wrong shape", {{"member_decisions_json", quoted("[1]")}}},
        {"member decisions nobody knows", {{"member_decisions_json", quoted("[{\"changeSetId\":\"cs-1\",\"decision\":\"bogus\"}]")}}},
        {"a workflow the source state does not name", {{"workflow_id", quoted("some-other-workflow")}}},
        {"a recovery flag the status does not allow", {{"recovery_eligible", "1"}}},
        {"a source state that is not the document", {{"source_state_json", "source_state_json || ' '"}}},
    };

    for (const Damage& damage : damages) {
        auto fx = newFixture();
        const auto env = fx->env(fx);
        RawConnection raw(fx->workspace.database);
        const auto first = oc::mustAppend(fx->repository, oc::briefA(env, "run-1", kProjectOne), OutcomeBriefExpectedHead::none(), "A");
        const auto second = oc::mustAppend(fx->repository, oc::briefB(env, "run-1", kProjectOne), OutcomeBriefExpectedHead::any(), "B");
        oc::mustAppend(fx->repository, oc::briefA(env, "run-other", kProjectTwo), OutcomeBriefExpectedHead::none(), "a healthy Run elsewhere");

        // The damaged row is the THIRD revision and a copy of the first one: its source state is a real one that differs from
        // the revision before it, so no schema guard objects and only the damage itself can make the gate refuse it.
        std::map<std::string, std::string> overrides = damage.overrides;
        overrides["id"] = quoted("damaged-row");
        overrides["ordinal"] = "3";
        const std::string refused = raw.exec(cloneRevisionSql(first.head.id, overrides));
        require(refused.empty(), damage.name + ": the schema alone lets the damaged row in, but got: " + refused);

        const auto refuse = [&](const auto& result, const std::string& where) {
            require(!result.success() && result.error()->code == "persistence.invalid_data",
                damage.name + " (" + where + "): the damaged row must be refused as invalid data, got "
                    + (result.error().has_value() ? result.error()->code : std::string("success")));
        };
        refuse(fx->repository.findHead("run-1"), "findHead");
        refuse(fx->repository.findHeadVersionForAppend("run-1"), "findHeadVersionForAppend");
        refuse(fx->repository.findRevision("damaged-row"), "findRevision");
        refuse(fx->repository.listRevisions("run-1", 10), "listRevisions");
        refuse(fx->repository.listHeads(kProjectOne, 10), "listHeads");
        refuse(fx->repository.append(oc::briefB(env, "run-1", kProjectOne), OutcomeBriefExpectedHead::any()), "append");
        require(raw.scalar("SELECT COUNT(*) FROM outcome_brief_revisions WHERE run_id = 'run-1'") == 3, damage.name + ": nothing was built on it");
        require(oc::headOf(fx->repository, "run-other").has_value(), damage.name + ": a Run elsewhere is still readable");
        require(fx->repository.listHeads(kProjectTwo, 10).success(), damage.name + ": another project is still listable");
        // The revisions before it are still fine.
        const auto healthy = fx->repository.findRevision(second.head.id);
        require(healthy.success() && healthy.value().has_value() && oc::sameRevision(*healthy.value(), second.head),
            damage.name + ": the revision before it is intact");
    }
}

// ---- migration and reopen -------------------------------------------------------------------------------------------------

void testPersistenceAcrossReopenAndTheV64Migration()
{
    BriefWorkspace workspace;
    OutcomeBriefRevision storedA;
    OutcomeBriefRevision storedB;
    {
        const auto database = openDatabase(workspace.database);
        seedRun(database, "run-1", kProjectOne);
        oc::Env env;
        env.projectId = kProjectOne;
        env.otherProjectId = kProjectTwo;
        SqliteOutcomeBriefRepository repository(database);
        storedA = oc::mustAppend(repository, oc::briefA(env, "run-1", kProjectOne), OutcomeBriefExpectedHead::none(), "A").head;
        storedB = oc::mustAppend(repository, oc::briefB(env, "run-1", kProjectOne), OutcomeBriefExpectedHead::any(), "B").head;
    }
    {
        // A reopen reads exactly what was written, in the same transaction order.
        const auto database = openDatabase(workspace.database);
        SqliteOutcomeBriefRepository repository(database);
        const auto head = oc::headOf(repository, "run-1");
        require(head.has_value() && oc::sameRevision(*head, storedB), "The head survives a reopen");
        const auto history = repository.listRevisions("run-1", 10).value();
        require(history.size() == 2 && oc::sameRevision(history[0], storedA) && oc::sameRevision(history[1], storedB),
            "...and so does the whole history");
        RawConnection raw(workspace.database);
        require(raw.scalar("PRAGMA user_version") == 70, "The database is at the latest schema");
    }

    // The v63 shape: everything the earlier schemas hold, and none of the two new tables.
    {
        RawConnection raw(workspace.database, /*foreignKeys=*/false);
        require(raw.exec("DROP TABLE outcome_brief_heads; DROP TABLE outcome_brief_revisions; PRAGMA user_version = 63;").empty(),
            "setup: rewinding to v63");
        require(raw.scalar("SELECT COUNT(*) FROM sqlite_master WHERE name LIKE 'outcome_brief%'") == 0, "setup: no brief object is left");
    }
    {
        const auto database = openDatabase(workspace.database);
        RawConnection raw(workspace.database);
        require(raw.scalar("PRAGMA user_version") == 70, "A v63 database migrates to the latest schema");
        require(raw.scalar("SELECT COUNT(*) FROM outcome_brief_revisions") == 0 && raw.scalar("SELECT COUNT(*) FROM outcome_brief_heads") == 0,
            "...with empty brief tables");
        require(raw.scalar("SELECT COUNT(*) FROM runs WHERE id = 'run-1'") == 1, "...and the older data intact");
        SqliteOutcomeBriefRepository repository(database);
        oc::Env env;
        env.projectId = kProjectOne;
        env.otherProjectId = kProjectTwo;
        const auto first = oc::mustAppend(repository, oc::briefA(env, "run-1", kProjectOne), OutcomeBriefExpectedHead::none(), "after the migration");
        require(first.appended && first.head.ordinal == 1, "The migrated database takes the first brief");
        oc::mustAppend(repository, oc::briefB(env, "run-1", kProjectOne), OutcomeBriefExpectedHead::any(), "a second brief");
        require(raw.scalar("SELECT COUNT(*) FROM sqlite_master WHERE type = 'trigger' AND name LIKE 'outcome_brief%'") == 7,
            "The guards exist (seven triggers)");
    }

    // Running the migration again over a database that already has the tables and the data keeps both, and puts the guards
    // back (a trigger that was lost is created again).
    {
        RawConnection raw(workspace.database, /*foreignKeys=*/false);
        require(raw.exec("DROP TRIGGER outcome_brief_revisions_reject_update; PRAGMA user_version = 63;").empty(), "setup: v63 again, one guard lost");
    }
    {
        const auto database = openDatabase(workspace.database);
        RawConnection raw(workspace.database);
        require(raw.scalar("PRAGMA user_version") == 70, "The second migration ends at the latest schema too");
        require(raw.scalar("SELECT COUNT(*) FROM outcome_brief_revisions") == 2 && raw.scalar("SELECT COUNT(*) FROM outcome_brief_heads") == 1,
            "...keeping the revisions and the head");
        expectFailureContaining(raw.exec("UPDATE outcome_brief_revisions SET state = state;"), "outcome_brief_revision_immutable",
            "the lost guard is back");
        SqliteOutcomeBriefRepository repository(database);
        const auto history = repository.listRevisions("run-1", 10);
        require(history.success() && history.value().size() == 2 && history.value()[1].brief.state == OutcomeBriefState::ResolvedApplied,
            "The history is still readable");
    }
}

// ---- several connections -------------------------------------------------------------------------------------------------

// Two connections to one file (as two processes would be): the write lock, not the in-process mutex, orders the writers.
void testWritersOnSeparateConnectionsAreOrderedByTheDatabase()
{
    auto fx = newFixture();
    const auto env = fx->env(fx);
    const auto secondDatabase = openDatabase(fx->workspace.database);
    SqliteOutcomeBriefRepository second(secondDatabase);
    IOutcomeBriefRepository* repositories[] = {&fx->repository, &second};
    constexpr int kWriters = 8;

    {
        // Everybody expects "no head": one wins whatever the connection.
        std::vector<OutcomeBrief> briefs;
        for (int index = 1; index <= kWriters; ++index) briefs.push_back(oc::briefMany(env, "run-1", kProjectOne, index));
        std::atomic<int> won {0};
        oc::runTogether(kWriters, [&](int index) {
            const auto result = repositories[index % 2]->append(briefs[static_cast<std::size_t>(index)], OutcomeBriefExpectedHead::none());
            if (result.success()) {
                ++won;
            } else {
                require(result.error()->code == "outcome_brief.head_changed", "a loser hears head_changed, got " + result.error()->code);
            }
        });
        require(won.load() == 1, "Exactly one writer wins across two connections");
        require(oc::revisionCount(fx->repository, "run-1") == 1 && oc::revisionCount(second, "run-1") == 1, "...and both connections see one revision");
    }
    {
        // Optimistic writers on both connections: every brief lands once, the numbering has no gap.
        constexpr int kOther = 6;
        std::vector<OutcomeBrief> briefs;
        for (int index = 1; index <= kOther; ++index) briefs.push_back(oc::briefMany(env, "run-2", kProjectOne, index));
        oc::runTogether(kOther, [&](int index) {
            auto& repository = *repositories[index % 2];
            for (int attempt = 0; attempt < 400; ++attempt) {
                const auto head = repository.findHead("run-2");
                require(head.success(), "findHead must succeed");
                const auto expected = head.value().has_value() ? OutcomeBriefExpectedHead::revision(head.value()->id) : OutcomeBriefExpectedHead::none();
                const auto result = repository.append(briefs[static_cast<std::size_t>(index)], expected);
                if (result.success()) return;
                require(result.error()->code == "outcome_brief.head_changed", "only head_changed may be retried, got " + result.error()->code);
            }
            throw std::runtime_error("a writer never got through");
        });
        const auto history = fx->repository.listRevisions("run-2", 100).value();
        require(history.size() == static_cast<std::size_t>(kOther), "Every optimistic writer's brief was stored once");
        for (std::size_t index = 0; index < history.size(); ++index) {
            require(history[index].ordinal == index + 1, "The ordinals are 1..n without gaps");
        }
        RawConnection raw(fx->workspace.database);
        require(raw.text("SELECT revision_id FROM outcome_brief_heads WHERE run_id = 'run-2'") == history.back().id,
            "The head row follows the last revision");
    }
    {
        // The same brief from both connections, no expectation: one revision.
        const OutcomeBrief brief = oc::briefA(env, "run-3", kProjectOne);
        std::atomic<int> appended {0};
        oc::runTogether(kWriters, [&](int index) {
            const auto result = repositories[index % 2]->append(brief, OutcomeBriefExpectedHead::any());
            require(result.success(), "a repeat must succeed");
            if (result.value().appended) ++appended;
        });
        require(appended.load() == 1 && oc::revisionCount(second, "run-3") == 1, "Racing repeats on two connections are one revision");
    }
}

} // namespace

int runOutcomeBriefRepositoryTests()
{
    testTheContractOnSqlite();
    testRunPagingOnSqlite();
    testEveryKindOfBriefRoundTrips();
    testRevisionAndHeadAreWrittenInOneTransaction();
    testABriefOfAnotherProjectIsNeverRead();
    testTheSchemaRefusesWhatTheRepositoryNeverWrites();
    testTheOrdinalIsUniqueEvenWithoutItsTrigger();
    testTheHeadRowIsOnlyADerivedPointer();
    testDamagedRowsAreRefusedOnRead();
    testPersistenceAcrossReopenAndTheV64Migration();
    testWritersOnSeparateConnectionsAreOrderedByTheDatabase();
    return 0;
}
