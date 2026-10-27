#include "../SqliteConnection.hpp"
#include "../SqlitePrimitives.hpp"
#include "../execution/SqliteRunStatusMapping.hpp"
#include "SqliteRunDraftGroupStorage.hpp"

#include "ai/application/IFollowUpPlanRequestRepository.hpp"
#include "ai/application/planning/RunDraftGroup.hpp"

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

using application::RunDraft;
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
using sqlite_detail::optionalTime;
using sqlite_detail::bindOptionalTime;
using sqlite_detail::runStatusFromString;
using sqlite_detail::insertRunDraftGroupRows;
using sqlite_detail::findRunDraftGroupWhere;
using sqlite_detail::readRunDraftGroupDrafts;

} // namespace

SqliteFollowUpPlanRepository::SqliteFollowUpPlanRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

namespace {

constexpr const char* followUpRequestColumns =
    "id,project_id,source_run_id,kind,goal,max_drafts,split_hint,outcome_source_state_fingerprint,snapshot_fingerprint,"
    "context_files_json,context_policy_id,context_fingerprint,provider_id,model_id,provider_kind,provider_base_url,"
    "provider_credential_reference,provider_identity_fingerprint,prompt_cache_mode,pricing_identity_json,"
    "pricing_identity_fingerprint,estimated_input_tokens,cacheable_input_tokens,effective_max_output_tokens,cap_micros,"
    "estimated_cost_micros,state,created_at_ms,quote_expires_at_ms,updated_at_ms,authorized_at_ms,claimed_at_ms,"
    "finished_at_ms,reply_text,plan_fingerprint,advisories_json,failure_code,failure_message,draft_group_id,accounting_json";

// What EVERY read selects (so no read can skip the Run authority): the request, then the project of the Run it is about. The
// LEFT JOIN keeps a request whose Run is gone in the result, so that it can be refused instead of disappearing. Columns 0..39
// are followUpRequestColumns.
constexpr const char* followUpRequestSelect =
    "SELECT r.id,r.project_id,r.source_run_id,r.kind,r.goal,r.max_drafts,r.split_hint,r.outcome_source_state_fingerprint,"
    "r.snapshot_fingerprint,r.context_files_json,r.context_policy_id,r.context_fingerprint,r.provider_id,r.model_id,"
    "r.provider_kind,r.provider_base_url,r.provider_credential_reference,r.provider_identity_fingerprint,"
    "r.prompt_cache_mode,r.pricing_identity_json,r.pricing_identity_fingerprint,r.estimated_input_tokens,"
    "r.cacheable_input_tokens,r.effective_max_output_tokens,r.cap_micros,r.estimated_cost_micros,r.state,r.created_at_ms,"
    "r.quote_expires_at_ms,r.updated_at_ms,r.authorized_at_ms,r.claimed_at_ms,r.finished_at_ms,r.reply_text,"
    "r.plan_fingerprint,r.advisories_json,r.failure_code,r.failure_message,r.draft_group_id,r.accounting_json,runs.project_id "
    "FROM follow_up_plan_requests AS r LEFT JOIN runs ON runs.id = r.source_run_id ";

constexpr const char* followUpOpenStates = "('quoted','authorized','composing','plan_ready')";

// A stored row is read back like any consumer would: it must still be exactly what the rules could have produced and still
// belong to the project of its source Run. A row that was damaged or made behind the repository's back is refused, never
// repaired and never shown.
common::Result<application::FollowUpPlanRequest> readFollowUpRequest(sqlite3_stmt* statement)
{
    using Result = common::Result<application::FollowUpPlanRequest>;
    const auto invalid = [](const std::string& what) {
        return Result::failure(error("persistence.invalid_data", "Follow-up request row " + what));
    };
    application::FollowUpPlanRequest request;
    request.id = columnText(statement, 0);
    request.projectId = columnText(statement, 1);
    request.sourceRunId = columnText(statement, 2);
    const auto kind = application::followUpPlanKindFromString(columnText(statement, 3));
    if (!kind.has_value()) return invalid("has an unknown kind");
    request.kind = *kind;
    request.goal = columnText(statement, 4);
    const std::int64_t maxDrafts = sqlite3_column_int64(statement, 5);
    if (maxDrafts < 1 || maxDrafts > 255) return invalid("has an invalid draft limit");
    request.maxDrafts = static_cast<std::uint32_t>(maxDrafts);
    const auto hint = application::followUpSplitHintFromString(columnText(statement, 6));
    if (!hint.has_value()) return invalid("has an unknown split hint");
    request.splitHint = *hint;
    request.outcomeSourceStateFingerprint = columnText(statement, 7);
    request.snapshotFingerprint = columnText(statement, 8);
    auto files = application::parseFollowUpPinnedFiles(columnText(statement, 9));
    if (!files.success()) return Result::failure(*files.error());
    request.files = std::move(files.value());

    auto& quote = request.quote;
    quote.policyId = columnText(statement, 10);
    quote.contextFingerprint = columnText(statement, 11);
    quote.providerIdentity.providerId = columnText(statement, 12);
    quote.providerIdentity.kind = columnText(statement, 14);
    quote.providerIdentity.baseUrl = columnText(statement, 15);
    quote.providerIdentity.credentialReference = optionalText(statement, 16);
    quote.providerIdentityFingerprint = columnText(statement, 17);
    const auto cacheMode = application::followUpPromptCacheModeFromString(columnText(statement, 18));
    if (!cacheMode.has_value()) return invalid("has an unknown cache mode");
    auto pricing = application::parseFollowUpPricingIdentity(columnText(statement, 19));
    if (!pricing.success()) return Result::failure(*pricing.error());
    if (pricing.value().promptCache.mode != *cacheMode || pricing.value().modelId != columnText(statement, 13)
            || pricing.value().providerId != quote.providerIdentity.providerId) {
        return invalid("has copies of its identity that disagree with the stored pricing identity");
    }
    quote.worstCase.pricingIdentity = std::move(pricing.value());
    quote.worstCase.pricingIdentityFingerprint = columnText(statement, 20);
    quote.worstCase.estimatedInputTokens = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 21));
    quote.worstCase.cacheableInputTokens = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 22));
    quote.worstCase.maximumOutputTokens = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 23));
    quote.capMicros = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 24));
    quote.worstCase.estimatedCostMicros = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 25));
    quote.worstCase.priceKnown = true;

    const auto state = application::followUpPlanRequestStateFromString(columnText(statement, 26));
    if (!state.has_value()) return invalid("has an unknown state");
    request.state = *state;
    request.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 27));
    request.quoteExpiresAt = fromMilliseconds(sqlite3_column_int64(statement, 28));
    request.updatedAt = fromMilliseconds(sqlite3_column_int64(statement, 29));
    request.authorizedAt = optionalTime(statement, 30);
    request.claimedAt = optionalTime(statement, 31);
    request.finishedAt = optionalTime(statement, 32);

    const auto reply = optionalText(statement, 33);
    const auto planFingerprint = optionalText(statement, 34);
    const auto advisories = optionalText(statement, 35);
    const auto failureCode = optionalText(statement, 36);
    const auto failureMessage = optionalText(statement, 37);
    if (planFingerprint.has_value()) {
        if (!reply.has_value() || !advisories.has_value()) return invalid("has a plan without its reply or advisories");
        request.answer = application::FollowUpPlanAnswer {*reply, *planFingerprint, *advisories};
    } else if (reply.has_value() && !failureCode.has_value()) {
        return invalid("has a reply that is neither a plan nor a failure");
    }
    if (failureCode.has_value()) {
        if (!failureMessage.has_value()) return invalid("has a failure code without a message");
        request.failure = application::FollowUpPlanFailure {*failureCode, *failureMessage, reply};
    }
    request.draftGroupId = optionalText(statement, 38);
    if (const auto accounting = optionalText(statement, 39); accounting.has_value()) {
        auto parsed = application::parseFollowUpAccounting(*accounting);
        if (!parsed.success()) return Result::failure(*parsed.error());
        request.accounting = std::move(parsed.value());
    }

    const auto gate = application::validateFollowUpPlanRequest(request);
    if (!gate.success()) return invalid("does not pass the follow-up request gate: " + gate.error()->message);

    std::optional<application::ProjectId> runProject;
    if (sqlite3_column_type(statement, 40) != SQLITE_NULL) runProject = columnText(statement, 40);
    const auto authority = application::checkFollowUpRequestReadAuthority(request, runProject);
    if (!authority.success()) return Result::failure(*authority.error());
    return Result::ok(std::move(request));
}

