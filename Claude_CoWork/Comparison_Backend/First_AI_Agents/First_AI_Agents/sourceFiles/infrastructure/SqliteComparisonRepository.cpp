#include "ai/infrastructure/SqliteComparisonRepository.hpp"
#include "ai/infrastructure/SqliteDatabase_Impl.hpp"

#include "ai/application/ComparisonTypes.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace ai::infrastructure {

namespace {

using application::ComparisonId;
using application::ComparisonResult;
using application::ComparisonResultId;
using application::ComparisonResultStatus;
using application::ComparisonSession;
using application::ComparisonStatus;
using application::ProjectId;

// ──────────────────────────────────────────────────────────────────────────
// Helpers — identical in behaviour to those in SqliteRepositories.cpp but
// re-declared here so this TU compiles independently.

common::Error dbError(sqlite3* handle, std::string code, const char* op)
{
    std::string msg(op);
    msg += ": ";
    msg += (handle ? sqlite3_errmsg(handle) : "SQLite handle unavailable");
    return {std::move(code), std::move(msg)};
}

class Statement final {
public:
    Statement() = default;
    ~Statement() { if (stmt_) sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    sqlite3_stmt** out() noexcept { return &stmt_; }
    sqlite3_stmt* get() const noexcept { return stmt_; }
private:
    sqlite3_stmt* stmt_ {nullptr};
};

common::Result<void> exec(sqlite3* h, const char* sql, std::string code)
{
    char* raw = nullptr;
    if (sqlite3_exec(h, sql, nullptr, nullptr, &raw) == SQLITE_OK) {
        return common::Result<void>::ok();
    }
    std::string msg = raw ? raw : sqlite3_errmsg(h);
    sqlite3_free(raw);
    return common::Result<void>::failure({std::move(code), std::move(msg)});
}

common::Result<void> prepare(sqlite3* h, const char* sql, Statement& s)
{
    if (sqlite3_prepare_v2(h, sql, -1, s.out(), nullptr) != SQLITE_OK) {
        return common::Result<void>::failure(
            dbError(h, "persistence.prepare_failed", "Could not prepare statement"));
    }
    return common::Result<void>::ok();
}

common::Result<void> bindText(sqlite3* h, sqlite3_stmt* s, int idx, const std::string& v)
{
    if (sqlite3_bind_text(s, idx, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
        return common::Result<void>::failure(
            dbError(h, "persistence.bind_failed", "Could not bind text"));
    }
    return common::Result<void>::ok();
}

common::Result<void> bindNullableText(sqlite3* h, sqlite3_stmt* s, int idx, const std::optional<std::string>& v)
{
    if (v.has_value()) return bindText(h, s, idx, *v);
    if (sqlite3_bind_null(s, idx) != SQLITE_OK) {
        return common::Result<void>::failure(
            dbError(h, "persistence.bind_failed", "Could not bind null text"));
    }
    return common::Result<void>::ok();
}

common::Result<void> bindInt64(sqlite3* h, sqlite3_stmt* s, int idx, std::int64_t v)
{
    if (sqlite3_bind_int64(s, idx, v) != SQLITE_OK) {
        return common::Result<void>::failure(
            dbError(h, "persistence.bind_failed", "Could not bind integer"));
    }
    return common::Result<void>::ok();
}

std::string colText(sqlite3_stmt* s, int col)
{
    const auto* v = sqlite3_column_text(s, col);
    return v ? reinterpret_cast<const char*>(v) : std::string{};
}

std::optional<std::string> optText(sqlite3_stmt* s, int col)
{
    if (sqlite3_column_type(s, col) == SQLITE_NULL) return std::nullopt;
    return colText(s, col);
}

std::int64_t toMs(std::chrono::system_clock::time_point tp)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

std::chrono::system_clock::time_point fromMs(std::int64_t ms)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

std::optional<std::chrono::system_clock::time_point> optTime(sqlite3_stmt* s, int col)
{
    if (sqlite3_column_type(s, col) == SQLITE_NULL) return std::nullopt;
    return fromMs(sqlite3_column_int64(s, col));
}

// ──────────────────────────────────────────────────────────────────────────
// Pipe-separated list encoding for strengths/warnings vectors.

std::string joinPipe(const std::vector<std::string>& items)
{
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += '|';
        out += items[i];
    }
    return out;
}

std::vector<std::string> splitPipe(const std::string& s)
{
    if (s.empty()) return {};
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '|') {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

// ──────────────────────────────────────────────────────────────────────────
// Enum conversions

ComparisonStatus sessionStatusFromString(std::string_view v)
{
    if (v == "archived") return ComparisonStatus::Archived;
    return ComparisonStatus::Active;
}

ComparisonResultStatus resultStatusFromString(std::string_view v)
{
    if (v == "running")   return ComparisonResultStatus::Running;
    if (v == "completed") return ComparisonResultStatus::Completed;
    if (v == "failed")    return ComparisonResultStatus::Failed;
    if (v == "partial")   return ComparisonResultStatus::Partial;
    return ComparisonResultStatus::Pending;
}

// ──────────────────────────────────────────────────────────────────────────
// Row readers

ComparisonSession readSession(sqlite3_stmt* s)
{
    ComparisonSession sess;
    sess.id        = colText(s, 0);
    sess.projectId = colText(s, 1);
    sess.title     = colText(s, 2);
    sess.task      = colText(s, 3);
    sess.status    = sessionStatusFromString(colText(s, 4));
    sess.createdAt = fromMs(sqlite3_column_int64(s, 5));
    sess.updatedAt = fromMs(sqlite3_column_int64(s, 6));
    return sess;
}

ComparisonResult readResult(sqlite3_stmt* s)
{
    ComparisonResult r;
    r.id                  = colText(s, 0);
    r.comparisonId        = colText(s, 1);
    r.providerId          = colText(s, 2);
    r.providerName        = colText(s, 3);
    r.modelId             = colText(s, 4);
    r.modelName           = colText(s, 5);
    r.providerKind        = colText(s, 6);
    r.apiModelId          = colText(s, 7);
    r.status              = resultStatusFromString(colText(s, 8));
    r.content             = colText(s, 9);
    r.strengths           = splitPipe(colText(s, 10));
    r.warnings            = splitPipe(colText(s, 11));

    const auto inTok  = sqlite3_column_int64(s, 12);
    const auto outTok = sqlite3_column_int64(s, 13);
    r.inputTokens  = inTok  >= 0 ? static_cast<std::uint64_t>(inTok)  : 0;
    r.outputTokens = outTok >= 0 ? static_cast<std::uint64_t>(outTok) : 0;

    r.totalCost            = colText(s, 14);
    r.latencyMs            = sqlite3_column_int64(s, 15);
    r.qualityScore         = sqlite3_column_int(s, 16);
    r.isBest               = sqlite3_column_int(s, 17) != 0;
    r.selectedForSynthesis = sqlite3_column_int(s, 18) != 0;
    r.errorMessage         = colText(s, 19);
    r.startedAt            = optTime(s, 20);
    r.finishedAt           = optTime(s, 21);
    return r;
}

// ──────────────────────────────────────────────────────────────────────────
// Bind helpers for insert/update

#define BIND_OR_RETURN(result) do { if (!(result).success()) return common::Result<void>::failure(*(result).error()); } while(0)

common::Result<void> bindSessionFields(
    sqlite3* h, sqlite3_stmt* s, const ComparisonSession& sess)
{
    const common::Result<void> bindings[] = {
        bindText(h, s, 1, sess.id),
        bindText(h, s, 2, sess.projectId),
        bindText(h, s, 3, sess.title),
        bindText(h, s, 4, sess.task),
        bindText(h, s, 5, application::toString(sess.status)),
        bindInt64(h, s, 6, toMs(sess.createdAt)),
        bindInt64(h, s, 7, toMs(sess.updatedAt))};
    for (const auto& b : bindings) {
        if (!b.success()) return common::Result<void>::failure(*b.error());
    }
    return common::Result<void>::ok();
}

common::Result<void> bindResultFields(
    sqlite3* h, sqlite3_stmt* s, const ComparisonResult& r)
{
    const std::string strengths = joinPipe(r.strengths);
    const std::string warnings  = joinPipe(r.warnings);

    const common::Result<void> bindings[] = {
        bindText(h, s, 1,  r.id),
        bindText(h, s, 2,  r.comparisonId),
        bindText(h, s, 3,  r.providerId),
        bindText(h, s, 4,  r.providerName),
        bindText(h, s, 5,  r.modelId),
        bindText(h, s, 6,  r.modelName),
        bindText(h, s, 7,  r.providerKind),
        bindText(h, s, 8,  r.apiModelId),
        bindText(h, s, 9,  application::toString(r.status)),
        bindText(h, s, 10, r.content),
        bindText(h, s, 11, strengths),
        bindText(h, s, 12, warnings),
        bindInt64(h, s, 13, static_cast<std::int64_t>(r.inputTokens)),
        bindInt64(h, s, 14, static_cast<std::int64_t>(r.outputTokens)),
        bindText(h, s, 15, r.totalCost),
        bindInt64(h, s, 16, r.latencyMs),
        bindInt64(h, s, 17, static_cast<std::int64_t>(r.qualityScore)),
        bindInt64(h, s, 18, r.isBest               ? 1 : 0),
        bindInt64(h, s, 19, r.selectedForSynthesis  ? 1 : 0),
        bindText(h, s, 20, r.errorMessage)};
    for (const auto& b : bindings) {
        if (!b.success()) return common::Result<void>::failure(*b.error());
    }

    // Optional timestamps
    auto bindOpt = [&](int idx, const std::optional<std::chrono::system_clock::time_point>& tp)
        -> common::Result<void> {
        if (tp.has_value()) return bindInt64(h, s, idx, toMs(*tp));
        if (sqlite3_bind_null(s, idx) != SQLITE_OK) {
            return common::Result<void>::failure(
                dbError(h, "persistence.bind_failed", "Could not bind null timestamp"));
        }
        return common::Result<void>::ok();
    };

    const auto ba = bindOpt(21, r.startedAt);
    if (!ba.success()) return common::Result<void>::failure(*ba.error());
    const auto bb = bindOpt(22, r.finishedAt);
    if (!bb.success()) return common::Result<void>::failure(*bb.error());

    return common::Result<void>::ok();
}

constexpr const char* sessionColumns =
    "id,project_id,title,task,status,created_at_ms,updated_at_ms";

constexpr const char* resultColumns =
    "id,comparison_id,provider_id,provider_name,model_id,model_name,"
    "provider_kind,api_model_id,status,content,strengths,warnings,"
    "input_tokens,output_tokens,total_cost,latency_ms,quality_score,"
    "is_best,selected_for_synthesis,error_message,started_at_ms,finished_at_ms";

} // namespace

// ──────────────────────────────────────────────────────────────────────────
// SqliteComparisonRepository

SqliteComparisonRepository::SqliteComparisonRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

// ─── Sessions ─────────────────────────────────────────────────────────────

application::common::Result<ComparisonSession> SqliteComparisonRepository::insertSession(
    ComparisonSession session)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* h = database_->impl_->handle;
    Statement s;
    const auto p = prepare(h,
        "INSERT INTO comparison_sessions (id,project_id,title,task,status,created_at_ms,updated_at_ms) "
        "VALUES (?,?,?,?,?,?,?);", s);
    if (!p.success()) return common::Result<ComparisonSession>::failure(*p.error());
    const auto b = bindSessionFields(h, s.get(), session);
    if (!b.success()) return common::Result<ComparisonSession>::failure(*b.error());
    if (sqlite3_step(s.get()) != SQLITE_DONE) {
        return common::Result<ComparisonSession>::failure(
            dbError(h, "persistence.write_failed", "Could not insert comparison session"));
    }
    return common::Result<ComparisonSession>::ok(std::move(session));
}

application::common::Result<ComparisonSession> SqliteComparisonRepository::updateSession(
    ComparisonSession session)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* h = database_->impl_->handle;
    Statement s;
    const auto p = prepare(h,
        "UPDATE comparison_sessions SET project_id=?,title=?,task=?,status=?,"
        "created_at_ms=?,updated_at_ms=? WHERE id=?;", s);
    if (!p.success()) return common::Result<ComparisonSession>::failure(*p.error());

