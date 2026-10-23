#include "../SqliteConnection.hpp"
#include "../SqlitePrimitives.hpp"
#include "../execution/SqliteRunStatusMapping.hpp"

#include "ai/application/IOutcomeBriefRepository.hpp"

#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ai::infrastructure {
namespace {

using sqlite_detail::Statement;
using sqlite_detail::error;
using sqlite_detail::databaseError;
using sqlite_detail::execute;
using sqlite_detail::prepare;
using sqlite_detail::bindText;
using sqlite_detail::bindNullableText;
using sqlite_detail::bindInt64;
using sqlite_detail::bindUnsigned;
using sqlite_detail::toMilliseconds;
using sqlite_detail::fromMilliseconds;
using sqlite_detail::columnText;
using sqlite_detail::optionalText;
using sqlite_detail::runStatusFromString;

} // namespace

SqliteOutcomeBriefRepository::SqliteOutcomeBriefRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

namespace {

constexpr const char* outcomeBriefRevisionColumns =
    "id,run_id,project_id,ordinal,schema_version,policy_id,workflow_id,terminal_status,state,reasons_json,counts_json,"
    "member_decisions_json,recovery_eligible,requires_inspection,continuation_source_run_id,"
    "continuation_child_run_id,manual_handoff_id,source_state_json,source_state_fingerprint,rendered_text,"
    "content_fingerprint,created_at_ms";

// What EVERY read selects (so no read can skip the Run authority): the revision, then - from the Run it is about - the Run's
// current project and status, and whether this revision is the newest of its Run. The LEFT JOIN keeps a revision whose Run is
// gone in the result, so that it can be refused instead of disappearing. Columns 0..21 are outcomeBriefRevisionColumns.
constexpr const char* outcomeBriefSelect =
    "SELECT r.id,r.run_id,r.project_id,r.ordinal,r.schema_version,r.policy_id,r.workflow_id,r.terminal_status,r.state,"
    "r.reasons_json,r.counts_json,r.member_decisions_json,r.recovery_eligible,r.requires_inspection,"
    "r.continuation_source_run_id,r.continuation_child_run_id,r.manual_handoff_id,r.source_state_json,"
    "r.source_state_fingerprint,r.rendered_text,r.content_fingerprint,r.created_at_ms,"
    "runs.project_id,runs.status,"
    "r.ordinal = (SELECT MAX(ordinal) FROM outcome_brief_revisions WHERE run_id = r.run_id) "
    "FROM outcome_brief_revisions AS r LEFT JOIN runs ON runs.id = r.run_id ";

using OutcomeBriefCountField = std::uint32_t application::OutcomeBriefCounts::*;
// One table for both directions, so what is written is exactly what is read back.
const std::pair<const char*, OutcomeBriefCountField> outcomeBriefCountFields[] = {
    {"members", &application::OutcomeBriefCounts::members},
    {"reviewable", &application::OutcomeBriefCounts::reviewable},
    {"applied", &application::OutcomeBriefCounts::applied},
    {"humanRejected", &application::OutcomeBriefCounts::humanRejected},
    {"approved", &application::OutcomeBriefCounts::approved},
    {"undecided", &application::OutcomeBriefCounts::undecided},
    {"applyFailed", &application::OutcomeBriefCounts::applyFailed},
    {"noChanges", &application::OutcomeBriefCounts::noChanges},
    {"validatorRejected", &application::OutcomeBriefCounts::validatorRejected},
    {"superseded", &application::OutcomeBriefCounts::superseded},
    {"unsettled", &application::OutcomeBriefCounts::unsettled},
    {"inconsistent", &application::OutcomeBriefCounts::inconsistent},
};

std::string outcomeBriefReasonsJson(const std::vector<application::OutcomeBriefReason>& reasons)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto reason : reasons) array.push_back(application::toString(reason));
    return array.dump();
}