common::Result<std::vector<application::FollowUpPlanRequest>> collectFollowUpRequests(
    sqlite3* handle, sqlite3_stmt* statement)
{
    using Result = common::Result<std::vector<application::FollowUpPlanRequest>>;
    std::vector<application::FollowUpPlanRequest> requests;
    for (;;) {
        const int step = sqlite3_step(statement);
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return Result::failure(databaseError(handle, "persistence.read_failed", "Could not read follow-up requests"));
        }
        auto request = readFollowUpRequest(statement);
        if (!request.success()) return Result::failure(*request.error());
        requests.push_back(std::move(request.value()));
    }
    return Result::ok(std::move(requests));
}

common::Result<std::optional<application::FollowUpPlanRequest>> loadFollowUpRequest(
    sqlite3* handle, const application::FollowUpPlanRequestId& id)
{
    using Result = common::Result<std::optional<application::FollowUpPlanRequest>>;
    Statement statement;
    const std::string sql = std::string(followUpRequestSelect) + "WHERE r.id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success()) return Result::failure(*bound.error());
    auto rows = collectFollowUpRequests(handle, statement.get());
    if (!rows.success()) return Result::failure(*rows.error());
    if (rows.value().empty()) return Result::ok(std::nullopt);
    return Result::ok(std::move(rows.value().front()));
}

