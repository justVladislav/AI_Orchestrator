#include "ai/infrastructure/SqliteRepositories.hpp"
#include "../SqliteConnection.hpp"
#include "../SqlitePrimitives.hpp"
#include "../catalog/SqliteProfileMapping.hpp"
#include "../roster/SqliteRosterMapping.hpp"
#include "../execution/SqliteRunStatusMapping.hpp"
#include "../planning/SqliteRunDraftGroupStorage.hpp"

#include "ai/application/ExecutionAcceptanceContract.hpp"
#include "ai/application/CacheDurationAccounting.hpp"
#include "ai/application/conversations/ChatProjectContext.hpp"
#include "ai/application/planning/RunDraftGroup.hpp"
#include "ai/application/WorkspaceBaselineProvenance.hpp"

// Native SQLite C API only. This backend must build and link without the Qt
// SDK (no Qt6::Sql/QSqlDatabase); see CODEX_CONTEXT.md for why an earlier
// Qt SQL based revision of this file was reverted.
#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ai::infrastructure {

namespace {

using application::Artifact;
using application::ArtifactKind;
using application::ApplyHashEntry;
using application::ApprovalRecord;
using application::ChangeSetState;
using application::CacheCreationDurationUsageProblem;
using application::cacheCreationDurationUsageProblemCode;
using application::cacheCreationDurationUsageProblemMessage;
using application::checkCacheCreationDurationUsage;
using application::ChangeId;
using application::ChangeSetId;
using application::FileChangeOperation;
using application::LongContextPricing;
using application::LongContextPricingProblem;
using application::ProjectMemoryCompactionRecord;
using application::ProjectMemoryCompactionStatus;
using application::ProposalReviewRejection;
using application::ProposedChangeSet;
using application::ProposedFileChange;
using application::ValidationFinding;
using application::ChatConversation;
using application::ChatMemorySegment;
using application::ChatMessage;
using application::ChatProjectContextAttachment;
using application::PreparedChatTurn;
using application::ExecutionProfile;
using application::RunDraft;
using application::RunDraftId;
using application::RunLaunchMode;
using application::Model;
using application::ModelExecution;
using application::QualityEvaluation;
using application::Project;
using application::ProjectMemorySegment;
using application::ProjectTask;
using application::ProjectStatus;
using application::Provider;
using application::ProviderExecutionBudgetOverrides;
using application::ProviderExecutionBudgetPolicy;
using application::ProviderExecutionBudgetUsage;
using application::Run;
using application::RunStage;
using application::RunStatus;
using application::StageModelAssignment;
using application::StageStatus;
using application::TaskPriority;
using application::TaskStatus;

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
using sqlite_detail::bindOptionalTime;
using sqlite_detail::optionalTime;
using sqlite_detail::columnText;
using sqlite_detail::optionalText;
using sqlite_detail::pathToUtf8;
using sqlite_detail::pathFromUtf8;
using sqlite_detail::providerBudgetOverridesJson;
using sqlite_detail::parseProviderBudgetOverrides;
using sqlite_detail::readAssignment;
using sqlite_detail::serializeStringVector;
using sqlite_detail::parseStringVector;
using sqlite_detail::readRunAssignment;
using sqlite_detail::runStatusFromString;
using sqlite_detail::insertRunDraftGroupRows;
using sqlite_detail::findRunDraftGroupWhere;
using sqlite_detail::readRunDraftGroupDrafts;



std::string agentToolSessionStateToString(application::AgentToolSessionState s)
{
    switch (s) {
    case application::AgentToolSessionState::Active:      return "active";
    case application::AgentToolSessionState::Completed:   return "completed";
    case application::AgentToolSessionState::Interrupted: return "interrupted";
    }
    return "active";
}

std::optional<application::AgentToolSessionState> agentToolSessionStateFromString(
    std::string_view v)
{
    if (v == "active")      return application::AgentToolSessionState::Active;
    if (v == "completed")   return application::AgentToolSessionState::Completed;
    if (v == "interrupted") return application::AgentToolSessionState::Interrupted;
    return std::nullopt;
}

application::AgentToolSession readAgentToolSession(sqlite3_stmt* s)
{
    application::AgentToolSession sess;
    sess.sessionId          = columnText(s, 0);
    sess.runId              = columnText(s, 1);
    sess.providerId         = columnText(s, 2);
    sess.modelId            = columnText(s, 3);
    const auto state        = agentToolSessionStateFromString(columnText(s, 4));
    sess.state              = state.value_or(application::AgentToolSessionState::Active);
    sess.totalTurns         = static_cast<std::size_t>(sqlite3_column_int64(s, 5));
    sess.totalCalls         = static_cast<std::size_t>(sqlite3_column_int64(s, 6));
    sess.totalReturnedBytes = static_cast<std::size_t>(sqlite3_column_int64(s, 7));
    sess.createdAt          = fromMilliseconds(sqlite3_column_int64(s, 8));
    sess.finishedAt         = optionalTime(s, 9);
    return sess;
}

} // namespace