std::string outcomeBriefCountsJson(const application::OutcomeBriefCounts& counts)
{
    nlohmann::json object = nlohmann::json::object();
    for (const auto& [name, field] : outcomeBriefCountFields) object[name] = counts.*field;
    return object.dump();
}

std::string outcomeBriefMemberDecisionsJson(const std::vector<application::OutcomeBriefMemberDecision>& members)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& member : members) {
        array.push_back({{"changeSetId", member.changeSetId}, {"decision", application::toString(member.decision)}});
    }
    return array.dump();
}

bool parseOutcomeBriefReasons(const std::string& text, std::vector<application::OutcomeBriefReason>& reasons)
{
    const auto parsed = nlohmann::json::parse(text, nullptr, false);
    if (!parsed.is_array()) return false;
    for (const auto& item : parsed) {
        if (!item.is_string()) return false;
        const auto reason = application::outcomeBriefReasonFromString(item.get_ref<const std::string&>());
        if (!reason.has_value()) return false;
        reasons.push_back(*reason);
    }
    return true;
}

bool parseOutcomeBriefCounts(const std::string& text, application::OutcomeBriefCounts& counts)
{
    const auto parsed = nlohmann::json::parse(text, nullptr, false);
    if (!parsed.is_object() || parsed.size() != std::size(outcomeBriefCountFields)) return false;
    for (const auto& [name, field] : outcomeBriefCountFields) {
        const auto found = parsed.find(name);
        if (found == parsed.end() || !found->is_number_unsigned()) return false;
        const auto value = found->get<std::uint64_t>();
        if (value > std::numeric_limits<std::uint32_t>::max()) return false;
        counts.*field = static_cast<std::uint32_t>(value);
    }
    return true;
}

bool parseOutcomeBriefMemberDecisions(const std::string& text, std::vector<application::OutcomeBriefMemberDecision>& members)
{
    const auto parsed = nlohmann::json::parse(text, nullptr, false);
    if (!parsed.is_array()) return false;
    for (const auto& item : parsed) {
        if (!item.is_object() || item.size() != 2) return false;
        const auto id = item.find("changeSetId");
        const auto decision = item.find("decision");
        if (id == item.end() || decision == item.end() || !id->is_string() || !decision->is_string()) return false;
        const auto parsedDecision = application::outcomeProposalDecisionFromString(decision->get_ref<const std::string&>());
        if (!parsedDecision.has_value()) return false;
        members.push_back({id->get<std::string>(), *parsedDecision});
    }
    return true;
}