common::Result<std::optional<application::FollowUpPlanRequest>> loadOpenFollowUpRequest(
    sqlite3* handle, const application::RunId& runId)
{
    using Result = common::Result<std::optional<application::FollowUpPlanRequest>>;
    Statement statement;
    const std::string sql = std::string(followUpRequestSelect) + "WHERE r.source_run_id=? AND r.state IN "
        + followUpOpenStates + ";";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) return Result::failure(*bound.error());
    auto rows = collectFollowUpRequests(handle, statement.get());
    if (!rows.success()) return Result::failure(*rows.error());
    if (rows.value().empty()) return Result::ok(std::nullopt);
    return Result::ok(std::move(rows.value().front()));
}

// The Run's project, read inside the write transaction: the write authority (shared with the in-memory adapter).
common::Result<std::optional<application::ProjectId>> loadRunProject(sqlite3* handle, const application::RunId& runId)
{
    using Result = common::Result<std::optional<application::ProjectId>>;
    Statement statement;
    const auto prepared = prepare(handle, "SELECT project_id FROM runs WHERE id=?;", statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) return Result::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return Result::ok(std::nullopt);
    if (step != SQLITE_ROW) return Result::failure(databaseError(handle, "persistence.read_failed", "Could not read the Run"));
    return Result::ok(std::optional<application::ProjectId>(columnText(statement.get(), 0)));
}

// The columns a transition may change, bound from `index` on: state, updated, authorized, claimed, finished, reply,
// plan fingerprint, advisories, failure code, failure message, group id, accounting.
common::Result<void> bindFollowUpMutableColumns(
    sqlite3* handle, sqlite3_stmt* statement, int index, const application::FollowUpPlanRequest& request)
{
    std::optional<std::string> reply;
    if (request.answer.has_value()) reply = request.answer->rawReply;
    else if (request.failure.has_value()) reply = request.failure->rawReply;
    const common::Result<void> bindings[] = {
        bindText(handle, statement, index, application::toString(request.state)),
        bindInt64(handle, statement, index + 1, toMilliseconds(request.updatedAt)),
        bindOptionalTime(handle, statement, index + 2, request.authorizedAt),
        bindOptionalTime(handle, statement, index + 3, request.claimedAt),
        bindOptionalTime(handle, statement, index + 4, request.finishedAt),
        bindNullableText(handle, statement, index + 5, reply),
        bindNullableText(handle, statement, index + 6,
            request.answer.has_value() ? std::optional<std::string>(request.answer->planFingerprint) : std::nullopt),
        bindNullableText(handle, statement, index + 7,
            request.answer.has_value() ? std::optional<std::string>(request.answer->advisoriesJson) : std::nullopt),
        bindNullableText(handle, statement, index + 8,
            request.failure.has_value() ? std::optional<std::string>(request.failure->code) : std::nullopt),
        bindNullableText(handle, statement, index + 9,
            request.failure.has_value() ? std::optional<std::string>(request.failure->message) : std::nullopt),
        bindNullableText(handle, statement, index + 10, request.draftGroupId),
        bindNullableText(handle, statement, index + 11,
            request.accounting.has_value() ? std::optional<std::string>(application::serializeFollowUpAccounting(*request.accounting))
                                           : std::nullopt),
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return binding;
    }
    return common::Result<void>::ok();
}

