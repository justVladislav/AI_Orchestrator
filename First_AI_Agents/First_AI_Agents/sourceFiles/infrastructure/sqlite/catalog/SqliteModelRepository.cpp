#include "../SqliteConnection.hpp"
#include "../SqlitePrimitives.hpp"

#include "ai/application/ProviderCatalogTypes.hpp"

#include <sqlite3.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ai::infrastructure {
namespace {

using application::LongContextPricingProblem;
using application::Model;
using sqlite_detail::Statement;
using sqlite_detail::error;
using sqlite_detail::databaseError;
using sqlite_detail::execute;
using sqlite_detail::prepare;
using sqlite_detail::bindText;
using sqlite_detail::bindInt64;
using sqlite_detail::bindUnsigned;
using sqlite_detail::toMilliseconds;
using sqlite_detail::fromMilliseconds;
using sqlite_detail::columnText;

constexpr const char* modelColumns =
    "id, provider_id, api_model_id, display_name, context_window_tokens, max_output_tokens, "
    "input_price_micros_per_million_tokens, output_price_micros_per_million_tokens, enabled, "
    "is_default, created_at_ms, updated_at_ms, cache_write_price_micros_per_million_tokens, "
    "cache_read_price_micros_per_million_tokens, long_context_threshold_input_tokens, "
    "long_context_input_price_micros_per_million_tokens, "
    "long_context_output_price_micros_per_million_tokens, "
    "long_context_cache_write_price_micros_per_million_tokens, "
    "long_context_cache_read_price_micros_per_million_tokens, "
    "cache_write_one_hour_price_micros_per_million_tokens, "
    "long_context_cache_write_one_hour_price_micros_per_million_tokens";

common::Result<Model> readModel(sqlite3_stmt* statement)
{
    const std::int64_t contextWindow = sqlite3_column_int64(statement, 4);
    const std::int64_t maxOutput = sqlite3_column_int64(statement, 5);
    const std::int64_t inputPrice = sqlite3_column_int64(statement, 6);
    const std::int64_t outputPrice = sqlite3_column_int64(statement, 7);
    const std::int64_t cacheWritePrice = sqlite3_column_int64(statement, 12);
    const std::int64_t cacheReadPrice = sqlite3_column_int64(statement, 13);
    const std::int64_t longThreshold = sqlite3_column_int64(statement, 14);
    const std::int64_t longInputPrice = sqlite3_column_int64(statement, 15);
    const std::int64_t longOutputPrice = sqlite3_column_int64(statement, 16);
    const std::int64_t longCacheWritePrice = sqlite3_column_int64(statement, 17);
    const std::int64_t longCacheReadPrice = sqlite3_column_int64(statement, 18);
    const std::int64_t cacheWriteOneHourPrice = sqlite3_column_int64(statement, 19);
    const std::int64_t longCacheWriteOneHourPrice = sqlite3_column_int64(statement, 20);
    if (contextWindow < 0 || maxOutput < 0 || inputPrice < 0 || outputPrice < 0
        || cacheWritePrice < 0 || cacheReadPrice < 0
        || longThreshold < 0 || longInputPrice < 0 || longOutputPrice < 0
        || longCacheWritePrice < 0 || longCacheReadPrice < 0
        || cacheWriteOneHourPrice < 0 || longCacheWriteOneHourPrice < 0) {
        return common::Result<Model>::failure(
            error("persistence.invalid_data", "Model row contains a negative unsigned value"));
    }

    Model model;
    model.id = columnText(statement, 0);
    model.providerId = columnText(statement, 1);
    model.apiModelId = columnText(statement, 2);
    model.displayName = columnText(statement, 3);
    model.contextWindowTokens = static_cast<std::uint64_t>(contextWindow);
    model.maxOutputTokens = static_cast<std::uint64_t>(maxOutput);
    model.inputPriceMicrosPerMillionTokens = static_cast<std::uint64_t>(inputPrice);
    model.outputPriceMicrosPerMillionTokens = static_cast<std::uint64_t>(outputPrice);
    model.enabled = sqlite3_column_int(statement, 8) != 0;
    model.isDefault = sqlite3_column_int(statement, 9) != 0;
    model.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 10));
    model.updatedAt = fromMilliseconds(sqlite3_column_int64(statement, 11));
    model.cacheWritePriceMicrosPerMillionTokens = static_cast<std::uint64_t>(cacheWritePrice);
    model.cacheReadPriceMicrosPerMillionTokens = static_cast<std::uint64_t>(cacheReadPrice);
    model.longContextPricing.thresholdInputTokens = static_cast<std::uint64_t>(longThreshold);
    model.longContextPricing.inputPriceMicrosPerMillionTokens =
        static_cast<std::uint64_t>(longInputPrice);
    model.longContextPricing.outputPriceMicrosPerMillionTokens =
        static_cast<std::uint64_t>(longOutputPrice);
    model.longContextPricing.cacheWritePriceMicrosPerMillionTokens =
        static_cast<std::uint64_t>(longCacheWritePrice);
    model.longContextPricing.cacheReadPriceMicrosPerMillionTokens =
        static_cast<std::uint64_t>(longCacheReadPrice);
    model.cacheWriteOneHourPriceMicrosPerMillionTokens =
        static_cast<std::uint64_t>(cacheWriteOneHourPrice);
    model.longContextPricing.cacheWriteOneHourPriceMicrosPerMillionTokens =
        static_cast<std::uint64_t>(longCacheWriteOneHourPrice);
    // A row that is self-consistent SQL but logically forbidden (long prices without a threshold -
    // which would read as FLAT pricing and understate a cost - or a threshold the window cannot
    // reach) is never trusted: the read fails closed instead of handing the cost step a wrong tier.
    const auto problem =
        checkLongContextPricing(model.contextWindowTokens, model.longContextPricing);
    if (problem != LongContextPricingProblem::None) {
        return common::Result<Model>::failure(error(
            "persistence.invalid_data",
            std::string("Model row has an invalid long-context price tier: ")
                + longContextPricingProblemMessage(problem)));
    }
    return common::Result<Model>::ok(std::move(model));
}