// A stored row is read back like any consumer would: it must still be exactly what the resolver could have derived, and still
// agree with the Run it is about (project of every revision; terminal status of the current one when `headStatusAuthority`).
// A row that was damaged behind the repository's back, or went stale, is refused, never repaired and never shown.
common::Result<application::OutcomeBriefRevision> readOutcomeBriefRevision(sqlite3_stmt* statement, bool headStatusAuthority)
{
    using Result = common::Result<application::OutcomeBriefRevision>;
    const auto invalid = [](const std::string& what) {
        return Result::failure(error("persistence.invalid_data", "Outcome brief row " + what));
    };
    application::OutcomeBriefRevision revision;
    application::OutcomeBrief& brief = revision.brief;
    revision.id = columnText(statement, 0);
    brief.runId = columnText(statement, 1);
    brief.projectId = columnText(statement, 2);
    const std::int64_t ordinal = sqlite3_column_int64(statement, 3);
    if (ordinal < 1) return invalid("has an invalid ordinal");
    revision.ordinal = static_cast<std::uint64_t>(ordinal);
    brief.schemaVersion = columnText(statement, 4);
    brief.policyId = columnText(statement, 5);
    brief.workflowId = columnText(statement, 6);
    const auto status = runStatusFromString(columnText(statement, 7));
    if (!status.has_value()) return invalid("contains an unknown terminal status");
    brief.terminalStatus = *status;
    const auto state = application::outcomeBriefStateFromString(columnText(statement, 8));
    if (!state.has_value()) return invalid("contains an unknown state");
    brief.state = *state;
    if (!parseOutcomeBriefReasons(columnText(statement, 9), brief.reasons)) return invalid("has unreadable reasons");
    if (!parseOutcomeBriefCounts(columnText(statement, 10), brief.counts)) return invalid("has unreadable counts");
    if (!parseOutcomeBriefMemberDecisions(columnText(statement, 11), brief.memberDecisions)) {
        return invalid("has unreadable member decisions");
    }
    brief.recoveryEligible = sqlite3_column_int(statement, 12) != 0;
    brief.requiresInspection = sqlite3_column_int(statement, 13) != 0;
    brief.continuationSourceRunId = optionalText(statement, 14);
    brief.continuationChildRunId = optionalText(statement, 15);
    brief.manualHandoffId = optionalText(statement, 16);
    brief.sourceStateJson = columnText(statement, 17);
    brief.sourceStateFingerprint = columnText(statement, 18);
    brief.renderedText = columnText(statement, 19);
    brief.contentFingerprint = columnText(statement, 20);
    revision.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 21));

    const auto gate = application::validateOutcomeBrief(brief);
    if (!gate.success()) {
        return invalid("does not pass the Outcome Brief gate: " + gate.error()->message);
    }

    std::optional<application::OutcomeBriefRunOwner> owner;
    if (sqlite3_column_type(statement, 22) != SQLITE_NULL) {
        const auto ownerStatus = runStatusFromString(columnText(statement, 23));
        if (!ownerStatus.has_value()) return invalid("is about a Run with an unknown status");
        owner = application::OutcomeBriefRunOwner {columnText(statement, 22), *ownerStatus};
    }
    const bool isHead = sqlite3_column_int(statement, 24) != 0;
    const auto authority = application::checkOutcomeBriefReadAuthority(brief, owner, isHead && headStatusAuthority);
    if (!authority.success()) return Result::failure(*authority.error());
    return Result::ok(std::move(revision));
}

common::Result<std::vector<application::OutcomeBriefRevision>> collectOutcomeBriefRevisions(
    sqlite3* handle, sqlite3_stmt* statement, bool headStatusAuthority = true)
{
    using Result = common::Result<std::vector<application::OutcomeBriefRevision>>;
    std::vector<application::OutcomeBriefRevision> revisions;
    for (;;) {
        const int step = sqlite3_step(statement);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return Result::failure(databaseError(handle, "persistence.read_failed", "Could not read outcome briefs"));
        }
        auto revision = readOutcomeBriefRevision(statement, headStatusAuthority);
        if (!revision.success()) return Result::failure(*revision.error());
        revisions.push_back(std::move(revision.value()));
    }
    return Result::ok(std::move(revisions));
}

// The head of a Run is its revision with the highest ordinal. The outcome_brief_heads row is only a derived pointer to it
// (maintained by append and guarded by triggers), so a lost or stale pointer can never hide or invent a state. `append` reads it
// with `headStatusAuthority = false`: the head it compares with may have gone stale because the Run moved on, and the new brief
// that replaces it must not be blocked by that (a foreign project, a missing Run or a damaged row still refuse).
common::Result<std::optional<application::OutcomeBriefRevision>> readOutcomeBriefHead(
    sqlite3* handle, const application::RunId& runId, bool headStatusAuthority)
{
    using Result = common::Result<std::optional<application::OutcomeBriefRevision>>;
    Statement statement;
    const std::string sql = std::string(outcomeBriefSelect) + "WHERE r.run_id=? ORDER BY r.ordinal DESC LIMIT 1;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) return Result::failure(*bound.error());
    auto rows = collectOutcomeBriefRevisions(handle, statement.get(), headStatusAuthority);
    if (!rows.success()) return Result::failure(*rows.error());
    if (rows.value().empty()) return Result::ok(std::nullopt);
    return Result::ok(std::move(rows.value().front()));
}