common::Result<void> insertFollowUpRequestRow(sqlite3* handle, const application::FollowUpPlanRequest& request)
{
    Statement statement;
    const std::string sql = std::string("INSERT INTO follow_up_plan_requests (") + followUpRequestColumns
        + ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return prepared;
    const auto& quote = request.quote;
    const auto& worst = quote.worstCase;
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, request.id),
        bindText(handle, statement.get(), 2, request.projectId),
        bindText(handle, statement.get(), 3, request.sourceRunId),
        bindText(handle, statement.get(), 4, application::toString(request.kind)),
        bindText(handle, statement.get(), 5, request.goal),
        bindInt64(handle, statement.get(), 6, request.maxDrafts),
        bindText(handle, statement.get(), 7, application::toString(request.splitHint)),
        bindText(handle, statement.get(), 8, request.outcomeSourceStateFingerprint),
        bindText(handle, statement.get(), 9, request.snapshotFingerprint),
        bindText(handle, statement.get(), 10, application::serializeFollowUpPinnedFiles(request.files)),
        bindText(handle, statement.get(), 11, quote.policyId),
        bindText(handle, statement.get(), 12, quote.contextFingerprint),
        bindText(handle, statement.get(), 13, quote.providerIdentity.providerId),
        bindText(handle, statement.get(), 14, worst.pricingIdentity.modelId),
        bindText(handle, statement.get(), 15, quote.providerIdentity.kind),
        bindText(handle, statement.get(), 16, quote.providerIdentity.baseUrl),
        bindNullableText(handle, statement.get(), 17, quote.providerIdentity.credentialReference),
        bindText(handle, statement.get(), 18, quote.providerIdentityFingerprint),
        bindText(handle, statement.get(), 19, std::string(models::toString(worst.pricingIdentity.promptCache.mode))),
        bindText(handle, statement.get(), 20, application::serializeFollowUpPricingIdentity(worst.pricingIdentity)),
        bindText(handle, statement.get(), 21, worst.pricingIdentityFingerprint),
        bindUnsigned(handle, statement.get(), 22, worst.estimatedInputTokens),
        bindUnsigned(handle, statement.get(), 23, worst.cacheableInputTokens),
        bindUnsigned(handle, statement.get(), 24, worst.maximumOutputTokens),
        bindUnsigned(handle, statement.get(), 25, quote.capMicros),
        bindUnsigned(handle, statement.get(), 26, worst.estimatedCostMicros),
        bindText(handle, statement.get(), 27, application::toString(request.state)),
        bindInt64(handle, statement.get(), 28, toMilliseconds(request.createdAt)),
        bindInt64(handle, statement.get(), 29, toMilliseconds(request.quoteExpiresAt)),
        bindInt64(handle, statement.get(), 30, toMilliseconds(request.updatedAt)),
        bindOptionalTime(handle, statement.get(), 31, request.authorizedAt),
        bindOptionalTime(handle, statement.get(), 32, request.claimedAt),
        bindOptionalTime(handle, statement.get(), 33, request.finishedAt),
        bindNullableText(handle, statement.get(), 34,
            request.answer.has_value() ? std::optional<std::string>(request.answer->rawReply)
                : request.failure.has_value() ? request.failure->rawReply : std::nullopt),
        bindNullableText(handle, statement.get(), 35,
            request.answer.has_value() ? std::optional<std::string>(request.answer->planFingerprint) : std::nullopt),
        bindNullableText(handle, statement.get(), 36,
            request.answer.has_value() ? std::optional<std::string>(request.answer->advisoriesJson) : std::nullopt),
        bindNullableText(handle, statement.get(), 37,
            request.failure.has_value() ? std::optional<std::string>(request.failure->code) : std::nullopt),
        bindNullableText(handle, statement.get(), 38,
            request.failure.has_value() ? std::optional<std::string>(request.failure->message) : std::nullopt),
        bindNullableText(handle, statement.get(), 39, request.draftGroupId),
        bindNullableText(handle, statement.get(), 40,
            request.accounting.has_value() ? std::optional<std::string>(application::serializeFollowUpAccounting(*request.accounting))
                                           : std::nullopt),
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return binding;
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<void>::failure(
                error("follow_up.duplicate_id", "A follow-up request with this id already exists"));
        }
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not insert the follow-up request"));
    }
    return common::Result<void>::ok();
}