SqliteAgentToolSessionRepository::SqliteAgentToolSessionRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<application::AgentToolSession>
SqliteAgentToolSessionRepository::insertSession(application::AgentToolSession session)
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(INSERT INTO agent_tool_sessions
              (id,run_id,provider_id,model_id,state,
               total_turns,total_calls,total_returned_bytes,
               created_at_ms,finished_at_ms)
              VALUES (?,?,?,?,?,?,?,?,?,?))sql",
        stmt);
    if (!prepared.success()) return common::Result<application::AgentToolSession>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    auto* s = stmt.get();
    if (auto r = bindText(h,s,1,session.sessionId);       !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindText(h,s,2,session.runId);           !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindText(h,s,3,session.providerId);      !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindText(h,s,4,session.modelId);         !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindText(h,s,5,agentToolSessionStateToString(session.state)); !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,6,session.totalTurns);          !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,7,session.totalCalls);          !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,8,session.totalReturnedBytes);  !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindInt64(h,s,9,toMilliseconds(session.createdAt)); !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindOptionalTime(h,s,10,session.finishedAt);     !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());

    if (sqlite3_step(s) != SQLITE_DONE)
        return common::Result<application::AgentToolSession>::failure(
            databaseError(h, "persistence.insert_failed", "agent_tool_sessions insert"));

    return common::Result<application::AgentToolSession>::ok(std::move(session));
}

common::Result<application::AgentToolSession>
SqliteAgentToolSessionRepository::updateSession(application::AgentToolSession session)
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(UPDATE agent_tool_sessions SET
              run_id=?,provider_id=?,model_id=?,state=?,
              total_turns=?,total_calls=?,total_returned_bytes=?,
              created_at_ms=?,finished_at_ms=?
              WHERE id=?)sql",
        stmt);
    if (!prepared.success()) return common::Result<application::AgentToolSession>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    auto* s = stmt.get();
    if (auto r = bindText(h,s,1,session.runId);           !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindText(h,s,2,session.providerId);      !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindText(h,s,3,session.modelId);         !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindText(h,s,4,agentToolSessionStateToString(session.state)); !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,5,session.totalTurns);          !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,6,session.totalCalls);          !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,7,session.totalReturnedBytes);  !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindInt64(h,s,8,toMilliseconds(session.createdAt)); !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindOptionalTime(h,s,9,session.finishedAt);      !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());
    if (auto r = bindText(h,s,10,session.sessionId);      !r.success()) return common::Result<application::AgentToolSession>::failure(*r.error());

    if (sqlite3_step(s) != SQLITE_DONE)
        return common::Result<application::AgentToolSession>::failure(
            databaseError(h, "persistence.update_failed", "agent_tool_sessions update"));

    if (sqlite3_changes(h) == 0)
        return common::Result<application::AgentToolSession>::failure(
            error("agent_session.not_found", "No agent_tool_session row with that id"));

    return common::Result<application::AgentToolSession>::ok(std::move(session));
}

common::Result<std::optional<application::AgentToolSession>>
SqliteAgentToolSessionRepository::findSession(const std::string& sessionId) const
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(SELECT id,run_id,provider_id,model_id,state,
                     total_turns,total_calls,total_returned_bytes,
                     created_at_ms,finished_at_ms
              FROM agent_tool_sessions WHERE id=?)sql",
        stmt);
    if (!prepared.success())
        return common::Result<std::optional<application::AgentToolSession>>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    if (auto r = bindText(h, stmt.get(), 1, sessionId); !r.success())
        return common::Result<std::optional<application::AgentToolSession>>::failure(*r.error());

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE)
        return common::Result<std::optional<application::AgentToolSession>>::ok(std::nullopt);
    if (rc != SQLITE_ROW)
        return common::Result<std::optional<application::AgentToolSession>>::failure(
            databaseError(h, "persistence.query_failed", "agent_tool_sessions findSession"));

    return common::Result<std::optional<application::AgentToolSession>>::ok(
        readAgentToolSession(stmt.get()));
}

common::Result<std::vector<application::AgentToolSession>>
SqliteAgentToolSessionRepository::listForRun(const std::string& runId) const
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(SELECT id,run_id,provider_id,model_id,state,
                     total_turns,total_calls,total_returned_bytes,
                     created_at_ms,finished_at_ms
              FROM agent_tool_sessions WHERE run_id=? ORDER BY created_at_ms ASC)sql",
        stmt);
    if (!prepared.success())
        return common::Result<std::vector<application::AgentToolSession>>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    if (auto r = bindText(h, stmt.get(), 1, runId); !r.success())
        return common::Result<std::vector<application::AgentToolSession>>::failure(*r.error());

    std::vector<application::AgentToolSession> result;
    int rc;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW)
        result.push_back(readAgentToolSession(stmt.get()));

    if (rc != SQLITE_DONE)
        return common::Result<std::vector<application::AgentToolSession>>::failure(
            databaseError(h, "persistence.query_failed", "agent_tool_sessions listForRun"));

    return common::Result<std::vector<application::AgentToolSession>>::ok(std::move(result));
}