common::Result<void> bindOutcomeBriefRevision(
    sqlite3* handle, sqlite3_stmt* statement, const application::OutcomeBriefRevision& revision)
{
    const application::OutcomeBrief& brief = revision.brief;
    const common::Result<void> bindings[] = {
        bindText(handle, statement, 1, revision.id),
        bindText(handle, statement, 2, brief.runId),
        bindText(handle, statement, 3, brief.projectId),
        bindUnsigned(handle, statement, 4, revision.ordinal),
        bindText(handle, statement, 5, brief.schemaVersion),
        bindText(handle, statement, 6, brief.policyId),
        bindText(handle, statement, 7, brief.workflowId),
        bindText(handle, statement, 8, application::toString(brief.terminalStatus)),
        bindText(handle, statement, 9, application::toString(brief.state)),
        bindText(handle, statement, 10, outcomeBriefReasonsJson(brief.reasons)),
        bindText(handle, statement, 11, outcomeBriefCountsJson(brief.counts)),
        bindText(handle, statement, 12, outcomeBriefMemberDecisionsJson(brief.memberDecisions)),
        bindInt64(handle, statement, 13, brief.recoveryEligible ? 1 : 0),
        bindInt64(handle, statement, 14, brief.requiresInspection ? 1 : 0),
        bindNullableText(handle, statement, 15, brief.continuationSourceRunId),
        bindNullableText(handle, statement, 16, brief.continuationChildRunId),
        bindNullableText(handle, statement, 17, brief.manualHandoffId),
        bindText(handle, statement, 18, brief.sourceStateJson),
        bindText(handle, statement, 19, brief.sourceStateFingerprint),
        bindText(handle, statement, 20, brief.renderedText),
        bindText(handle, statement, 21, brief.contentFingerprint),
        bindInt64(handle, statement, 22, toMilliseconds(revision.createdAt)),
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return binding;
    }
    return common::Result<void>::ok();
}

// Points the head row at the newest revision. A pointer that is already right is left alone (the DO UPDATE does not fire),
// one that is missing is created and one that is stale is moved: the same statement repairs a lost pointer.
common::Result<void> pointOutcomeBriefHead(
    sqlite3* handle, const application::RunId& runId, const std::string& revisionId,
    std::chrono::system_clock::time_point updatedAt)
{
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO outcome_brief_heads (run_id,revision_id,updated_at_ms) VALUES (?,?,?) "
        "ON CONFLICT(run_id) DO UPDATE SET revision_id=excluded.revision_id, updated_at_ms=excluded.updated_at_ms "
        "WHERE outcome_brief_heads.revision_id <> excluded.revision_id;",
        statement);
    if (!prepared.success()) return prepared;
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, runId),
        bindText(handle, statement.get(), 2, revisionId),
        bindInt64(handle, statement.get(), 3, toMilliseconds(updatedAt)),
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return binding;
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not move the outcome brief head"));
    }
    return common::Result<void>::ok();
}

} // namespace