// The compare-and-set write of a computed next record: only while the stored state is still `expected`.
common::Result<void> updateFollowUpRequestRow(
    sqlite3* handle, const application::FollowUpPlanRequest& next, application::FollowUpPlanRequestState expected)
{
    Statement statement;
    const auto prepared = prepare(handle,
        "UPDATE follow_up_plan_requests SET state=?,updated_at_ms=?,authorized_at_ms=?,claimed_at_ms=?,finished_at_ms=?,"
        "reply_text=?,plan_fingerprint=?,advisories_json=?,failure_code=?,failure_message=?,draft_group_id=?,accounting_json=? "
        "WHERE id=? AND state=?;",
        statement);
    if (!prepared.success()) return prepared;
    const auto bound = bindFollowUpMutableColumns(handle, statement.get(), 1, next);
    if (!bound.success()) return bound;
    const auto idBound = bindText(handle, statement.get(), 13, next.id);
    if (!idBound.success()) return idBound;
    const auto stateBound = bindText(handle, statement.get(), 14, application::toString(expected));
    if (!stateBound.success()) return stateBound;
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not update the follow-up request"));
    }
    if (sqlite3_changes(handle) != 1) {
        return common::Result<void>::failure(
            error("follow_up.state_changed", "The follow-up request is no longer in the state the step expected"));
    }
    return common::Result<void>::ok();
}

} // namespace

common::Result<application::FollowUpInsertResult> SqliteFollowUpPlanRepository::insertQuoted(
    application::FollowUpPlanRequest request)
{
    using Result = common::Result<application::FollowUpInsertResult>;
    // Validated before the first statement; a refusal never opens a transaction.
    const auto gate = application::validateFollowUpPlanRequest(request);
    if (!gate.success()) return Result::failure(*gate.error());
    if (request.state != application::FollowUpPlanRequestState::Quoted) {
        return Result::failure(error("follow_up.invalid_request", "A request is inserted as quoted"));
    }

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    // BEGIN IMMEDIATE takes the write lock first, so a second writer waits here and sees what this transaction leaves behind.
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return Result::failure(*began.error());
    const auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return Result::failure(failure);
    };

    const auto runProject = loadRunProject(handle, request.sourceRunId);
    if (!runProject.success()) return rollbackWith(*runProject.error());
    const auto authority = application::checkFollowUpRequestWriteAuthority(request, runProject.value());
    if (!authority.success()) return rollbackWith(*authority.error());

    const auto open = loadOpenFollowUpRequest(handle, request.sourceRunId);
    if (!open.success()) return rollbackWith(*open.error());
    const auto replaced = application::planFollowUpQuotedInsert(request, open.value());
    if (!replaced.success()) return rollbackWith(*replaced.error());
    if (replaced.value().has_value()) {
        application::FollowUpTransition discard;
        discard.event = application::FollowUpPlanEvent::Discard;
        discard.expectedState = application::FollowUpPlanRequestState::Quoted;
        const auto next = application::applyFollowUpTransition(*open.value(), discard, request.createdAt);
        if (!next.success()) return rollbackWith(*next.error());
        const auto stored = updateFollowUpRequestRow(handle, next.value(), application::FollowUpPlanRequestState::Quoted);
        if (!stored.success()) return rollbackWith(*stored.error());
    }
    const auto inserted = insertFollowUpRequestRow(handle, request);
    if (!inserted.success()) return rollbackWith(*inserted.error());

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    application::FollowUpInsertResult result;
    result.request = std::move(request);
    result.discardedQuoteId = replaced.value();
    return Result::ok(std::move(result));
}