common::Result<void> bindModel(sqlite3* handle, sqlite3_stmt* statement, const Model& model)
{
    const common::Result<void> bindings[] = {
        bindText(handle, statement, 1, model.id),
        bindText(handle, statement, 2, model.providerId),
        bindText(handle, statement, 3, model.apiModelId),
        bindText(handle, statement, 4, model.displayName),
        bindUnsigned(handle, statement, 5, model.contextWindowTokens),
        bindUnsigned(handle, statement, 6, model.maxOutputTokens),
        bindUnsigned(handle, statement, 7, model.inputPriceMicrosPerMillionTokens),
        bindUnsigned(handle, statement, 8, model.outputPriceMicrosPerMillionTokens),
        bindInt64(handle, statement, 9, model.enabled ? 1 : 0),
        bindInt64(handle, statement, 10, model.isDefault ? 1 : 0),
        bindInt64(handle, statement, 11, toMilliseconds(model.createdAt)),
        bindInt64(handle, statement, 12, toMilliseconds(model.updatedAt)),
        bindUnsigned(handle, statement, 13, model.cacheWritePriceMicrosPerMillionTokens),
        bindUnsigned(handle, statement, 14, model.cacheReadPriceMicrosPerMillionTokens),
        bindUnsigned(handle, statement, 15, model.longContextPricing.thresholdInputTokens),
        bindUnsigned(handle, statement, 16,
            model.longContextPricing.inputPriceMicrosPerMillionTokens),
        bindUnsigned(handle, statement, 17,
            model.longContextPricing.outputPriceMicrosPerMillionTokens),
        bindUnsigned(handle, statement, 18,
            model.longContextPricing.cacheWritePriceMicrosPerMillionTokens),
        bindUnsigned(handle, statement, 19,
            model.longContextPricing.cacheReadPriceMicrosPerMillionTokens),
        bindUnsigned(handle, statement, 20,
            model.cacheWriteOneHourPriceMicrosPerMillionTokens),
        bindUnsigned(handle, statement, 21,
            model.longContextPricing.cacheWriteOneHourPriceMicrosPerMillionTokens)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    return common::Result<void>::ok();
}

} // namespace

SqliteModelRepository::SqliteModelRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<Model> SqliteModelRepository::insert(Model model)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO models (id,provider_id,api_model_id,display_name,context_window_tokens,"
        "max_output_tokens,input_price_micros_per_million_tokens,output_price_micros_per_million_tokens,"
        "enabled,is_default,created_at_ms,updated_at_ms,cache_write_price_micros_per_million_tokens,"
        "cache_read_price_micros_per_million_tokens,long_context_threshold_input_tokens,"
        "long_context_input_price_micros_per_million_tokens,"
        "long_context_output_price_micros_per_million_tokens,"
        "long_context_cache_write_price_micros_per_million_tokens,"
        "long_context_cache_read_price_micros_per_million_tokens,"
        "cache_write_one_hour_price_micros_per_million_tokens,"
        "long_context_cache_write_one_hour_price_micros_per_million_tokens) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<Model>::failure(*prepared.error());
    const auto bound = bindModel(handle, statement.get(), model);
    if (!bound.success()) return common::Result<Model>::failure(*bound.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        const int code = sqlite3_extended_errcode(handle);
        if (code == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<Model>::failure(error("model.duplicate_id", "Model id already exists"));
        }
        if (code == SQLITE_CONSTRAINT_UNIQUE) {
            return common::Result<Model>::failure(error(
                "model.duplicate_api_model_id",
                "A model with this apiModelId already exists for this provider"));
        }
        return common::Result<Model>::failure(
            databaseError(handle, "persistence.write_failed", "Could not insert model"));
    }
    return common::Result<Model>::ok(std::move(model));
}