    // Re-bind with the update layout (1..6 = fields, 7 = WHERE id)
    const common::Result<void> bindings[] = {
        bindText(h, s.get(), 1, session.projectId),
        bindText(h, s.get(), 2, session.title),
        bindText(h, s.get(), 3, session.task),
        bindText(h, s.get(), 4, application::toString(session.status)),
        bindInt64(h, s.get(), 5, toMs(session.createdAt)),
        bindInt64(h, s.get(), 6, toMs(session.updatedAt)),
        bindText(h, s.get(), 7, session.id)};
    for (const auto& b : bindings) {
        if (!b.success()) return common::Result<ComparisonSession>::failure(*b.error());
    }
    if (sqlite3_step(s.get()) != SQLITE_DONE || sqlite3_changes(h) == 0) {
        return common::Result<ComparisonSession>::failure(
            dbError(h, "persistence.write_failed", "Could not update comparison session"));
    }
    return common::Result<ComparisonSession>::ok(std::move(session));
}

application::common::Result<std::optional<ComparisonSession>>
SqliteComparisonRepository::findSessionById(const ComparisonId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* h = database_->impl_->handle;
    Statement s;
    const std::string sql = std::string("SELECT ") + sessionColumns
        + " FROM comparison_sessions WHERE id=?;";
    const auto p = prepare(h, sql.c_str(), s);
    if (!p.success()) {
        return common::Result<std::optional<ComparisonSession>>::failure(*p.error());
    }
    const auto b = bindText(h, s.get(), 1, id);
    if (!b.success()) {
        return common::Result<std::optional<ComparisonSession>>::failure(*b.error());
    }
    const int step = sqlite3_step(s.get());
    if (step == SQLITE_DONE) {
        return common::Result<std::optional<ComparisonSession>>::ok(std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<ComparisonSession>>::failure(
            dbError(h, "persistence.read_failed", "Could not read comparison session"));
    }
    return common::Result<std::optional<ComparisonSession>>::ok(readSession(s.get()));
}

application::common::Result<std::vector<ComparisonSession>>
SqliteComparisonRepository::listSessionsByProject(const ProjectId& projectId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* h = database_->impl_->handle;
    Statement s;
    const std::string sql = std::string("SELECT ") + sessionColumns
        + " FROM comparison_sessions WHERE project_id=? ORDER BY created_at_ms DESC,id;";
    const auto p = prepare(h, sql.c_str(), s);
    if (!p.success()) {
        return common::Result<std::vector<ComparisonSession>>::failure(*p.error());
    }
    const auto b = bindText(h, s.get(), 1, projectId);
    if (!b.success()) {
        return common::Result<std::vector<ComparisonSession>>::failure(*b.error());
    }
    std::vector<ComparisonSession> out;
    for (;;) {
        const int step = sqlite3_step(s.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<ComparisonSession>>::failure(
                dbError(h, "persistence.read_failed", "Could not read comparison sessions"));
        }
        out.push_back(readSession(s.get()));
    }
    return common::Result<std::vector<ComparisonSession>>::ok(std::move(out));
}

// ─── Results ──────────────────────────────────────────────────────────────

application::common::Result<ComparisonResult> SqliteComparisonRepository::insertResult(
    ComparisonResult result)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* h = database_->impl_->handle;
    Statement s;
    const auto p = prepare(h,
        "INSERT INTO comparison_results "
        "(id,comparison_id,provider_id,provider_name,model_id,model_name,"
        " provider_kind,api_model_id,status,content,strengths,warnings,"
        " input_tokens,output_tokens,total_cost,latency_ms,quality_score,"
        " is_best,selected_for_synthesis,error_message,started_at_ms,finished_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);", s);
    if (!p.success()) return common::Result<ComparisonResult>::failure(*p.error());
    const auto b = bindResultFields(h, s.get(), result);
    if (!b.success()) return common::Result<ComparisonResult>::failure(*b.error());
    if (sqlite3_step(s.get()) != SQLITE_DONE) {
        return common::Result<ComparisonResult>::failure(
            dbError(h, "persistence.write_failed", "Could not insert comparison result"));
    }
    return common::Result<ComparisonResult>::ok(std::move(result));
}