common::Result<application::FollowUpPlanRequest> SqliteFollowUpPlanRepository::transition(
    const application::FollowUpPlanRequestId& id,
    application::FollowUpTransition transition,
    std::chrono::system_clock::time_point now)
{
    using Result = common::Result<application::FollowUpPlanRequest>;
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return Result::failure(*began.error());
    const auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return Result::failure(failure);
    };

    const auto current = loadFollowUpRequest(handle, id);
    if (!current.success()) return rollbackWith(*current.error());
    if (!current.value().has_value()) return rollbackWith(error("follow_up.not_found", "No such follow-up request"));
    auto next = application::applyFollowUpTransition(*current.value(), transition, now);
    if (!next.success()) return rollbackWith(*next.error());
    const auto stored = updateFollowUpRequestRow(handle, next.value(), transition.expectedState);
    if (!stored.success()) return rollbackWith(*stored.error());

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return Result::ok(std::move(next.value()));
}

common::Result<std::optional<application::FollowUpPlanRequest>> SqliteFollowUpPlanRepository::findById(
    const application::FollowUpPlanRequestId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    return loadFollowUpRequest(database_->impl_->handle, id);
}

common::Result<std::optional<application::FollowUpPlanRequest>> SqliteFollowUpPlanRepository::findOpenForSourceRun(
    const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    return loadOpenFollowUpRequest(database_->impl_->handle, runId);
}

common::Result<std::optional<application::FollowUpPlanRequest>> SqliteFollowUpPlanRepository::findLatestForSourceRun(
    const application::RunId& runId) const
{
    using Result = common::Result<std::optional<application::FollowUpPlanRequest>>;
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string(followUpRequestSelect)
        + "WHERE r.source_run_id=? ORDER BY r.created_at_ms DESC, r.id DESC LIMIT 1;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) return Result::failure(*bound.error());
    auto rows = collectFollowUpRequests(handle, statement.get());
    if (!rows.success()) return Result::failure(*rows.error());
    if (rows.value().empty()) return Result::ok(std::nullopt);
    return Result::ok(std::move(rows.value().front()));
}

common::Result<std::vector<application::FollowUpPlanRequest>> SqliteFollowUpPlanRepository::listForProject(
    const application::ProjectId& projectId, std::size_t limit) const
{
    using Result = common::Result<std::vector<application::FollowUpPlanRequest>>;
    const auto checked = application::checkFollowUpListLimit(limit);
    if (!checked.success()) return Result::failure(*checked.error());

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    // A request belongs to the list of the project it NAMES and to the list of the project its Run belongs to: when the two
    // differ, both lists meet the row and both fail closed.
    const std::string sql = std::string(followUpRequestSelect)
        + "WHERE (r.project_id=? OR runs.project_id=?) ORDER BY r.created_at_ms DESC, r.id DESC LIMIT ?;";
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
    return collectFollowUpRequests(handle, statement.get());
}

common::Result<std::vector<application::FollowUpPlanRequest>> SqliteFollowUpPlanRepository::listByState(
    application::FollowUpPlanRequestState state, std::size_t limit) const
{
    return listByStateAfter(state, limit, std::nullopt);
}

