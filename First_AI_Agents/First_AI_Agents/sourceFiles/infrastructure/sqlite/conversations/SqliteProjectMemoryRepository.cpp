#include "../SqliteConnection.hpp"
#include "../SqlitePrimitives.hpp"

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/conversations/IProjectMemoryRepository.hpp"

#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ai::infrastructure {
namespace {

using application::ProjectMemorySegment;
using sqlite_detail::Statement;
using sqlite_detail::error;
using sqlite_detail::databaseError;
using sqlite_detail::prepare;
using sqlite_detail::bindText;
using sqlite_detail::bindInt64;
using sqlite_detail::bindUnsigned;
using sqlite_detail::toMilliseconds;
using sqlite_detail::fromMilliseconds;
using sqlite_detail::columnText;

} // namespace

namespace {
constexpr const char* projectMemorySegmentColumns =
    "id,project_id,run_id,summary,source_token_count,summary_token_count,created_at_ms";

common::Result<ProjectMemorySegment> readProjectMemorySegment(sqlite3_stmt* s)
{
    ProjectMemorySegment value;
    value.id = columnText(s, 0);
    value.projectId = columnText(s, 1);
    value.runId = columnText(s, 2);
    value.summary = columnText(s, 3);
    value.sourceTokenCount = static_cast<std::uint64_t>(sqlite3_column_int64(s, 4));
    value.summaryTokenCount = static_cast<std::uint64_t>(sqlite3_column_int64(s, 5));
    value.createdAt = fromMilliseconds(sqlite3_column_int64(s, 6));
    return common::Result<ProjectMemorySegment>::ok(std::move(value));
}
}

SqliteProjectMemoryRepository::SqliteProjectMemoryRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<ProjectMemorySegment> SqliteProjectMemoryRepository::insertMemorySegment(ProjectMemorySegment v)
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h = database_->impl_->handle; Statement s;
    const auto p = prepare(h,
        "INSERT INTO project_memory_segments (id,project_id,run_id,"
        "summary,source_token_count,summary_token_count,created_at_ms) VALUES (?,?,?,?,?,?,?);", s);
    if (!p.success()) return common::Result<ProjectMemorySegment>::failure(*p.error());
    const common::Result<void> b[] = {bindText(h,s.get(),1,v.id),bindText(h,s.get(),2,v.projectId),
        bindText(h,s.get(),3,v.runId),bindText(h,s.get(),4,v.summary),
        bindUnsigned(h,s.get(),5,v.sourceTokenCount),bindUnsigned(h,s.get(),6,v.summaryTokenCount),
        bindInt64(h,s.get(),7,toMilliseconds(v.createdAt))};
    for (const auto& x : b) if (!x.success()) return common::Result<ProjectMemorySegment>::failure(*x.error());
    if (sqlite3_step(s.get()) != SQLITE_DONE) {
        const int code = sqlite3_extended_errcode(h);
        if (code == SQLITE_CONSTRAINT_PRIMARYKEY)
            return common::Result<ProjectMemorySegment>::failure(
                error("project_memory.duplicate_id", "Memory segment id already exists"));
        if (code == SQLITE_CONSTRAINT_FOREIGNKEY)
            return common::Result<ProjectMemorySegment>::failure(
                error("project_memory.project_not_found", "Project does not exist"));
        return common::Result<ProjectMemorySegment>::failure(
            databaseError(h, "persistence.write_failed", "Could not insert project memory segment"));
    }
    return common::Result<ProjectMemorySegment>::ok(std::move(v));
}

common::Result<std::vector<ProjectMemorySegment>> SqliteProjectMemoryRepository::listMemorySegments(
    const application::ProjectId& projectId) const
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h = database_->impl_->handle; Statement s;
    // Stable chronological order: created_at_ms first, id as deterministic tiebreak.
    const std::string sql = std::string("SELECT ") + projectMemorySegmentColumns
        + " FROM project_memory_segments WHERE project_id=? ORDER BY created_at_ms,id;";
    const auto p = prepare(h, sql.c_str(), s);
    if (!p.success()) return common::Result<std::vector<ProjectMemorySegment>>::failure(*p.error());
    const auto b = bindText(h, s.get(), 1, projectId);
    if (!b.success()) return common::Result<std::vector<ProjectMemorySegment>>::failure(*b.error());
    std::vector<ProjectMemorySegment> out;
    for (;;) {
        const int step = sqlite3_step(s.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW)
            return common::Result<std::vector<ProjectMemorySegment>>::failure(
                databaseError(h, "persistence.read_failed", "Could not list project memory segments"));
        auto v = readProjectMemorySegment(s.get());
        if (!v.success()) return common::Result<std::vector<ProjectMemorySegment>>::failure(*v.error());
        out.push_back(std::move(v.value()));
    }
    return common::Result<std::vector<ProjectMemorySegment>>::ok(std::move(out));
}