common::Result<Model> SqliteModelRepository::update(Model model)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    // is_default is deliberately not part of this UPDATE: only
    // setDefaultModel() below may change it.
    const auto prepared = prepare(handle,
        "UPDATE models SET provider_id=?,api_model_id=?,display_name=?,context_window_tokens=?,"
        "max_output_tokens=?,input_price_micros_per_million_tokens=?,"
        "output_price_micros_per_million_tokens=?,enabled=?,created_at_ms=?,updated_at_ms=?,"
        "cache_write_price_micros_per_million_tokens=?,cache_read_price_micros_per_million_tokens=?,"
        "long_context_threshold_input_tokens=?,"
        "long_context_input_price_micros_per_million_tokens=?,"
        "long_context_output_price_micros_per_million_tokens=?,"
        "long_context_cache_write_price_micros_per_million_tokens=?,"
        "long_context_cache_read_price_micros_per_million_tokens=?,"
        "cache_write_one_hour_price_micros_per_million_tokens=?,"
        "long_context_cache_write_one_hour_price_micros_per_million_tokens=? "
        "WHERE id=?;",
        statement);
    if (!prepared.success()) return common::Result<Model>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, model.providerId),
        bindText(handle, statement.get(), 2, model.apiModelId),
        bindText(handle, statement.get(), 3, model.displayName),
        bindUnsigned(handle, statement.get(), 4, model.contextWindowTokens),
        bindUnsigned(handle, statement.get(), 5, model.maxOutputTokens),
        bindUnsigned(handle, statement.get(), 6, model.inputPriceMicrosPerMillionTokens),
        bindUnsigned(handle, statement.get(), 7, model.outputPriceMicrosPerMillionTokens),
        bindInt64(handle, statement.get(), 8, model.enabled ? 1 : 0),
        bindInt64(handle, statement.get(), 9, toMilliseconds(model.createdAt)),
        bindInt64(handle, statement.get(), 10, toMilliseconds(model.updatedAt)),
        bindUnsigned(handle, statement.get(), 11, model.cacheWritePriceMicrosPerMillionTokens),
        bindUnsigned(handle, statement.get(), 12, model.cacheReadPriceMicrosPerMillionTokens),
        bindUnsigned(handle, statement.get(), 13, model.longContextPricing.thresholdInputTokens),
        bindUnsigned(handle, statement.get(), 14,
            model.longContextPricing.inputPriceMicrosPerMillionTokens),
        bindUnsigned(handle, statement.get(), 15,
            model.longContextPricing.outputPriceMicrosPerMillionTokens),
        bindUnsigned(handle, statement.get(), 16,
            model.longContextPricing.cacheWritePriceMicrosPerMillionTokens),
        bindUnsigned(handle, statement.get(), 17,
            model.longContextPricing.cacheReadPriceMicrosPerMillionTokens),
        bindUnsigned(handle, statement.get(), 18,
            model.cacheWriteOneHourPriceMicrosPerMillionTokens),
        bindUnsigned(handle, statement.get(), 19,
            model.longContextPricing.cacheWriteOneHourPriceMicrosPerMillionTokens),
        bindText(handle, statement.get(), 20, model.id)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<Model>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_UNIQUE) {
            return common::Result<Model>::failure(error(
                "model.duplicate_api_model_id",
                "A model with this apiModelId already exists for this provider"));
        }
        return common::Result<Model>::failure(
            databaseError(handle, "persistence.write_failed", "Could not update model"));
    }
    if (sqlite3_changes(handle) == 0) {
        return common::Result<Model>::failure(error("model.not_found", "Model not found"));
    }
    // is_default was intentionally not touched by the UPDATE above. The
    // Application service updates a model loaded from this repository, so the
    // incoming DTO already carries the current isDefault value. Do not call
    // findById() here while holding database_->impl_->mutex: the repository
    // mutex is non-recursive and doing so would deadlock Qt adapter pricing
    // updates.
    return common::Result<Model>::ok(std::move(model));
}