common::Result<std::vector<application::FollowUpPlanRequest>> SqliteFollowUpPlanRepository::listByStateAfter(
    application::FollowUpPlanRequestState state, std::size_t limit,
    const std::optional<application::FollowUpStateCursor>& after) const
{
    using Result = common::Result<std::vector<application::FollowUpPlanRequest>>;
    const auto checked = application::checkFollowUpListLimit(limit);
    if (!checked.success()) return Result::failure(*checked.error());

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    // Strictly after the cursor in (created_at_ms, id) order; without a cursor the bound is below every row.
    const std::string sql = std::string(followUpRequestSelect)
        + "WHERE r.state=? AND (r.created_at_ms > ? OR (r.created_at_ms = ? AND r.id > ?)) "
          "ORDER BY r.created_at_ms ASC, r.id ASC LIMIT ?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const std::int64_t afterMs = after.has_value() ? toMilliseconds(after->createdAt) : std::numeric_limits<std::int64_t>::min();
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, application::toString(state)),
        bindInt64(handle, statement.get(), 2, afterMs),
        bindInt64(handle, statement.get(), 3, afterMs),
        bindText(handle, statement.get(), 4, after.has_value() ? after->id : std::string()),
        bindInt64(handle, statement.get(), 5, static_cast<std::int64_t>(limit)),
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return Result::failure(*binding.error());
    }
    return collectFollowUpRequests(handle, statement.get());
}

common::Result<application::FollowUpConfirmResult> SqliteFollowUpPlanRepository::confirmDrafts(
    const application::FollowUpPlanRequestId& requestId,
    const std::string& expectedPlanFingerprint,
    application::RunDraftGroup group,
    std::vector<RunDraft> drafts,
    std::chrono::system_clock::time_point now)
{
    using Result = common::Result<application::FollowUpConfirmResult>;
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return Result::failure(*began.error());
    const auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return Result::failure(failure);
    };

    const auto loaded = loadFollowUpRequest(handle, requestId);
    if (!loaded.success()) return rollbackWith(*loaded.error());
    if (!loaded.value().has_value()) return rollbackWith(error("follow_up.not_found", "No such follow-up request"));
    const auto& request = *loaded.value();
    const auto action = application::planFollowUpConfirm(request, expectedPlanFingerprint, group, drafts);
    if (!action.success()) return rollbackWith(*action.error());

    if (action.value() == application::FollowUpConfirmAction::ReturnExisting) {
        const auto storedGroup = findRunDraftGroupWhere(handle, "id=?", *request.draftGroupId);
        if (!storedGroup.success()) return rollbackWith(*storedGroup.error());
        if (!storedGroup.value().has_value()
                || storedGroup.value()->sourceRequestId != std::optional<std::string>(requestId)) {
            return rollbackWith(error("follow_up.drafts_removed",
                "The drafts of this request were removed after they were saved"));
        }
        auto stored = readRunDraftGroupDrafts(handle, storedGroup.value()->id);
        if (!stored.success()) return rollbackWith(*stored.error());
        application::FollowUpConfirmResult result;
        result.request = request;
        result.group = std::move(*storedGroup.value());
        result.drafts = std::move(stored.value());
        result.created = false;
        // Nothing was written: the transaction only read.
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return Result::ok(std::move(result));
    }

    auto next = application::applyFollowUpConfirm(request, group.id, now);
    if (!next.success()) return rollbackWith(*next.error());
    const auto valid = application::validateRunDraftGroupInsertion(group, drafts);
    if (!valid.success()) return rollbackWith(*valid.error());
    // Every Run the group names is read INSIDE this transaction: it exists and belongs to the request's project.
    const auto sources = application::checkFollowUpGroupSourceRuns(group, request.projectId,
        [&](const application::RunId& runId) { return loadRunProject(handle, runId); });
    if (!sources.success()) return rollbackWith(*sources.error());
    std::sort(drafts.begin(), drafts.end(), [](const RunDraft& left, const RunDraft& right) {
        return left.provenance->ordinal < right.provenance->ordinal;
    });
    // The group first (its trigger requires the request to be plan_ready), then the request (its trigger requires the group).
    const auto written = insertRunDraftGroupRows(handle, group, drafts);
    if (!written.success()) return rollbackWith(*written.error());
    const auto stored = updateFollowUpRequestRow(handle, next.value(), application::FollowUpPlanRequestState::PlanReady);
    if (!stored.success()) return rollbackWith(*stored.error());

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    application::FollowUpConfirmResult result;
    result.request = std::move(next.value());
    result.group = std::move(group);
    result.drafts = std::move(drafts);
    result.created = true;
    return Result::ok(std::move(result));
}

} // namespace ai::infrastructure