common::Result<application::AgentProviderTurn>
SqliteAgentToolSessionRepository::insertTurn(application::AgentProviderTurn turn)
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(INSERT INTO agent_provider_turns
              (id,session_id,turn_index,finish_reason,
               input_tokens,output_tokens,call_count,created_at_ms)
              VALUES (?,?,?,?,?,?,?,?))sql",
        stmt);
    if (!prepared.success()) return common::Result<application::AgentProviderTurn>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    auto* s = stmt.get();
    if (auto r = bindText(h,s,1,turn.turnId);              !r.success()) return common::Result<application::AgentProviderTurn>::failure(*r.error());
    if (auto r = bindText(h,s,2,turn.sessionId);           !r.success()) return common::Result<application::AgentProviderTurn>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,3,turn.turnIndex);       !r.success()) return common::Result<application::AgentProviderTurn>::failure(*r.error());
    if (auto r = bindText(h,s,4,turn.finishReason);        !r.success()) return common::Result<application::AgentProviderTurn>::failure(*r.error());
    if (auto r = bindInt64(h,s,5,static_cast<std::int64_t>(turn.inputTokens));  !r.success()) return common::Result<application::AgentProviderTurn>::failure(*r.error());
    if (auto r = bindInt64(h,s,6,static_cast<std::int64_t>(turn.outputTokens)); !r.success()) return common::Result<application::AgentProviderTurn>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,7,turn.callCount);       !r.success()) return common::Result<application::AgentProviderTurn>::failure(*r.error());
    if (auto r = bindInt64(h,s,8,toMilliseconds(turn.createdAt)); !r.success()) return common::Result<application::AgentProviderTurn>::failure(*r.error());

    if (sqlite3_step(s) != SQLITE_DONE)
        return common::Result<application::AgentProviderTurn>::failure(
            databaseError(h, "persistence.insert_failed", "agent_provider_turns insert"));

    return common::Result<application::AgentProviderTurn>::ok(std::move(turn));
}

common::Result<std::vector<application::AgentProviderTurn>>
SqliteAgentToolSessionRepository::listTurns(const std::string& sessionId) const
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(SELECT id,session_id,turn_index,finish_reason,
                     input_tokens,output_tokens,call_count,created_at_ms
              FROM agent_provider_turns WHERE session_id=? ORDER BY turn_index ASC)sql",
        stmt);
    if (!prepared.success())
        return common::Result<std::vector<application::AgentProviderTurn>>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    if (auto r = bindText(h, stmt.get(), 1, sessionId); !r.success())
        return common::Result<std::vector<application::AgentProviderTurn>>::failure(*r.error());

    std::vector<application::AgentProviderTurn> result;
    int rc;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        application::AgentProviderTurn turn;
        turn.turnId       = columnText(stmt.get(), 0);
        turn.sessionId    = columnText(stmt.get(), 1);
        turn.turnIndex    = static_cast<std::size_t>(sqlite3_column_int64(stmt.get(), 2));
        turn.finishReason = columnText(stmt.get(), 3);
        turn.inputTokens  = static_cast<int>(sqlite3_column_int64(stmt.get(), 4));
        turn.outputTokens = static_cast<int>(sqlite3_column_int64(stmt.get(), 5));
        turn.callCount    = static_cast<std::size_t>(sqlite3_column_int64(stmt.get(), 6));
        turn.createdAt    = fromMilliseconds(sqlite3_column_int64(stmt.get(), 7));
        result.push_back(std::move(turn));
    }
    if (rc != SQLITE_DONE)
        return common::Result<std::vector<application::AgentProviderTurn>>::failure(
            databaseError(h, "persistence.query_failed", "agent_provider_turns listTurns"));

    return common::Result<std::vector<application::AgentProviderTurn>>::ok(std::move(result));
}

common::Result<std::size_t> SqliteAgentToolSessionRepository::interruptActiveSessions()
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    const std::int64_t nowMs = toMilliseconds(std::chrono::system_clock::now());
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        "UPDATE agent_tool_sessions SET state='interrupted',finished_at_ms=? WHERE state='active'",
        stmt);
    if (!prepared.success())
        return common::Result<std::size_t>::failure(*prepared.error());

    if (auto r = bindInt64(database_->impl_->handle, stmt.get(), 1, nowMs); !r.success())
        return common::Result<std::size_t>::failure(*r.error());

    if (sqlite3_step(stmt.get()) != SQLITE_DONE)
        return common::Result<std::size_t>::failure(
            databaseError(database_->impl_->handle,
                "persistence.update_failed", "agent_tool_sessions interruptActiveSessions"));

    return common::Result<std::size_t>::ok(
        static_cast<std::size_t>(sqlite3_changes(database_->impl_->handle)));
}

} // namespace ai::infrastructure