application::common::Result<ComparisonResult> SqliteComparisonRepository::updateResult(
    ComparisonResult result)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* h = database_->impl_->handle;
    Statement s;
    const auto p = prepare(h,
        "UPDATE comparison_results SET "
        "comparison_id=?,provider_id=?,provider_name=?,model_id=?,model_name=?,"
        "provider_kind=?,api_model_id=?,status=?,content=?,strengths=?,warnings=?,"
        "input_tokens=?,output_tokens=?,total_cost=?,latency_ms=?,quality_score=?,"
        "is_best=?,selected_for_synthesis=?,error_message=?,started_at_ms=?,finished_at_ms=? "
        "WHERE id=?;", s);
    if (!p.success()) return common::Result<ComparisonResult>::failure(*p.error());

    const std::string strengths = joinPipe(result.strengths);
    const std::string warnings  = joinPipe(result.warnings);

    // UPDATE layout: SET fields at 1..21, WHERE id at 22.
    const common::Result<void> bindings[] = {
        bindText(h, s.get(),  1, result.comparisonId),
        bindText(h, s.get(),  2, result.providerId),
        bindText(h, s.get(),  3, result.providerName),
        bindText(h, s.get(),  4, result.modelId),
        bindText(h, s.get(),  5, result.modelName),
        bindText(h, s.get(),  6, result.providerKind),
        bindText(h, s.get(),  7, result.apiModelId),
        bindText(h, s.get(),  8, application::toString(result.status)),
        bindText(h, s.get(),  9, result.content),
        bindText(h, s.get(), 10, strengths),
        bindText(h, s.get(), 11, warnings),
        bindInt64(h, s.get(), 12, static_cast<std::int64_t>(result.inputTokens)),
        bindInt64(h, s.get(), 13, static_cast<std::int64_t>(result.outputTokens)),
        bindText(h, s.get(), 14, result.totalCost),
        bindInt64(h, s.get(), 15, result.latencyMs),
        bindInt64(h, s.get(), 16, static_cast<std::int64_t>(result.qualityScore)),
        bindInt64(h, s.get(), 17, result.isBest               ? 1 : 0),
        bindInt64(h, s.get(), 18, result.selectedForSynthesis  ? 1 : 0),
        bindText(h, s.get(), 19, result.errorMessage),
        bindText(h, s.get(), 22, result.id)};  // WHERE id
    for (const auto& b : bindings) {
        if (!b.success()) return common::Result<ComparisonResult>::failure(*b.error());
    }

    // Optional timestamps at positions 20 and 21
    auto bindOpt = [&](int idx, const std::optional<std::chrono::system_clock::time_point>& tp)
        -> common::Result<void> {
        if (tp.has_value()) return bindInt64(h, s.get(), idx, toMs(*tp));
        if (sqlite3_bind_null(s.get(), idx) != SQLITE_OK) {
            return common::Result<void>::failure(
                dbError(h, "persistence.bind_failed", "Could not bind null timestamp"));
        }
        return common::Result<void>::ok();
    };
    const auto ba = bindOpt(20, result.startedAt);
    if (!ba.success()) return common::Result<ComparisonResult>::failure(*ba.error());
    const auto bb = bindOpt(21, result.finishedAt);
    if (!bb.success()) return common::Result<ComparisonResult>::failure(*bb.error());

    if (sqlite3_step(s.get()) != SQLITE_DONE || sqlite3_changes(h) == 0) {
        return common::Result<ComparisonResult>::failure(
            dbError(h, "persistence.write_failed", "Could not update comparison result"));
    }
    return common::Result<ComparisonResult>::ok(std::move(result));
}