common::Result<std::optional<Model>> SqliteModelRepository::findById(const application::ModelId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + modelColumns + " FROM models WHERE id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<Model>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success()) return common::Result<std::optional<Model>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<Model>>::ok(std::nullopt);
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<Model>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read model"));
    }
    const auto model = readModel(statement.get());
    if (!model.success()) return common::Result<std::optional<Model>>::failure(*model.error());
    return common::Result<std::optional<Model>>::ok(model.value());
}

common::Result<std::vector<Model>> SqliteModelRepository::listAll(
    const std::optional<application::ProviderId>& providerId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + modelColumns + " FROM models"
        + (providerId.has_value() ? " WHERE provider_id=?" : "")
        + " ORDER BY created_at_ms,id;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<Model>>::failure(*prepared.error());
    if (providerId.has_value()) {
        const auto bound = bindText(handle, statement.get(), 1, *providerId);
        if (!bound.success()) return common::Result<std::vector<Model>>::failure(*bound.error());
    }

    std::vector<Model> models;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<Model>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list models"));
        }
        auto model = readModel(statement.get());
        if (!model.success()) return common::Result<std::vector<Model>>::failure(*model.error());
        models.push_back(model.value());
    }
    return common::Result<std::vector<Model>>::ok(std::move(models));
}

common::Result<void> SqliteModelRepository::setDefaultModel(
    const application::ProviderId& providerId,
    const application::ModelId& modelId)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return began;

    auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return common::Result<void>::failure(failure);
    };

    {
        // Unset first so the partial unique index (at most one default per
        // provider) never observes two defaults, even transiently within
        // this same transaction.
        Statement unset;
        const auto unsetPrepared = prepare(
            handle, "UPDATE models SET is_default=0 WHERE provider_id=? AND is_default=1;", unset);
        if (!unsetPrepared.success()) return rollbackWith(*unsetPrepared.error());
        const auto unsetBound = bindText(handle, unset.get(), 1, providerId);
        if (!unsetBound.success()) return rollbackWith(*unsetBound.error());
        if (sqlite3_step(unset.get()) != SQLITE_DONE) {
            return rollbackWith(
                databaseError(handle, "persistence.write_failed", "Could not clear previous default model"));
        }
    }

    {
        Statement setDefault;
        const auto setPrepared = prepare(
            handle, "UPDATE models SET is_default=1 WHERE id=? AND provider_id=?;", setDefault);
        if (!setPrepared.success()) return rollbackWith(*setPrepared.error());
        const common::Result<void> bindings[] = {
            bindText(handle, setDefault.get(), 1, modelId),
            bindText(handle, setDefault.get(), 2, providerId)};
        for (const auto& binding : bindings) {
            if (!binding.success()) return rollbackWith(*binding.error());
        }
        if (sqlite3_step(setDefault.get()) != SQLITE_DONE) {
            return rollbackWith(
                databaseError(handle, "persistence.write_failed", "Could not set default model"));
        }
        if (sqlite3_changes(handle) == 0) {
            // Zero rows means modelId does not exist or does not belong to
            // providerId; both collapse to this one code, matching the
            // ambiguous-zero-row convention already used elsewhere in this
            // file (e.g. SqliteRunRepository::updateStage).
            return rollbackWith(error(
                "model.provider_mismatch", "Model does not belong to the given provider"));
        }
    }

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<void>::ok();
}

common::Result<void> SqliteModelRepository::remove(const application::ModelId& id)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle, "DELETE FROM models WHERE id=?;", statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success()) return common::Result<void>::failure(*bound.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        // This connection has extended result codes enabled
        // (sqlite3_extended_result_codes(), see SqliteDatabase::open()), so
        // sqlite3_errcode() returns the extended value too rather than
        // masking it back down - mask manually to get the base code.
        // Empirically, an ON DELETE RESTRICT violation on the *parent* side
        // (a child row in run_stage_assignments/execution_profile_assignments/
        // run_draft_assignments/model_executions/chat_messages/etc. still
        // references this model) surfaces as SQLITE_CONSTRAINT_TRIGGER
        // (1811), not SQLITE_CONSTRAINT_FOREIGNKEY (that extended subtype is
        // for a bad reference on the *child* side - insert/update with an
        // unknown model_id). Matching on the base code instead of one
        // specific extended subtype is correct either way: this DELETE
        // statement has no other constraint that could fail.
        if ((sqlite3_extended_errcode(handle) & 0xff) == SQLITE_CONSTRAINT) {
            return common::Result<void>::failure(error(
                "model.in_use",
                "This model has execution history, saved profiles, drafts, or chats "
                "referencing it and cannot be deleted. Disable it instead."));
        }
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not remove model"));
    }
    if (sqlite3_changes(handle) == 0) {
        return common::Result<void>::failure(error("model.not_found", "Model not found"));
    }
    return common::Result<void>::ok();
}

} // namespace ai::infrastructure