common::Result<application::OutcomeBriefAppendResult> SqliteOutcomeBriefRepository::append(
    application::OutcomeBrief brief, application::OutcomeBriefExpectedHead expected)
{
    using Result = common::Result<application::OutcomeBriefAppendResult>;
    // Validated before the first statement; a refusal never opens a transaction.
    const auto checked = application::checkOutcomeBriefAppendRequest(brief, expected);
    if (!checked.success()) return Result::failure(*checked.error());

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    // BEGIN IMMEDIATE takes the write lock first, so a second writer (another thread or another connection) waits here
    // and then sees the head this transaction leaves behind: the compare-and-set below cannot be raced.
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return Result::failure(*began.error());
    const auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return Result::failure(failure);
    };

    {
        // The write authority (shared with the in-memory adapter): the Run this brief is about must exist, belong to the
        // brief's project and still be in the status the brief says.
        Statement statement;
        const auto prepared = prepare(handle, "SELECT project_id,status FROM runs WHERE id=?;", statement);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const auto bound = bindText(handle, statement.get(), 1, brief.runId);
        if (!bound.success()) return rollbackWith(*bound.error());
        const int step = sqlite3_step(statement.get());
        if (step != SQLITE_DONE && step != SQLITE_ROW) {
            return rollbackWith(databaseError(handle, "persistence.read_failed", "Could not read the Run"));
        }
        std::optional<application::OutcomeBriefRunOwner> owner;
        if (step == SQLITE_ROW) {
            const auto ownerStatus = runStatusFromString(columnText(statement.get(), 1));
            if (!ownerStatus.has_value()) {
                return rollbackWith(error("persistence.invalid_data", "Run row contains an unknown status"));
            }
            owner = application::OutcomeBriefRunOwner {columnText(statement.get(), 0), *ownerStatus};
        }
        const auto authority = application::checkOutcomeBriefWriteAuthority(brief, owner);
        if (!authority.success()) return rollbackWith(*authority.error());
    }

    auto head = readOutcomeBriefHead(handle, brief.runId, /*headStatusAuthority=*/false);
    if (!head.success()) return rollbackWith(*head.error());
    const auto plan = application::planOutcomeBriefAppend(brief, expected, head.value());
    if (!plan.success()) return rollbackWith(*plan.error());

    application::OutcomeBriefAppendResult result;
    if (!plan.value().appendRevision) {
        // Nothing to add. The pointer is still made right, so a lost one is repaired by the next ordinary reconcile.
        const auto pointed = pointOutcomeBriefHead(
            handle, head.value()->brief.runId, head.value()->id, head.value()->createdAt);
        if (!pointed.success()) return rollbackWith(*pointed.error());
        result.head = *head.value();
        result.appended = false;
    } else {
        auto revision = application::makeOutcomeBriefRevision(std::move(brief), plan.value().ordinal);
        {
            Statement statement;
            const std::string sql = std::string("INSERT INTO outcome_brief_revisions (") + outcomeBriefRevisionColumns
                + ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
            const auto prepared = prepare(handle, sql.c_str(), statement);
            if (!prepared.success()) return rollbackWith(*prepared.error());
            const auto bound = bindOutcomeBriefRevision(handle, statement.get(), revision);
            if (!bound.success()) return rollbackWith(*bound.error());
            if (sqlite3_step(statement.get()) != SQLITE_DONE) {
                return rollbackWith(
                    databaseError(handle, "persistence.write_failed", "Could not insert the outcome brief revision"));
            }
        }
        const auto pointed = pointOutcomeBriefHead(handle, revision.brief.runId, revision.id, revision.createdAt);
        if (!pointed.success()) return rollbackWith(*pointed.error());
        result.head = std::move(revision);
        result.appended = true;
    }

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return Result::ok(std::move(result));
}

common::Result<std::optional<application::OutcomeBriefRevision>> SqliteOutcomeBriefRepository::findHead(
    const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    return readOutcomeBriefHead(database_->impl_->handle, runId, /*headStatusAuthority=*/true);
}

common::Result<std::optional<application::OutcomeBriefRevision>> SqliteOutcomeBriefRepository::findRevision(
    const application::OutcomeBriefRevisionId& id) const
{
    using Result = common::Result<std::optional<application::OutcomeBriefRevision>>;
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string(outcomeBriefSelect) + "WHERE r.id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success()) return Result::failure(*bound.error());
    auto rows = collectOutcomeBriefRevisions(handle, statement.get());
    if (!rows.success()) return Result::failure(*rows.error());
    if (rows.value().empty()) return Result::ok(std::nullopt);
    return Result::ok(std::move(rows.value().front()));
}