common::Result<application::ProjectMemoryRecentPage> SqliteProjectMemoryRepository::listRecentMemorySegments(
    const application::ProjectId& projectId, std::size_t limit) const
{
    using Page = application::ProjectMemoryRecentPage;
    // One lock for both statements, so the count and the rows describe the same state.
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h = database_->impl_->handle;
    Page page;
    {
        Statement count;
        const auto p = prepare(h, "SELECT COUNT(*) FROM project_memory_segments WHERE project_id=?;", count);
        if (!p.success()) return common::Result<Page>::failure(*p.error());
        const auto b = bindText(h, count.get(), 1, projectId);
        if (!b.success()) return common::Result<Page>::failure(*b.error());
        if (sqlite3_step(count.get()) != SQLITE_ROW) {
            return common::Result<Page>::failure(
                databaseError(h, "persistence.read_failed", "Could not count project memory segments"));
        }
        page.totalCount = static_cast<std::uint64_t>(sqlite3_column_int64(count.get(), 0));
    }
    if (limit == 0 || page.totalCount == 0) return common::Result<Page>::ok(std::move(page));
    Statement s;
    // Newest first; id descending is the deterministic tiebreak that the snapshot relies on.
    const std::string sql = std::string("SELECT ") + projectMemorySegmentColumns
        + " FROM project_memory_segments WHERE project_id=? ORDER BY created_at_ms DESC,id DESC LIMIT ?;";
    const auto p = prepare(h, sql.c_str(), s);
    if (!p.success()) return common::Result<Page>::failure(*p.error());
    const auto b = bindText(h, s.get(), 1, projectId);
    if (!b.success()) return common::Result<Page>::failure(*b.error());
    const auto l = bindUnsigned(h, s.get(), 2, static_cast<std::uint64_t>(limit));
    if (!l.success()) return common::Result<Page>::failure(*l.error());
    for (;;) {
        const int step = sqlite3_step(s.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<Page>::failure(
                databaseError(h, "persistence.read_failed", "Could not list recent project memory segments"));
        }
        auto v = readProjectMemorySegment(s.get());
        if (!v.success()) return common::Result<Page>::failure(*v.error());
        page.newest.push_back(std::move(v.value()));
    }
    return common::Result<Page>::ok(std::move(page));
}

common::Result<std::optional<ProjectMemorySegment>> SqliteProjectMemoryRepository::findMemorySegment(
    const application::ProjectMemorySegmentId& id) const
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h = database_->impl_->handle; Statement s;
    const std::string sql = std::string("SELECT ") + projectMemorySegmentColumns
        + " FROM project_memory_segments WHERE id=?;";
    const auto p = prepare(h, sql.c_str(), s);
    if (!p.success()) return common::Result<std::optional<ProjectMemorySegment>>::failure(*p.error());
    const auto b = bindText(h, s.get(), 1, id);
    if (!b.success()) return common::Result<std::optional<ProjectMemorySegment>>::failure(*b.error());
    const int step = sqlite3_step(s.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<ProjectMemorySegment>>::ok(std::nullopt);
    if (step != SQLITE_ROW)
        return common::Result<std::optional<ProjectMemorySegment>>::failure(
            databaseError(h, "persistence.read_failed", "Could not read project memory segment"));
    auto v = readProjectMemorySegment(s.get());
    if (!v.success()) return common::Result<std::optional<ProjectMemorySegment>>::failure(*v.error());
    return common::Result<std::optional<ProjectMemorySegment>>::ok(std::move(v.value()));
}

common::Result<void> SqliteProjectMemoryRepository::removeMemorySegments(
    const application::ProjectId& projectId)
{
    // Deleting zero rows is success: "no memory for this project" is a valid state.
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h = database_->impl_->handle; Statement s;
    const auto p = prepare(h, "DELETE FROM project_memory_segments WHERE project_id=?;", s);
    if (!p.success()) return p;
    const auto b = bindText(h, s.get(), 1, projectId);
    if (!b.success()) return b;
    if (sqlite3_step(s.get()) != SQLITE_DONE)
        return common::Result<void>::failure(
            databaseError(h, "persistence.write_failed", "Could not remove project memory segments"));
    return common::Result<void>::ok();
}

} // namespace ai::infrastructure