application::common::Result<std::optional<ComparisonResult>>
SqliteComparisonRepository::findResultById(const ComparisonResultId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* h = database_->impl_->handle;
    Statement s;
    const std::string sql = std::string("SELECT ") + resultColumns
        + " FROM comparison_results WHERE id=?;";
    const auto p = prepare(h, sql.c_str(), s);
    if (!p.success()) {
        return common::Result<std::optional<ComparisonResult>>::failure(*p.error());
    }
    const auto b = bindText(h, s.get(), 1, id);
    if (!b.success()) {
        return common::Result<std::optional<ComparisonResult>>::failure(*b.error());
    }
    const int step = sqlite3_step(s.get());
    if (step == SQLITE_DONE) {
        return common::Result<std::optional<ComparisonResult>>::ok(std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<ComparisonResult>>::failure(
            dbError(h, "persistence.read_failed", "Could not read comparison result"));
    }
    return common::Result<std::optional<ComparisonResult>>::ok(readResult(s.get()));
}

application::common::Result<std::vector<ComparisonResult>>
SqliteComparisonRepository::listResultsBySession(const ComparisonId& comparisonId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* h = database_->impl_->handle;
    Statement s;
    const std::string sql = std::string("SELECT ") + resultColumns
        + " FROM comparison_results WHERE comparison_id=? ORDER BY rowid;";
    const auto p = prepare(h, sql.c_str(), s);
    if (!p.success()) {
        return common::Result<std::vector<ComparisonResult>>::failure(*p.error());
    }
    const auto b = bindText(h, s.get(), 1, comparisonId);
    if (!b.success()) {
        return common::Result<std::vector<ComparisonResult>>::failure(*b.error());
    }
    std::vector<ComparisonResult> out;
    for (;;) {
        const int step = sqlite3_step(s.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<ComparisonResult>>::failure(
                dbError(h, "persistence.read_failed", "Could not read comparison results"));
        }
        out.push_back(readResult(s.get()));
    }
    return common::Result<std::vector<ComparisonResult>>::ok(std::move(out));
}

// ──────────────────────────────────────────────────────────────────────────
// Schema DDL (returned as a string constant for the v5 migration block).

const char* comparisonSchemaDDL() noexcept
{
    return R"sql(
BEGIN IMMEDIATE;
CREATE TABLE comparison_sessions (
    id           TEXT PRIMARY KEY NOT NULL,
    project_id   TEXT NOT NULL REFERENCES projects(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    title        TEXT NOT NULL,
    task         TEXT NOT NULL,
    status       TEXT NOT NULL DEFAULT 'active'
                     CHECK(status IN ('active','archived')),
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL
);
CREATE INDEX comparison_sessions_project_idx ON comparison_sessions(project_id, created_at_ms DESC, id);
CREATE TABLE comparison_results (
    id                    TEXT PRIMARY KEY NOT NULL,
    comparison_id         TEXT NOT NULL
                              REFERENCES comparison_sessions(id) ON UPDATE CASCADE ON DELETE CASCADE,
    provider_id           TEXT NOT NULL,
    provider_name         TEXT NOT NULL,
    model_id              TEXT NOT NULL,
    model_name            TEXT NOT NULL,
    provider_kind         TEXT NOT NULL,
    api_model_id          TEXT NOT NULL,
    status                TEXT NOT NULL DEFAULT 'pending'
                              CHECK(status IN ('pending','running','completed','failed','partial')),
    content               TEXT NOT NULL DEFAULT '',
    strengths             TEXT NOT NULL DEFAULT '',
    warnings              TEXT NOT NULL DEFAULT '',
    input_tokens          INTEGER NOT NULL DEFAULT 0 CHECK(input_tokens >= 0),
    output_tokens         INTEGER NOT NULL DEFAULT 0 CHECK(output_tokens >= 0),
    total_cost            TEXT NOT NULL DEFAULT '',
    latency_ms            INTEGER NOT NULL DEFAULT 0,
    quality_score         INTEGER NOT NULL DEFAULT 0,
    is_best               INTEGER NOT NULL DEFAULT 0 CHECK(is_best IN (0,1)),
    selected_for_synthesis INTEGER NOT NULL DEFAULT 0 CHECK(selected_for_synthesis IN (0,1)),
    error_message         TEXT NOT NULL DEFAULT '',
    started_at_ms         INTEGER,
    finished_at_ms        INTEGER
);
CREATE INDEX comparison_results_session_idx ON comparison_results(comparison_id, rowid);
PRAGMA user_version = 5;
COMMIT;
)sql";
}

} // namespace ai::infrastructure