common::Result<std::vector<application::OutcomeBriefRevision>> SqliteOutcomeBriefRepository::listRevisions(
    const application::RunId& runId, std::size_t limit) const
{
    using Result = common::Result<std::vector<application::OutcomeBriefRevision>>;
    const auto checked = application::checkOutcomeBriefListLimit(limit);
    if (!checked.success()) return Result::failure(*checked.error());

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    // The newest `limit` revisions, handed back oldest first.
    const std::string sql = std::string(outcomeBriefSelect) + "WHERE r.run_id=? ORDER BY r.ordinal DESC LIMIT ?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, runId),
        bindInt64(handle, statement.get(), 2, static_cast<std::int64_t>(limit)),
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return Result::failure(*binding.error());
    }
    auto rows = collectOutcomeBriefRevisions(handle, statement.get());
    if (!rows.success()) return Result::failure(*rows.error());
    std::reverse(rows.value().begin(), rows.value().end());
    return Result::ok(std::move(rows.value()));
}

common::Result<std::vector<application::OutcomeBriefRevision>> SqliteOutcomeBriefRepository::listHeads(
    const application::ProjectId& projectId, std::size_t limit) const
{
    using Result = common::Result<std::vector<application::OutcomeBriefRevision>>;
    const auto checked = application::checkOutcomeBriefListLimit(limit);
    if (!checked.success()) return Result::failure(*checked.error());

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    // Same definition of the head as readOutcomeBriefHead (the highest ordinal of the Run), and the same order as
    // application::outcomeBriefHeadComesBefore. A head belongs to the list of the project it NAMES and to the list of the project
    // its Run belongs to: when the two differ, both lists meet the row and both fail closed.
    const std::string sql = std::string(outcomeBriefSelect)
        + "WHERE (r.project_id=? OR runs.project_id=?) AND r.ordinal = "
          "(SELECT MAX(ordinal) FROM outcome_brief_revisions WHERE run_id = r.run_id) "
          "ORDER BY r.created_at_ms DESC, r.run_id DESC LIMIT ?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, projectId),
        bindText(handle, statement.get(), 2, projectId),
        bindInt64(handle, statement.get(), 3, static_cast<std::int64_t>(limit)),
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return Result::failure(*binding.error());
    }
    return collectOutcomeBriefRevisions(handle, statement.get());
}

common::Result<std::optional<application::OutcomeBriefHeadVersion>> SqliteOutcomeBriefRepository::findHeadVersionForAppend(
    const application::RunId& runId) const
{
    using Result = common::Result<std::optional<application::OutcomeBriefHeadVersion>>;
    std::lock_guard lock(database_->impl_->mutex);
    // The very read that `append` compares with (gate, Run and project checked, status NOT), reduced to which revision it is.
    const auto head = readOutcomeBriefHead(database_->impl_->handle, runId, /*headStatusAuthority=*/false);
    if (!head.success()) return Result::failure(*head.error());
    if (!head.value().has_value()) {
        // No head: the Run must still exist ("no head" must not hide "no Run").
        Statement statement;
        const auto prepared = prepare(database_->impl_->handle, "SELECT 1 FROM runs WHERE id=?;", statement);
        if (!prepared.success()) return Result::failure(*prepared.error());
        const auto bound = bindText(database_->impl_->handle, statement.get(), 1, runId);
        if (!bound.success()) return Result::failure(*bound.error());
        const int step = sqlite3_step(statement.get());
        if (step != SQLITE_ROW && step != SQLITE_DONE) {
            return Result::failure(databaseError(database_->impl_->handle, "persistence.read_failed", "Could not read the Run"));
        }
        const auto exists = application::checkOutcomeBriefRunExists(step == SQLITE_ROW);
        if (!exists.success()) return Result::failure(*exists.error());
        return Result::ok(std::nullopt);
    }
    return Result::ok(application::OutcomeBriefHeadVersion {head.value()->id, head.value()->ordinal});
}

} // namespace ai::infrastructure
