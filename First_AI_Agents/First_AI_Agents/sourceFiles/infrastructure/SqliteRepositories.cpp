#include "ai/infrastructure/SqliteRepositories.hpp"

#include "ai/application/ExecutionAcceptanceContract.hpp"
#include "ai/application/CacheDurationAccounting.hpp"
#include "ai/application/ChatProjectContext.hpp"
#include "ai/application/RunDraftGroup.hpp"
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
using application::ComparisonSession;
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

constexpr int supportedSchemaVersion = 70;

// PC-5B/5C/5D: the SQL of the follow-up request table and of everything that lives on it, written ONCE. Step 66 creates it; step 67
// creates it again for a database whose v66 table was made before the table was amended in place (see step 67). Both therefore
// produce byte-for-byte the same objects, which a test compares with a freshly created database.
std::string followUpPlanRequestsObjectsSql()
{
    return R"sql(
CREATE TABLE IF NOT EXISTS follow_up_plan_requests (
    id TEXT PRIMARY KEY NOT NULL CHECK(length(id) BETWEEN 1 AND 64),
    -- RESTRICT on purpose, like the Outcome Brief: a request is evidence of what was sent and paid for, so neither its
    -- project nor its source Run (nor the provider and model it was quoted for) can be deleted from under it.
    project_id TEXT NOT NULL REFERENCES projects(id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    source_run_id TEXT NOT NULL REFERENCES runs(id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    kind TEXT NOT NULL CHECK(kind IN ('next_run','recovery')),
    goal TEXT NOT NULL CHECK(length(CAST(goal AS BLOB)) BETWEEN 1 AND 4096),
    max_drafts INTEGER NOT NULL CHECK(max_drafts BETWEEN 1 AND 4),
    split_hint TEXT NOT NULL CHECK(split_hint IN ('one','many','auto')),
    outcome_source_state_fingerprint TEXT NOT NULL CHECK(length(outcome_source_state_fingerprint) = 64
        AND outcome_source_state_fingerprint NOT GLOB '*[^0-9a-f]*'),
    snapshot_fingerprint TEXT NOT NULL CHECK(length(snapshot_fingerprint) = 64
        AND snapshot_fingerprint NOT GLOB '*[^0-9a-f]*'),
    context_files_json TEXT NOT NULL CHECK(substr(context_files_json, 1, 1) = '[' AND substr(context_files_json, -1, 1) = ']'),
    context_policy_id TEXT NOT NULL CHECK(context_policy_id = 'follow-up-composer.v1'),
    context_fingerprint TEXT NOT NULL CHECK(length(context_fingerprint) = 64 AND context_fingerprint NOT GLOB '*[^0-9a-f]*'),
    provider_id TEXT NOT NULL REFERENCES providers(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    model_id TEXT NOT NULL REFERENCES models(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    provider_kind TEXT NOT NULL CHECK(length(provider_kind) BETWEEN 1 AND 64),
    provider_base_url TEXT NOT NULL CHECK(length(CAST(provider_base_url AS BLOB)) <= 2048),
    provider_credential_reference TEXT CHECK(provider_credential_reference IS NULL
        OR length(CAST(provider_credential_reference AS BLOB)) <= 1024),
    provider_identity_fingerprint TEXT NOT NULL CHECK(length(provider_identity_fingerprint) = 64
        AND provider_identity_fingerprint NOT GLOB '*[^0-9a-f]*'),
    prompt_cache_mode TEXT NOT NULL CHECK(prompt_cache_mode IN ('disabled','ephemeral_5m','ephemeral_1h')),
    pricing_identity_json TEXT NOT NULL CHECK(substr(pricing_identity_json, 1, 1) = '{' AND substr(pricing_identity_json, -1, 1) = '}'),
    pricing_identity_fingerprint TEXT NOT NULL CHECK(length(pricing_identity_fingerprint) = 64
        AND pricing_identity_fingerprint NOT GLOB '*[^0-9a-f]*'),
    estimated_input_tokens INTEGER NOT NULL CHECK(estimated_input_tokens BETWEEN 1 AND 48000),
    cacheable_input_tokens INTEGER NOT NULL CHECK(cacheable_input_tokens >= 0),
    effective_max_output_tokens INTEGER NOT NULL CHECK(effective_max_output_tokens BETWEEN 4096 AND 16384),
    cap_micros INTEGER NOT NULL CHECK(cap_micros BETWEEN 1 AND 360000),
    estimated_cost_micros INTEGER NOT NULL CHECK(estimated_cost_micros >= 0),
    state TEXT NOT NULL CHECK(state IN ('quoted','authorized','composing','plan_ready','drafts_saved','stale','expired',
        'cancelled','discarded','interrupted','failed')),
    created_at_ms INTEGER NOT NULL,
    quote_expires_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL,
    authorized_at_ms INTEGER,
    -- The claim stamp: present for every request that left the claim behind (the provider call may have been made and
    -- billed), absent for a request that never reached it. Free versus possibly charged is decided by THIS, not by the
    -- name of a terminal state (cancelled and discarded exist on both sides).
    claimed_at_ms INTEGER,
    finished_at_ms INTEGER,
    reply_text TEXT CHECK(reply_text IS NULL OR length(CAST(reply_text AS BLOB)) <= 262144),
    plan_fingerprint TEXT CHECK(plan_fingerprint IS NULL OR (length(plan_fingerprint) = 64
        AND plan_fingerprint NOT GLOB '*[^0-9a-f]*')),
    advisories_json TEXT CHECK(advisories_json IS NULL OR (substr(advisories_json, 1, 1) = '['
        AND substr(advisories_json, -1, 1) = ']' AND length(CAST(advisories_json AS BLOB)) <= 65536)),
    failure_code TEXT CHECK(failure_code IS NULL OR length(CAST(failure_code AS BLOB)) BETWEEN 1 AND 128),
    failure_message TEXT CHECK(failure_message IS NULL OR length(CAST(failure_message AS BLOB)) <= 4096),
    -- No foreign key on purpose: the user may remove every draft of a saved group (that removes the group), and the
    -- request stays what it was, a record that its plan was saved. The group points back with a RESTRICT key.
    draft_group_id TEXT CHECK(draft_group_id IS NULL OR length(draft_group_id) BETWEEN 1 AND 64),
    -- What the provider reported for the one paid call and what it cost, written in the SAME step that settles the request: the durable
    -- copy from which the execution log is written again if it failed to settle. Present exactly when a provider response arrived.
    accounting_json TEXT CHECK(accounting_json IS NULL OR (substr(accounting_json, 1, 1) = '{'
        AND substr(accounting_json, -1, 1) = '}' AND length(CAST(accounting_json AS BLOB)) <= 1024)),
    CHECK(split_hint <> 'many' OR max_drafts >= 2),
    CHECK(cacheable_input_tokens <= estimated_input_tokens),
    CHECK((prompt_cache_mode = 'disabled') = (cacheable_input_tokens = 0)),
    CHECK(estimated_cost_micros <= cap_micros),
    CHECK(quote_expires_at_ms = created_at_ms + 900000),
    CHECK(updated_at_ms >= created_at_ms),
    CHECK(authorized_at_ms IS NULL OR (authorized_at_ms >= created_at_ms AND authorized_at_ms <= updated_at_ms)),
    CHECK(claimed_at_ms IS NULL OR (authorized_at_ms IS NOT NULL AND claimed_at_ms >= authorized_at_ms
        AND claimed_at_ms <= updated_at_ms)),
    CHECK(finished_at_ms IS NULL OR (finished_at_ms = updated_at_ms
        AND finished_at_ms >= COALESCE(claimed_at_ms, authorized_at_ms, created_at_ms))),
    -- The step that leaves a request quoted / authorized / composing sets its stamp and the update time from one value.
    CHECK(state <> 'quoted' OR updated_at_ms = created_at_ms),
    CHECK(state <> 'authorized' OR updated_at_ms = authorized_at_ms),
    CHECK(state <> 'composing' OR updated_at_ms = claimed_at_ms),
    CHECK((plan_fingerprint IS NULL) = (advisories_json IS NULL)),
    CHECK(plan_fingerprint IS NULL OR reply_text IS NOT NULL),
    CHECK(accounting_json IS NULL OR claimed_at_ms IS NOT NULL),
    CHECK(plan_fingerprint IS NULL OR accounting_json IS NOT NULL),
    CHECK((failure_code IS NULL) = (failure_message IS NULL)),
    CHECK(
        (state = 'quoted' AND authorized_at_ms IS NULL AND claimed_at_ms IS NULL AND finished_at_ms IS NULL
            AND reply_text IS NULL AND plan_fingerprint IS NULL AND failure_code IS NULL AND draft_group_id IS NULL
            AND accounting_json IS NULL)
     OR (state = 'authorized' AND authorized_at_ms IS NOT NULL AND claimed_at_ms IS NULL AND finished_at_ms IS NULL
            AND reply_text IS NULL AND plan_fingerprint IS NULL AND failure_code IS NULL AND draft_group_id IS NULL
            AND accounting_json IS NULL)
     OR (state = 'composing' AND authorized_at_ms IS NOT NULL AND claimed_at_ms IS NOT NULL AND finished_at_ms IS NULL
            AND reply_text IS NULL AND plan_fingerprint IS NULL AND failure_code IS NULL AND draft_group_id IS NULL
            AND accounting_json IS NULL)
     OR (state = 'plan_ready' AND authorized_at_ms IS NOT NULL AND claimed_at_ms IS NOT NULL AND finished_at_ms IS NULL
            AND plan_fingerprint IS NOT NULL AND failure_code IS NULL AND draft_group_id IS NULL)
     OR (state = 'drafts_saved' AND authorized_at_ms IS NOT NULL AND claimed_at_ms IS NOT NULL AND finished_at_ms IS NOT NULL
            AND plan_fingerprint IS NOT NULL AND failure_code IS NULL AND draft_group_id IS NOT NULL)
     OR (state = 'failed' AND authorized_at_ms IS NOT NULL AND claimed_at_ms IS NOT NULL AND finished_at_ms IS NOT NULL
            AND plan_fingerprint IS NULL AND failure_code IS NOT NULL AND draft_group_id IS NULL)
     OR (state = 'interrupted' AND authorized_at_ms IS NOT NULL AND claimed_at_ms IS NOT NULL AND finished_at_ms IS NOT NULL
            AND reply_text IS NULL AND plan_fingerprint IS NULL AND failure_code IS NULL AND draft_group_id IS NULL
            AND accounting_json IS NULL)
     OR (state IN ('stale','expired') AND claimed_at_ms IS NULL AND finished_at_ms IS NOT NULL
            AND reply_text IS NULL AND plan_fingerprint IS NULL AND failure_code IS NULL AND draft_group_id IS NULL
            AND accounting_json IS NULL)
     OR (state = 'cancelled' AND finished_at_ms IS NOT NULL
            AND reply_text IS NULL AND plan_fingerprint IS NULL AND failure_code IS NULL AND draft_group_id IS NULL)
     OR (state = 'discarded' AND finished_at_ms IS NOT NULL AND failure_code IS NULL AND draft_group_id IS NULL
            AND ((authorized_at_ms IS NULL AND claimed_at_ms IS NULL AND plan_fingerprint IS NULL AND reply_text IS NULL
                    AND accounting_json IS NULL)
              OR (authorized_at_ms IS NOT NULL AND claimed_at_ms IS NOT NULL AND plan_fingerprint IS NOT NULL
                    AND accounting_json IS NOT NULL)))
    )
);
-- At most ONE open request per source Run. A new quote replaces a `quoted` one (the repository discards it in the same
-- transaction); anything further along is never displaced.
CREATE UNIQUE INDEX IF NOT EXISTS follow_up_requests_one_open_per_run
    ON follow_up_plan_requests(source_run_id) WHERE state IN ('quoted','authorized','composing','plan_ready');
CREATE INDEX IF NOT EXISTS follow_up_requests_project_created_idx
    ON follow_up_plan_requests(project_id, created_at_ms DESC, id DESC);
CREATE INDEX IF NOT EXISTS follow_up_requests_state_created_idx
    ON follow_up_plan_requests(state, created_at_ms, id);
DROP TRIGGER IF EXISTS follow_up_requests_insert_quoted;
CREATE TRIGGER follow_up_requests_insert_quoted
BEFORE INSERT ON follow_up_plan_requests
WHEN NEW.state <> 'quoted'
BEGIN
    SELECT RAISE(ABORT, 'follow_up_request_must_start_quoted');
END;
DROP TRIGGER IF EXISTS follow_up_requests_identity_immutable;
CREATE TRIGGER follow_up_requests_identity_immutable
BEFORE UPDATE OF id, project_id, source_run_id, kind, goal, max_drafts, split_hint, outcome_source_state_fingerprint,
    snapshot_fingerprint, context_files_json, context_policy_id, context_fingerprint, provider_id, model_id, provider_kind,
    provider_base_url, provider_credential_reference, provider_identity_fingerprint, prompt_cache_mode,
    pricing_identity_json, pricing_identity_fingerprint, estimated_input_tokens, cacheable_input_tokens,
    effective_max_output_tokens, cap_micros, estimated_cost_micros, created_at_ms, quote_expires_at_ms
ON follow_up_plan_requests
WHEN NEW.id IS NOT OLD.id OR NEW.project_id IS NOT OLD.project_id OR NEW.source_run_id IS NOT OLD.source_run_id
  OR NEW.kind IS NOT OLD.kind OR NEW.goal IS NOT OLD.goal OR NEW.max_drafts IS NOT OLD.max_drafts
  OR NEW.split_hint IS NOT OLD.split_hint
  OR NEW.outcome_source_state_fingerprint IS NOT OLD.outcome_source_state_fingerprint
  OR NEW.snapshot_fingerprint IS NOT OLD.snapshot_fingerprint OR NEW.context_files_json IS NOT OLD.context_files_json
  OR NEW.context_policy_id IS NOT OLD.context_policy_id OR NEW.context_fingerprint IS NOT OLD.context_fingerprint
  OR NEW.provider_id IS NOT OLD.provider_id OR NEW.model_id IS NOT OLD.model_id
  OR NEW.provider_kind IS NOT OLD.provider_kind OR NEW.provider_base_url IS NOT OLD.provider_base_url
  OR NEW.provider_credential_reference IS NOT OLD.provider_credential_reference
  OR NEW.provider_identity_fingerprint IS NOT OLD.provider_identity_fingerprint
  OR NEW.prompt_cache_mode IS NOT OLD.prompt_cache_mode OR NEW.pricing_identity_json IS NOT OLD.pricing_identity_json
  OR NEW.pricing_identity_fingerprint IS NOT OLD.pricing_identity_fingerprint
  OR NEW.estimated_input_tokens IS NOT OLD.estimated_input_tokens
  OR NEW.cacheable_input_tokens IS NOT OLD.cacheable_input_tokens
  OR NEW.effective_max_output_tokens IS NOT OLD.effective_max_output_tokens OR NEW.cap_micros IS NOT OLD.cap_micros
  OR NEW.estimated_cost_micros IS NOT OLD.estimated_cost_micros OR NEW.created_at_ms IS NOT OLD.created_at_ms
  OR NEW.quote_expires_at_ms IS NOT OLD.quote_expires_at_ms
BEGIN
    SELECT RAISE(ABORT, 'follow_up_request_identity_immutable');
END;
DROP TRIGGER IF EXISTS follow_up_requests_result_write_once;
CREATE TRIGGER follow_up_requests_result_write_once
BEFORE UPDATE OF authorized_at_ms, claimed_at_ms, finished_at_ms, reply_text, plan_fingerprint, advisories_json,
    failure_code, failure_message, draft_group_id, accounting_json
ON follow_up_plan_requests
WHEN (OLD.authorized_at_ms IS NOT NULL AND NEW.authorized_at_ms IS NOT OLD.authorized_at_ms)
  OR (OLD.claimed_at_ms IS NOT NULL AND NEW.claimed_at_ms IS NOT OLD.claimed_at_ms)
  OR (OLD.finished_at_ms IS NOT NULL AND NEW.finished_at_ms IS NOT OLD.finished_at_ms)
  OR (OLD.reply_text IS NOT NULL AND NEW.reply_text IS NOT OLD.reply_text)
  OR (OLD.plan_fingerprint IS NOT NULL AND NEW.plan_fingerprint IS NOT OLD.plan_fingerprint)
  OR (OLD.advisories_json IS NOT NULL AND NEW.advisories_json IS NOT OLD.advisories_json)
  OR (OLD.failure_code IS NOT NULL AND NEW.failure_code IS NOT OLD.failure_code)
  OR (OLD.failure_message IS NOT NULL AND NEW.failure_message IS NOT OLD.failure_message)
  OR (OLD.draft_group_id IS NOT NULL AND NEW.draft_group_id IS NOT OLD.draft_group_id)
  OR (OLD.accounting_json IS NOT NULL AND NEW.accounting_json IS NOT OLD.accounting_json)
BEGIN
    SELECT RAISE(ABORT, 'follow_up_request_result_immutable');
END;
-- Every UPDATE is a state transition, and only on the edges of transitionFollowUpPlanRequest (the same 15 pairs).
DROP TRIGGER IF EXISTS follow_up_requests_transition;
CREATE TRIGGER follow_up_requests_transition
BEFORE UPDATE ON follow_up_plan_requests
WHEN NEW.state IS OLD.state
  OR NOT ((OLD.state = 'quoted' AND NEW.state IN ('authorized','stale','expired','cancelled','discarded'))
       OR (OLD.state = 'authorized' AND NEW.state IN ('composing','stale','expired','cancelled'))
       OR (OLD.state = 'composing' AND NEW.state IN ('plan_ready','failed','cancelled','interrupted'))
       OR (OLD.state = 'plan_ready' AND NEW.state IN ('drafts_saved','discarded')))
BEGIN
    SELECT RAISE(ABORT, 'follow_up_request_invalid_transition');
END;
DROP TRIGGER IF EXISTS follow_up_requests_reject_delete;
CREATE TRIGGER follow_up_requests_reject_delete
BEFORE DELETE ON follow_up_plan_requests
BEGIN
    SELECT RAISE(ABORT, 'follow_up_request_immutable');
END;
)sql";
}

// A request becomes drafts_saved only together with a group that names it. It reads run_draft_groups, so step 66 creates it AFTER
// the rebuild of that table (it must not exist while the table is renamed).
std::string followUpConfirmNeedsGroupTriggerSql()
{
    return R"sql(
-- A request becomes drafts_saved only together with a group that names it. (Created here, after the rebuild: it reads
-- run_draft_groups, so it must not exist while that table is renamed.)
DROP TRIGGER IF EXISTS follow_up_requests_confirm_needs_group;
CREATE TRIGGER follow_up_requests_confirm_needs_group
BEFORE UPDATE OF state ON follow_up_plan_requests
WHEN NEW.state = 'drafts_saved'
 AND NOT EXISTS(SELECT 1 FROM run_draft_groups
                WHERE id = NEW.draft_group_id AND source_request_id = NEW.id AND project_id = NEW.project_id)
BEGIN
    SELECT RAISE(ABORT, 'follow_up_request_confirm_needs_group');
END;
)sql";
}

common::Error error(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

common::Error databaseError(sqlite3* handle, std::string code, std::string_view operation)
{
    std::string message(operation);
    message += ": ";
    message += handle != nullptr ? sqlite3_errmsg(handle) : "SQLite handle is unavailable";
    return error(std::move(code), std::move(message));
}

// sqlite3_stmt has one owner and must be finalized on every early return,
// including the many early-return branches these repository methods take.
class Statement final {
public:
    Statement() = default;
    ~Statement()
    {
        if (statement_ != nullptr) {
            sqlite3_finalize(statement_);
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt** output() noexcept { return &statement_; }
    sqlite3_stmt* get() const noexcept { return statement_; }

private:
    sqlite3_stmt* statement_ {nullptr};
};

common::Result<void> execute(sqlite3* handle, const char* sql, std::string code)
{
    char* rawMessage = nullptr;
    const int result = sqlite3_exec(handle, sql, nullptr, nullptr, &rawMessage);
    if (result == SQLITE_OK) {
        return common::Result<void>::ok();
    }

    std::string message = rawMessage != nullptr ? rawMessage : sqlite3_errmsg(handle);
    sqlite3_free(rawMessage);
    return common::Result<void>::failure(error(std::move(code), std::move(message)));
}

common::Result<void> prepare(sqlite3* handle, const char* sql, Statement& statement)
{
    if (sqlite3_prepare_v2(handle, sql, -1, statement.output(), nullptr) != SQLITE_OK) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.prepare_failed", "Could not prepare SQLite statement"));
    }
    return common::Result<void>::ok();
}

common::Result<void> bindText(sqlite3* handle, sqlite3_stmt* statement, int index, const std::string& value)
{
    if (sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT)
        != SQLITE_OK) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.bind_failed", "Could not bind text value"));
    }
    return common::Result<void>::ok();
}

common::Result<void> bindNullableText(
    sqlite3* handle,
    sqlite3_stmt* statement,
    int index,
    const std::optional<std::string>& value)
{
    if (value.has_value()) {
        return bindText(handle, statement, index, *value);
    }
    if (sqlite3_bind_null(statement, index) != SQLITE_OK) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.bind_failed", "Could not bind null text value"));
    }
    return common::Result<void>::ok();
}

common::Result<void> bindInt64(sqlite3* handle, sqlite3_stmt* statement, int index, std::int64_t value)
{
    if (sqlite3_bind_int64(statement, index, value) != SQLITE_OK) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.bind_failed", "Could not bind integer value"));
    }
    return common::Result<void>::ok();
}

common::Result<void> bindUnsigned(sqlite3* handle, sqlite3_stmt* statement, int index, std::uint64_t value)
{
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return common::Result<void>::failure(
            error("persistence.value_out_of_range", "Unsigned value exceeds SQLite INTEGER range"));
    }
    return bindInt64(handle, statement, index, static_cast<std::int64_t>(value));
}

std::int64_t toMilliseconds(std::chrono::system_clock::time_point value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

std::chrono::system_clock::time_point fromMilliseconds(std::int64_t value)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(value));
}

common::Result<void> bindOptionalTime(
    sqlite3* handle,
    sqlite3_stmt* statement,
    int index,
    const std::optional<std::chrono::system_clock::time_point>& value)
{
    if (value.has_value()) {
        return bindInt64(handle, statement, index, toMilliseconds(*value));
    }
    if (sqlite3_bind_null(statement, index) != SQLITE_OK) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.bind_failed", "Could not bind null timestamp"));
    }
    return common::Result<void>::ok();
}

std::optional<std::chrono::system_clock::time_point> optionalTime(sqlite3_stmt* statement, int column)
{
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    return fromMilliseconds(sqlite3_column_int64(statement, column));
}

std::string columnText(sqlite3_stmt* statement, int column)
{
    const auto* value = sqlite3_column_text(statement, column);
    if (value == nullptr) {
        return {};
    }
    return reinterpret_cast<const char*>(value);
}

std::optional<std::string> optionalText(sqlite3_stmt* statement, int column)
{
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    return columnText(statement, column);
}

// std::filesystem::path has no portable narrow encoding across platforms
// (native() is UTF-16 on Windows). Persist an explicit UTF-8 encoding so the
// database file itself is portable and non-ASCII project paths round-trip.
std::string pathToUtf8(const std::filesystem::path& path)
{
    const std::u8string value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::filesystem::path pathFromUtf8(const std::string& value)
{
    const auto* begin = reinterpret_cast<const char8_t*>(value.data());
    return std::filesystem::path(std::u8string(begin, begin + value.size()));
}

nlohmann::json executionManifestToJson(
    const application::ExecutionManifest& manifest)
{
    nlohmann::json tasks = nlohmann::json::array();
    for (const auto& task : manifest.tasks) {
        const auto pathsToJson = [](const std::vector<std::filesystem::path>& paths) {
            nlohmann::json array = nlohmann::json::array();
            for (const auto& path : paths) {
                array.push_back(pathToUtf8(path));
            }
            return array;
        };
        const auto excludedOrDeferredToJson =
            [](const std::vector<application::ManifestExcludedOrDeferredPath>& paths) {
                nlohmann::json array = nlohmann::json::array();
                for (const auto& entry : paths) {
                    array.push_back({{"path", pathToUtf8(entry.path)}, {"reason", entry.reason}});
                }
                return array;
            };
        nlohmann::json taskJson = {
            {"id", task.id},
            {"objective", task.objective},
            {"role", task.role},
            {"dependencies", task.dependencies},
            {"ownedPaths", pathsToJson(task.ownedPaths)},
            // AG-15: previously only ownedPaths round-tripped through this
            // repository - a real Run (bb56e001-eac0-4b59-8d0b-d037c6bc1031)
            // froze a RunWorkspaceScope with real planned_create entries
            // (proving the in-memory manifest had them at freeze time), yet
            // the reloaded manifest showed none, because these six fields
            // were never written to manifest_json at all. Every task using
            // AG-15 roles silently lost them on the very first persist.
            {"plannedCreatePaths", pathsToJson(task.plannedCreatePaths)},
            {"requiredReadPaths", pathsToJson(task.requiredReadPaths)},
            {"referencePaths", pathsToJson(task.referencePaths)},
            {"excludedPaths", excludedOrDeferredToJson(task.excludedPaths)},
            {"deferredPaths", excludedOrDeferredToJson(task.deferredPaths)},
            {"baselineOnlyReadPaths", pathsToJson(task.baselineOnlyReadPaths)},
            {"acceptanceCriteria", task.acceptanceCriteria},
            {"contextReferences", task.contextReferences},
            {"requestedCapability", application::toString(task.requestedCapability)},
            // AG-17.1: absent (null) = the single worker of a single-assignment Manifest.
            {"assigneeMemberId", task.assigneeMemberId.has_value()
                ? nlohmann::json(*task.assigneeMemberId) : nlohmann::json(nullptr)}
        };
        // Previously omitted entirely: a frozen manifest's structured
        // acceptanceContract never round-tripped through this repository, so
        // every reload (a resumed Run, a restarted app, or combined
        // verification re-reading the frozen manifest) silently lost every
        // task's acceptance requirements - the exact condition that produced
        // a real verification.invalid_evidence_reference failure with no
        // recoverable diagnostic, since the contract that verification
        // needed was already gone from what got persisted.
        if (task.acceptanceContract.has_value()) {
            nlohmann::json requirements = nlohmann::json::array();
            for (const auto& requirement : task.acceptanceContract->requirements) {
                nlohmann::json arguments = nlohmann::json::array();
                for (const auto& argument : requirement.verificationArguments) {
                    arguments.push_back({
                        {"name", argument.name}, {"value", argument.value}});
                }
                // AG-18.2: the Backend-resolved selection identity that
                // produced verificationArguments above - persisted so a
                // restarted process reads back the same immutable binding
                // rather than ever re-resolving it. Absent (null) for
                // inspect_paths and for any pre-AG-18.2 frozen manifest.
                const auto optionalSelection =
                    [](const std::optional<std::string>& value) {
                        return value.has_value()
                            ? nlohmann::json(*value) : nlohmann::json(nullptr);
                    };
                requirements.push_back({
                    {"id", requirement.id},
                    {"description", requirement.description},
                    {"requiredOutputs", requirement.requiredOutputs},
                    {"verificationKind", application::toString(requirement.verificationKind)},
                    {"verificationArguments", std::move(arguments)},
                    {"required", requirement.required},
                    {"verificationProfileId", optionalSelection(requirement.verificationProfileId)},
                    {"verificationBuildProfileId",
                        optionalSelection(requirement.verificationBuildProfileId)},
                    {"verificationTargetId", optionalSelection(requirement.verificationTargetId)},
                    {"qmlLintDiagnosticPolicyId",
                        optionalSelection(requirement.qmlLintDiagnosticPolicyId)},
                    // AG-17.0: absent in a pre-AG-17.0 row = planner_declared.
                    {"origin", application::toString(requirement.origin)}
                });
            }
            taskJson["acceptanceContract"] = {
                {"schemaVersion", task.acceptanceContract->schemaVersion},
                {"requirements", std::move(requirements)}
            };
        } else {
            taskJson["acceptanceContract"] = nullptr;
        }
        tasks.push_back(std::move(taskJson));
    }

    const auto optionalUnsigned = [](const std::optional<std::uint64_t>& value) {
        return value.has_value() ? nlohmann::json(*value) : nlohmann::json(nullptr);
    };
    // AG-17.0: the frozen mandatory-verification decision record (null when
    // the policy was not evaluated, e.g. a pre-AG-17.0 Manifest).
    nlohmann::json mandatoryVerification = nullptr;
    if (manifest.mandatoryVerification.has_value()) {
        nlohmann::json decisions = nlohmann::json::array();
        for (const auto& decision : manifest.mandatoryVerification->decisions) {
            decisions.push_back({
                {"kind", application::toString(decision.kind)},
                {"status", application::toString(decision.status)},
                {"reasonCode", decision.reasonCode},
                {"requirementId", decision.requirementId},
                {"taskId", decision.taskId},
                {"profileId", decision.profileId},
                {"buildProfileId", decision.buildProfileId},
                {"targetId", decision.targetId}
            });
        }
        mandatoryVerification = {
            {"policyId", manifest.mandatoryVerification->policyId},
            {"mode", manifest.mandatoryVerification->mode},
            {"decisions", std::move(decisions)}
        };
    }
    // AG-17.1: the frozen Backend-owned routing record (null when none).
    nlohmann::json routing = nullptr;
    if (manifest.routing.has_value()) {
        nlohmann::json members = nlohmann::json::array();
        for (const auto& member : manifest.routing->members) {
            members.push_back({
                {"memberId", member.memberId},
                {"providerId", member.providerId},
                {"modelId", member.modelId},
                {"role", member.role},
                {"leadArchitect", member.leadArchitect},
                {"workspaceAccess", member.workspaceAccess},
                {"instructionsHash", member.instructionsHash},
                {"position", member.position},
                {"timeoutMs", member.timeoutMs}
            });
        }
        routing = {
            {"schemaVersion", manifest.routing->schemaVersion},
            {"mode", application::toString(manifest.routing->mode)},
            {"members", std::move(members)},
            {"fingerprint", manifest.routing->fingerprint}
        };
    }
    return {
        {"schemaVersion", manifest.schemaVersion},
        {"runId", manifest.runId},
        {"tasks", std::move(tasks)},
        {"routing", std::move(routing)},
        {"mandatoryVerification", std::move(mandatoryVerification)},
        {"requestedMaxParallelism", manifest.requestedMaxParallelism},
        {"requestedTokenBudget", optionalUnsigned(manifest.requestedTokenBudget)},
        {"requestedCostMicros", optionalUnsigned(manifest.requestedCostMicros)},
        {"policy", {
            {"maxTasks", manifest.policy.maxTasks},
            {"maxDepth", manifest.policy.maxDepth},
            {"maxParallelism", manifest.policy.maxParallelism},
            {"maxOwnedPathsPerTask", manifest.policy.maxOwnedPathsPerTask},
            {"maxAcceptanceCriteriaPerTask", manifest.policy.maxAcceptanceCriteriaPerTask},
            {"maxContextReferencesPerTask", manifest.policy.maxContextReferencesPerTask},
            {"maximumCapability", application::toString(manifest.policy.maximumCapability)},
            {"tokenBudgetCeiling", optionalUnsigned(manifest.policy.tokenBudgetCeiling)},
            {"costMicrosCeiling", optionalUnsigned(manifest.policy.costMicrosCeiling)}
        }}
    };
}

common::Result<application::ExecutionManifest> executionManifestFromJson(
    const std::string& raw,
    const std::string& storedSchemaVersion,
    std::chrono::system_clock::time_point createdAt)
{
    try {
        const auto document = nlohmann::json::parse(raw);
        application::ExecutionManifest manifest;
        manifest.schemaVersion = document.at("schemaVersion").get<std::string>();
        manifest.runId = document.at("runId").get<std::string>();
        manifest.requestedMaxParallelism =
            document.at("requestedMaxParallelism").get<std::uint32_t>();
        if (!document.at("requestedTokenBudget").is_null())
            manifest.requestedTokenBudget =
                document.at("requestedTokenBudget").get<std::uint64_t>();
        if (!document.at("requestedCostMicros").is_null())
            manifest.requestedCostMicros =
                document.at("requestedCostMicros").get<std::uint64_t>();

        const auto& policy = document.at("policy");
        manifest.policy.maxTasks = policy.at("maxTasks").get<std::uint32_t>();
        manifest.policy.maxDepth = policy.at("maxDepth").get<std::uint32_t>();
        manifest.policy.maxParallelism = policy.at("maxParallelism").get<std::uint32_t>();
        manifest.policy.maxOwnedPathsPerTask =
            policy.at("maxOwnedPathsPerTask").get<std::uint32_t>();
        manifest.policy.maxAcceptanceCriteriaPerTask =
            policy.at("maxAcceptanceCriteriaPerTask").get<std::uint32_t>();
        manifest.policy.maxContextReferencesPerTask =
            policy.at("maxContextReferencesPerTask").get<std::uint32_t>();
        const auto maximumCapability = application::workspaceCapabilityFromString(
            policy.at("maximumCapability").get<std::string>());
        if (!maximumCapability.has_value())
            throw std::invalid_argument("invalid maximumCapability");
        manifest.policy.maximumCapability = *maximumCapability;
        if (!policy.at("tokenBudgetCeiling").is_null())
            manifest.policy.tokenBudgetCeiling =
                policy.at("tokenBudgetCeiling").get<std::uint64_t>();
        if (!policy.at("costMicrosCeiling").is_null())
            manifest.policy.costMicrosCeiling =
                policy.at("costMicrosCeiling").get<std::uint64_t>();

        for (const auto& taskJson : document.at("tasks")) {
            application::ExecutionManifestTask task;
            task.id = taskJson.at("id").get<std::string>();
            task.objective = taskJson.at("objective").get<std::string>();
            task.role = taskJson.at("role").get<std::string>();
            task.dependencies = taskJson.at("dependencies").get<std::vector<std::string>>();
            for (const auto& path : taskJson.at("ownedPaths"))
                task.ownedPaths.push_back(pathFromUtf8(path.get<std::string>()));
            // AG-15: absent entirely for a manifest persisted before this
            // fix (or any other pre-AG-15 row) - backward compatible,
            // defaults to empty exactly as an unpopulated field would.
            const auto readPaths = [&taskJson](const char* key) {
                std::vector<std::filesystem::path> paths;
                if (taskJson.contains(key)) {
                    for (const auto& path : taskJson.at(key))
                        paths.push_back(pathFromUtf8(path.get<std::string>()));
                }
                return paths;
            };
            task.plannedCreatePaths = readPaths("plannedCreatePaths");
            task.requiredReadPaths = readPaths("requiredReadPaths");
            task.referencePaths = readPaths("referencePaths");
            task.baselineOnlyReadPaths = readPaths("baselineOnlyReadPaths");
            const auto readExcludedOrDeferred = [&taskJson](const char* key) {
                std::vector<application::ManifestExcludedOrDeferredPath> paths;
                if (taskJson.contains(key)) {
                    for (const auto& entryJson : taskJson.at(key)) {
                        application::ManifestExcludedOrDeferredPath entry;
                        entry.path = pathFromUtf8(entryJson.at("path").get<std::string>());
                        entry.reason = entryJson.at("reason").get<std::string>();
                        paths.push_back(std::move(entry));
                    }
                }
                return paths;
            };
            task.excludedPaths = readExcludedOrDeferred("excludedPaths");
            task.deferredPaths = readExcludedOrDeferred("deferredPaths");
            task.acceptanceCriteria =
                taskJson.at("acceptanceCriteria").get<std::vector<std::string>>();
            task.contextReferences =
                taskJson.at("contextReferences").get<std::vector<std::string>>();
            const auto capability = application::workspaceCapabilityFromString(
                taskJson.at("requestedCapability").get<std::string>());
            if (!capability.has_value())
                throw std::invalid_argument("invalid requestedCapability");
            task.requestedCapability = *capability;
            // AG-17.1: absent in a pre-AG-17.1 row; a present non-string is corrupt.
            if (taskJson.contains("assigneeMemberId")
                    && !taskJson.at("assigneeMemberId").is_null()) {
                task.assigneeMemberId = taskJson.at("assigneeMemberId").get<std::string>();
            }
            if (taskJson.contains("acceptanceContract")
                    && !taskJson.at("acceptanceContract").is_null()) {
                const auto& contractJson = taskJson.at("acceptanceContract");
                application::ExecutionAcceptanceContract contract;
                contract.schemaVersion = contractJson.at("schemaVersion").get<std::string>();
                for (const auto& reqJson : contractJson.at("requirements")) {
                    application::ExecutionAcceptanceRequirement requirement;
                    requirement.id = reqJson.at("id").get<std::string>();
                    requirement.description = reqJson.at("description").get<std::string>();
                    requirement.requiredOutputs =
                        reqJson.at("requiredOutputs").get<std::vector<std::string>>();
                    const auto kind = application::executionVerificationKindFromString(
                        reqJson.at("verificationKind").get<std::string>());
                    if (!kind.has_value())
                        throw std::invalid_argument("invalid verificationKind");
                    requirement.verificationKind = *kind;
                    for (const auto& argJson : reqJson.at("verificationArguments")) {
                        requirement.verificationArguments.push_back({
                            argJson.at("name").get<std::string>(),
                            argJson.at("value").get<std::string>()});
                    }
                    requirement.required = reqJson.at("required").get<bool>();
                    // AG-18.2: absent entirely for a pre-AG-18.2 persisted
                    // manifest (backward compatible - defaults to nullopt,
                    // exactly as a fresh inspect_paths requirement would),
                    // present-but-null for inspect_paths itself.
                    const auto readOptionalSelection =
                        [&reqJson](const char* key) -> std::optional<std::string> {
                            if (!reqJson.contains(key) || reqJson.at(key).is_null())
                                return std::nullopt;
                            return reqJson.at(key).get<std::string>();
                        };
                    requirement.verificationProfileId =
                        readOptionalSelection("verificationProfileId");
                    requirement.verificationBuildProfileId =
                        readOptionalSelection("verificationBuildProfileId");
                    requirement.verificationTargetId =
                        readOptionalSelection("verificationTargetId");
                    requirement.qmlLintDiagnosticPolicyId =
                        readOptionalSelection("qmlLintDiagnosticPolicyId");
                    // AG-17.0: absent entirely for a pre-AG-17.0 manifest =
                    // planner_declared; a present but unknown value is corrupt.
                    if (reqJson.contains("origin")) {
                        const auto origin = application::executionRequirementOriginFromString(
                            reqJson.at("origin").get<std::string>());
                        if (!origin.has_value())
                            throw std::invalid_argument("invalid requirement origin");
                        requirement.origin = *origin;
                    }
                    contract.requirements.push_back(std::move(requirement));
                }
                task.acceptanceContract = std::move(contract);
            }
            manifest.tasks.push_back(std::move(task));
        }
        if (manifest.schemaVersion != storedSchemaVersion)
            throw std::invalid_argument("schema version mismatch");
        // AG-17.1: absent/null for a Manifest frozen before routing existed.
        if (document.contains("routing") && !document.at("routing").is_null()) {
            const auto& routingJson = document.at("routing");
            application::ManifestRoutingRecord routing;
            routing.schemaVersion = routingJson.at("schemaVersion").get<std::string>();
            const auto mode = application::manifestRoutingModeFromString(
                routingJson.at("mode").get<std::string>());
            if (!mode.has_value()) throw std::invalid_argument("invalid routing mode");
            routing.mode = *mode;
            for (const auto& memberJson : routingJson.at("members")) {
                application::ManifestRoutingMember member;
                member.memberId = memberJson.at("memberId").get<std::string>();
                member.providerId = memberJson.at("providerId").get<std::string>();
                member.modelId = memberJson.at("modelId").get<std::string>();
                member.role = memberJson.at("role").get<std::string>();
                member.leadArchitect = memberJson.at("leadArchitect").get<bool>();
                member.workspaceAccess = memberJson.at("workspaceAccess").get<bool>();
                member.instructionsHash = memberJson.at("instructionsHash").get<std::string>();
                member.position = memberJson.at("position").get<std::uint32_t>();
                member.timeoutMs = memberJson.at("timeoutMs").get<std::uint64_t>();
                routing.members.push_back(std::move(member));
            }
            routing.fingerprint = routingJson.at("fingerprint").get<std::string>();
            manifest.routing = std::move(routing);
        }
        // AG-17.0: absent/null for a Manifest frozen before the policy existed.
        if (document.contains("mandatoryVerification")
                && !document.at("mandatoryVerification").is_null()) {
            const auto& recordJson = document.at("mandatoryVerification");
            application::ManifestMandatoryVerificationRecord record;
            record.policyId = recordJson.at("policyId").get<std::string>();
            record.mode = recordJson.at("mode").get<std::string>();
            for (const auto& decisionJson : recordJson.at("decisions")) {
                application::ManifestMandatoryVerificationDecision decision;
                const auto kind = application::executionVerificationKindFromString(
                    decisionJson.at("kind").get<std::string>());
                const auto status = application::mandatoryVerificationDecisionStatusFromString(
                    decisionJson.at("status").get<std::string>());
                if (!kind.has_value() || !status.has_value())
                    throw std::invalid_argument("invalid mandatory verification decision");
                decision.kind = *kind;
                decision.status = *status;
                decision.reasonCode = decisionJson.at("reasonCode").get<std::string>();
                decision.requirementId = decisionJson.at("requirementId").get<std::string>();
                decision.taskId = decisionJson.at("taskId").get<std::string>();
                decision.profileId = decisionJson.at("profileId").get<std::string>();
                decision.buildProfileId = decisionJson.at("buildProfileId").get<std::string>();
                decision.targetId = decisionJson.at("targetId").get<std::string>();
                record.decisions.push_back(std::move(decision));
            }
            manifest.mandatoryVerification = std::move(record);
        }
        manifest.createdAt = createdAt;
        return common::Result<application::ExecutionManifest>::ok(std::move(manifest));
    } catch (const std::exception& exception) {
        return common::Result<application::ExecutionManifest>::failure(error(
            "manifest.corrupt_snapshot",
            std::string("Stored execution manifest is invalid: ") + exception.what()));
    }
}

std::optional<ProjectStatus> projectStatusFromString(std::string_view value)
{
    if (value == "active") return ProjectStatus::Active;
    if (value == "archived") return ProjectStatus::Archived;
    if (value == "unavailable") return ProjectStatus::Unavailable;
    return std::nullopt;
}

std::optional<RunStatus> runStatusFromString(std::string_view value)
{
    if (value == "prepared") return RunStatus::Prepared;
    if (value == "queued") return RunStatus::Queued;
    if (value == "running") return RunStatus::Running;
    if (value == "cancelling") return RunStatus::Cancelling;
    if (value == "completed") return RunStatus::Completed;
    if (value == "failed") return RunStatus::Failed;
    if (value == "cancelled") return RunStatus::Cancelled;
    if (value == "interrupted") return RunStatus::Interrupted;
    return std::nullopt;
}

std::optional<StageStatus> stageStatusFromString(std::string_view value)
{
    if (value == "pending") return StageStatus::Pending;
    if (value == "running") return StageStatus::Running;
    if (value == "completed") return StageStatus::Completed;
    if (value == "failed") return StageStatus::Failed;
    if (value == "skipped") return StageStatus::Skipped;
    if (value == "cancelled") return StageStatus::Cancelled;
    return std::nullopt;
}

std::optional<TaskStatus> taskStatusFromString(std::string_view value)
{
    if (value == "backlog") return TaskStatus::Backlog;
    if (value == "planned") return TaskStatus::Planned;
    if (value == "in_progress") return TaskStatus::InProgress;
    if (value == "blocked") return TaskStatus::Blocked;
    if (value == "done") return TaskStatus::Done;
    if (value == "cancelled") return TaskStatus::Cancelled;
    return std::nullopt;
}

std::optional<TaskPriority> taskPriorityFromString(std::string_view value)
{
    if (value == "low") return TaskPriority::Low;
    if (value == "normal") return TaskPriority::Normal;
    if (value == "high") return TaskPriority::High;
    if (value == "critical") return TaskPriority::Critical;
    return std::nullopt;
}

common::Result<Project> readProject(sqlite3_stmt* statement)
{
    const auto status = projectStatusFromString(columnText(statement, 4));
    if (!status.has_value()) {
        return common::Result<Project>::failure(
            error("persistence.invalid_data", "Project row contains an unknown status"));
    }

    Project project;
    project.id = columnText(statement, 0);
    project.name = columnText(statement, 1);
    project.description = columnText(statement, 2);
    project.rootPath = pathFromUtf8(columnText(statement, 3));
    project.status = *status;
    project.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 5));
    project.updatedAt = fromMilliseconds(sqlite3_column_int64(statement, 6));
    return common::Result<Project>::ok(std::move(project));
}

common::Result<ProjectTask> readTask(sqlite3_stmt* statement)
{
    const auto status = taskStatusFromString(columnText(statement, 4));
    const auto priority = taskPriorityFromString(columnText(statement, 5));
    if (!status.has_value() || !priority.has_value()) {
        return common::Result<ProjectTask>::failure(
            error("persistence.invalid_data", "Task row contains an unknown status or priority"));
    }
    ProjectTask task;
    task.id = columnText(statement, 0);
    task.projectId = columnText(statement, 1);
    task.title = columnText(statement, 2);
    task.description = columnText(statement, 3);
    task.status = *status;
    task.priority = *priority;
    task.requiresAttention = sqlite3_column_int(statement, 6) != 0;
    task.dueAt = optionalTime(statement, 7);
    task.plannedRunAt = optionalTime(statement, 8);
    task.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 9));
    task.updatedAt = fromMilliseconds(sqlite3_column_int64(statement, 10));
    return common::Result<ProjectTask>::ok(std::move(task));
}

std::string providerBudgetPolicyJson(const ProviderExecutionBudgetPolicy& value)
{
    return nlohmann::json {
        {"schemaVersion", value.schemaVersion},
        {"maxInputTokens", value.maxInputTokens},
        {"maxOutputTokens", value.maxOutputTokens},
        {"maxTurns", value.maxTurns},
        {"maxToolCalls", value.maxToolCalls},
        {"maxToolResultBytes", value.maxToolResultBytes},
        {"maxWallTimeMs", value.maxWallTimeMs},
        {"maxRequestTimeMs", value.maxRequestTimeMs},
        {"autoExtendTurns", value.autoExtendTurns},
    }.dump();
}

common::Result<ProviderExecutionBudgetPolicy> parseProviderBudgetPolicy(
    std::string_view raw)
{
    try {
        const auto document = nlohmann::json::parse(raw);
        if (!document.is_object()) throw std::runtime_error("not an object");
        ProviderExecutionBudgetPolicy value;
        if (!document.empty()) {
            static const std::array<const char*, 8> fields {
                "schemaVersion", "maxInputTokens", "maxOutputTokens", "maxTurns",
                "maxToolCalls", "maxToolResultBytes", "maxWallTimeMs", "maxRequestTimeMs"};
            // autoExtendTurns was added after rows already existed without it, and a frozen
            // policy cannot say whether its limit was typed or defaulted. A row frozen with
            // exactly the Backend default turn limit is read as automatic (so Retry / Edit &
            // rerun of such a Run is not doomed to repeat a shortfall the Backend now covers);
            // any other value was chosen by somebody and stays pinned.
            const bool hasAutoExtend = document.contains("autoExtendTurns");
            if (document.size() != fields.size() + (hasAutoExtend ? 1 : 0)) {
                throw std::runtime_error("field count");
            }
            for (const auto* field : fields) {
                if (!document.contains(field)) throw std::runtime_error("missing field");
            }
            value.autoExtendTurns = hasAutoExtend
                ? document.at("autoExtendTurns").get<bool>()
                : document.at("maxTurns").get<std::uint64_t>()
                    == ProviderExecutionBudgetPolicy {}.maxTurns;
            value.schemaVersion = document.at("schemaVersion").get<std::string>();
            value.maxInputTokens = document.at("maxInputTokens").get<std::uint64_t>();
            value.maxOutputTokens = document.at("maxOutputTokens").get<std::uint64_t>();
            value.maxTurns = document.at("maxTurns").get<std::uint64_t>();
            value.maxToolCalls = document.at("maxToolCalls").get<std::uint64_t>();
            value.maxToolResultBytes = document.at("maxToolResultBytes").get<std::uint64_t>();
            value.maxWallTimeMs = document.at("maxWallTimeMs").get<std::uint64_t>();
            value.maxRequestTimeMs = document.at("maxRequestTimeMs").get<std::uint64_t>();
        } else {
            // '{}' - a Run that carries no budget of its own: the Backend defaults, automatic.
            value.autoExtendTurns = true;
        }
        const auto valid = validateProviderExecutionBudgetPolicy(value);
        if (!valid.success()) {
            return common::Result<ProviderExecutionBudgetPolicy>::failure(*valid.error());
        }
        return common::Result<ProviderExecutionBudgetPolicy>::ok(std::move(value));
    } catch (...) {
        return common::Result<ProviderExecutionBudgetPolicy>::failure(error(
            "persistence.invalid_data", "Run row contains a corrupt provider budget policy"));
    }
}

std::string providerBudgetOverridesJson(const ProviderExecutionBudgetOverrides& value)
{
    nlohmann::json document = nlohmann::json::object();
    const auto put = [&](const char* name, const std::optional<std::uint64_t>& field) {
        if (field) document[name] = *field;
    };
    put("maxInputTokens", value.maxInputTokens);
    put("maxOutputTokens", value.maxOutputTokens);
    put("maxTurns", value.maxTurns);
    put("maxToolCalls", value.maxToolCalls);
    put("maxToolResultBytes", value.maxToolResultBytes);
    put("maxWallTimeMs", value.maxWallTimeMs);
    put("maxRequestTimeMs", value.maxRequestTimeMs);
    return document.dump();
}

common::Result<ProviderExecutionBudgetOverrides> parseProviderBudgetOverrides(
    std::string_view raw)
{
    try {
        const auto document = nlohmann::json::parse(raw);
        if (!document.is_object() || document.size() > 7)
            throw std::runtime_error("invalid object");
        ProviderExecutionBudgetOverrides value;
        const auto get = [&](const char* name, std::optional<std::uint64_t>& field) {
            if (document.contains(name)) field = document.at(name).get<std::uint64_t>();
        };
        get("maxInputTokens", value.maxInputTokens);
        get("maxOutputTokens", value.maxOutputTokens);
        get("maxTurns", value.maxTurns);
        get("maxToolCalls", value.maxToolCalls);
        get("maxToolResultBytes", value.maxToolResultBytes);
        get("maxWallTimeMs", value.maxWallTimeMs);
        get("maxRequestTimeMs", value.maxRequestTimeMs);
        const auto valid = resolveProviderExecutionBudget(value);
        if (!valid.success()) {
            return common::Result<ProviderExecutionBudgetOverrides>::failure(*valid.error());
        }
        return common::Result<ProviderExecutionBudgetOverrides>::ok(std::move(value));
    } catch (...) {
        return common::Result<ProviderExecutionBudgetOverrides>::failure(error(
            "persistence.invalid_data", "Execution profile contains corrupt provider budget limits"));
    }
}

std::string providerBudgetUsageJson(const ProviderExecutionBudgetUsage& value)
{
    nlohmann::json document {
        {"turnsStarted", value.turnsStarted},
        {"turnsCompleted", value.turnsCompleted},
        {"inputTokens", value.inputTokens},
        {"outputTokens", value.outputTokens},
        {"toolCalls", value.toolCalls},
        {"toolResultBytes", value.toolResultBytes},
        {"usageAuthoritative", value.usageAuthoritative},
        {"stopCode", value.stopCode},
        {"cacheReadInputTokens", value.cacheReadInputTokens},
        {"cacheCreationInputTokens", value.cacheCreationInputTokens},
        {"cacheCreationDurationPartitionKnown", value.cacheCreationDuration.known},
        {"cacheCreationFiveMinuteInputTokens", value.cacheCreationDuration.fiveMinuteTokens},
        {"cacheCreationOneHourInputTokens", value.cacheCreationDuration.oneHourTokens},
    };
    // Written only once the Backend actually raised the turn limit, so a Run
    // that never needed it keeps its exact historical shape.
    if (value.grantedMaxTurns > 0) document["grantedMaxTurns"] = value.grantedMaxTurns;
    return document.dump();
}

common::Result<void> validateProviderBudgetUsageForPersistence(
    const ProviderExecutionBudgetUsage& value)
{
    const auto durationProblem = checkCacheCreationDurationUsage(
        value.cacheCreationInputTokens, value.cacheCreationDuration);
    if (durationProblem != CacheCreationDurationUsageProblem::None) {
        return common::Result<void>::failure(error(
            cacheCreationDurationUsageProblemCode(durationProblem),
            cacheCreationDurationUsageProblemMessage(durationProblem)));
    }
    if (value.cacheCreationInputTokens > value.inputTokens) {
        return common::Result<void>::failure(error(
            "persistence.invalid_data", "Run contains invalid provider budget usage"));
    }
    return common::Result<void>::ok();
}

common::Result<ProviderExecutionBudgetUsage> parseProviderBudgetUsage(
    std::string_view raw)
{
    try {
        const auto document = nlohmann::json::parse(raw);
        if (!document.is_object()) throw std::runtime_error("not an object");
        ProviderExecutionBudgetUsage value;
        if (document.empty()) return common::Result<ProviderExecutionBudgetUsage>::ok(value);
        static const std::array<const char*, 8> requiredFields {
            "turnsStarted", "turnsCompleted", "inputTokens", "outputTokens",
            "toolCalls", "toolResultBytes", "usageAuthoritative", "stopCode"};
        // cacheReadInputTokens and cacheCreationInputTokens (498dd8c6/
        // af4e5703 cache-budget fixes, 2026-09-15) were each added after rows
        // already existed without them; accept any of the three shapes (0,
        // 1 or 2 of these present) rather than rejecting an older Run's
        // persisted usage as corrupt.
        static const std::array<const char*, 6> optionalFields {
            "cacheReadInputTokens", "cacheCreationInputTokens", "grantedMaxTurns",
            "cacheCreationDurationPartitionKnown", "cacheCreationFiveMinuteInputTokens",
            "cacheCreationOneHourInputTokens"};
        for (const auto* field : requiredFields) {
            if (!document.contains(field)) throw std::runtime_error("missing field");
        }
        std::size_t knownFieldCount = requiredFields.size();
        for (const auto* field : optionalFields) {
            if (document.contains(field)) ++knownFieldCount;
        }
        if (document.size() != knownFieldCount) throw std::runtime_error("field count");
        value.turnsStarted = document.at("turnsStarted").get<std::uint64_t>();
        value.turnsCompleted = document.at("turnsCompleted").get<std::uint64_t>();
        value.inputTokens = document.at("inputTokens").get<std::uint64_t>();
        value.outputTokens = document.at("outputTokens").get<std::uint64_t>();
        value.toolCalls = document.at("toolCalls").get<std::uint64_t>();
        value.toolResultBytes = document.at("toolResultBytes").get<std::uint64_t>();
        value.usageAuthoritative = document.at("usageAuthoritative").get<bool>();
        value.stopCode = document.at("stopCode").get<std::string>();
        value.cacheReadInputTokens = document.contains("cacheReadInputTokens")
            ? document.at("cacheReadInputTokens").get<std::uint64_t>()
            : static_cast<std::uint64_t>(0);
        value.cacheCreationInputTokens = document.contains("cacheCreationInputTokens")
            ? document.at("cacheCreationInputTokens").get<std::uint64_t>()
            : static_cast<std::uint64_t>(0);
        value.grantedMaxTurns = document.contains("grantedMaxTurns")
            ? document.at("grantedMaxTurns").get<std::uint64_t>()
            : static_cast<std::uint64_t>(0);
        const bool hasDurationKnown = document.contains("cacheCreationDurationPartitionKnown");
        const bool hasFiveMinute = document.contains("cacheCreationFiveMinuteInputTokens");
        const bool hasOneHour = document.contains("cacheCreationOneHourInputTokens");
        if (hasDurationKnown != hasFiveMinute || hasDurationKnown != hasOneHour) {
            throw std::runtime_error("partial cache duration usage");
        }
        value.cacheCreationDuration.known = hasDurationKnown
            ? document.at("cacheCreationDurationPartitionKnown").get<bool>() : false;
        value.cacheCreationDuration.fiveMinuteTokens = hasFiveMinute
            ? document.at("cacheCreationFiveMinuteInputTokens").get<std::uint64_t>() : 0;
        value.cacheCreationDuration.oneHourTokens = hasOneHour
            ? document.at("cacheCreationOneHourInputTokens").get<std::uint64_t>() : 0;
        if (value.turnsCompleted > value.turnsStarted) throw std::runtime_error("turn count");
        if (checkCacheCreationDurationUsage(
                value.cacheCreationInputTokens, value.cacheCreationDuration)
                != CacheCreationDurationUsageProblem::None
            || value.cacheCreationInputTokens > value.inputTokens) {
            throw std::runtime_error("cache duration usage");
        }
        return common::Result<ProviderExecutionBudgetUsage>::ok(std::move(value));
    } catch (...) {
        return common::Result<ProviderExecutionBudgetUsage>::failure(error(
            "persistence.invalid_data", "Run row contains corrupt provider budget usage"));
    }
}

common::Result<Run> readRun(sqlite3_stmt* statement)
{
    const auto status = runStatusFromString(columnText(statement, 6));
    if (!status.has_value()) {
        return common::Result<Run>::failure(
            error("persistence.invalid_data", "Run row contains an unknown status"));
    }
    const auto planningCompletionPolicy =
        application::planningCompletionPolicyFromString(columnText(statement, 19));
    if (!planningCompletionPolicy.has_value()) {
        return common::Result<Run>::failure(error(
            "persistence.invalid_data",
            "Run row contains an unknown planning completion policy"));
    }
    const auto verificationRepairPolicy =
        application::verificationRepairPolicyFromString(columnText(statement, 24));
    if (!verificationRepairPolicy.has_value()) {
        return common::Result<Run>::failure(error(
            "persistence.invalid_data",
            "Run row contains an unknown verification repair policy"));
    }

    const std::int64_t inputTokens = sqlite3_column_int64(statement, 10);
    const std::int64_t outputTokens = sqlite3_column_int64(statement, 11);
    const std::int64_t revision = sqlite3_column_int64(statement, 15);
    const std::int64_t maxParallelism = sqlite3_column_int64(statement, 17);
    const std::int64_t cacheCreationInputTokens = sqlite3_column_int64(statement, 22);
    const std::int64_t cacheReadInputTokens = sqlite3_column_int64(statement, 23);
    const std::int64_t cacheDurationKnown = sqlite3_column_int64(statement, 26);
    const std::int64_t cacheFiveMinuteTokens = sqlite3_column_int64(statement, 27);
    const std::int64_t cacheOneHourTokens = sqlite3_column_int64(statement, 28);
    if (inputTokens < 0 || outputTokens < 0 || revision < 0
        || maxParallelism <= 0 || maxParallelism > 32
        || cacheCreationInputTokens < 0 || cacheReadInputTokens < 0
        || (cacheDurationKnown != 0 && cacheDurationKnown != 1)
        || cacheFiveMinuteTokens < 0 || cacheOneHourTokens < 0) {
        return common::Result<Run>::failure(
            error("persistence.invalid_data", "Run row contains a negative unsigned value"));
    }

    Run run;
    run.id = columnText(statement, 0);
    run.projectId = columnText(statement, 1);
    run.parentRunId = optionalText(statement, 2);
    run.title = columnText(statement, 3);
    run.task = columnText(statement, 4);
    run.workflowId = columnText(statement, 5);
    run.status = *status;
    run.progress = sqlite3_column_double(statement, 7);
    run.currentStageId = optionalText(statement, 8);
    run.errorMessage = columnText(statement, 9);
    run.usage.inputTokens = static_cast<std::uint64_t>(inputTokens);
    run.usage.outputTokens = static_cast<std::uint64_t>(outputTokens);
    run.usage.cacheCreationInputTokens = static_cast<std::uint64_t>(cacheCreationInputTokens);
    run.usage.cacheReadInputTokens = static_cast<std::uint64_t>(cacheReadInputTokens);
    run.usage.cacheCreationDuration.known = cacheDurationKnown != 0;
    run.usage.cacheCreationDuration.fiveMinuteTokens =
        static_cast<std::uint64_t>(cacheFiveMinuteTokens);
    run.usage.cacheCreationDuration.oneHourTokens =
        static_cast<std::uint64_t>(cacheOneHourTokens);
    if (!run.usage.cacheBreakdownIsConsistent()
        || checkCacheCreationDurationUsage(
            run.usage.cacheCreationInputTokens,
            run.usage.cacheCreationDuration)
        != CacheCreationDurationUsageProblem::None) {
        return common::Result<Run>::failure(error(
            "persistence.invalid_data", "Run row contains inconsistent cache usage"));
    }
    run.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 12));
    run.startedAt = optionalTime(statement, 13);
    run.finishedAt = optionalTime(statement, 14);
    run.revision = static_cast<std::uint64_t>(revision);
    // assignmentSnapshot is intentionally not read here: it lives in the
    // separate run_stage_assignments table and is attached by the caller
    // (see fetchAssignmentsForRun) so this single-row parse never needs a
    // second query of its own.
    run.executionProfileId = optionalText(statement, 16);
    run.maxParallelism = static_cast<std::uint32_t>(maxParallelism);
    const auto workspaceText = optionalText(statement, 18);
    run.workspacePath = workspaceText.has_value()
        ? std::optional<std::filesystem::path>(pathFromUtf8(*workspaceText))
        : std::nullopt;
    run.planningCompletionPolicy = *planningCompletionPolicy;
    const auto providerBudget = parseProviderBudgetPolicy(columnText(statement, 20));
    if (!providerBudget.success()) return common::Result<Run>::failure(*providerBudget.error());
    const auto providerBudgetUsage = parseProviderBudgetUsage(columnText(statement, 21));
    if (!providerBudgetUsage.success())
        return common::Result<Run>::failure(*providerBudgetUsage.error());
    run.providerBudget = providerBudget.value();
    run.providerBudgetUsage = providerBudgetUsage.value();
    run.verificationRepairPolicy = *verificationRepairPolicy;
    // A damaged or non-canonical contracts column is reported, never repaired or dropped (PC-2).
    const auto interfaceContracts =
        application::parseRunInterfaceContracts(columnText(statement, 25));
    if (!interfaceContracts.success())
        return common::Result<Run>::failure(*interfaceContracts.error());
    run.interfaceContracts = interfaceContracts.value();
    return common::Result<Run>::ok(std::move(run));
}

common::Result<RunStage> readStage(sqlite3_stmt* statement)
{
    const auto status = stageStatusFromString(columnText(statement, 6));
    if (!status.has_value()) {
        return common::Result<RunStage>::failure(
            error("persistence.invalid_data", "Stage row contains an unknown status"));
    }

    const std::int64_t inputTokens = sqlite3_column_int64(statement, 10);
    const std::int64_t outputTokens = sqlite3_column_int64(statement, 11);
    if (inputTokens < 0 || outputTokens < 0) {
        return common::Result<RunStage>::failure(
            error("persistence.invalid_data", "Stage row contains a negative token count"));
    }

    RunStage stage;
    stage.id = columnText(statement, 0);
    stage.runId = columnText(statement, 1);
    stage.type = columnText(statement, 2);
    stage.title = columnText(statement, 3);
    stage.description = columnText(statement, 4);
    stage.status = *status;
    stage.progress = sqlite3_column_double(statement, 7);
    stage.providerId = columnText(statement, 8);
    stage.modelId = columnText(statement, 9);
    stage.usage.inputTokens = static_cast<std::uint64_t>(inputTokens);
    stage.usage.outputTokens = static_cast<std::uint64_t>(outputTokens);
    stage.errorMessage = columnText(statement, 12);
    stage.startedAt = optionalTime(statement, 13);
    stage.finishedAt = optionalTime(statement, 14);
    return common::Result<RunStage>::ok(std::move(stage));
}

// Column order for readArtifact/bindArtifact/artifactColumns below:
// id, run_id, stage_id, kind, name, media_type, relative_path, byte_size, sha256, created_at_ms
common::Result<Artifact> readArtifact(sqlite3_stmt* statement)
{
    const auto kind = application::artifactKindFromString(columnText(statement, 3));
    if (!kind.has_value()) {
        return common::Result<Artifact>::failure(
            error("persistence.invalid_data", "Artifact row contains an unknown kind"));
    }
    const std::int64_t byteSize = sqlite3_column_int64(statement, 7);
    if (byteSize < 0) {
        return common::Result<Artifact>::failure(
            error("persistence.invalid_data", "Artifact row contains a negative byte size"));
    }

    Artifact artifact;
    artifact.id = columnText(statement, 0);
    artifact.runId = columnText(statement, 1);
    artifact.stageId = optionalText(statement, 2);
    artifact.kind = *kind;
    artifact.name = columnText(statement, 4);
    artifact.mediaType = columnText(statement, 5);
    artifact.relativePath = pathFromUtf8(columnText(statement, 6));
    artifact.byteSize = static_cast<std::uint64_t>(byteSize);
    artifact.sha256 = columnText(statement, 8);
    artifact.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 9));
    return common::Result<Artifact>::ok(std::move(artifact));
}

common::Result<void> bindArtifact(sqlite3* handle, sqlite3_stmt* statement, const Artifact& artifact)
{
    const std::string relativePath = pathToUtf8(artifact.relativePath);
    const common::Result<void> bindings[] = {
        bindText(handle, statement, 1, artifact.id),
        bindText(handle, statement, 2, artifact.runId),
        bindNullableText(handle, statement, 3, artifact.stageId),
        bindText(handle, statement, 4, application::toString(artifact.kind)),
        bindText(handle, statement, 5, artifact.name),
        bindText(handle, statement, 6, artifact.mediaType),
        bindText(handle, statement, 7, relativePath),
        bindUnsigned(handle, statement, 8, artifact.byteSize),
        bindText(handle, statement, 9, artifact.sha256),
        bindInt64(handle, statement, 10, toMilliseconds(artifact.createdAt))};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    return common::Result<void>::ok();
}

common::Result<void> bindProject(sqlite3* handle, sqlite3_stmt* statement, const Project& project)
{
    const std::string path = pathToUtf8(project.rootPath);
    const common::Result<void> bindings[] = {
        bindText(handle, statement, 1, project.id),
        bindText(handle, statement, 2, project.name),
        bindText(handle, statement, 3, project.description),
        bindText(handle, statement, 4, path),
        bindText(handle, statement, 5, application::toString(project.status)),
        bindInt64(handle, statement, 6, toMilliseconds(project.createdAt)),
        bindInt64(handle, statement, 7, toMilliseconds(project.updatedAt))};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    return common::Result<void>::ok();
}

common::Result<void> bindRunFields(sqlite3* handle, sqlite3_stmt* statement, const Run& run)
{
    const common::Result<void> bindings[] = {
        bindText(handle, statement, 1, run.id),
        bindText(handle, statement, 2, run.projectId),
        bindNullableText(handle, statement, 3, run.parentRunId),
        bindText(handle, statement, 4, run.title),
        bindText(handle, statement, 5, run.task),
        bindText(handle, statement, 6, run.workflowId),
        bindText(handle, statement, 7, application::toString(run.status)),
        bindInt64(handle, statement, 8, 0),
        bindNullableText(handle, statement, 9, run.currentStageId),
        bindText(handle, statement, 10, run.errorMessage),
        bindUnsigned(handle, statement, 11, run.usage.inputTokens),
        bindUnsigned(handle, statement, 12, run.usage.outputTokens),
        bindInt64(handle, statement, 13, toMilliseconds(run.createdAt)),
        bindOptionalTime(handle, statement, 14, run.startedAt),
        bindOptionalTime(handle, statement, 15, run.finishedAt),
        bindUnsigned(handle, statement, 16, run.revision),
        bindNullableText(handle, statement, 17, run.executionProfileId),
        bindUnsigned(handle, statement, 18, run.maxParallelism),
        bindNullableText(handle, statement, 19,
            run.workspacePath.has_value()
                ? std::optional<std::string>(pathToUtf8(*run.workspacePath))
                : std::nullopt),
        bindText(handle, statement, 20,
            application::toString(run.planningCompletionPolicy)),
        bindText(handle, statement, 21, providerBudgetPolicyJson(run.providerBudget)),
        bindText(handle, statement, 22, providerBudgetUsageJson(run.providerBudgetUsage)),
        bindUnsigned(handle, statement, 23, run.usage.cacheCreationInputTokens),
        bindUnsigned(handle, statement, 24, run.usage.cacheReadInputTokens),
        bindText(handle, statement, 25,
            application::toString(run.verificationRepairPolicy)),
        bindText(handle, statement, 26,
            application::serializeRunInterfaceContracts(run.interfaceContracts)),
        bindInt64(handle, statement, 27, run.usage.cacheCreationDuration.known ? 1 : 0),
        bindUnsigned(handle, statement, 28,
            run.usage.cacheCreationDuration.fiveMinuteTokens),
        bindUnsigned(handle, statement, 29,
            run.usage.cacheCreationDuration.oneHourTokens)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_bind_double(statement, 8, run.progress) != SQLITE_OK) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.bind_failed", "Could not bind run progress"));
    }
    return common::Result<void>::ok();
}

common::Result<void> bindStageFields(
    sqlite3* handle,
    sqlite3_stmt* statement,
    const RunStage& stage,
    std::optional<std::int64_t> position)
{
    const common::Result<void> bindings[] = {
        bindText(handle, statement, 1, stage.id),
        bindText(handle, statement, 2, stage.runId),
        bindText(handle, statement, 3, stage.type),
        bindText(handle, statement, 4, stage.title),
        bindText(handle, statement, 5, stage.description),
        bindText(handle, statement, 6, application::toString(stage.status)),
        bindInt64(handle, statement, 7, 0),
        bindText(handle, statement, 8, stage.providerId),
        bindText(handle, statement, 9, stage.modelId),
        bindUnsigned(handle, statement, 10, stage.usage.inputTokens),
        bindUnsigned(handle, statement, 11, stage.usage.outputTokens),
        bindText(handle, statement, 12, stage.errorMessage),
        bindOptionalTime(handle, statement, 13, stage.startedAt),
        bindOptionalTime(handle, statement, 14, stage.finishedAt)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_bind_double(statement, 7, stage.progress) != SQLITE_OK) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.bind_failed", "Could not bind stage progress"));
    }
    if (position.has_value()) {
        return bindInt64(handle, statement, 15, *position);
    }
    return common::Result<void>::ok();
}

common::Result<void> validateProgress(double progress, std::string_view aggregate)
{
    if (!std::isfinite(progress) || progress < 0.0 || progress > 1.0) {
        return common::Result<void>::failure(error(
            "persistence.invalid_progress",
            std::string(aggregate) + " progress must be finite and in the [0, 1] range"));
    }
    return common::Result<void>::ok();
}

common::Result<void> validateRunCacheDuration(const Run& run)
{
    if (!run.usage.cacheBreakdownIsConsistent()) {
        return common::Result<void>::failure(error(
            "persistence.invalid_data",
            "Run cache-creation and cache-read tokens must fit within input tokens"));
    }
    const auto problem = checkCacheCreationDurationUsage(
        run.usage.cacheCreationInputTokens, run.usage.cacheCreationDuration);
    if (problem != CacheCreationDurationUsageProblem::None) {
        return common::Result<void>::failure(error(
            cacheCreationDurationUsageProblemCode(problem),
            cacheCreationDurationUsageProblemMessage(problem)));
    }
    return common::Result<void>::ok();
}

common::Result<void> insertRunRow(sqlite3* handle, const Run& run)
{
    // Shared by the legacy fixture/recovery insert and the aggregate
    // transaction so their bindings and typed duplicate behavior cannot drift.
    const auto validCacheDuration = validateRunCacheDuration(run);
    if (!validCacheDuration.success()) return validCacheDuration;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO runs (id,project_id,parent_run_id,title,task,workflow_id,status,progress,"
        "current_stage_id,error_message,input_tokens,output_tokens,created_at_ms,started_at_ms,"
        "finished_at_ms,revision,execution_profile_id,max_parallelism,workspace_path,"
        "planning_completion_policy,provider_budget_json,provider_budget_usage_json,"
        "cache_creation_input_tokens,cache_read_input_tokens,verification_repair_policy,"
        "run_interface_contracts_json,cache_creation_duration_partition_known,"
        "cache_creation_five_minute_input_tokens,cache_creation_one_hour_input_tokens) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return prepared;
    const auto bound = bindRunFields(handle, statement.get(), run);
    if (!bound.success()) return bound;
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<void>::failure(error("run.duplicate_id", "Run id already exists"));
        }
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not insert run"));
    }
    return common::Result<void>::ok();
}

common::Result<void> insertStageRow(
    sqlite3* handle,
    const RunStage& stage,
    std::size_t position)
{
    // Position belongs to persistence metadata rather than the domain DTO;
    // writing it here preserves the exact workflow-declared order on reload.
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO run_stages (id,run_id,type,title,description,status,progress,provider_id,model_id,"
        "input_tokens,output_tokens,error_message,started_at_ms,finished_at_ms,position) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return prepared;
    const auto bound = bindStageFields(
        handle, statement.get(), stage, static_cast<std::int64_t>(position));
    if (!bound.success()) return bound;
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not insert run stage"));
    }
    return common::Result<void>::ok();
}

std::string serializeStringVector(const std::vector<std::string>& values);
common::Result<std::vector<std::string>> parseStringVector(const std::string& encoded, std::string_view field);

// Writes one immutable snapshot row. Called only from insertRunWithStages,
// inside the same transaction as the run/stage rows, so a Run row can never
// become visible with a partially-written snapshot.
common::Result<void> insertRunStageAssignmentRow(
    sqlite3* handle,
    const application::RunId& runId,
    const StageModelAssignment& assignment)
{
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO run_stage_assignments (run_id,stage_type,provider_id,model_id,timeout_ms,"
        "agent_name,role,instructions,position,lead_architect,dependencies_json,max_attempts,"
        "fallback_provider_id,fallback_model_id,workspace_access) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return prepared;
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, runId),
        bindText(handle, statement.get(), 2, assignment.stageType),
        bindText(handle, statement.get(), 3, assignment.providerId),
        bindText(handle, statement.get(), 4, assignment.modelId),
        bindInt64(handle, statement.get(), 5, static_cast<std::int64_t>(assignment.timeout.count())),
        bindText(handle, statement.get(), 6, assignment.agentName),
        bindText(handle, statement.get(), 7, assignment.role),
        bindText(handle, statement.get(), 8, assignment.instructions),
        bindInt64(handle, statement.get(), 9, assignment.position),
        bindInt64(handle, statement.get(), 10, assignment.leadArchitect ? 1 : 0),
        bindText(handle, statement.get(), 11, serializeStringVector(assignment.dependencies)),
        bindUnsigned(handle, statement.get(), 12, assignment.maxAttempts),
        bindNullableText(handle, statement.get(), 13, assignment.fallbackProviderId),
        bindNullableText(handle, statement.get(), 14, assignment.fallbackModelId),
        bindInt64(handle, statement.get(), 15, assignment.workspaceAccess ? 1 : 0)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return binding;
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<void>::failure(error(
                "execution_profile.duplicate_stage_type",
                "A stage type cannot appear more than once in one run's assignment snapshot"));
        }
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not insert run stage assignment"));
    }
    return common::Result<void>::ok();
}

constexpr const char* projectColumns =
    "id, name, description, root_path, status, created_at_ms, updated_at_ms";
constexpr const char* taskColumns =
    "id, project_id, title, description, status, priority, requires_attention, "
    "due_at_ms, planned_run_at_ms, created_at_ms, updated_at_ms";
constexpr const char* runColumns =
    "id, project_id, parent_run_id, title, task, workflow_id, status, progress, "
    "current_stage_id, error_message, input_tokens, output_tokens, created_at_ms, "
    "started_at_ms, finished_at_ms, revision, execution_profile_id, max_parallelism, "
    "workspace_path, planning_completion_policy, provider_budget_json, provider_budget_usage_json, "
    "cache_creation_input_tokens, cache_read_input_tokens, verification_repair_policy, "
    "run_interface_contracts_json, cache_creation_duration_partition_known, "
    "cache_creation_five_minute_input_tokens, cache_creation_one_hour_input_tokens";
constexpr const char* runStageAssignmentColumns =
    "run_id, stage_type, provider_id, model_id, timeout_ms, agent_name, role, instructions, "
    "position, lead_architect, dependencies_json, max_attempts, fallback_provider_id, fallback_model_id, "
    "workspace_access";
constexpr const char* stageColumns =
    "id, run_id, type, title, description, position, status, progress, provider_id, "
    "model_id, input_tokens, output_tokens, error_message, started_at_ms, finished_at_ms";
constexpr const char* artifactColumns =
    "id, run_id, stage_id, kind, name, media_type, relative_path, byte_size, sha256, created_at_ms";
constexpr const char* providerColumns =
    "id, name, kind, base_url, enabled, credential_reference, created_at_ms, updated_at_ms";
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
constexpr const char* executionProfileColumns =
    "id, name, workflow_id, enabled, is_default, created_at_ms, updated_at_ms, provider_budget_json";
constexpr const char* executionProfileAssignmentColumns =
    "profile_id, stage_type, provider_id, model_id, timeout_ms";
constexpr const char* runDraftColumns =
    "id, name, project_id, title, task, workflow_id, launch_mode, execution_profile_id, "
    "max_parallelism, workspace_path, created_at_ms, updated_at_ms, verification_repair_policy, "
    "provider_budget_json, run_interface_contracts_json, "
    "group_id, group_ordinal, plan_key, depends_on_json, created_content_fingerprint";
constexpr const char* runDraftGroupColumns =
    "id, project_id, origin_kind, idempotency_key, plan_title, plan_fingerprint, source_conversation_id, "
    "source_message_id, source_message_fingerprint, context_fingerprint, source_run_ids_json, created_at_ms, "
    "source_request_id";
// Same column shape/order as runStageAssignmentColumns (minus the first
// column being draft_id instead of run_id) so readRunAssignment() can be
// reused verbatim for both tables.
constexpr const char* runDraftAssignmentColumns =
    "draft_id, stage_type, provider_id, model_id, timeout_ms, agent_name, role, instructions, "
    "position, lead_architect, dependencies_json, max_attempts, fallback_provider_id, fallback_model_id, "
    "workspace_access";

common::Result<Provider> readProvider(sqlite3_stmt* statement)
{
    Provider provider;
    provider.id = columnText(statement, 0);
    provider.name = columnText(statement, 1);
    provider.kind = columnText(statement, 2);
    provider.baseUrl = columnText(statement, 3);
    provider.enabled = sqlite3_column_int(statement, 4) != 0;
    provider.credentialReference = optionalText(statement, 5);
    provider.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 6));
    provider.updatedAt = fromMilliseconds(sqlite3_column_int64(statement, 7));
    return common::Result<Provider>::ok(std::move(provider));
}

common::Result<void> bindProvider(sqlite3* handle, sqlite3_stmt* statement, const Provider& provider)
{
    const common::Result<void> bindings[] = {
        bindText(handle, statement, 1, provider.id),
        bindText(handle, statement, 2, provider.name),
        bindText(handle, statement, 3, provider.kind),
        bindText(handle, statement, 4, provider.baseUrl),
        bindInt64(handle, statement, 5, provider.enabled ? 1 : 0),
        bindNullableText(handle, statement, 6, provider.credentialReference),
        bindInt64(handle, statement, 7, toMilliseconds(provider.createdAt)),
        bindInt64(handle, statement, 8, toMilliseconds(provider.updatedAt))};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    return common::Result<void>::ok();
}

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

common::Result<ExecutionProfile> readExecutionProfile(sqlite3_stmt* statement)
{
    ExecutionProfile profile;
    profile.id = columnText(statement, 0);
    profile.name = columnText(statement, 1);
    profile.workflowId = columnText(statement, 2);
    profile.enabled = sqlite3_column_int(statement, 3) != 0;
    profile.isDefault = sqlite3_column_int(statement, 4) != 0;
    profile.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 5));
    profile.updatedAt = fromMilliseconds(sqlite3_column_int64(statement, 6));
    const auto providerBudget = parseProviderBudgetOverrides(columnText(statement, 7));
    if (!providerBudget.success())
        return common::Result<ExecutionProfile>::failure(*providerBudget.error());
    profile.providerBudget = providerBudget.value();
    return common::Result<ExecutionProfile>::ok(std::move(profile));
}

common::Result<void> bindExecutionProfile(
    sqlite3* handle, sqlite3_stmt* statement, const ExecutionProfile& profile)
{
    const common::Result<void> bindings[] = {
        bindText(handle, statement, 1, profile.id),
        bindText(handle, statement, 2, profile.name),
        bindText(handle, statement, 3, profile.workflowId),
        bindInt64(handle, statement, 4, profile.enabled ? 1 : 0),
        bindInt64(handle, statement, 5, profile.isDefault ? 1 : 0),
        bindInt64(handle, statement, 6, toMilliseconds(profile.createdAt)),
        bindInt64(handle, statement, 7, toMilliseconds(profile.updatedAt)),
        bindText(handle, statement, 8, providerBudgetOverridesJson(profile.providerBudget))};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    return common::Result<void>::ok();
}

// The five provenance columns of a run_drafts row starting at `first` (group_id, group_ordinal, plan_key,
// depends_on_json, created_content_fingerprint). All NULL = a manual draft (no provenance); all set = a draft of a
// group. Anything in between was not written by this code and is reported, never repaired.
common::Result<std::optional<application::RunDraftProvenance>> readRunDraftProvenance(
    sqlite3_stmt* statement, int first)
{
    using Result = common::Result<std::optional<application::RunDraftProvenance>>;
    std::size_t present = 0;
    for (int column = first; column < first + 5; ++column) {
        if (sqlite3_column_type(statement, column) != SQLITE_NULL) ++present;
    }
    if (present == 0) return Result::ok(std::nullopt);
    const std::int64_t ordinal = sqlite3_column_int64(statement, first + 1);
    if (present != 5 || ordinal < 0 || ordinal > std::numeric_limits<std::uint32_t>::max()) {
        return Result::failure(error("persistence.invalid_data", "Run draft row contains incomplete group provenance"));
    }
    auto dependsOn = parseStringVector(columnText(statement, first + 3), "Run draft depends_on_json");
    if (!dependsOn.success()) return Result::failure(*dependsOn.error());
    application::RunDraftProvenance value;
    value.groupId = columnText(statement, first);
    value.ordinal = static_cast<std::uint32_t>(ordinal);
    value.planKey = columnText(statement, first + 2);
    value.dependsOn = std::move(dependsOn.value());
    value.createdContentFingerprint = columnText(statement, first + 4);
    return Result::ok(std::move(value));
}

common::Result<RunDraft> readRunDraft(sqlite3_stmt* statement)
{
    // runDraftColumns order: id(0) name(1) project_id(2) title(3) task(4)
    // workflow_id(5) launch_mode(6) execution_profile_id(7)
    // max_parallelism(8) workspace_path(9) created_at_ms(10) updated_at_ms(11)
    // verification_repair_policy(12) provider_budget_json(13) run_interface_contracts_json(14)
    const std::int64_t launchModeValue = sqlite3_column_int64(statement, 6);
    if (launchModeValue < 0 || launchModeValue > 2) {
        return common::Result<RunDraft>::failure(
            error("persistence.invalid_data", "Run draft row contains an unknown launch mode"));
    }
    const std::int64_t maxParallelism = sqlite3_column_int64(statement, 8);
    if (maxParallelism <= 0) {
        return common::Result<RunDraft>::failure(
            error("persistence.invalid_data", "Run draft row contains a non-positive max parallelism"));
    }
    const auto verificationRepairPolicy =
        application::verificationRepairPolicyFromString(columnText(statement, 12));
    if (!verificationRepairPolicy.has_value()) {
        return common::Result<RunDraft>::failure(error(
            "persistence.invalid_data",
            "Run draft row contains an unknown verification repair policy"));
    }
    const auto providerBudget = parseProviderBudgetOverrides(columnText(statement, 13));
    if (!providerBudget.success()) {
        return common::Result<RunDraft>::failure(error(
            "persistence.invalid_data", "Run draft row contains corrupt provider budget limits"));
    }
    const auto interfaceContracts =
        application::parseRunInterfaceContracts(columnText(statement, 14));
    if (!interfaceContracts.success()) {
        return common::Result<RunDraft>::failure(*interfaceContracts.error());
    }

    // PC-3B provenance: group_id(15) group_ordinal(16) plan_key(17) depends_on_json(18)
    // created_content_fingerprint(19).
    auto provenance = readRunDraftProvenance(statement, 15);
    if (!provenance.success()) return common::Result<RunDraft>::failure(*provenance.error());

    RunDraft draft;
    draft.id = columnText(statement, 0);
    draft.name = columnText(statement, 1);
    draft.projectId = columnText(statement, 2);
    draft.title = columnText(statement, 3);
    draft.task = columnText(statement, 4);
    draft.workflowId = columnText(statement, 5);
    draft.launchMode = static_cast<RunLaunchMode>(launchModeValue);
    draft.executionProfileId = optionalText(statement, 7);
    draft.maxParallelism = static_cast<std::uint32_t>(maxParallelism);
    const auto workspaceText = optionalText(statement, 9);
    draft.workspacePath = workspaceText.has_value()
        ? std::optional<std::filesystem::path>(pathFromUtf8(*workspaceText))
        : std::nullopt;
    draft.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 10));
    draft.updatedAt = fromMilliseconds(sqlite3_column_int64(statement, 11));
    draft.verificationRepairPolicy = *verificationRepairPolicy;
    draft.providerBudget = providerBudget.value();
    draft.interfaceContracts = interfaceContracts.value();
    draft.provenance = std::move(provenance.value());
    return common::Result<RunDraft>::ok(std::move(draft));
}

common::Result<void> bindNullableInt64(
    sqlite3* handle, sqlite3_stmt* statement, int index, const std::optional<std::int64_t>& value)
{
    if (value.has_value()) return bindInt64(handle, statement, index, *value);
    if (sqlite3_bind_null(statement, index) != SQLITE_OK) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.bind_failed", "Could not bind null integer value"));
    }
    return common::Result<void>::ok();
}

// runDraftGroupColumns order: id(0) project_id(1) origin_kind(2) idempotency_key(3) plan_title(4)
// plan_fingerprint(5) source_conversation_id(6) source_message_id(7) source_message_fingerprint(8)
// context_fingerprint(9) source_run_ids_json(10) created_at_ms(11) source_request_id(12)
common::Result<application::RunDraftGroup> readRunDraftGroup(sqlite3_stmt* statement)
{
    const auto origin = application::runDraftGroupOriginFromString(columnText(statement, 2));
    if (!origin.has_value()) {
        return common::Result<application::RunDraftGroup>::failure(
            error("persistence.invalid_data", "Run draft group row contains an unknown origin"));
    }
    auto sourceRuns = parseStringVector(columnText(statement, 10), "Run draft group source_run_ids_json");
    if (!sourceRuns.success()) {
        return common::Result<application::RunDraftGroup>::failure(*sourceRuns.error());
    }
    application::RunDraftGroup group;
    group.id = columnText(statement, 0);
    group.projectId = columnText(statement, 1);
    group.origin = *origin;
    group.idempotencyKey = columnText(statement, 3);
    group.planTitle = columnText(statement, 4);
    group.planFingerprint = columnText(statement, 5);
    group.sourceConversationId = optionalText(statement, 6);
    group.sourceMessageId = optionalText(statement, 7);
    group.sourceMessageFingerprint = optionalText(statement, 8);
    group.contextFingerprint = optionalText(statement, 9);
    group.sourceRunIds = std::move(sourceRuns.value());
    group.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 11));
    group.sourceRequestId = optionalText(statement, 12);
    return common::Result<application::RunDraftGroup>::ok(std::move(group));
}

common::Result<void> bindRunDraftGroup(
    sqlite3* handle, sqlite3_stmt* statement, const application::RunDraftGroup& group)
{
    const common::Result<void> bindings[] = {
        bindText(handle, statement, 1, group.id),
        bindText(handle, statement, 2, group.projectId),
        bindText(handle, statement, 3, application::toString(group.origin)),
        bindText(handle, statement, 4, group.idempotencyKey),
        bindText(handle, statement, 5, group.planTitle),
        bindText(handle, statement, 6, group.planFingerprint),
        bindNullableText(handle, statement, 7, group.sourceConversationId),
        bindNullableText(handle, statement, 8, group.sourceMessageId),
        bindNullableText(handle, statement, 9, group.sourceMessageFingerprint),
        bindNullableText(handle, statement, 10, group.contextFingerprint),
        bindText(handle, statement, 11, serializeStringVector(group.sourceRunIds)),
        bindInt64(handle, statement, 12, toMilliseconds(group.createdAt)),
        bindNullableText(handle, statement, 13, group.sourceRequestId)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return binding;
    }
    return common::Result<void>::ok();
}

common::Result<void> bindRunDraft(sqlite3* handle, sqlite3_stmt* statement, const RunDraft& draft)
{
    const auto& provenance = draft.provenance;
    const common::Result<void> bindings[] = {
        bindText(handle, statement, 1, draft.id),
        bindText(handle, statement, 2, draft.name),
        bindText(handle, statement, 3, draft.projectId),
        bindText(handle, statement, 4, draft.title),
        bindText(handle, statement, 5, draft.task),
        bindText(handle, statement, 6, draft.workflowId),
        bindInt64(handle, statement, 7, static_cast<std::int64_t>(draft.launchMode)),
        bindNullableText(handle, statement, 8, draft.executionProfileId),
        bindUnsigned(handle, statement, 9, draft.maxParallelism),
        bindNullableText(handle, statement, 10,
            draft.workspacePath.has_value()
                ? std::optional<std::string>(pathToUtf8(*draft.workspacePath))
                : std::nullopt),
        bindInt64(handle, statement, 11, toMilliseconds(draft.createdAt)),
        bindInt64(handle, statement, 12, toMilliseconds(draft.updatedAt)),
        bindText(handle, statement, 13,
            application::toString(draft.verificationRepairPolicy)),
        bindText(handle, statement, 14, providerBudgetOverridesJson(draft.providerBudget)),
        bindText(handle, statement, 15,
            application::serializeRunInterfaceContracts(draft.interfaceContracts)),
        // A manual draft binds five NULLs; a draft of a group binds its provenance (insertGroup only).
        bindNullableText(handle, statement, 16,
            provenance.has_value() ? std::optional<std::string>(provenance->groupId) : std::nullopt),
        bindNullableInt64(handle, statement, 17,
            provenance.has_value() ? std::optional<std::int64_t>(provenance->ordinal) : std::nullopt),
        bindNullableText(handle, statement, 18,
            provenance.has_value() ? std::optional<std::string>(provenance->planKey) : std::nullopt),
        bindNullableText(handle, statement, 19,
            provenance.has_value() ? std::optional<std::string>(serializeStringVector(provenance->dependsOn))
                                   : std::nullopt),
        bindNullableText(handle, statement, 20,
            provenance.has_value() ? std::optional<std::string>(provenance->createdContentFingerprint)
                                   : std::nullopt)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    return common::Result<void>::ok();
}

common::Result<StageModelAssignment> readAssignment(sqlite3_stmt* statement)
{
    // executionProfileAssignmentColumns order: profile_id(0), stage_type(1),
    // provider_id(2), model_id(3), timeout_ms(4).
    const std::int64_t timeoutMs = sqlite3_column_int64(statement, 4);
    if (timeoutMs <= 0) {
        return common::Result<StageModelAssignment>::failure(
            error("persistence.invalid_data", "Assignment row contains a non-positive timeout"));
    }
    StageModelAssignment assignment;
    assignment.stageType = columnText(statement, 1);
    assignment.providerId = columnText(statement, 2);
    assignment.modelId = columnText(statement, 3);
    assignment.timeout = std::chrono::milliseconds(timeoutMs);
    return common::Result<StageModelAssignment>::ok(std::move(assignment));
}

std::string serializeStringVector(const std::vector<std::string>& values)
{
    nlohmann::ordered_json encoded = nlohmann::ordered_json::array();
    for (const auto& value : values) encoded.push_back(value);
    return encoded.dump();
}

common::Result<std::vector<std::string>> parseStringVector(
    const std::string& encoded,
    std::string_view field)
{
    const auto value = nlohmann::json::parse(encoded, nullptr, false);
    if (value.is_discarded() || !value.is_array()) {
        return common::Result<std::vector<std::string>>::failure(error(
            "persistence.invalid_data", std::string(field) + " is not a JSON array"));
    }
    std::vector<std::string> result;
    for (const auto& item : value) {
        if (!item.is_string() || item.get_ref<const std::string&>().empty()) {
            return common::Result<std::vector<std::string>>::failure(error(
                "persistence.invalid_data", std::string(field) + " contains an invalid id"));
        }
        result.push_back(item.get<std::string>());
    }
    return common::Result<std::vector<std::string>>::ok(std::move(result));
}

common::Result<StageModelAssignment> readRunAssignment(sqlite3_stmt* statement)
{
    auto result = readAssignment(statement);
    if (!result.success()) return result;
    result.value().agentName = columnText(statement, 5);
    result.value().role = columnText(statement, 6);
    result.value().instructions = columnText(statement, 7);
    const auto position = sqlite3_column_int64(statement, 8);
    if (position < 0) {
        return common::Result<StageModelAssignment>::failure(
            error("persistence.invalid_data", "Run assignment position is negative"));
    }
    result.value().position = static_cast<std::uint32_t>(position);
    result.value().leadArchitect = sqlite3_column_int(statement, 9) != 0;
    const auto dependencies = parseStringVector(
        columnText(statement, 10), "Run assignment dependencies");
    if (!dependencies.success()) {
        return common::Result<StageModelAssignment>::failure(*dependencies.error());
    }
    const auto maxAttempts = sqlite3_column_int64(statement, 11);
    if (maxAttempts <= 0 || maxAttempts > 10) {
        return common::Result<StageModelAssignment>::failure(error(
            "persistence.invalid_data", "Run assignment attempt bound is invalid"));
    }
    result.value().dependencies = dependencies.value();
    result.value().maxAttempts = static_cast<std::uint32_t>(maxAttempts);
    result.value().fallbackProviderId = optionalText(statement, 12);
    result.value().fallbackModelId = optionalText(statement, 13);
    result.value().workspaceAccess = sqlite3_column_int(statement, 14) != 0;
    return result;
}

// Shared by findById and listAll so both read exactly the same
// deterministically-ordered assignment set for a profile.
common::Result<std::vector<StageModelAssignment>> fetchAssignmentsForProfile(
    sqlite3* handle,
    const std::string& profileId)
{
    Statement statement;
    const std::string sql = std::string("SELECT ") + executionProfileAssignmentColumns
        + " FROM execution_profile_assignments WHERE profile_id=? ORDER BY stage_type;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) {
        return common::Result<std::vector<StageModelAssignment>>::failure(*prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, profileId);
    if (!bound.success()) {
        return common::Result<std::vector<StageModelAssignment>>::failure(*bound.error());
    }

    std::vector<StageModelAssignment> assignments;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<StageModelAssignment>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not read execution profile assignments"));
        }
        auto assignment = readAssignment(statement.get());
        if (!assignment.success()) {
            return common::Result<std::vector<StageModelAssignment>>::failure(*assignment.error());
        }
        assignments.push_back(assignment.value());
    }
    return common::Result<std::vector<StageModelAssignment>>::ok(std::move(assignments));
}

// Shared by findRun/listRuns/listUnfinishedRuns so every Run read path
// attaches the same immutable roster snapshot in execution order.
common::Result<std::vector<StageModelAssignment>> fetchAssignmentsForRun(
    sqlite3* handle,
    const std::string& runId)
{
    Statement statement;
    const std::string sql = std::string("SELECT ") + runStageAssignmentColumns
        + " FROM run_stage_assignments WHERE run_id=? ORDER BY position,stage_type;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) {
        return common::Result<std::vector<StageModelAssignment>>::failure(*prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::vector<StageModelAssignment>>::failure(*bound.error());
    }

    std::vector<StageModelAssignment> assignments;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<StageModelAssignment>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not read run stage assignments"));
        }
        auto assignment = readRunAssignment(statement.get());
        if (!assignment.success()) {
            return common::Result<std::vector<StageModelAssignment>>::failure(*assignment.error());
        }
        assignments.push_back(assignment.value());
    }
    return common::Result<std::vector<StageModelAssignment>>::ok(std::move(assignments));
}

// Same column shape as run_stage_assignments (see runDraftAssignmentColumns),
// so readRunAssignment() is reused as-is.
common::Result<std::vector<StageModelAssignment>> fetchAssignmentsForDraft(
    sqlite3* handle,
    const std::string& draftId)
{
    Statement statement;
    const std::string sql = std::string("SELECT ") + runDraftAssignmentColumns
        + " FROM run_draft_assignments WHERE draft_id=? ORDER BY position,stage_type;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) {
        return common::Result<std::vector<StageModelAssignment>>::failure(*prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, draftId);
    if (!bound.success()) {
        return common::Result<std::vector<StageModelAssignment>>::failure(*bound.error());
    }

    std::vector<StageModelAssignment> assignments;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<StageModelAssignment>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not read run draft assignments"));
        }
        auto assignment = readRunAssignment(statement.get());
        if (!assignment.success()) {
            return common::Result<std::vector<StageModelAssignment>>::failure(*assignment.error());
        }
        assignments.push_back(assignment.value());
    }
    return common::Result<std::vector<StageModelAssignment>>::ok(std::move(assignments));
}

common::Result<void> insertRunDraftAssignmentRow(
    sqlite3* handle,
    const application::RunDraftId& draftId,
    const StageModelAssignment& assignment)
{
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO run_draft_assignments (draft_id,stage_type,provider_id,model_id,timeout_ms,"
        "agent_name,role,instructions,position,lead_architect,dependencies_json,max_attempts,"
        "fallback_provider_id,fallback_model_id,workspace_access) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return prepared;
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, draftId),
        bindText(handle, statement.get(), 2, assignment.stageType),
        bindText(handle, statement.get(), 3, assignment.providerId),
        bindText(handle, statement.get(), 4, assignment.modelId),
        bindInt64(handle, statement.get(), 5, static_cast<std::int64_t>(assignment.timeout.count())),
        bindText(handle, statement.get(), 6, assignment.agentName),
        bindText(handle, statement.get(), 7, assignment.role),
        bindText(handle, statement.get(), 8, assignment.instructions),
        bindInt64(handle, statement.get(), 9, assignment.position),
        bindInt64(handle, statement.get(), 10, assignment.leadArchitect ? 1 : 0),
        bindText(handle, statement.get(), 11, serializeStringVector(assignment.dependencies)),
        bindUnsigned(handle, statement.get(), 12, assignment.maxAttempts),
        bindNullableText(handle, statement.get(), 13, assignment.fallbackProviderId),
        bindNullableText(handle, statement.get(), 14, assignment.fallbackModelId),
        bindInt64(handle, statement.get(), 15, assignment.workspaceAccess ? 1 : 0)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return binding;
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<void>::failure(error(
                "run_draft.duplicate_stage_type",
                "A stage type cannot appear more than once in one draft's roster"));
        }
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not insert run draft assignment"));
    }
    return common::Result<void>::ok();
}

// Batched counterpart of fetchAssignmentsForRun for listRuns/listUnfinishedRuns:
// those callers previously issued one fetchAssignmentsForRun query PER run in
// the result set (N+1), which the UI feels directly - RunController::handleEvent
// re-runs a full listRuns() on every single RunEvent (stage started, each
// tool-call progress tick, terminal), on the GUI thread, while holding the
// shared SQLite mutex for the whole call. With run history now in the
// dozens and growing, and a workspaceAccess agent's turn loop emitting one
// progress event per turn, this N+1 was a real, measurable source of the
// reported UI freeze during an active run. One IN(...) query replaces the
// N separate ones; an empty runIds list short-circuits (SQL "IN ()" is
// invalid) and returns an empty map without touching the database.
common::Result<std::unordered_map<std::string, std::vector<StageModelAssignment>>>
fetchAssignmentsForRuns(sqlite3* handle, const std::vector<std::string>& runIds)
{
    std::unordered_map<std::string, std::vector<StageModelAssignment>> byRunId;
    if (runIds.empty()) {
        return common::Result<std::unordered_map<std::string, std::vector<StageModelAssignment>>>::ok(
            std::move(byRunId));
    }

    std::string placeholders(runIds.size() * 2 - 1, ',');
    for (std::size_t i = 0; i < runIds.size(); ++i) placeholders[i * 2] = '?';

    Statement statement;
    const std::string sql = std::string("SELECT ") + runStageAssignmentColumns
        + " FROM run_stage_assignments WHERE run_id IN (" + placeholders + ") "
          "ORDER BY run_id,position,stage_type;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) {
        return common::Result<std::unordered_map<std::string, std::vector<StageModelAssignment>>>::failure(
            *prepared.error());
    }
    for (std::size_t i = 0; i < runIds.size(); ++i) {
        const auto bound = bindText(handle, statement.get(), static_cast<int>(i) + 1, runIds[i]);
        if (!bound.success()) {
            return common::Result<std::unordered_map<std::string, std::vector<StageModelAssignment>>>::failure(
                *bound.error());
        }
    }

    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::unordered_map<std::string, std::vector<StageModelAssignment>>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not read run stage assignments"));
        }
        const std::string runId = columnText(statement.get(), 0);
        auto assignment = readRunAssignment(statement.get());
        if (!assignment.success()) {
            return common::Result<std::unordered_map<std::string, std::vector<StageModelAssignment>>>::failure(
                *assignment.error());
        }
        byRunId[runId].push_back(std::move(assignment.value()));
    }
    return common::Result<std::unordered_map<std::string, std::vector<StageModelAssignment>>>::ok(
        std::move(byRunId));
}

constexpr const char* comparisonSessionColumns =
    "id,project_id,title,prompt,status,request_hash,context_hash,"
    "preferred_execution_id,synthesis_selection_json,synthesis_execution_id,"
    "error_code,error_message,created_at_ms,started_at_ms,finished_at_ms,revision";

std::string serializeExecutionIds(const std::vector<application::ModelExecutionId>& ids)
{
    nlohmann::ordered_json value = nlohmann::ordered_json::array();
    for (const auto& id : ids) value.push_back(id);
    return value.dump();
}

common::Result<std::vector<application::ModelExecutionId>> parseExecutionIds(
    const std::string& encoded)
{
    const auto value = nlohmann::json::parse(encoded, nullptr, false);
    if (value.is_discarded() || !value.is_array()) {
        return common::Result<std::vector<application::ModelExecutionId>>::failure(
            error("persistence.invalid_data", "Comparison synthesis selection is not a JSON array"));
    }
    std::vector<application::ModelExecutionId> ids;
    for (const auto& item : value) {
        if (!item.is_string() || item.get_ref<const std::string&>().empty()) {
            return common::Result<std::vector<application::ModelExecutionId>>::failure(
                error("persistence.invalid_data", "Comparison synthesis selection contains an invalid id"));
        }
        ids.push_back(item.get<std::string>());
    }
    return common::Result<std::vector<application::ModelExecutionId>>::ok(std::move(ids));
}

common::Result<ComparisonSession> readComparisonSession(sqlite3_stmt* statement)
{
    const auto status = application::comparisonSessionStatusFromString(columnText(statement, 4));
    if (!status.has_value()) {
        return common::Result<ComparisonSession>::failure(
            error("persistence.invalid_data", "Comparison session has an unknown status"));
    }
    const auto selection = parseExecutionIds(columnText(statement, 8));
    if (!selection.success())
        return common::Result<ComparisonSession>::failure(*selection.error());
    const auto revision = sqlite3_column_int64(statement, 15);
    if (revision < 0) {
        return common::Result<ComparisonSession>::failure(
            error("persistence.invalid_data", "Comparison session revision is negative"));
    }

    ComparisonSession value;
    value.id = columnText(statement, 0);
    value.projectId = columnText(statement, 1);
    value.title = columnText(statement, 2);
    value.prompt = columnText(statement, 3);
    value.status = *status;
    value.requestHash = columnText(statement, 5);
    value.contextHash = columnText(statement, 6);
    value.preferredExecutionId = optionalText(statement, 7);
    value.synthesisSelection = selection.value();
    value.synthesisExecutionId = optionalText(statement, 9);
    value.errorCode = columnText(statement, 10);
    value.errorMessage = columnText(statement, 11);
    value.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 12));
    value.startedAt = optionalTime(statement, 13);
    value.finishedAt = optionalTime(statement, 14);
    value.revision = static_cast<std::uint64_t>(revision);
    return common::Result<ComparisonSession>::ok(std::move(value));
}

} // namespace

// Owns the single native sqlite3 connection and the mutex that serializes
// every statement this process issues against it. Both repository classes
// share this one connection (via shared_ptr) rather than opening a
// connection per call: SQLITE_OPEN_FULLMUTEX plus this mutex already make
// each repository method atomic with respect to every other thread, so a
// persistent connection is both simpler and avoids repeatedly paying
// sqlite3_open_v2/close_v2 cost on every single read.
struct SqliteDatabase::Impl {
    sqlite3* handle {nullptr};
    std::filesystem::path databasePath;
    mutable std::mutex mutex;

    ~Impl()
    {
        // Impl owns the native handle even while open()/migration is failing;
        // this closes every early-return path as well as normal destruction.
        if (handle != nullptr) {
            sqlite3_close_v2(handle);
        }
    }
};

SqliteDatabase::SqliteDatabase(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

SqliteDatabase::~SqliteDatabase() = default;

common::Result<std::shared_ptr<SqliteDatabase>> SqliteDatabase::open(
    const std::filesystem::path& databasePath)
{
    if (databasePath.empty()) {
        return common::Result<std::shared_ptr<SqliteDatabase>>::failure(
            error("persistence.empty_path", "SQLite database path cannot be empty"));
    }

    std::error_code filesystemError;
    const auto parent = databasePath.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, filesystemError);
        if (filesystemError) {
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(error(
                "persistence.directory_failed",
                "Could not create SQLite database directory: " + filesystemError.message()));
        }
    }

    auto impl = std::make_unique<Impl>();
    impl->databasePath = databasePath;
    const std::string utf8Path = pathToUtf8(databasePath);
    // SQLITE_OPEN_FULLMUTEX forces serialized threading mode for this
    // connection regardless of how the linked sqlite3 library was compiled,
    // so the Impl-level mutex above and SQLite's own internal locking agree
    // on "one statement in flight at a time" instead of relying on a
    // compile-time default this project does not control.
    if (sqlite3_open_v2(
            utf8Path.c_str(),
            &impl->handle,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            nullptr)
        != SQLITE_OK) {
        const common::Error openError = databaseError(
            impl->handle,
            "persistence.open_failed",
            "Could not open SQLite database");
        if (impl->handle != nullptr) sqlite3_close_v2(impl->handle);
        impl->handle = nullptr;
        return common::Result<std::shared_ptr<SqliteDatabase>>::failure(openError);
    }

    sqlite3_extended_result_codes(impl->handle, 1);
    sqlite3_busy_timeout(impl->handle, 5000);

    const auto foreignKeys = execute(impl->handle, "PRAGMA foreign_keys = ON;", "migration.pragma_failed");
    if (!foreignKeys.success()) {
        return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*foreignKeys.error());
    }

    // WAL instead of the default rollback journal: the UI thread's own reads
    // (e.g. AgentSessionController::refresh() on a QML poll timer) share this
    // one connection/mutex with the run worker thread's writes. In the
    // default journal mode a writer and a reader exclude each other at the
    // SQLite level too, so a burst of writes from several genuinely-parallel
    // workspaceAccess agents (tool-call audit rows, turns, proposals) could
    // stall a UI-thread read for the whole burst - observed as the GUI
    // freezing while a run is active. WAL lets readers proceed alongside a
    // writer; NORMAL is the documented safe-with-WAL synchronous level (only
    // fsyncs at checkpoints, not on every commit).
    const auto walMode = execute(impl->handle, "PRAGMA journal_mode = WAL;", "migration.pragma_failed");
    if (!walMode.success()) {
        return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*walMode.error());
    }
    const auto synchronousMode = execute(impl->handle, "PRAGMA synchronous = NORMAL;", "migration.pragma_failed");
    if (!synchronousMode.success()) {
        return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*synchronousMode.error());
    }

    int schemaVersion = 0;
    {
        // Finalize the PRAGMA statement before beginning a migration transaction.
        Statement versionStatement;
        const auto versionPrepared = prepare(impl->handle, "PRAGMA user_version;", versionStatement);
        if (!versionPrepared.success()) {
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*versionPrepared.error());
        }
        if (sqlite3_step(versionStatement.get()) != SQLITE_ROW) {
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(
                databaseError(impl->handle, "migration.version_failed", "Could not read schema version"));
        }
        schemaVersion = sqlite3_column_int(versionStatement.get(), 0);
    }
    if (schemaVersion > supportedSchemaVersion) {
        return common::Result<std::shared_ptr<SqliteDatabase>>::failure(error(
            "migration.unsupported_version",
            "Database schema is newer than this application supports"));
    }

    if (schemaVersion < 1) {
        // Version 1 is installed as one transaction: a crash cannot leave a
        // partially-created schema that nevertheless reports a newer version.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE projects (
    id TEXT PRIMARY KEY NOT NULL,
    name TEXT NOT NULL,
    description TEXT NOT NULL,
    root_path TEXT NOT NULL UNIQUE,
    status TEXT NOT NULL CHECK(status IN ('active','archived','unavailable')),
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL
);
CREATE TABLE runs (
    id TEXT PRIMARY KEY NOT NULL,
    project_id TEXT NOT NULL REFERENCES projects(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    parent_run_id TEXT REFERENCES runs(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    title TEXT NOT NULL,
    task TEXT NOT NULL,
    workflow_id TEXT NOT NULL,
    status TEXT NOT NULL CHECK(status IN ('queued','running','cancelling','completed','failed','cancelled','interrupted')),
    progress REAL NOT NULL CHECK(progress >= 0.0 AND progress <= 1.0),
    current_stage_id TEXT,
    error_message TEXT NOT NULL,
    input_tokens INTEGER NOT NULL CHECK(input_tokens >= 0),
    output_tokens INTEGER NOT NULL CHECK(output_tokens >= 0),
    created_at_ms INTEGER NOT NULL,
    started_at_ms INTEGER,
    finished_at_ms INTEGER,
    revision INTEGER NOT NULL CHECK(revision >= 0)
);
CREATE INDEX runs_project_created_idx ON runs(project_id, created_at_ms, id);
CREATE INDEX runs_status_idx ON runs(status);
CREATE TABLE run_stages (
    id TEXT PRIMARY KEY NOT NULL,
    run_id TEXT NOT NULL REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,
    type TEXT NOT NULL,
    title TEXT NOT NULL,
    description TEXT NOT NULL,
    position INTEGER NOT NULL CHECK(position >= 0),
    status TEXT NOT NULL CHECK(status IN ('pending','running','completed','failed','skipped','cancelled')),
    progress REAL NOT NULL CHECK(progress >= 0.0 AND progress <= 1.0),
    provider_id TEXT NOT NULL,
    model_id TEXT NOT NULL,
    input_tokens INTEGER NOT NULL CHECK(input_tokens >= 0),
    output_tokens INTEGER NOT NULL CHECK(output_tokens >= 0),
    error_message TEXT NOT NULL,
    started_at_ms INTEGER,
    finished_at_ms INTEGER,
    UNIQUE(run_id, position)
);
CREATE INDEX run_stages_run_idx ON run_stages(run_id, position);
PRAGMA user_version = 1;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v1_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 2) {
        // Artifact metadata only; contents live in the filesystem artifact
        // store (see FileArtifactStore) and are never written to SQLite.
        // relative_path is a UTF-8 path relative to the artifact store root -
        // never an absolute filesystem path - so this row alone can never be
        // used to escape the root even if read directly from the database.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE artifacts (
    id TEXT PRIMARY KEY NOT NULL,
    run_id TEXT NOT NULL REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,
    stage_id TEXT REFERENCES run_stages(id) ON UPDATE CASCADE ON DELETE SET NULL,
    kind TEXT NOT NULL CHECK(kind IN ('context_snapshot','report','patch','source','log','other')),
    name TEXT NOT NULL,
    media_type TEXT NOT NULL,
    relative_path TEXT NOT NULL,
    byte_size INTEGER NOT NULL CHECK(byte_size >= 0),
    sha256 TEXT NOT NULL,
    created_at_ms INTEGER NOT NULL
);
CREATE INDEX artifacts_run_created_idx ON artifacts(run_id, created_at_ms, id);
PRAGMA user_version = 2;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v2_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 3) {
        // Workspace/provider-catalog/execution-profile foundation. No
        // secret ever appears in any of these tables: providers stores only
        // credential_reference (an opaque pointer into a future
        // ICredentialStore), never a secret value. workspace_state is a
        // singleton row (id is CHECK-constrained to 1) that stores only the
        // stable active_project_id - never a copy of the Project's own
        // name/description/root_path/status, which always stays owned by
        // the projects table and is re-read live through it.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE workspace_state (
    id INTEGER PRIMARY KEY CHECK(id = 1),
    active_project_id TEXT REFERENCES projects(id) ON UPDATE CASCADE ON DELETE SET NULL
);
INSERT INTO workspace_state (id, active_project_id) VALUES (1, NULL);
CREATE TABLE providers (
    id TEXT PRIMARY KEY NOT NULL,
    name TEXT NOT NULL,
    kind TEXT NOT NULL CHECK(kind IN ('openai','anthropic','openai_compatible','local')),
    base_url TEXT NOT NULL,
    enabled INTEGER NOT NULL CHECK(enabled IN (0,1)),
    credential_reference TEXT,
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL
);
CREATE INDEX providers_created_idx ON providers(created_at_ms, id);
CREATE TABLE models (
    id TEXT PRIMARY KEY NOT NULL,
    provider_id TEXT NOT NULL REFERENCES providers(id) ON UPDATE CASCADE ON DELETE CASCADE,
    api_model_id TEXT NOT NULL,
    display_name TEXT NOT NULL,
    context_window_tokens INTEGER NOT NULL CHECK(context_window_tokens >= 0),
    max_output_tokens INTEGER NOT NULL CHECK(max_output_tokens >= 0),
    input_price_micros_per_million_tokens INTEGER NOT NULL CHECK(input_price_micros_per_million_tokens >= 0),
    output_price_micros_per_million_tokens INTEGER NOT NULL CHECK(output_price_micros_per_million_tokens >= 0),
    enabled INTEGER NOT NULL CHECK(enabled IN (0,1)),
    is_default INTEGER NOT NULL CHECK(is_default IN (0,1)),
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL,
    UNIQUE(provider_id, api_model_id)
);
CREATE INDEX models_provider_created_idx ON models(provider_id, created_at_ms, id);
CREATE UNIQUE INDEX models_one_default_per_provider_idx ON models(provider_id) WHERE is_default = 1;
CREATE TABLE execution_profiles (
    id TEXT PRIMARY KEY NOT NULL,
    name TEXT NOT NULL,
    workflow_id TEXT NOT NULL,
    enabled INTEGER NOT NULL CHECK(enabled IN (0,1)),
    is_default INTEGER NOT NULL CHECK(is_default IN (0,1)),
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL
);
CREATE INDEX execution_profiles_created_idx ON execution_profiles(created_at_ms, id);
CREATE UNIQUE INDEX execution_profiles_one_default_idx ON execution_profiles(is_default) WHERE is_default = 1;
CREATE TABLE execution_profile_assignments (
    profile_id TEXT NOT NULL REFERENCES execution_profiles(id) ON UPDATE CASCADE ON DELETE CASCADE,
    stage_type TEXT NOT NULL CHECK(stage_type IN ('architect','implementation','documentation')),
    provider_id TEXT NOT NULL REFERENCES providers(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    model_id TEXT NOT NULL REFERENCES models(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    timeout_ms INTEGER NOT NULL CHECK(timeout_ms > 0),
    PRIMARY KEY (profile_id, stage_type)
);
PRAGMA user_version = 3;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v3_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 4) {
        // Execution-profile-aware run routing: which profile (if any) a run
        // was started against, plus the immutable per-stage
        // provider/model/timeout snapshot actually used. Both are set once
        // by the aggregate insert (insertRunWithStages) and never touched by
        // updateRun's UPDATE statement afterward, so this is where the
        // "immutable snapshot" contract is enforced at the storage layer,
        // not just by Application-layer convention. A run created before
        // this migration (schema v3 and earlier) simply reads back with
        // execution_profile_id NULL and no assignment rows - the Mock
        // workflow never had either, so old Mock runs are unaffected.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
ALTER TABLE runs ADD COLUMN execution_profile_id TEXT
    REFERENCES execution_profiles(id) ON UPDATE CASCADE ON DELETE RESTRICT;
CREATE TABLE run_stage_assignments (
    run_id TEXT NOT NULL REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,
    stage_type TEXT NOT NULL CHECK(stage_type IN ('architect','implementation','documentation')),
    provider_id TEXT NOT NULL REFERENCES providers(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    model_id TEXT NOT NULL REFERENCES models(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    timeout_ms INTEGER NOT NULL CHECK(timeout_ms > 0),
    PRIMARY KEY (run_id, stage_type)
);
PRAGMA user_version = 4;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v4_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 5) {
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE chat_conversations (
    id TEXT PRIMARY KEY,
    project_id TEXT REFERENCES projects(id) ON UPDATE CASCADE ON DELETE SET NULL,
    provider_id TEXT NOT NULL REFERENCES providers(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    model_id TEXT NOT NULL REFERENCES models(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    title TEXT NOT NULL,
    preview TEXT NOT NULL,
    pinned INTEGER NOT NULL CHECK(pinned IN (0,1)),
    archived INTEGER NOT NULL CHECK(archived IN (0,1)),
    input_tokens INTEGER NOT NULL,
    output_tokens INTEGER NOT NULL,
    estimated_cost_micros INTEGER NOT NULL,
    last_latency_ms INTEGER NOT NULL,
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL
);
CREATE TABLE chat_messages (
    id TEXT PRIMARY KEY,
    conversation_id TEXT NOT NULL REFERENCES chat_conversations(id) ON UPDATE CASCADE ON DELETE CASCADE,
    role TEXT NOT NULL CHECK(role IN ('user','assistant','system','error')),
    status TEXT NOT NULL CHECK(status IN ('sent','generating','failed','stopped')),
    content TEXT NOT NULL,
    input_tokens INTEGER NOT NULL,
    output_tokens INTEGER NOT NULL,
    cost_micros INTEGER NOT NULL,
    latency_ms INTEGER NOT NULL,
    created_at_ms INTEGER NOT NULL
);
CREATE INDEX chat_messages_conversation_order ON chat_messages(conversation_id,created_at_ms,id);
PRAGMA user_version = 5;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v5_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 6) {
        // Snapshot the provider/model used by every assistant message and
        // seed the compiled built-in catalogs for already-existing OpenAI
        // and Anthropic providers. Stable ids make the migration idempotent.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
ALTER TABLE chat_messages ADD COLUMN provider_id TEXT REFERENCES providers(id) ON UPDATE CASCADE ON DELETE RESTRICT;
ALTER TABLE chat_messages ADD COLUMN model_id TEXT REFERENCES models(id) ON UPDATE CASCADE ON DELETE RESTRICT;

INSERT OR IGNORE INTO models SELECT id||':gpt-5.6-sol',id,'gpt-5.6-sol','GPT-5.6 Sol',400000,128000,0,0,1,CASE WHEN NOT EXISTS(SELECT 1 FROM models m WHERE m.provider_id=providers.id AND m.is_default=1) THEN 1 ELSE 0 END,created_at_ms,updated_at_ms FROM providers WHERE kind='openai';
INSERT OR IGNORE INTO models SELECT id||':gpt-5.6-terra',id,'gpt-5.6-terra','GPT-5.6 Terra',400000,128000,0,0,1,0,created_at_ms,updated_at_ms FROM providers WHERE kind='openai';
INSERT OR IGNORE INTO models SELECT id||':gpt-5.6-luna',id,'gpt-5.6-luna','GPT-5.6 Luna',400000,128000,0,0,1,0,created_at_ms,updated_at_ms FROM providers WHERE kind='openai';
INSERT OR IGNORE INTO models SELECT id||':gpt-5.4',id,'gpt-5.4','GPT-5.4',400000,128000,0,0,1,0,created_at_ms,updated_at_ms FROM providers WHERE kind='openai';
INSERT OR IGNORE INTO models SELECT id||':gpt-5',id,'gpt-5','GPT-5',400000,128000,0,0,1,0,created_at_ms,updated_at_ms FROM providers WHERE kind='openai';
INSERT OR IGNORE INTO models SELECT id||':gpt-5-mini',id,'gpt-5-mini','GPT-5 mini',400000,128000,0,0,1,0,created_at_ms,updated_at_ms FROM providers WHERE kind='openai';
INSERT OR IGNORE INTO models SELECT id||':gpt-5-nano',id,'gpt-5-nano','GPT-5 nano',400000,128000,0,0,1,0,created_at_ms,updated_at_ms FROM providers WHERE kind='openai';

INSERT OR IGNORE INTO models SELECT id||':claude-fable-5',id,'claude-fable-5','Claude Fable 5',200000,64000,0,0,1,CASE WHEN NOT EXISTS(SELECT 1 FROM models m WHERE m.provider_id=providers.id AND m.is_default=1) THEN 1 ELSE 0 END,created_at_ms,updated_at_ms FROM providers WHERE kind='anthropic';
INSERT OR IGNORE INTO models SELECT id||':claude-opus-4-8',id,'claude-opus-4-8','Claude Opus 4.8',200000,64000,0,0,1,0,created_at_ms,updated_at_ms FROM providers WHERE kind='anthropic';
INSERT OR IGNORE INTO models SELECT id||':claude-sonnet-5',id,'claude-sonnet-5','Claude Sonnet 5',200000,64000,0,0,1,0,created_at_ms,updated_at_ms FROM providers WHERE kind='anthropic';
INSERT OR IGNORE INTO models SELECT id||':claude-haiku-4-5-20251001',id,'claude-haiku-4-5-20251001','Claude Haiku 4.5',200000,64000,0,0,1,0,created_at_ms,updated_at_ms FROM providers WHERE kind='anthropic';

PRAGMA user_version = 6;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v6_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 7) {
        // Shared provider-attempt ledger for dynamic Runs, Comparison, and
        // future Chat unification. Outputs remain reviewable artifacts; the
        // database stores only a short preview and optional artifact id.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE model_executions (
    id TEXT PRIMARY KEY NOT NULL,
    source_type TEXT NOT NULL CHECK(source_type IN ('run','comparison','chat')),
    source_id TEXT NOT NULL,
    project_id TEXT REFERENCES projects(id) ON UPDATE CASCADE ON DELETE SET NULL,
    provider_id TEXT NOT NULL REFERENCES providers(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    model_id TEXT NOT NULL REFERENCES models(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    role TEXT NOT NULL,
    position INTEGER NOT NULL CHECK(position >= 0),
    status TEXT NOT NULL CHECK(status IN ('pending','running','completed','partial','failed','cancelled')),
    request_hash TEXT NOT NULL,
    context_hash TEXT NOT NULL,
    output_artifact_id TEXT REFERENCES artifacts(id) ON UPDATE CASCADE ON DELETE SET NULL,
    output_preview TEXT NOT NULL,
    input_tokens INTEGER NOT NULL CHECK(input_tokens >= 0),
    output_tokens INTEGER NOT NULL CHECK(output_tokens >= 0),
    cost_micros INTEGER NOT NULL CHECK(cost_micros >= 0),
    latency_ms INTEGER NOT NULL CHECK(latency_ms >= 0),
    error_code TEXT NOT NULL,
    error_message TEXT NOT NULL,
    created_at_ms INTEGER NOT NULL,
    started_at_ms INTEGER,
    finished_at_ms INTEGER,
    UNIQUE(source_type, source_id, position)
);
CREATE INDEX model_executions_source_idx ON model_executions(source_type,source_id,position);
CREATE INDEX model_executions_project_created_idx ON model_executions(project_id,created_at_ms,id);
CREATE TABLE quality_evaluations (
    id TEXT PRIMARY KEY NOT NULL,
    execution_id TEXT NOT NULL REFERENCES model_executions(id) ON UPDATE CASCADE ON DELETE CASCADE,
    evaluator_kind TEXT NOT NULL CHECK(evaluator_kind IN ('automated','blinded_model','human')),
    evaluator_provider_id TEXT REFERENCES providers(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    evaluator_model_id TEXT REFERENCES models(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    rubric_version TEXT NOT NULL,
    score_basis_points INTEGER NOT NULL CHECK(score_basis_points >= 0 AND score_basis_points <= 10000),
    criteria_json TEXT NOT NULL,
    explanation TEXT NOT NULL,
    evidence TEXT NOT NULL,
    created_at_ms INTEGER NOT NULL
);
CREATE INDEX quality_evaluations_execution_idx ON quality_evaluations(execution_id,created_at_ms,id);
PRAGMA user_version = 7;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v7_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 8) {
        // Run-local dynamic agent rosters. Execution-profile assignments keep
        // their deliberately small canonical schema; only the immutable run
        // snapshot needs arbitrary stage ids and per-agent prompt metadata.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
ALTER TABLE run_stage_assignments RENAME TO run_stage_assignments_v7;
CREATE TABLE run_stage_assignments (
    run_id TEXT NOT NULL REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,
    stage_type TEXT NOT NULL,
    provider_id TEXT NOT NULL REFERENCES providers(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    model_id TEXT NOT NULL REFERENCES models(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    timeout_ms INTEGER NOT NULL CHECK(timeout_ms > 0),
    agent_name TEXT NOT NULL DEFAULT '',
    role TEXT NOT NULL DEFAULT '',
    instructions TEXT NOT NULL DEFAULT '',
    position INTEGER NOT NULL CHECK(position >= 0),
    lead_architect INTEGER NOT NULL CHECK(lead_architect IN (0,1)),
    PRIMARY KEY (run_id, stage_type),
    UNIQUE (run_id, position)
);
INSERT INTO run_stage_assignments
    (run_id,stage_type,provider_id,model_id,timeout_ms,agent_name,role,instructions,position,lead_architect)
SELECT run_id,stage_type,provider_id,model_id,timeout_ms,'',stage_type,'',
       CASE stage_type WHEN 'architect' THEN 0 WHEN 'implementation' THEN 1 ELSE 2 END,
       CASE WHEN stage_type='architect' THEN 1 ELSE 0 END
FROM run_stage_assignments_v7;
DROP TABLE run_stage_assignments_v7;
PRAGMA user_version = 8;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v8_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 9) {
        // Comparison owns only session metadata. Candidate outputs, usage,
        // latency, cost and quality stay normalized in model_executions and
        // quality_evaluations, keyed by source_type='comparison'/source_id.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE comparison_sessions (
    id TEXT PRIMARY KEY NOT NULL,
    project_id TEXT NOT NULL REFERENCES projects(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    title TEXT NOT NULL,
    prompt TEXT NOT NULL,
    status TEXT NOT NULL CHECK(status IN ('pending','running','completed','partial','failed','cancelled')),
    request_hash TEXT NOT NULL,
    context_hash TEXT NOT NULL,
    preferred_execution_id TEXT REFERENCES model_executions(id) ON UPDATE CASCADE ON DELETE SET NULL,
    synthesis_selection_json TEXT NOT NULL DEFAULT '[]',
    synthesis_execution_id TEXT REFERENCES model_executions(id) ON UPDATE CASCADE ON DELETE SET NULL,
    error_code TEXT NOT NULL,
    error_message TEXT NOT NULL,
    created_at_ms INTEGER NOT NULL,
    started_at_ms INTEGER,
    finished_at_ms INTEGER,
    revision INTEGER NOT NULL CHECK(revision >= 0)
);
CREATE INDEX comparison_sessions_project_created_idx
    ON comparison_sessions(project_id,created_at_ms,id);
CREATE INDEX comparison_sessions_status_idx ON comparison_sessions(status);
PRAGMA user_version = 9;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v9_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 10) {
        // Full provider output is required by durable Comparison evaluation
        // and synthesis. output_preview stays intentionally compact for list
        // projections; output_content is the authoritative response body.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
ALTER TABLE model_executions ADD COLUMN output_content TEXT NOT NULL DEFAULT '';
PRAGMA user_version = 10;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v10_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 11) {
        // Durable execution-graph policy for production Runs. Existing rows
        // retain sequential-safe defaults, while new runs can persist bounded
        // parallelism, explicit dependencies, retry limits and a fallback
        // provider/model pair. ModelExecution.attempt makes every retry
        // independently auditable in the shared ledger.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
ALTER TABLE runs ADD COLUMN max_parallelism INTEGER NOT NULL DEFAULT 4
    CHECK(max_parallelism >= 1 AND max_parallelism <= 32);
ALTER TABLE run_stage_assignments ADD COLUMN dependencies_json TEXT NOT NULL DEFAULT '[]';
ALTER TABLE run_stage_assignments ADD COLUMN max_attempts INTEGER NOT NULL DEFAULT 1
    CHECK(max_attempts >= 1 AND max_attempts <= 10);
ALTER TABLE run_stage_assignments ADD COLUMN fallback_provider_id TEXT
    REFERENCES providers(id) ON UPDATE CASCADE ON DELETE RESTRICT;
ALTER TABLE run_stage_assignments ADD COLUMN fallback_model_id TEXT
    REFERENCES models(id) ON UPDATE CASCADE ON DELETE RESTRICT;
ALTER TABLE model_executions ADD COLUMN attempt INTEGER NOT NULL DEFAULT 1
    CHECK(attempt >= 1);
PRAGMA user_version = 11;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v11_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 12) {
        // One typed, durable application-preferences aggregate. Secrets and
        // destructive-action state never belong here; provider credentials
        // remain exclusively in ICredentialStore.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE application_settings (
    id INTEGER PRIMARY KEY CHECK(id = 1),
    theme_mode TEXT NOT NULL CHECK(theme_mode IN ('light','dark','transparent')),
    interface_scale_percent INTEGER NOT NULL
        CHECK(interface_scale_percent = 0 OR interface_scale_percent BETWEEN 75 AND 200),
    compact_mode INTEGER NOT NULL CHECK(compact_mode IN (0,1)),
    reduce_animations INTEGER NOT NULL CHECK(reduce_animations IN (0,1)),
    run_context_policy TEXT NOT NULL
        CHECK(run_context_policy IN ('active_project','ask_every_time','no_project')),
    default_execution_profile_id TEXT
        REFERENCES execution_profiles(id) ON UPDATE CASCADE ON DELETE SET NULL,
    default_timeout_ms INTEGER NOT NULL CHECK(default_timeout_ms BETWEEN 1000 AND 86400000),
    default_max_parallel_agents INTEGER NOT NULL
        CHECK(default_max_parallel_agents BETWEEN 1 AND 32),
    confirm_many_models INTEGER NOT NULL CHECK(confirm_many_models IN (0,1)),
    chat_context_policy TEXT NOT NULL
        CHECK(chat_context_policy IN ('active_project','ask_every_time','no_project')),
    default_chat_provider_id TEXT
        REFERENCES providers(id) ON UPDATE CASCADE ON DELETE SET NULL,
    default_chat_model_id TEXT
        REFERENCES models(id) ON UPDATE CASCADE ON DELETE SET NULL,
    chat_history_limit INTEGER NOT NULL CHECK(chat_history_limit BETWEEN 1 AND 10000),
    revision INTEGER NOT NULL CHECK(revision >= 0),
    updated_at_ms INTEGER NOT NULL
);
INSERT INTO application_settings VALUES (
    1,'dark',0,0,0,'active_project',NULL,300000,4,1,
    'active_project',NULL,NULL,200,0,0);
PRAGMA user_version = 12;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v12_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 13) {
        // Schema v11 added ModelExecution.attempt but the original v7 UNIQUE
        // constraint still admitted only one row per logical stage/candidate
        // position. A production Run therefore persisted attempt 1 and then
        // failed before retry/fallback could call a provider. Rebuild the
        // table so every attempt is independently durable while preserving
        // all existing execution ids referenced by Comparison and quality
        // evaluation rows.
        const auto foreignKeysOff = execute(
            impl->handle, "PRAGMA foreign_keys = OFF;", "migration.v13_foreign_keys_off_failed");
        if (!foreignKeysOff.success()) {
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*foreignKeysOff.error());
        }
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE model_executions_v13 (
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
    finished_at_ms INTEGER,
    UNIQUE(source_type, source_id, position, attempt)
);
INSERT INTO model_executions_v13 (
    id,source_type,source_id,project_id,provider_id,model_id,role,position,attempt,status,
    request_hash,context_hash,output_artifact_id,output_content,output_preview,input_tokens,
    output_tokens,cost_micros,latency_ms,error_code,error_message,created_at_ms,started_at_ms,
    finished_at_ms)
SELECT
    id,source_type,source_id,project_id,provider_id,model_id,role,position,attempt,status,
    request_hash,context_hash,output_artifact_id,output_content,output_preview,input_tokens,
    output_tokens,cost_micros,latency_ms,error_code,error_message,created_at_ms,started_at_ms,
    finished_at_ms
FROM model_executions;
DROP TABLE model_executions;
ALTER TABLE model_executions_v13 RENAME TO model_executions;
CREATE INDEX model_executions_source_idx
    ON model_executions(source_type,source_id,position,attempt);
CREATE INDEX model_executions_project_created_idx
    ON model_executions(project_id,created_at_ms,id);
PRAGMA user_version = 13;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v13_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            static_cast<void>(execute(
                impl->handle, "PRAGMA foreign_keys = ON;", "migration.foreign_keys_restore_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
        const auto foreignKeysOn = execute(
            impl->handle, "PRAGMA foreign_keys = ON;", "migration.v13_foreign_keys_on_failed");
        if (!foreignKeysOn.success()) {
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*foreignKeysOn.error());
        }
        Statement foreignKeyCheck;
        const auto foreignKeyCheckPrepared = prepare(
            impl->handle, "PRAGMA foreign_key_check;", foreignKeyCheck);
        if (!foreignKeyCheckPrepared.success()) {
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(
                *foreignKeyCheckPrepared.error());
        }
        const int foreignKeyCheckStep = sqlite3_step(foreignKeyCheck.get());
        if (foreignKeyCheckStep == SQLITE_ROW) {
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(error(
                "migration.v13_foreign_key_check_failed",
                "Database contains an invalid foreign-key reference after migration"));
        }
        if (foreignKeyCheckStep != SQLITE_DONE) {
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(databaseError(
                impl->handle,
                "migration.v13_foreign_key_check_failed",
                "Could not validate foreign keys after migration"));
        }
    }

    if (schemaVersion < 14) {
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS project_tasks (
    id TEXT PRIMARY KEY NOT NULL,
    project_id TEXT NOT NULL REFERENCES projects(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    title TEXT NOT NULL,
    description TEXT NOT NULL,
    status TEXT NOT NULL CHECK(status IN ('backlog','planned','in_progress','blocked','done','cancelled')),
    priority TEXT NOT NULL CHECK(priority IN ('low','normal','high','critical')),
    requires_attention INTEGER NOT NULL CHECK(requires_attention IN (0,1)),
    due_at_ms INTEGER,
    planned_run_at_ms INTEGER,
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS project_tasks_project_status_idx
    ON project_tasks(project_id,status,updated_at_ms DESC);
CREATE TABLE IF NOT EXISTS task_run_links (
    task_id TEXT NOT NULL REFERENCES project_tasks(id) ON UPDATE CASCADE ON DELETE CASCADE,
    run_id TEXT NOT NULL UNIQUE REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,
    PRIMARY KEY(task_id,run_id)
);
PRAGMA user_version = 14;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v14_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 15) {
        // Seed the user-approved reference price table only for built-in
        // models that are still completely unpriced. This one-time migration
        // never overwrites a custom non-zero tariff; after v15 users may also
        // deliberately edit either price (including back to zero) in the UI.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
UPDATE models SET
    input_price_micros_per_million_tokens = CASE api_model_id
        WHEN 'gpt-5' THEN 1250000 WHEN 'gpt-5-mini' THEN 250000
        WHEN 'gpt-5-nano' THEN 50000 WHEN 'gpt-5.4' THEN 2000000
        WHEN 'gpt-5.6-luna' THEN 1000000 WHEN 'gpt-5.6-sol' THEN 1500000
        WHEN 'gpt-5.6-terra' THEN 2500000
        WHEN 'claude-fable-5' THEN 10000000 WHEN 'claude-haiku-4-5-20251001' THEN 1000000
        WHEN 'claude-opus-4-8' THEN 5000000 WHEN 'claude-sonnet-5' THEN 3000000
        ELSE input_price_micros_per_million_tokens END,
    output_price_micros_per_million_tokens = CASE api_model_id
        WHEN 'gpt-5' THEN 10000000 WHEN 'gpt-5-mini' THEN 1250000
        WHEN 'gpt-5-nano' THEN 200000 WHEN 'gpt-5.4' THEN 8000000
        WHEN 'gpt-5.6-luna' THEN 5000000 WHEN 'gpt-5.6-sol' THEN 7500000
        WHEN 'gpt-5.6-terra' THEN 15000000
        WHEN 'claude-fable-5' THEN 50000000 WHEN 'claude-haiku-4-5-20251001' THEN 5000000
        WHEN 'claude-opus-4-8' THEN 25000000 WHEN 'claude-sonnet-5' THEN 15000000
        ELSE output_price_micros_per_million_tokens END
WHERE input_price_micros_per_million_tokens = 0
  AND output_price_micros_per_million_tokens = 0
  AND api_model_id IN (
      'gpt-5','gpt-5-mini','gpt-5-nano','gpt-5.4','gpt-5.6-luna','gpt-5.6-sol','gpt-5.6-terra',
      'claude-fable-5','claude-haiku-4-5-20251001','claude-opus-4-8','claude-sonnet-5');
PRAGMA user_version = 15;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v15_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 16) {
        // Align previously seeded built-in GPT entries with the 400K context
        // supplied alongside the reference pricing table. Preserve any model
        // whose context was already customized away from the old 128K seed.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
UPDATE models SET context_window_tokens = 400000
WHERE context_window_tokens = 128000
  AND api_model_id IN (
      'gpt-5','gpt-5-mini','gpt-5-nano','gpt-5.4',
      'gpt-5.6-luna','gpt-5.6-sol','gpt-5.6-terra');
PRAGMA user_version = 16;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v16_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 17) {
        // Hybrid chat-memory layer: summarized slices of conversation history.
        // Purely additive — chat_messages stays the untouched source of truth,
        // and deleting a conversation cascades into its memory segments.
        // IF NOT EXISTS keeps this idempotent for the schema-v12 downgrade
        // fixture, which replays every migration after v12 on a database that
        // already contains the final schema.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS chat_memory_segments (
    id TEXT PRIMARY KEY,
    conversation_id TEXT NOT NULL REFERENCES chat_conversations(id) ON UPDATE CASCADE ON DELETE CASCADE,
    first_message_id TEXT NOT NULL,
    last_message_id TEXT NOT NULL,
    summary TEXT NOT NULL,
    source_token_count INTEGER NOT NULL DEFAULT 0,
    summary_token_count INTEGER NOT NULL DEFAULT 0,
    created_at_ms INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS chat_memory_segments_conversation_order
    ON chat_memory_segments(conversation_id,created_at_ms,id);
PRAGMA user_version = 17;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v17_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 18) {
        // Project-level memory layer: append-only summaries of run outcomes,
        // separate from the hand-authored Project::description. Purely
        // additive — no existing table changes. run_id intentionally carries
        // no foreign key: memory belongs to the project and must survive the
        // deletion of any single run. IF NOT EXISTS keeps this idempotent for
        // the schema-v12 downgrade fixture, which replays every migration
        // after v12 on a database that already contains the final schema.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS project_memory_segments (
    id TEXT PRIMARY KEY,
    project_id TEXT NOT NULL REFERENCES projects(id) ON UPDATE CASCADE ON DELETE CASCADE,
    run_id TEXT NOT NULL,
    summary TEXT NOT NULL,
    source_token_count INTEGER NOT NULL DEFAULT 0,
    summary_token_count INTEGER NOT NULL DEFAULT 0,
    created_at_ms INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS project_memory_segments_project_order
    ON project_memory_segments(project_id,created_at_ms,id);
PRAGMA user_version = 18;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v18_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 19) {
        // Per-run workspace override: nullable, no default, absent means
        // "use the project's own rootPath". Unlike v18's CREATE TABLE, SQLite's
        // ALTER TABLE ADD COLUMN has no IF NOT EXISTS form, so idempotency for
        // the schema-v12 downgrade fixture (which replays every migration
        // after v12, including this one, on a database that may already have
        // the column) is enforced explicitly here via PRAGMA table_info.
        bool hasWorkspaceColumn = false;
        {
            Statement info;
            const auto prepared = prepare(impl->handle, "PRAGMA table_info(runs);", info);
            if (!prepared.success()) {
                return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*prepared.error());
            }
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<std::shared_ptr<SqliteDatabase>>::failure(databaseError(
                        impl->handle, "migration.v19_failed", "Could not inspect runs table schema"));
                }
                if (columnText(info.get(), 1) == "workspace_path") {
                    hasWorkspaceColumn = true;
                    break;
                }
            }
        }
        const std::string migration = hasWorkspaceColumn
            ? "BEGIN IMMEDIATE;\nPRAGMA user_version = 19;\nCOMMIT;"
            : "BEGIN IMMEDIATE;\nALTER TABLE runs ADD COLUMN workspace_path TEXT;\n"
              "PRAGMA user_version = 19;\nCOMMIT;";
        const auto migrated = execute(impl->handle, migration.c_str(), "migration.v19_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 20) {
        // Stage 1 safe change-proposal pipeline metadata.
        // proposed_change_sets: one row per change-set (no content blobs).
        // proposed_file_changes: one row per file operation.
        // proposal_validation_findings: one row per finding (ordered by ordinal).
        // Content blobs live exclusively in the FileChangeProposalStore filesystem.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS proposed_change_sets (
    id TEXT PRIMARY KEY,
    run_id TEXT NOT NULL,
    project_id TEXT NOT NULL,
    provider_id TEXT NOT NULL DEFAULT '',
    model_id TEXT NOT NULL DEFAULT '',
    model_execution_id TEXT,
    agent_role TEXT NOT NULL DEFAULT '',
    summary TEXT NOT NULL DEFAULT '',
    base_revision TEXT NOT NULL DEFAULT '',
    policy_snapshot_hash TEXT NOT NULL DEFAULT '',
    artifact_relative_root TEXT NOT NULL DEFAULT '',
    state TEXT NOT NULL DEFAULT 'received',
    schema_version TEXT NOT NULL DEFAULT 'proposal.v1',
    created_at_ms INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS proposed_change_sets_run
    ON proposed_change_sets(run_id, created_at_ms);
CREATE INDEX IF NOT EXISTS proposed_change_sets_project
    ON proposed_change_sets(project_id, created_at_ms);

CREATE TABLE IF NOT EXISTS proposed_file_changes (
    id TEXT PRIMARY KEY,
    change_set_id TEXT NOT NULL REFERENCES proposed_change_sets(id) ON DELETE CASCADE,
    change_id TEXT NOT NULL DEFAULT '',
    operation TEXT NOT NULL,
    relative_path TEXT NOT NULL,
    source_path TEXT,
    expected_original_hash TEXT,
    ordinal INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS proposed_file_changes_cs
    ON proposed_file_changes(change_set_id, ordinal);

CREATE TABLE IF NOT EXISTS proposal_validation_findings (
    rowid INTEGER PRIMARY KEY AUTOINCREMENT,
    change_set_id TEXT NOT NULL REFERENCES proposed_change_sets(id) ON DELETE CASCADE,
    code TEXT NOT NULL,
    message TEXT NOT NULL,
    change_id TEXT NOT NULL DEFAULT '',
    ordinal INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS proposal_validation_findings_cs
    ON proposal_validation_findings(change_set_id, ordinal);

PRAGMA user_version = 20;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v20_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 21) {
        // Stage 3 audit persistence.
        // agent_tool_sessions: one row per agent tool session inside a Run.
        // agent_provider_turns: one row per model-response turn in a session.
        // agent_tool_calls: one row per individual tool call audit record.
        //
        // Security constraints enforced at this schema level:
        //   - no TEXT column holds raw arguments, raw results, or file content;
        //   - no TEXT column holds absolute workspace paths;
        //   - arguments/result content is represented only as sha256 hex strings;
        //   - artifact references are relative IDs, never filesystem paths.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS agent_tool_sessions (
    id TEXT PRIMARY KEY,
    run_id TEXT NOT NULL,
    provider_id TEXT NOT NULL DEFAULT '',
    model_id TEXT NOT NULL DEFAULT '',
    state TEXT NOT NULL DEFAULT 'active'
        CHECK (state IN ('active','completed','interrupted')),
    total_turns INTEGER NOT NULL DEFAULT 0,
    total_calls INTEGER NOT NULL DEFAULT 0,
    total_returned_bytes INTEGER NOT NULL DEFAULT 0,
    created_at_ms INTEGER NOT NULL,
    finished_at_ms INTEGER
);
CREATE INDEX IF NOT EXISTS agent_tool_sessions_run
    ON agent_tool_sessions(run_id, created_at_ms);

CREATE TABLE IF NOT EXISTS agent_provider_turns (
    id TEXT PRIMARY KEY,
    session_id TEXT NOT NULL REFERENCES agent_tool_sessions(id) ON DELETE CASCADE,
    turn_index INTEGER NOT NULL,
    finish_reason TEXT NOT NULL DEFAULT '',
    input_tokens INTEGER NOT NULL DEFAULT 0,
    output_tokens INTEGER NOT NULL DEFAULT 0,
    call_count INTEGER NOT NULL DEFAULT 0,
    created_at_ms INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS agent_provider_turns_session
    ON agent_provider_turns(session_id, turn_index);

CREATE TABLE IF NOT EXISTS agent_tool_calls (
    id TEXT PRIMARY KEY,
    session_id TEXT NOT NULL REFERENCES agent_tool_sessions(id) ON DELETE CASCADE,
    run_id TEXT NOT NULL DEFAULT '',
    provider_call_id TEXT NOT NULL DEFAULT '',
    tool_name TEXT NOT NULL,
    state TEXT NOT NULL DEFAULT 'requested'
        CHECK (state IN ('requested','running','completed','failed','interrupted')),
    arguments_sha256 TEXT NOT NULL DEFAULT '',
    result_sha256 TEXT NOT NULL DEFAULT '',
    result_artifact_id TEXT,
    error_code TEXT NOT NULL DEFAULT '',
    returned_bytes INTEGER NOT NULL DEFAULT 0,
    truncated INTEGER NOT NULL DEFAULT 0,
    created_at_ms INTEGER NOT NULL,
    finished_at_ms INTEGER
);
CREATE INDEX IF NOT EXISTS agent_tool_calls_session
    ON agent_tool_calls(session_id, created_at_ms);
CREATE INDEX IF NOT EXISTS agent_tool_calls_run
    ON agent_tool_calls(run_id, created_at_ms);

PRAGMA user_version = 21;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v21_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 22) {
        // Raise only the old built-in run timeout default. User-customized
        // values must remain untouched.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
UPDATE application_settings
SET default_timeout_ms = 300000
WHERE id = 1 AND default_timeout_ms = 120000;
PRAGMA user_version = 22;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v22_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 23) {
        // Per-agent workspace tool access: lets a roster agent run through
        // the same read-only-workspace-tools + propose_change_set pipeline
        // as read-only analysis, instead of a single-shot completion.
        // Existing rows default to false (their unchanged current behavior).
        // SQLite's ALTER TABLE ADD COLUMN has no IF NOT EXISTS form, so
        // idempotency for the schema-v12 downgrade fixture (which replays
        // every migration after v12, including this one, on a database that
        // may already have the column) is enforced explicitly here via
        // PRAGMA table_info, the same pattern schema v19 already established.
        bool hasWorkspaceAccessColumn = false;
        {
            Statement info;
            const auto prepared = prepare(impl->handle, "PRAGMA table_info(run_stage_assignments);", info);
            if (!prepared.success()) {
                return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*prepared.error());
            }
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<std::shared_ptr<SqliteDatabase>>::failure(databaseError(
                        impl->handle, "migration.v23_failed",
                        "Could not inspect run_stage_assignments table schema"));
                }
                if (columnText(info.get(), 1) == "workspace_access") {
                    hasWorkspaceAccessColumn = true;
                    break;
                }
            }
        }
        const std::string migration = hasWorkspaceAccessColumn
            ? "BEGIN IMMEDIATE;\nPRAGMA user_version = 23;\nCOMMIT;"
            : "BEGIN IMMEDIATE;\n"
              "ALTER TABLE run_stage_assignments ADD COLUMN workspace_access INTEGER NOT NULL DEFAULT 0\n"
              "    CHECK(workspace_access IN (0,1));\n"
              "PRAGMA user_version = 23;\nCOMMIT;";
        const auto migrated = execute(impl->handle, migration.c_str(), "migration.v23_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 24) {
        // Before v24, NULL meant "look up the Project's current root_path".
        // That made historical Runs mutable: changing a Project path could
        // retarget Retry and proposal Apply. Backfill the best available
        // snapshot for legacy rows. New Runs always persist a canonical path
        // in RunManager, so no future row depends on this fallback.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
UPDATE runs
SET workspace_path = (
    SELECT projects.root_path
    FROM projects
    WHERE projects.id = runs.project_id
)
WHERE workspace_path IS NULL;
PRAGMA user_version = 24;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v24_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 25) {
        // Approval audit trail for ChangeSetApplyService::apply(): one row
        // per change set, written once by approve() (snapshot_hash/
        // approved_at_ms/approved_by) and updated once more by
        // recordApplyOutcome() once apply() actually runs (the applied_*/
        // apply_* columns, NULL until then). apply() refuses to run at all
        // without a matching row here - see ChangeSetApplyService.cpp.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS change_set_approvals (
    change_set_id TEXT PRIMARY KEY REFERENCES proposed_change_sets(id) ON DELETE CASCADE,
    snapshot_hash TEXT NOT NULL,
    approved_at_ms INTEGER NOT NULL,
    approved_by TEXT,
    applied_at_ms INTEGER,
    apply_result TEXT,
    apply_error_code TEXT,
    apply_hash_log TEXT
);
PRAGMA user_version = 25;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v25_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 26) {
        // Saved, not-yet-submitted New Run configurations ("drafts"). Mirrors
        // run_stage_assignments' rich per-agent column set (not
        // execution_profile_assignments' deliberately narrow one - see
        // RunDraftTypes.hpp) since a draft must round-trip anything
        // NewRunDialog's custom roster editor can produce. launch_mode is
        // the RunLaunchMode enum ordinal (0=Mock,1=SavedExecutionProfile,
        // 2=CustomRoster); reopening a draft never deletes it, so a
        // frequently-reused one doubles as a lightweight template.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS run_drafts (
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
    updated_at_ms INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS run_drafts_updated_idx ON run_drafts(updated_at_ms, id);
CREATE INDEX IF NOT EXISTS run_drafts_project_idx ON run_drafts(project_id);
CREATE TABLE IF NOT EXISTS run_draft_assignments (
    draft_id TEXT NOT NULL REFERENCES run_drafts(id) ON UPDATE CASCADE ON DELETE CASCADE,
    stage_type TEXT NOT NULL,
    provider_id TEXT NOT NULL REFERENCES providers(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    model_id TEXT NOT NULL REFERENCES models(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    timeout_ms INTEGER NOT NULL CHECK(timeout_ms > 0),
    agent_name TEXT NOT NULL DEFAULT '',
    role TEXT NOT NULL DEFAULT '',
    instructions TEXT NOT NULL DEFAULT '',
    position INTEGER NOT NULL CHECK(position >= 0),
    lead_architect INTEGER NOT NULL CHECK(lead_architect IN (0,1)),
    dependencies_json TEXT NOT NULL DEFAULT '[]',
    max_attempts INTEGER NOT NULL DEFAULT 1 CHECK(max_attempts >= 1 AND max_attempts <= 10),
    fallback_provider_id TEXT,
    fallback_model_id TEXT,
    workspace_access INTEGER NOT NULL DEFAULT 0 CHECK(workspace_access IN (0,1)),
    PRIMARY KEY (draft_id, stage_type),
    UNIQUE(draft_id, position)
);
PRAGMA user_version = 26;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v26_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 27) {
        // Proposal audit-evidence gate (REMAINING_GAPS_EXECUTION_PLAN.md
        // Phase 1): a typed reason for a NoChangesNeeded change set whose
        // empty changes[] was not backed by any successful workspace
        // inspection this session, plus a self-referencing link from a
        // repair retry's persisted row back to the attempt it superseded.
        // Existing table from v20 - guarded like v23's ALTER TABLE ADD
        // COLUMN, since SQLite has no ADD COLUMN IF NOT EXISTS and a
        // downgrade-fixture test may replay this against an already-current
        // schema.
        bool hasSupersedesColumn = false;
        {
            Statement info;
            const auto prepared = prepare(impl->handle, "PRAGMA table_info(proposed_change_sets);", info);
            if (!prepared.success()) return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*prepared.error());
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<std::shared_ptr<SqliteDatabase>>::failure(databaseError(
                        impl->handle, "migration.v27_failed", "Could not inspect proposed_change_sets table schema"));
                }
                if (columnText(info.get(), 1) == "supersedes_change_set_id") { hasSupersedesColumn = true; break; }
            }
        }
        const std::string migration = hasSupersedesColumn
            ? "BEGIN IMMEDIATE;\nPRAGMA user_version = 27;\nCOMMIT;"
            : "BEGIN IMMEDIATE;\n"
              "ALTER TABLE proposed_change_sets ADD COLUMN missing_evidence_code TEXT;\n"
              "ALTER TABLE proposed_change_sets ADD COLUMN missing_evidence_message TEXT;\n"
              "ALTER TABLE proposed_change_sets ADD COLUMN supersedes_change_set_id TEXT "
              "    REFERENCES proposed_change_sets(id) ON UPDATE CASCADE ON DELETE SET NULL;\n"
              "PRAGMA user_version = 27;\nCOMMIT;";
        const auto migrated = execute(impl->handle, migration.c_str(), "migration.v27_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 28) {
        // Phase 2 execution contract. The whole accepted manifest is stored
        // as one versioned document keyed 1:1 by Run. There is intentionally
        // no update operation; the trigger makes that immutability true even
        // for direct SQL callers while ON DELETE CASCADE still permits normal
        // Run lifecycle cleanup.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS execution_manifests (
    run_id TEXT PRIMARY KEY NOT NULL
        REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,
    schema_version TEXT NOT NULL,
    manifest_json TEXT NOT NULL,
    created_at_ms INTEGER NOT NULL
);
CREATE TRIGGER IF NOT EXISTS execution_manifests_reject_update
BEFORE UPDATE ON execution_manifests
BEGIN
    SELECT RAISE(ABORT, 'execution_manifest_immutable');
END;
PRAGMA user_version = 28;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v28_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 29) {
        // Phase 5 (manifest UI): per-task execution progress, separate from
        // the frozen-once execution_manifests document above - a task's
        // status legitimately changes several times while its Run is in
        // flight (and, through the bounded retry path, can even revisit
        // Pending), which the immutable manifest table above must never do.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS manifest_task_executions (
    run_id TEXT NOT NULL REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,
    task_id TEXT NOT NULL,
    status TEXT NOT NULL,
    error_code TEXT NOT NULL DEFAULT '',
    error_message TEXT NOT NULL DEFAULT '',
    change_set_id TEXT,
    attempts INTEGER NOT NULL DEFAULT 0,
    output_summary TEXT NOT NULL DEFAULT '',
    updated_at_ms INTEGER NOT NULL,
    PRIMARY KEY(run_id, task_id)
);
PRAGMA user_version = 29;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v29_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 30) {
        // AG-07: durable human reject/discard decision, kept strictly
        // separate from proposed_change_sets.state (validator truth) and
        // from change_set_approvals (the positive approve/apply audit
        // trail) - see ProposalReviewTypes.hpp. One row per change set;
        // recording a rejection is that change set's terminal human
        // decision, so there is no update path, only insert-once + read.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS change_set_reviewer_rejections (
    change_set_id TEXT PRIMARY KEY REFERENCES proposed_change_sets(id) ON DELETE CASCADE,
    rejected_at_ms INTEGER NOT NULL,
    rejected_by TEXT,
    reason TEXT
);
PRAGMA user_version = 30;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v30_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 31) {
        // AG-11: durable provenance for a project-memory compaction attempt,
        // kept even when the attempt failed or was cancelled - see
        // ProjectMemoryCompactionTypes.hpp. provider_id/model_id are plain
        // text (not a foreign key): a compaction record is a historical fact
        // that must remain readable even after the provider/model catalog
        // entry it used is later deleted, unlike model_executions' stricter
        // in-use protection.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS project_memory_compaction_records (
    id TEXT PRIMARY KEY,
    project_id TEXT NOT NULL REFERENCES projects(id) ON UPDATE CASCADE ON DELETE CASCADE,
    provider_id TEXT NOT NULL,
    model_id TEXT NOT NULL,
    source_segment_ids TEXT NOT NULL,
    source_content_hash TEXT NOT NULL,
    result_segment_id TEXT,
    input_tokens INTEGER NOT NULL DEFAULT 0,
    output_tokens INTEGER NOT NULL DEFAULT 0,
    cost_micros INTEGER NOT NULL DEFAULT 0,
    status TEXT NOT NULL CHECK(status IN ('completed','failed','cancelled')),
    error_code TEXT NOT NULL DEFAULT '',
    error_message TEXT NOT NULL DEFAULT '',
    created_at_ms INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS project_memory_compaction_records_project_order
    ON project_memory_compaction_records(project_id,created_at_ms,id);
PRAGMA user_version = 31;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v31_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 32) {
        // AG-04: durable, terminal-only evidence for a task whose worker
        // resolved as no-op (ManifestWorkerNoChanges) - see
        // IManifestTaskExecutionRepository.hpp's doc comment on why this is a
        // separate table from manifest_task_executions rather than nullable
        // columns bolted onto that hot-write table.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS manifest_task_no_changes (
    run_id TEXT NOT NULL REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,
    task_id TEXT NOT NULL,
    schema_version TEXT NOT NULL,
    reason TEXT NOT NULL,
    summary TEXT NOT NULL DEFAULT '',
    criterion_evidence TEXT NOT NULL DEFAULT '[]',
    inspected_relative_paths TEXT NOT NULL DEFAULT '[]',
    updated_at_ms INTEGER NOT NULL,
    PRIMARY KEY(run_id, task_id)
);
PRAGMA user_version = 32;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v32_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 33) {
        // Existing rows were created when evidence-backed planner no-op was
        // always accepted. Preserve their exact Retry semantics, while every
        // new Run explicitly writes the safe RequireManifest default.
        bool hasPlanningCompletionPolicyColumn = false;
        {
            Statement info;
            const auto prepared = prepare(impl->handle, "PRAGMA table_info(runs);", info);
            if (!prepared.success()) {
                return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*prepared.error());
            }
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<std::shared_ptr<SqliteDatabase>>::failure(databaseError(
                        impl->handle, "migration.v33_failed",
                        "Could not inspect runs table schema"));
                }
                if (columnText(info.get(), 1) == "planning_completion_policy") {
                    hasPlanningCompletionPolicyColumn = true;
                    break;
                }
            }
        }
        const std::string migration = hasPlanningCompletionPolicyColumn
            ? "BEGIN IMMEDIATE;\nPRAGMA user_version = 33;\nCOMMIT;"
            : "BEGIN IMMEDIATE;\n"
              "ALTER TABLE runs ADD COLUMN planning_completion_policy TEXT NOT NULL\n"
              "    DEFAULT 'allow_evidence_backed_no_changes'\n"
              "    CHECK(planning_completion_policy IN "
              "('require_manifest','allow_evidence_backed_no_changes'));\n"
              "PRAGMA user_version = 33;\nCOMMIT;";
        const auto migrated = execute(
            impl->handle, migration.c_str(), "migration.v33_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 34) {
        // AG-14: the durable Project Work Ledger. Two tables, the same
        // "immutable terminal row vs. append-only evidence" split as
        // manifest_task_no_changes above: manifest_work_ledger_entries is
        // written exactly once per terminal Manifest Run (see
        // ManifestWorkLedgerService::recordTerminalRun), while
        // manifest_work_ledger_transitions accumulates afterward as a human
        // reviews/applies that Run's proposals - never FK'd to the entries
        // table itself, since a transition can occur for a change set whose
        // owning Run predates this feature, is not a Manifest-workflow Run
        // at all, or has not yet gone terminal.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS manifest_work_ledger_entries (
    run_id TEXT PRIMARY KEY REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,
    project_id TEXT NOT NULL,
    schema_version TEXT NOT NULL,
    terminal_status TEXT NOT NULL,
    terminal_reason TEXT NOT NULL DEFAULT '',
    baseline_fingerprint TEXT NOT NULL DEFAULT '',
    disposition TEXT NOT NULL,
    task_dispositions TEXT NOT NULL DEFAULT '[]',
    proposal_ids TEXT NOT NULL DEFAULT '[]',
    affected_relative_paths TEXT NOT NULL DEFAULT '[]',
    verification_summary TEXT NOT NULL DEFAULT '',
    remaining_work_note TEXT NOT NULL DEFAULT '',
    created_at_ms INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_manifest_work_ledger_entries_project
    ON manifest_work_ledger_entries(project_id, created_at_ms);
CREATE TABLE IF NOT EXISTS manifest_work_ledger_transitions (
    id TEXT PRIMARY KEY,
    run_id TEXT NOT NULL REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,
    change_set_id TEXT NOT NULL,
    kind TEXT NOT NULL,
    occurred_at_ms INTEGER NOT NULL,
    actor TEXT,
    reason TEXT,
    post_action_workspace_fingerprint TEXT
);
CREATE INDEX IF NOT EXISTS idx_manifest_work_ledger_transitions_run
    ON manifest_work_ledger_transitions(run_id);
PRAGMA user_version = 34;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v34_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 35) {
        // AG-14.2: v34 shipped (in commit 7d229c9) without
        // manifest_schema_version/proposal_lineage/verification_identity_json.
        // AG-14.1 mistakenly added these columns to the v34 CREATE TABLE
        // in place, which left any database that had already been opened by
        // that original binary permanently stuck at user_version=34 without
        // ever seeing the new columns. This is now its own migration step so
        // that case is upgraded correctly via ALTER TABLE instead of being
        // silently skipped.
        //
        // Same defensive existence check as the v33 migration above (see its
        // own comment on hasPlanningCompletionPolicyColumn): this table can
        // be observed already carrying these columns when schemaVersion is
        // (re-)evaluated below 35 - re-running a bare ALTER TABLE ADD COLUMN
        // in that case would fail with "duplicate column name" instead of
        // being the idempotent no-op every other migration in this file is.
        bool hasManifestSchemaVersionColumn = false;
        {
            Statement info;
            const auto prepared = prepare(
                impl->handle, "PRAGMA table_info(manifest_work_ledger_entries);", info);
            if (!prepared.success()) {
                return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*prepared.error());
            }
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<std::shared_ptr<SqliteDatabase>>::failure(databaseError(
                        impl->handle, "migration.v35_failed",
                        "Could not inspect manifest_work_ledger_entries table schema"));
                }
                if (columnText(info.get(), 1) == "manifest_schema_version") {
                    hasManifestSchemaVersionColumn = true;
                    break;
                }
            }
        }
        const std::string migration = hasManifestSchemaVersionColumn
            ? "BEGIN IMMEDIATE;\nPRAGMA user_version = 35;\nCOMMIT;"
            : "BEGIN IMMEDIATE;\n"
              "ALTER TABLE manifest_work_ledger_entries\n"
              "    ADD COLUMN manifest_schema_version TEXT NOT NULL DEFAULT '';\n"
              "ALTER TABLE manifest_work_ledger_entries\n"
              "    ADD COLUMN proposal_lineage TEXT NOT NULL DEFAULT '[]';\n"
              "ALTER TABLE manifest_work_ledger_entries\n"
              "    ADD COLUMN verification_identity_json TEXT;\n"
              "PRAGMA user_version = 35;\nCOMMIT;";
        const auto migrated = execute(impl->handle, migration.c_str(), "migration.v35_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 36) {
        // AG-14.3: retain the authoritative post-Apply full-workspace
        // fingerprint in the approval audit row. Startup reconciliation can
        // then rebuild an Applied Work Ledger transition without inventing
        // the value from the narrower per-change hash log.
        bool hasPostApplyFingerprintColumn = false;
        {
            Statement info;
            const auto prepared = prepare(
                impl->handle, "PRAGMA table_info(change_set_approvals);", info);
            if (!prepared.success()) {
                return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*prepared.error());
            }
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<std::shared_ptr<SqliteDatabase>>::failure(databaseError(
                        impl->handle, "migration.v36_failed",
                        "Could not inspect change_set_approvals table schema"));
                }
                if (columnText(info.get(), 1) == "post_apply_workspace_fingerprint") {
                    hasPostApplyFingerprintColumn = true;
                    break;
                }
            }
        }
        const std::string migration = hasPostApplyFingerprintColumn
            ? "BEGIN IMMEDIATE;\nPRAGMA user_version = 36;\nCOMMIT;"
            : "BEGIN IMMEDIATE;\n"
              "ALTER TABLE change_set_approvals\n"
              "    ADD COLUMN post_apply_workspace_fingerprint TEXT;\n"
              "PRAGMA user_version = 36;\nCOMMIT;";
        const auto migrated = execute(impl->handle, migration.c_str(), "migration.v36_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 37) {
        // AG-15: the frozen per-Run workspace scope (immutable, one row per
        // Run - same "insert once" contract as execution_manifests above)
        // plus its append-only audited expansions, and one existence-checked
        // ALTER TABLE adding durable actual-inspection evidence to the
        // already-existing manifest_task_executions table (same shape/
        // precedent as manifest_task_no_changes.inspected_relative_paths
        // from v32).
        bool hasInspectedRelativePathsColumn = false;
        {
            Statement info;
            const auto prepared = prepare(
                impl->handle, "PRAGMA table_info(manifest_task_executions);", info);
            if (!prepared.success()) {
                return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*prepared.error());
            }
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<std::shared_ptr<SqliteDatabase>>::failure(databaseError(
                        impl->handle, "migration.v37_failed",
                        "Could not inspect manifest_task_executions table schema"));
                }
                if (columnText(info.get(), 1) == "inspected_relative_paths") {
                    hasInspectedRelativePathsColumn = true;
                    break;
                }
            }
        }
        const std::string alterTaskExecutions = hasInspectedRelativePathsColumn
            ? std::string()
            : "ALTER TABLE manifest_task_executions\n"
              "    ADD COLUMN inspected_relative_paths TEXT NOT NULL DEFAULT '[]';\n";
        const std::string migration = "BEGIN IMMEDIATE;\n"
            "CREATE TABLE IF NOT EXISTS run_workspace_scopes (\n"
            "    run_id TEXT PRIMARY KEY REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,\n"
            "    schema_version TEXT NOT NULL,\n"
            "    manifest_schema_version TEXT NOT NULL DEFAULT '',\n"
            "    entries_json TEXT NOT NULL,\n"
            "    dependency_edges_json TEXT NOT NULL DEFAULT '[]',\n"
            "    frozen_at_ms INTEGER NOT NULL\n"
            ");\n"
            "CREATE TRIGGER IF NOT EXISTS run_workspace_scopes_reject_update\n"
            "BEFORE UPDATE ON run_workspace_scopes\n"
            "BEGIN\n"
            "    SELECT RAISE(ABORT, 'run_workspace_scope_immutable');\n"
            "END;\n"
            "CREATE TABLE IF NOT EXISTS run_workspace_scope_expansions (\n"
            "    id TEXT PRIMARY KEY,\n"
            "    run_id TEXT NOT NULL REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,\n"
            "    task_id TEXT NOT NULL,\n"
            "    relative_path TEXT NOT NULL,\n"
            "    role TEXT NOT NULL,\n"
            "    reason TEXT NOT NULL,\n"
            "    baseline_hash TEXT,\n"
            "    approved_at_ms INTEGER NOT NULL,\n"
            "    UNIQUE(run_id, task_id, relative_path)\n"
            ");\n"
            "CREATE INDEX IF NOT EXISTS idx_run_workspace_scope_expansions_run\n"
            "    ON run_workspace_scope_expansions(run_id);\n"
            + alterTaskExecutions
            + "PRAGMA user_version = 37;\nCOMMIT;";
        const auto migrated = execute(impl->handle, migration.c_str(), "migration.v37_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 38) {
        // AG-15.3: fold this Run's frozen workspace scope identity + a
        // bounded summary into its Work Ledger entry (see
        // ManifestWorkLedgerService::recordTerminalRun) - v34-v37 untouched,
        // same existence-checked ALTER pattern as v35/v36/v37.
        bool hasWorkspaceScopeSchemaVersionColumn = false;
        {
            Statement info;
            const auto prepared = prepare(
                impl->handle, "PRAGMA table_info(manifest_work_ledger_entries);", info);
            if (!prepared.success()) {
                return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*prepared.error());
            }
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<std::shared_ptr<SqliteDatabase>>::failure(databaseError(
                        impl->handle, "migration.v38_failed",
                        "Could not inspect manifest_work_ledger_entries table schema"));
                }
                if (columnText(info.get(), 1) == "workspace_scope_schema_version") {
                    hasWorkspaceScopeSchemaVersionColumn = true;
                    break;
                }
            }
        }
        const std::string migration = hasWorkspaceScopeSchemaVersionColumn
            ? "BEGIN IMMEDIATE;\nPRAGMA user_version = 38;\nCOMMIT;"
            : "BEGIN IMMEDIATE;\n"
              "ALTER TABLE manifest_work_ledger_entries\n"
              "    ADD COLUMN workspace_scope_schema_version TEXT NOT NULL DEFAULT '';\n"
              "ALTER TABLE manifest_work_ledger_entries\n"
              "    ADD COLUMN workspace_scope_summary TEXT NOT NULL DEFAULT '';\n"
              "PRAGMA user_version = 38;\nCOMMIT;";
        const auto migrated = execute(impl->handle, migration.c_str(), "migration.v38_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 39) {
        bool hasWorkspaceSnapshotColumn = false;
        {
            Statement info;
            const auto prepared = prepare(
                impl->handle, "PRAGMA table_info(manifest_work_ledger_entries);", info);
            if (!prepared.success())
                return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*prepared.error());
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<std::shared_ptr<SqliteDatabase>>::failure(databaseError(
                        impl->handle, "migration.v39_failed",
                        "Could not inspect manifest_work_ledger_entries table schema"));
                }
                if (columnText(info.get(), 1) == "workspace_scope_snapshot_json") {
                    hasWorkspaceSnapshotColumn = true;
                    break;
                }
            }
        }
        const std::string migration = hasWorkspaceSnapshotColumn
            ? "BEGIN IMMEDIATE;\nPRAGMA user_version = 39;\nCOMMIT;"
            : "BEGIN IMMEDIATE;\n"
              "ALTER TABLE manifest_work_ledger_entries\n"
              "    ADD COLUMN workspace_scope_snapshot_json TEXT;\n"
              "PRAGMA user_version = 39;\nCOMMIT;";
        const auto migrated = execute(impl->handle, migration.c_str(), "migration.v39_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 40) {
        // AG-18.1: one immutable row per Run recording the Backend's own
        // discovered CMake verification environment - same shape/immutability
        // trigger as run_workspace_scopes (v37) above.
        const std::string migration = "BEGIN IMMEDIATE;\n"
            "CREATE TABLE IF NOT EXISTS verification_environment_snapshots (\n"
            "    run_id TEXT PRIMARY KEY REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,\n"
            "    project_id TEXT NOT NULL,\n"
            "    schema_version TEXT NOT NULL,\n"
            "    baseline_fingerprint TEXT NOT NULL DEFAULT '',\n"
            "    state TEXT NOT NULL,\n"
            "    profiles_json TEXT NOT NULL DEFAULT '[]',\n"
            "    unlinked_build_presets_json TEXT NOT NULL DEFAULT '[]',\n"
            "    diagnostic TEXT NOT NULL DEFAULT '',\n"
            "    discovered_at_ms INTEGER NOT NULL\n"
            ");\n"
            "CREATE TRIGGER IF NOT EXISTS verification_environment_snapshots_reject_update\n"
            "BEFORE UPDATE ON verification_environment_snapshots\n"
            "BEGIN\n"
            "    SELECT RAISE(ABORT, 'verification_environment_snapshot_immutable');\n"
            "END;\n"
            "CREATE INDEX IF NOT EXISTS idx_verification_environment_snapshots_project\n"
            "    ON verification_environment_snapshots(project_id, discovered_at_ms);\n"
            "PRAGMA user_version = 40;\nCOMMIT;";
        const auto migrated = execute(impl->handle, migration.c_str(), "migration.v40_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 41) {
        // AG-18.3: the bootstrap/action-required lifecycle. decisions are
        // immutable per Run (reject-update trigger, same shape as v40's
        // verification_environment_snapshots); transitions are append-only
        // (UNIQUE(run_id,to_disposition,reason_code) makes a duplicate
        // append a benign no-op at the repository layer instead of ever
        // needing INSERT OR REPLACE); retry_links carry at most one row per
        // predecessor Run (PRIMARY KEY).
        const std::string migration = "BEGIN IMMEDIATE;\n"
            "CREATE TABLE IF NOT EXISTS verification_bootstrap_decisions (\n"
            "    run_id TEXT PRIMARY KEY REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,\n"
            "    project_id TEXT NOT NULL,\n"
            "    schema_version TEXT NOT NULL,\n"
            "    baseline_fingerprint TEXT NOT NULL DEFAULT '',\n"
            "    verification_snapshot_schema_version TEXT NOT NULL DEFAULT '',\n"
            "    disposition TEXT NOT NULL,\n"
            "    reason_code TEXT NOT NULL,\n"
            "    message TEXT NOT NULL DEFAULT '',\n"
            "    bootstrap_setup_paths_json TEXT NOT NULL DEFAULT '[]',\n"
            "    owning_task_ids_json TEXT NOT NULL DEFAULT '[]',\n"
            "    predecessor_run_id TEXT,\n"
            "    decided_at_ms INTEGER NOT NULL\n"
            ");\n"
            "CREATE TRIGGER IF NOT EXISTS verification_bootstrap_decisions_reject_update\n"
            "BEFORE UPDATE ON verification_bootstrap_decisions\n"
            "BEGIN\n"
            "    SELECT RAISE(ABORT, 'verification_bootstrap_decision_immutable');\n"
            "END;\n"
            "CREATE INDEX IF NOT EXISTS idx_verification_bootstrap_decisions_project\n"
            "    ON verification_bootstrap_decisions(project_id, decided_at_ms);\n"
            "CREATE TABLE IF NOT EXISTS verification_bootstrap_transitions (\n"
            "    id TEXT PRIMARY KEY,\n"
            "    run_id TEXT NOT NULL REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,\n"
            "    schema_version TEXT NOT NULL,\n"
            "    to_disposition TEXT NOT NULL,\n"
            "    reason_code TEXT NOT NULL,\n"
            "    message TEXT NOT NULL DEFAULT '',\n"
            "    candidate_identity_json TEXT,\n"
            "    transitioned_at_ms INTEGER NOT NULL,\n"
            "    UNIQUE(run_id, to_disposition, reason_code)\n"
            ");\n"
            "CREATE INDEX IF NOT EXISTS idx_verification_bootstrap_transitions_run\n"
            "    ON verification_bootstrap_transitions(run_id);\n"
            "CREATE TABLE IF NOT EXISTS verification_bootstrap_retry_links (\n"
            "    predecessor_run_id TEXT PRIMARY KEY REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,\n"
            "    new_run_id TEXT NOT NULL,\n"
            "    project_id TEXT NOT NULL,\n"
            "    schema_version TEXT NOT NULL,\n"
            "    reason TEXT NOT NULL DEFAULT '',\n"
            "    created_at_ms INTEGER NOT NULL\n"
            ");\n"
            "PRAGMA user_version = 41;\nCOMMIT;";
        const auto migrated = execute(impl->handle, migration.c_str(), "migration.v41_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 42) {
        // Verification-repair reliability hardening: audited, append-only,
        // repair-only diagnostic read grants - a SEPARATE table from
        // run_workspace_scope_expansions (v37) on purpose, since a
        // diagnostic read may target a path frozen Excluded (an expansion
        // never can) and is granted only during the one bounded
        // combined-verification repair attempt, never ordinary dispatch -
        // see RunWorkspaceScopeDiagnosticRead's own doc comment. No role
        // column at all: a diagnostic read is always read-only, by
        // construction, never a write grant.
        const std::string migration = "BEGIN IMMEDIATE;\n"
            "CREATE TABLE IF NOT EXISTS run_workspace_scope_diagnostic_reads (\n"
            "    id TEXT PRIMARY KEY,\n"
            "    run_id TEXT NOT NULL REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,\n"
            "    task_id TEXT NOT NULL,\n"
            "    relative_path TEXT NOT NULL,\n"
            "    finding_code TEXT NOT NULL,\n"
            "    reason TEXT NOT NULL,\n"
            "    baseline_hash TEXT,\n"
            "    granted_at_ms INTEGER NOT NULL,\n"
            "    UNIQUE(run_id, task_id, relative_path)\n"
            ");\n"
            "CREATE INDEX IF NOT EXISTS idx_run_workspace_scope_diagnostic_reads_run\n"
            "    ON run_workspace_scope_diagnostic_reads(run_id);\n"
            "PRAGMA user_version = 42;\nCOMMIT;";
        const auto migrated = execute(impl->handle, migration.c_str(), "migration.v42_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 43) {
        // MW-P0: immutable per-Run provider budget policy, incrementally
        // persisted usage, and lower optional limits on saved profiles. The
        // empty-object defaults are parsed as today's Backend defaults so a
        // genuine v42 database upgrades without inventing historical usage.
        bool runHasPolicy = false;
        bool runHasUsage = false;
        bool profileHasPolicy = false;
        {
            Statement info;
            const auto prepared = prepare(impl->handle, "PRAGMA table_info(runs);", info);
            if (!prepared.success())
                return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*prepared.error());
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<std::shared_ptr<SqliteDatabase>>::failure(databaseError(
                        impl->handle, "migration.v43_failed", "Could not inspect runs table schema"));
                }
                const auto name = columnText(info.get(), 1);
                runHasPolicy = runHasPolicy || name == "provider_budget_json";
                runHasUsage = runHasUsage || name == "provider_budget_usage_json";
            }
        }
        {
            Statement info;
            const auto prepared = prepare(
                impl->handle, "PRAGMA table_info(execution_profiles);", info);
            if (!prepared.success())
                return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*prepared.error());
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<std::shared_ptr<SqliteDatabase>>::failure(databaseError(
                        impl->handle, "migration.v43_failed",
                        "Could not inspect execution_profiles table schema"));
                }
                profileHasPolicy = profileHasPolicy
                    || columnText(info.get(), 1) == "provider_budget_json";
            }
        }
        std::string migration = "BEGIN IMMEDIATE;\n";
        if (!runHasPolicy) {
            migration += "ALTER TABLE runs ADD COLUMN provider_budget_json TEXT NOT NULL DEFAULT '{}';\n";
        }
        if (!runHasUsage) {
            migration += "ALTER TABLE runs ADD COLUMN provider_budget_usage_json TEXT NOT NULL DEFAULT '{}';\n";
        }
        if (!profileHasPolicy) {
            migration += "ALTER TABLE execution_profiles ADD COLUMN provider_budget_json TEXT NOT NULL DEFAULT '{}';\n";
        }
        migration += "PRAGMA user_version = 43;\nCOMMIT;";
        const auto migrated = execute(impl->handle, migration.c_str(), "migration.v43_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 44) {
        // MW-P2: cumulative Anthropic prompt-cache token categories, mutable
        // alongside input_tokens/output_tokens (unlike provider_budget_json,
        // which stays immutable) - both start at 0 for a genuine v43
        // database, which is the truthful "no cache usage recorded yet"
        // state, not an invented history.
        bool runHasCacheCreation = false;
        bool runHasCacheRead = false;
        {
            Statement info;
            const auto prepared = prepare(impl->handle, "PRAGMA table_info(runs);", info);
            if (!prepared.success())
                return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*prepared.error());
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<std::shared_ptr<SqliteDatabase>>::failure(databaseError(
                        impl->handle, "migration.v44_failed", "Could not inspect runs table schema"));
                }
                const auto name = columnText(info.get(), 1);
                runHasCacheCreation = runHasCacheCreation || name == "cache_creation_input_tokens";
                runHasCacheRead = runHasCacheRead || name == "cache_read_input_tokens";
            }
        }
        std::string migration = "BEGIN IMMEDIATE;\n";
        if (!runHasCacheCreation) {
            migration += "ALTER TABLE runs ADD COLUMN cache_creation_input_tokens INTEGER NOT NULL DEFAULT 0;\n";
        }
        if (!runHasCacheRead) {
            migration += "ALTER TABLE runs ADD COLUMN cache_read_input_tokens INTEGER NOT NULL DEFAULT 0;\n";
        }
        migration += "PRAGMA user_version = 44;\nCOMMIT;";
        const auto migrated = execute(impl->handle, migration.c_str(), "migration.v44_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 45) {
        // MW-P2: Anthropic prompt-cache write/read rates on the Model
        // catalog, edited through ModelPricingDialog alongside the existing
        // input/output prices. Zero means "not entered", the same
        // convention the two existing price columns already use - never
        // fabricated as if refreshed from a live pricing source.
        bool modelHasCacheWrite = false;
        bool modelHasCacheRead = false;
        {
            Statement info;
            const auto prepared = prepare(impl->handle, "PRAGMA table_info(models);", info);
            if (!prepared.success())
                return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*prepared.error());
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<std::shared_ptr<SqliteDatabase>>::failure(databaseError(
                        impl->handle, "migration.v45_failed", "Could not inspect models table schema"));
                }
                const auto name = columnText(info.get(), 1);
                modelHasCacheWrite = modelHasCacheWrite
                    || name == "cache_write_price_micros_per_million_tokens";
                modelHasCacheRead = modelHasCacheRead
                    || name == "cache_read_price_micros_per_million_tokens";
            }
        }
        std::string migration = "BEGIN IMMEDIATE;\n";
        if (!modelHasCacheWrite) {
            migration += "ALTER TABLE models ADD COLUMN cache_write_price_micros_per_million_tokens "
                "INTEGER NOT NULL DEFAULT 0;\n";
        }
        if (!modelHasCacheRead) {
            migration += "ALTER TABLE models ADD COLUMN cache_read_price_micros_per_million_tokens "
                "INTEGER NOT NULL DEFAULT 0;\n";
        }
        migration += "PRAGMA user_version = 45;\nCOMMIT;";
        const auto migrated = execute(impl->handle, migration.c_str(), "migration.v45_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 46) {
        // MW-P2: was this execution's persisted cost_micros computed with a
        // fully known set of prices? Defaults to 1 (known) for pre-existing
        // rows - there is no way to retroactively determine whether their
        // price was known at the time, and treating them as known avoids
        // inventing a new "estimated" warning for historical data that never
        // carried one.
        bool hasCostPriceKnown = false;
        {
            Statement info;
            const auto prepared = prepare(impl->handle, "PRAGMA table_info(model_executions);", info);
            if (!prepared.success())
                return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*prepared.error());
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<std::shared_ptr<SqliteDatabase>>::failure(databaseError(
                        impl->handle, "migration.v46_failed",
                        "Could not inspect model_executions table schema"));
                }
                hasCostPriceKnown = hasCostPriceKnown
                    || columnText(info.get(), 1) == "cost_price_known";
            }
        }
        std::string migration = "BEGIN IMMEDIATE;\n";
        if (!hasCostPriceKnown) {
            migration += "ALTER TABLE model_executions ADD COLUMN cost_price_known "
                "INTEGER NOT NULL DEFAULT 1;\n";
        }
        migration += "PRAGMA user_version = 46;\nCOMMIT;";
        const auto migrated = execute(impl->handle, migration.c_str(), "migration.v46_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 47) {
        // MW-P3A: leak-safe identities for the paid retry guard. Keep the
        // migration idempotent for test/recovery environments that replay a
        // version step against an already-altered table.
        bool hasInputFingerprint = false;
        bool hasFailureSignature = false;
        {
            Statement info;
            const auto prepared = prepare(
                impl->handle, "PRAGMA table_info(manifest_work_ledger_entries);", info);
            if (!prepared.success())
                return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*prepared.error());
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<std::shared_ptr<SqliteDatabase>>::failure(databaseError(
                        impl->handle, "migration.v47_failed",
                        "Could not inspect manifest_work_ledger_entries table schema"));
                }
                const auto name = columnText(info.get(), 1);
                hasInputFingerprint = hasInputFingerprint
                    || name == "planning_input_fingerprint";
                hasFailureSignature = hasFailureSignature
                    || name == "planning_failure_signature";
            }
        }
        std::string migration = "BEGIN IMMEDIATE;\n";
        if (!hasInputFingerprint) {
            migration += "ALTER TABLE manifest_work_ledger_entries ADD COLUMN "
                "planning_input_fingerprint TEXT NOT NULL DEFAULT '';\n";
        }
        if (!hasFailureSignature) {
            migration += "ALTER TABLE manifest_work_ledger_entries ADD COLUMN "
                "planning_failure_signature TEXT NOT NULL DEFAULT '';\n";
        }
        migration += "PRAGMA user_version = 47;\nCOMMIT;";
        const auto migrated = execute(impl->handle, migration.c_str(), "migration.v47_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 48) {
        // MW-P5.1: durable admission checkpoint for verification-failure
        // continuation. source_run_id is the primary key - at most one
        // checkpoint per source Run, same 1:1-per-Run convention
        // run_workspace_scopes (v37) already uses. Immutable-after-insert,
        // same reject-update trigger pattern as run_workspace_scopes.
        const std::string migration = "BEGIN IMMEDIATE;\n"
            "CREATE TABLE IF NOT EXISTS manifest_verification_continuations (\n"
            "    source_run_id TEXT PRIMARY KEY REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,\n"
            "    id TEXT NOT NULL,\n"
            "    schema_version TEXT NOT NULL,\n"
            "    source_project_id TEXT NOT NULL,\n"
            "    status TEXT NOT NULL,\n"
            "    denial_code TEXT NOT NULL DEFAULT '',\n"
            "    denial_message TEXT NOT NULL DEFAULT '',\n"
            "    created_at_ms INTEGER NOT NULL\n"
            ");\n"
            "CREATE TRIGGER IF NOT EXISTS manifest_verification_continuations_reject_update\n"
            "BEFORE UPDATE ON manifest_verification_continuations\n"
            "BEGIN\n"
            "    SELECT RAISE(ABORT, 'manifest_verification_continuation_immutable');\n"
            "END;\n"
            "PRAGMA user_version = 48;\nCOMMIT;";
        const auto migrated = execute(impl->handle, migration.c_str(), "migration.v48_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 49) {
        // MW-P5.2: immutable normalized diagnostic attribution, deliberately
        // separate from P5.1's already-frozen admission decision.
        const std::string migration = "BEGIN IMMEDIATE;\n"
            "CREATE TABLE IF NOT EXISTS manifest_verification_attributions (\n"
            "    source_run_id TEXT PRIMARY KEY REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,\n"
            "    id TEXT NOT NULL,\n"
            "    schema_version TEXT NOT NULL,\n"
            "    source_project_id TEXT NOT NULL,\n"
            "    status TEXT NOT NULL,\n"
            "    block_code TEXT NOT NULL DEFAULT '',\n"
            "    block_message TEXT NOT NULL DEFAULT '',\n"
            "    baseline_fingerprint TEXT NOT NULL,\n"
            "    source_overlay_fingerprint TEXT NOT NULL,\n"
            "    findings_json TEXT NOT NULL,\n"
            "    created_at_ms INTEGER NOT NULL\n"
            ");\n"
            "CREATE TRIGGER IF NOT EXISTS manifest_verification_attributions_reject_update\n"
            "BEFORE UPDATE ON manifest_verification_attributions\n"
            "BEGIN\n"
            "    SELECT RAISE(ABORT, 'manifest_verification_attribution_immutable');\n"
            "END;\n"
            "PRAGMA user_version = 49;\nCOMMIT;";
        const auto migrated = execute(impl->handle, migration.c_str(), "migration.v49_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 50) {
        // MW-P5.3: immutable continuation input. It is intentionally keyed by
        // source_run_id and does not foreign-key child_run_id: the input and
        // reserved child identity are committed before the child aggregate,
        // so a crash can deterministically recreate that exact child id.
        const std::string migration = "BEGIN IMMEDIATE;\n"
            "CREATE TABLE IF NOT EXISTS manifest_verification_continuation_inputs (\n"
            "    source_run_id TEXT PRIMARY KEY REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,\n"
            "    child_run_id TEXT NOT NULL UNIQUE,\n"
            "    schema_version TEXT NOT NULL,\n"
            "    source_project_id TEXT NOT NULL,\n"
            "    attribution_id TEXT NOT NULL,\n"
            "    baseline_fingerprint TEXT NOT NULL,\n"
            "    source_workspace_identity TEXT NOT NULL,\n"
            "    source_overlay_fingerprint TEXT NOT NULL,\n"
            "    proposal_ids_json TEXT NOT NULL,\n"
            "    proposal_hashes_json TEXT NOT NULL,\n"
            "    input_fingerprint TEXT NOT NULL,\n"
            "    planner_requests INTEGER NOT NULL DEFAULT 0,\n"
            "    created_at_ms INTEGER NOT NULL\n"
            ");\n"
            "CREATE TRIGGER IF NOT EXISTS manifest_verification_continuation_inputs_reject_update\n"
            "BEFORE UPDATE ON manifest_verification_continuation_inputs\n"
            "BEGIN\n"
            "    SELECT RAISE(ABORT, 'manifest_verification_continuation_input_immutable');\n"
            "END;\n"
            "PRAGMA user_version = 50;\nCOMMIT;";
        const auto migrated = execute(impl->handle, migration.c_str(), "migration.v50_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 51) {
        // MW-P5.6D: immutable legacy-baseline-drift normalization proof -
        // see ManifestVerificationBaselineNormalization's own doc comment.
        const std::string migration = "BEGIN IMMEDIATE;\n"
            "CREATE TABLE IF NOT EXISTS manifest_verification_baseline_normalizations (\n"
            "    source_run_id TEXT PRIMARY KEY REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,\n"
            "    id TEXT NOT NULL,\n"
            "    schema_version TEXT NOT NULL,\n"
            "    policy_id TEXT NOT NULL,\n"
            "    durable_baseline_fingerprint TEXT NOT NULL,\n"
            "    normalized_baseline_fingerprint TEXT NOT NULL,\n"
            "    excluded_prefixes_json TEXT NOT NULL,\n"
            "    normalized_paths_json TEXT NOT NULL,\n"
            "    created_at_ms INTEGER NOT NULL\n"
            ");\n"
            "CREATE TRIGGER IF NOT EXISTS manifest_verification_baseline_normalizations_reject_update\n"
            "BEFORE UPDATE ON manifest_verification_baseline_normalizations\n"
            "BEGIN\n"
            "    SELECT RAISE(ABORT, 'manifest_verification_baseline_normalization_immutable');\n"
            "END;\n"
            "PRAGMA user_version = 51;\nCOMMIT;";
        const auto migrated = execute(impl->handle, migration.c_str(), "migration.v51_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 52) {
        // MW-P5.6E: RunStatus::Prepared was added for a durable continuation
        // child, but SQLite's original v1 CHECK constraint still rejected
        // the value. SQLite cannot ALTER a CHECK constraint, so rebuild the
        // parent table with every column accumulated through v46 while
        // foreign-key enforcement is temporarily disabled on this one
        // connection. Child tables continue to reference the same final
        // table name; indexes are recreated before the transaction commits.
        constexpr const char* migration = R"sql(
PRAGMA foreign_keys = OFF;
BEGIN IMMEDIATE;
CREATE TABLE runs_v52 (
    id TEXT PRIMARY KEY NOT NULL,
    project_id TEXT NOT NULL REFERENCES projects(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    parent_run_id TEXT REFERENCES runs_v52(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    title TEXT NOT NULL,
    task TEXT NOT NULL,
    workflow_id TEXT NOT NULL,
    status TEXT NOT NULL CHECK(status IN ('prepared','queued','running','cancelling','completed','failed','cancelled','interrupted')),
    progress REAL NOT NULL CHECK(progress >= 0.0 AND progress <= 1.0),
    current_stage_id TEXT,
    error_message TEXT NOT NULL,
    input_tokens INTEGER NOT NULL CHECK(input_tokens >= 0),
    output_tokens INTEGER NOT NULL CHECK(output_tokens >= 0),
    created_at_ms INTEGER NOT NULL,
    started_at_ms INTEGER,
    finished_at_ms INTEGER,
    revision INTEGER NOT NULL CHECK(revision >= 0),
    execution_profile_id TEXT REFERENCES execution_profiles(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    max_parallelism INTEGER NOT NULL DEFAULT 4 CHECK(max_parallelism >= 1 AND max_parallelism <= 32),
    workspace_path TEXT,
    planning_completion_policy TEXT NOT NULL DEFAULT 'allow_evidence_backed_no_changes'
        CHECK(planning_completion_policy IN ('require_manifest','allow_evidence_backed_no_changes')),
    provider_budget_json TEXT NOT NULL DEFAULT '{}',
    provider_budget_usage_json TEXT NOT NULL DEFAULT '{}',
    cache_creation_input_tokens INTEGER NOT NULL DEFAULT 0,
    cache_read_input_tokens INTEGER NOT NULL DEFAULT 0
);
INSERT INTO runs_v52 (
    id,project_id,parent_run_id,title,task,workflow_id,status,progress,current_stage_id,
    error_message,input_tokens,output_tokens,created_at_ms,started_at_ms,finished_at_ms,
    revision,execution_profile_id,max_parallelism,workspace_path,planning_completion_policy,
    provider_budget_json,provider_budget_usage_json,cache_creation_input_tokens,
    cache_read_input_tokens)
SELECT
    id,project_id,parent_run_id,title,task,workflow_id,status,progress,current_stage_id,
    error_message,input_tokens,output_tokens,created_at_ms,started_at_ms,finished_at_ms,
    revision,execution_profile_id,max_parallelism,workspace_path,planning_completion_policy,
    provider_budget_json,provider_budget_usage_json,cache_creation_input_tokens,
    cache_read_input_tokens
FROM runs;
DROP TABLE runs;
ALTER TABLE runs_v52 RENAME TO runs;
CREATE INDEX runs_project_created_idx ON runs(project_id, created_at_ms, id);
CREATE INDEX runs_status_idx ON runs(status);
PRAGMA user_version = 52;
COMMIT;
PRAGMA foreign_keys = ON;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v52_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle,
                "ROLLBACK; PRAGMA foreign_keys = ON;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
        Statement foreignKeyCheck;
        const auto prepared = prepare(impl->handle, "PRAGMA foreign_key_check;", foreignKeyCheck);
        if (!prepared.success() || sqlite3_step(foreignKeyCheck.get()) != SQLITE_DONE) {
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(error(
                "migration.v52_foreign_key_check_failed",
                "Run-status migration left an invalid foreign-key relationship"));
        }
    }

    if (schemaVersion < 55) {
        // QML-P4 / QML-P4R.1: the manual-handoff table set. History: v53
        // first created the handoffs/attempts pair (source_run_id, parallel
        // proposal_ids_json/proposal_hashes_json arrays); v54 hardened the
        // attempts table's uniqueness for crash-safe reservation-before-work.
        // QML-P4R.1 squashes both historical steps into this single v55
        // target shape rather than replaying them, because replaying v53's
        // own "CREATE TABLE IF NOT EXISTS" against a database that has
        // already been opened once at a LATER schema version is a no-op that
        // leaves the table in whatever shape it already has - not the v53
        // shape v54's own migration SQL (by column name) assumes. Squashing
        // avoids that entirely: this block only ever asks "is this database
        // below the v55 target?", never "is it currently in the intermediate
        // v53 shape?". The real retained scenario this feature exists for is
        // origin Manifest Run -> paid continuation child -> manual handoff,
        // not a single sourceRunId - hence origin_manifest_run_id (owner of
        // the frozen Manifest/tasks) + verification_anchor_run_id (the
        // terminal Run whose exact verification is being repaired - unique
        // key, since at most one handoff exists per verification being
        // repaired). The old parallel proposal_ids_json/proposal_hashes_json
        // arrays, which could only express "every member belongs to one
        // Run", are replaced by frozen_members_json (an aggregate snapshot
        // kept for convenient whole-handoff reads) PLUS a normalized,
        // indexed manifest_verification_manual_handoff_members table (one
        // row per {handoff_id, ordinal, proposal_id, owning_run_id,
        // content_hash, lineage_root_proposal_id}) - AG-16's reverse lookup
        // (an unmodified proposal's owning Run asking "was I frozen into some
        // handoff?") must resolve via an indexed proposal_id lookup, never by
        // scanning JSON or guessing which Run is "the anchor".
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
DROP TABLE IF EXISTS manifest_verification_manual_handoff_members;
DROP TABLE IF EXISTS manifest_verification_manual_handoff_attempts;
DROP TABLE IF EXISTS manifest_verification_manual_handoffs;
CREATE TABLE manifest_verification_manual_handoffs (
    id TEXT PRIMARY KEY,
    origin_manifest_run_id TEXT NOT NULL REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,
    verification_anchor_run_id TEXT NOT NULL UNIQUE REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,
    schema_version TEXT NOT NULL,
    source_project_id TEXT NOT NULL,
    storage_key TEXT NOT NULL,
    workspace_identity TEXT NOT NULL,
    baseline_fingerprint TEXT NOT NULL,
    source_overlay_fingerprint TEXT NOT NULL,
    frozen_members_json TEXT NOT NULL,
    allowed_paths_json TEXT NOT NULL,
    original_snapshot_json TEXT NOT NULL,
    created_at_ms INTEGER NOT NULL
);
CREATE TRIGGER manifest_verification_manual_handoffs_reject_update
BEFORE UPDATE ON manifest_verification_manual_handoffs
BEGIN
    SELECT RAISE(ABORT, 'manifest_verification_manual_handoff_immutable');
END;
CREATE TABLE manifest_verification_manual_handoff_attempts (
    id TEXT PRIMARY KEY,
    handoff_id TEXT NOT NULL REFERENCES manifest_verification_manual_handoffs(id)
        ON UPDATE CASCADE ON DELETE CASCADE,
    verification_anchor_run_id TEXT NOT NULL,
    attempt_run_id TEXT NOT NULL UNIQUE,
    schema_version TEXT NOT NULL,
    attempt_ordinal INTEGER NOT NULL CHECK(attempt_ordinal >= 1),
    previous_attempt_id TEXT,
    edited_snapshot_fingerprint TEXT NOT NULL,
    created_at_ms INTEGER NOT NULL,
    UNIQUE(handoff_id, attempt_ordinal)
);
CREATE TRIGGER manifest_verification_manual_handoff_attempts_reject_update
BEFORE UPDATE ON manifest_verification_manual_handoff_attempts
BEGIN
    SELECT RAISE(ABORT, 'manifest_verification_manual_handoff_attempt_immutable');
END;
CREATE INDEX idx_manual_handoff_attempts_handoff
    ON manifest_verification_manual_handoff_attempts(handoff_id, attempt_ordinal);
CREATE TABLE manifest_verification_manual_handoff_members (
    handoff_id TEXT NOT NULL REFERENCES manifest_verification_manual_handoffs(id)
        ON UPDATE CASCADE ON DELETE CASCADE,
    ordinal INTEGER NOT NULL,
    proposal_id TEXT NOT NULL UNIQUE,
    owning_run_id TEXT NOT NULL,
    content_hash TEXT NOT NULL,
    lineage_root_proposal_id TEXT NOT NULL,
    PRIMARY KEY (handoff_id, ordinal)
);
CREATE TRIGGER manifest_verification_manual_handoff_members_reject_update
BEFORE UPDATE ON manifest_verification_manual_handoff_members
BEGIN
    SELECT RAISE(ABORT, 'manifest_verification_manual_handoff_member_immutable');
END;
CREATE INDEX idx_manual_handoff_members_proposal
    ON manifest_verification_manual_handoff_members(proposal_id);
PRAGMA user_version = 55;
COMMIT;
)sql";
        // Fail closed if either pre-v55 table already contains rows, rather
        // than silently discarding them: "no production build has ever
        // shipped v1" is the justification for a clean drop and recreate,
        // never a substitute for actually checking. Checked before the
        // migration transaction above even begins, so a refusal here leaves
        // every existing row completely untouched.
        auto existingRowCount = [&](const char* tableName) -> common::Result<long long> {
            Statement existsStatement;
            const auto existsPrepared = prepare(impl->handle,
                "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?;", existsStatement);
            if (!existsPrepared.success()) {
                return common::Result<long long>::failure(*existsPrepared.error());
            }
            const auto existsBound = bindText(impl->handle, existsStatement.get(), 1, tableName);
            if (!existsBound.success()) {
                return common::Result<long long>::failure(*existsBound.error());
            }
            if (sqlite3_step(existsStatement.get()) != SQLITE_ROW) {
                return common::Result<long long>::ok(0);
            }
            Statement countStatement;
            const std::string countSql = std::string("SELECT COUNT(*) FROM ") + tableName + ";";
            const auto countPrepared = prepare(impl->handle, countSql.c_str(), countStatement);
            if (!countPrepared.success()) {
                return common::Result<long long>::failure(*countPrepared.error());
            }
            if (sqlite3_step(countStatement.get()) != SQLITE_ROW) {
                return common::Result<long long>::failure(databaseError(impl->handle,
                    "migration.v55_count_failed",
                    "Could not count existing pre-v55 manual-handoff rows"));
            }
            return common::Result<long long>::ok(sqlite3_column_int64(countStatement.get(), 0));
        };
        const auto handoffRowCount = existingRowCount("manifest_verification_manual_handoffs");
        if (!handoffRowCount.success()) {
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*handoffRowCount.error());
        }
        const auto attemptRowCount = existingRowCount("manifest_verification_manual_handoff_attempts");
        if (!attemptRowCount.success()) {
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*attemptRowCount.error());
        }
        if (handoffRowCount.value() > 0 || attemptRowCount.value() > 0) {
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(error(
                "migration.v55_refused_data_loss",
                "Refusing to upgrade to schema v55: the pre-v55 manual-handoff tables contain "
                "rows this migration cannot preserve. Discard them explicitly before upgrading."));
        }
        const auto migrated = execute(impl->handle, migration, "migration.v55_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 56) {
        // QML-P4R.3.4: one immutable, at-most-one-per-attempt typed result
        // row, cascade-deleted with its attempt exactly like an attempt
        // cascades with its handoff. No production build has ever shipped
        // this table (invented in this same unreleased branch, after v55),
        // so unlike v55's own squash this never needs a data-loss guard -
        // but it still needs its own DROP TABLE IF EXISTS, exactly like
        // v55's tables: a database whose stored user_version is rewound
        // below 56 (an older client catching up through every migration in
        // one open, or a test proving that) must re-run this block
        // idempotently even though the table may already exist from a
        // partial or full prior migration to v56.
        //
        // QML-P4R.3.4 hardening (same unreleased branch, same day): adds
        // 'rejected_before_verification' to the kind CHECK and the nullable
        // rejection_code/rejection_message pair it carries - amended
        // directly into this same v56 target shape rather than a separate
        // v57 ALTER, for the identical "nothing has ever shipped this
        // table" reasoning the block above already gives for its own
        // squash.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
DROP TABLE IF EXISTS manifest_verification_manual_handoff_attempt_results;
CREATE TABLE manifest_verification_manual_handoff_attempt_results (
    id TEXT PRIMARY KEY,
    attempt_id TEXT NOT NULL UNIQUE REFERENCES manifest_verification_manual_handoff_attempts(id)
        ON UPDATE CASCADE ON DELETE CASCADE,
    attempt_run_id TEXT NOT NULL,
    schema_version TEXT NOT NULL,
    kind TEXT NOT NULL CHECK(kind IN (
        'verified_success','verified_content_failure','rejected_before_verification','incomplete')),
    rejection_code TEXT,
    rejection_message TEXT,
    recorded_at_ms INTEGER NOT NULL,
    CHECK((kind = 'rejected_before_verification')
        = (rejection_code IS NOT NULL AND rejection_message IS NOT NULL))
);
CREATE TRIGGER manifest_verification_manual_handoff_attempt_results_reject_update
BEFORE UPDATE ON manifest_verification_manual_handoff_attempt_results
BEGIN
    SELECT RAISE(ABORT, 'manifest_verification_manual_handoff_attempt_result_immutable');
END;
PRAGMA user_version = 56;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v56_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 57) {
        // Per-Run verification repair is an immutable operator cost policy.
        // Existing Runs/drafts retain the historical one-wave behavior; only
        // an explicitly-created new Run may opt out. Keep the storage values
        // constrained so corrupt/unknown policy never silently falls back.
        bool runsHavePolicy = false;
        bool draftsHavePolicy = false;
        const auto inspectColumn = [&](const char* table, bool& found)
            -> common::Result<void> {
            Statement info;
            const std::string pragma = std::string("PRAGMA table_info(") + table + ");";
            const auto prepared = prepare(impl->handle, pragma.c_str(), info);
            if (!prepared.success()) return prepared;
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<void>::failure(databaseError(
                        impl->handle, "migration.v57_failed",
                        std::string("Could not inspect ") + table + " table schema"));
                }
                found = found || columnText(info.get(), 1) == "verification_repair_policy";
            }
            return common::Result<void>::ok();
        };
        const auto inspectedRuns = inspectColumn("runs", runsHavePolicy);
        if (!inspectedRuns.success()) {
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(
                *inspectedRuns.error());
        }
        const auto inspectedDrafts = inspectColumn("run_drafts", draftsHavePolicy);
        if (!inspectedDrafts.success()) {
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(
                *inspectedDrafts.error());
        }

        std::string migration = "BEGIN IMMEDIATE;\n";
        if (!runsHavePolicy) {
            migration +=
                "ALTER TABLE runs ADD COLUMN verification_repair_policy TEXT NOT NULL "
                "DEFAULT 'one_bounded_wave' CHECK(verification_repair_policy IN "
                "('one_bounded_wave','disabled'));\n";
        }
        if (!draftsHavePolicy) {
            migration +=
                "ALTER TABLE run_drafts ADD COLUMN verification_repair_policy TEXT NOT NULL "
                "DEFAULT 'one_bounded_wave' CHECK(verification_repair_policy IN "
                "('one_bounded_wave','disabled'));\n";
        }
        migration += "PRAGMA user_version = 57;\nCOMMIT;";
        const auto migrated = execute(
            impl->handle, migration.c_str(), "migration.v57_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 58) {
        // AG-17.7C: the immutable audit of a legacy roster -> Manifest conversion. One row per
        // converted Run, written in the same transaction as the Run (see
        // SqliteRunRepository::insertRunWithStagesAndLegacyConversion), never updated.
        // source_run_id deliberately carries no foreign key: the audit must outlive any
        // operation on the source Run. Free-form text (instructions) is never stored: the
        // dropped list holds lengths and closed vocabularies only. A new table needs its own
        // DROP TABLE IF EXISTS so a re-run of the migration cannot break on a leftover.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
DROP TABLE IF EXISTS run_legacy_conversions;
CREATE TABLE run_legacy_conversions (
    run_id TEXT PRIMARY KEY REFERENCES runs(id) ON UPDATE CASCADE ON DELETE CASCADE,
    source_run_id TEXT NOT NULL,
    source_workflow_id TEXT NOT NULL CHECK(source_workflow_id IN (
        'execution-profile-v1','read-only-analysis-v1')),
    policy_id TEXT NOT NULL CHECK(length(policy_id) BETWEEN 1 AND 64),
    reason_code TEXT NOT NULL CHECK(reason_code IN (
        'legacy_roster.converted_lone_workspace_lead','legacy_roster.converted_proposal_roster')),
    source_snapshot_fingerprint TEXT NOT NULL CHECK(length(source_snapshot_fingerprint) = 64),
    target_roster_fingerprint TEXT NOT NULL CHECK(length(target_roster_fingerprint) = 64),
    confirmation_fingerprint TEXT NOT NULL CHECK(length(confirmation_fingerprint) = 64),
    dropped_json TEXT NOT NULL CHECK(length(dropped_json) <= 65536),
    confirmation_mode TEXT NOT NULL CHECK(confirmation_mode IN (
        'explicit_user','not_required_safe_retry')),
    confirmed_at_ms INTEGER,
    created_at_ms INTEGER NOT NULL,
    CHECK((confirmation_mode = 'explicit_user') = (confirmed_at_ms IS NOT NULL)),
    CHECK(source_run_id <> run_id)
);
CREATE TRIGGER run_legacy_conversions_reject_update
BEFORE UPDATE ON run_legacy_conversions
BEGIN
    SELECT RAISE(ABORT, 'run_legacy_conversion_immutable');
END;
PRAGMA user_version = 58;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v58_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 59) {
        // AG-17 gpt-6-sol checkpoint M2: the optional long-context price tier of a model - a
        // threshold plus four long-context prices (0 threshold = flat pricing, 0 price = "not
        // entered", like the flat prices). Additive columns on an existing table, so the same
        // idempotent shape as v45/v57: inspect the table first and add only what is missing.
        // Every existing row keeps flat pricing (all zeros).
        static constexpr const char* longContextColumns[] = {
            "long_context_threshold_input_tokens",
            "long_context_input_price_micros_per_million_tokens",
            "long_context_output_price_micros_per_million_tokens",
            "long_context_cache_write_price_micros_per_million_tokens",
            "long_context_cache_read_price_micros_per_million_tokens"};
        bool present[std::size(longContextColumns)] = {};
        {
            Statement info;
            const auto prepared = prepare(impl->handle, "PRAGMA table_info(models);", info);
            if (!prepared.success()) {
                return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*prepared.error());
            }
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<std::shared_ptr<SqliteDatabase>>::failure(databaseError(
                        impl->handle, "migration.v59_failed", "Could not inspect models table schema"));
                }
                const auto name = columnText(info.get(), 1);
                for (std::size_t index = 0; index < std::size(longContextColumns); ++index) {
                    present[index] = present[index] || name == longContextColumns[index];
                }
            }
        }
        // Step 1: add the missing columns in a transaction that stays open until the existing rows
        // have been checked.
        std::string addColumns = "BEGIN IMMEDIATE;\n";
        for (std::size_t index = 0; index < std::size(longContextColumns); ++index) {
            if (present[index]) continue;
            addColumns += std::string("ALTER TABLE models ADD COLUMN ") + longContextColumns[index]
                + " INTEGER NOT NULL DEFAULT 0 CHECK(" + longContextColumns[index] + " >= 0);\n";
        }
        const auto added = execute(impl->handle, addColumns.c_str(), "migration.v59_failed");
        if (!added.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*added.error());
        }
        // Step 2: a partially migrated database may already hold tier values - for example long
        // prices that outlived a missing threshold column, which would now read back as FLAT
        // pricing, i.e. an understated cost. Check EVERY row against the shared rule and refuse the
        // whole migration on a violation: nothing is changed, the version is not raised and no
        // stored price is zeroed silently - the operator repairs the row and reopens.
        const auto verified = [&]() -> common::Result<void> {
            Statement rows;
            const auto prepared = prepare(impl->handle,
                "SELECT id, context_window_tokens, long_context_threshold_input_tokens, "
                "long_context_input_price_micros_per_million_tokens, "
                "long_context_output_price_micros_per_million_tokens, "
                "long_context_cache_write_price_micros_per_million_tokens, "
                "long_context_cache_read_price_micros_per_million_tokens FROM models;",
                rows);
            if (!prepared.success()) return prepared;
            for (;;) {
                const int step = sqlite3_step(rows.get());
                if (step == SQLITE_DONE) return common::Result<void>::ok();
                if (step != SQLITE_ROW) {
                    return common::Result<void>::failure(databaseError(
                        impl->handle, "migration.v59_failed",
                        "Could not verify the long-context tiers of the existing models"));
                }
                std::int64_t values[6] = {};
                bool negative = false;
                for (int column = 0; column < 6; ++column) {
                    values[column] = sqlite3_column_int64(rows.get(), column + 1);
                    negative = negative || values[column] < 0;
                }
                const LongContextPricingProblem problem = negative
                    ? LongContextPricingProblem::None
                    : checkLongContextPricing(
                          static_cast<std::uint64_t>(values[0]),
                          LongContextPricing {
                              static_cast<std::uint64_t>(values[1]),
                              static_cast<std::uint64_t>(values[2]),
                              static_cast<std::uint64_t>(values[3]),
                              static_cast<std::uint64_t>(values[4]),
                              static_cast<std::uint64_t>(values[5])});
                if (negative || problem != LongContextPricingProblem::None) {
                    return common::Result<void>::failure(error(
                        "migration.v59_failed",
                        "Model '" + columnText(rows.get(), 0)
                            + "' has an invalid long-context price tier ("
                            + (negative ? "a negative value" : longContextPricingProblemMessage(problem))
                            + "); repair it and reopen the database"));
                }
            }
        }();
        if (!verified.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*verified.error());
        }
        // Step 3: record the version and commit.
        const auto committed = execute(impl->handle,
            "PRAGMA user_version = 59;\nCOMMIT;", "migration.v59_failed");
        if (!committed.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*committed.error());
        }
    }

    if (schemaVersion < 60) {
        // Explicit per-Run provider limits (the optional overrides a per-Run roster carries) are
        // saved with a draft: one additive column on run_drafts holding the same optional-key JSON
        // object an execution profile stores. '{}' = no explicit limit (the Backend defaults, the
        // turn limit automatic), which is exactly what every existing draft meant. Idempotent in
        // the v45/v57/v59 shape: inspect the table first, add only what is missing.
        bool draftsHaveBudget = false;
        {
            Statement info;
            const auto prepared = prepare(impl->handle, "PRAGMA table_info(run_drafts);", info);
            if (!prepared.success()) {
                return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*prepared.error());
            }
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<std::shared_ptr<SqliteDatabase>>::failure(databaseError(
                        impl->handle, "migration.v60_failed",
                        "Could not inspect run_drafts table schema"));
                }
                draftsHaveBudget = draftsHaveBudget || columnText(info.get(), 1) == "provider_budget_json";
            }
        }
        std::string migration = "BEGIN IMMEDIATE;\n";
        if (!draftsHaveBudget) {
            migration +=
                "ALTER TABLE run_drafts ADD COLUMN provider_budget_json TEXT NOT NULL DEFAULT '{}';\n";
        }
        migration += "PRAGMA user_version = 60;\nCOMMIT;";
        const auto migrated = execute(impl->handle, migration.c_str(), "migration.v60_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 61) {
        // PC-2 (gap MW-P9): the Run author's authoritative interface contracts are frozen with the Run and
        // saved with a draft: one additive JSON column on each table. '[]' = no contracts, which is
        // exactly what every existing Run and draft meant. Idempotent in the v57/v60 shape: inspect each
        // table first, add only what is missing.
        bool runsHaveContracts = false;
        bool draftsHaveContracts = false;
        const auto inspectColumn = [&](const char* table, bool& found) -> common::Result<void> {
            Statement info;
            const std::string pragma = std::string("PRAGMA table_info(") + table + ");";
            const auto prepared = prepare(impl->handle, pragma.c_str(), info);
            if (!prepared.success()) return prepared;
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<void>::failure(databaseError(
                        impl->handle, "migration.v61_failed",
                        std::string("Could not inspect ") + table + " table schema"));
                }
                found = found || columnText(info.get(), 1) == "run_interface_contracts_json";
            }
            return common::Result<void>::ok();
        };
        const auto inspectedRuns = inspectColumn("runs", runsHaveContracts);
        if (!inspectedRuns.success()) {
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*inspectedRuns.error());
        }
        const auto inspectedDrafts = inspectColumn("run_drafts", draftsHaveContracts);
        if (!inspectedDrafts.success()) {
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*inspectedDrafts.error());
        }
        std::string migration = "BEGIN IMMEDIATE;\n";
        if (!runsHaveContracts) {
            migration +=
                "ALTER TABLE runs ADD COLUMN run_interface_contracts_json TEXT NOT NULL DEFAULT '[]';\n";
        }
        if (!draftsHaveContracts) {
            migration +=
                "ALTER TABLE run_drafts ADD COLUMN run_interface_contracts_json TEXT NOT NULL DEFAULT '[]';\n";
        }
        migration += "PRAGMA user_version = 61;\nCOMMIT;";
        const auto migrated = execute(impl->handle, migration.c_str(), "migration.v61_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 62) {
        // PC-3B (gap MW-P10): Run Draft GROUPS - drafts created together from one next-run plan.
        //  - run_draft_groups: one immutable row per group (origin, idempotency key, plan identity, source ids).
        //    source_* carry no foreign key on purpose: deleting a conversation or message must not remove drafts.
        //  - five nullable provenance columns on run_drafts: all NULL = a manual draft, all set = a draft of a
        //    group. Additive and defaulted to NULL, so every existing draft stays exactly what it was.
        // Triggers keep the provenance whole and unchangeable even for a writer that bypasses the repository:
        // incomplete provenance and a project that differs from the group's are refused on insert; the provenance
        // columns, a group row (project_id included) and the project of a grouped draft cannot be updated, so a
        // draft and its group can never end up in different projects. A project id is a UUID that never changes; a
        // rename of a project that owns groups is therefore refused here (its ON UPDATE CASCADE would update the
        // draft and the group one after the other and trip the guard).
        // Idempotent in the v57/v60/v61 shape, and deliberately NON-destructive: this block can run again on a
        // database whose version was rewound, and a DROP TABLE of run_draft_groups would take (or null) the
        // provenance of the drafts that already reference it. So: tables and indexes are CREATE ... IF NOT EXISTS,
        // the columns are inspected first and only the missing ones added, and a trigger (which holds no data) is
        // dropped and created again, so its definition is always the current one.
        std::unordered_set<std::string> draftColumns;
        {
            Statement info;
            const auto prepared = prepare(impl->handle, "PRAGMA table_info(run_drafts);", info);
            if (!prepared.success()) {
                return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*prepared.error());
            }
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<std::shared_ptr<SqliteDatabase>>::failure(databaseError(
                        impl->handle, "migration.v62_failed", "Could not inspect run_drafts table schema"));
                }
                draftColumns.insert(columnText(info.get(), 1));
            }
        }
        std::string migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS run_draft_groups (
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
);
)sql";
        struct DraftColumn {
            const char* name;
            const char* definition;
        };
        static constexpr DraftColumn provenanceColumns[] = {
            {"group_id", "group_id TEXT REFERENCES run_draft_groups(id)"},
            {"group_ordinal", "group_ordinal INTEGER CHECK(group_ordinal IS NULL OR group_ordinal >= 0)"},
            {"plan_key", "plan_key TEXT CHECK(plan_key IS NULL OR length(plan_key) BETWEEN 1 AND 64)"},
            {"depends_on_json", "depends_on_json TEXT"},
            {"created_content_fingerprint",
                "created_content_fingerprint TEXT CHECK(created_content_fingerprint IS NULL "
                "OR length(created_content_fingerprint) = 64)"},
        };
        for (const auto& column : provenanceColumns) {
            if (!draftColumns.contains(column.name)) {
                migration += std::string("ALTER TABLE run_drafts ADD COLUMN ") + column.definition + ";\n";
            }
        }
        migration += R"sql(
CREATE UNIQUE INDEX IF NOT EXISTS run_drafts_group_ordinal_uidx
    ON run_drafts(group_id, group_ordinal) WHERE group_id IS NOT NULL;
CREATE UNIQUE INDEX IF NOT EXISTS run_drafts_group_key_uidx
    ON run_drafts(group_id, plan_key COLLATE NOCASE) WHERE group_id IS NOT NULL;
DROP TRIGGER IF EXISTS run_draft_groups_reject_update;
CREATE TRIGGER run_draft_groups_reject_update
BEFORE UPDATE OF id, project_id, origin_kind, idempotency_key, plan_title, plan_fingerprint, source_conversation_id,
    source_message_id, source_message_fingerprint, context_fingerprint, source_run_ids_json, created_at_ms
ON run_draft_groups
BEGIN
    SELECT RAISE(ABORT, 'run_draft_group_immutable');
END;
DROP TRIGGER IF EXISTS run_drafts_provenance_complete;
CREATE TRIGGER run_drafts_provenance_complete
BEFORE INSERT ON run_drafts
WHEN NOT ((NEW.group_id IS NULL AND NEW.group_ordinal IS NULL AND NEW.plan_key IS NULL
            AND NEW.depends_on_json IS NULL AND NEW.created_content_fingerprint IS NULL)
        OR (NEW.group_id IS NOT NULL AND NEW.group_ordinal IS NOT NULL AND NEW.plan_key IS NOT NULL
            AND NEW.depends_on_json IS NOT NULL AND NEW.created_content_fingerprint IS NOT NULL))
BEGIN
    SELECT RAISE(ABORT, 'run_draft_provenance_incomplete');
END;
DROP TRIGGER IF EXISTS run_drafts_group_project_match;
CREATE TRIGGER run_drafts_group_project_match
BEFORE INSERT ON run_drafts
WHEN NEW.group_id IS NOT NULL
 AND NEW.project_id IS NOT (SELECT project_id FROM run_draft_groups WHERE id = NEW.group_id)
BEGIN
    SELECT RAISE(ABORT, 'run_draft_group_project_mismatch');
END;
DROP TRIGGER IF EXISTS run_drafts_group_project_immutable;
CREATE TRIGGER run_drafts_group_project_immutable
BEFORE UPDATE OF project_id ON run_drafts
WHEN OLD.group_id IS NOT NULL AND NEW.project_id IS NOT OLD.project_id
BEGIN
    SELECT RAISE(ABORT, 'run_draft_group_project_immutable');
END;
DROP TRIGGER IF EXISTS run_drafts_provenance_immutable;
CREATE TRIGGER run_drafts_provenance_immutable
BEFORE UPDATE OF group_id, group_ordinal, plan_key, depends_on_json, created_content_fingerprint ON run_drafts
WHEN NEW.group_id IS NOT OLD.group_id OR NEW.group_ordinal IS NOT OLD.group_ordinal
  OR NEW.plan_key IS NOT OLD.plan_key OR NEW.depends_on_json IS NOT OLD.depends_on_json
  OR NEW.created_content_fingerprint IS NOT OLD.created_content_fingerprint
BEGIN
    SELECT RAISE(ABORT, 'run_draft_provenance_immutable');
END;
PRAGMA user_version = 62;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration.c_str(), "migration.v62_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 63) {
        // PC-3E: the exact Project Context Snapshot text sent with one Assistant turn. The row is separate from
        // mutable message output and is immutable. Deleting its parent conversation/message still cascades, while
        // retrying or editing the Assistant output cannot alter the context that produced it.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS chat_project_context_attachments (
    assistant_message_id TEXT PRIMARY KEY NOT NULL
        REFERENCES chat_messages(id) ON UPDATE CASCADE ON DELETE CASCADE,
    conversation_id TEXT NOT NULL
        REFERENCES chat_conversations(id) ON UPDATE CASCADE ON DELETE CASCADE,
    -- RESTRICT on purpose: the sent context is evidence of what left the machine for that project, so a project that
    -- has one cannot be deleted from under it. The application archives projects and never deletes them today; a
    -- future project deletion must decide explicitly what happens to these rows.
    project_id TEXT NOT NULL REFERENCES projects(id) ON UPDATE CASCADE ON DELETE RESTRICT,
    schema_version TEXT NOT NULL CHECK(schema_version = 'chat-project-context-attachment.v1'),
    snapshot_fingerprint TEXT NOT NULL CHECK(length(snapshot_fingerprint) = 64
        AND snapshot_fingerprint NOT GLOB '*[^0-9a-f]*'),
    rendered_content_fingerprint TEXT NOT NULL CHECK(length(rendered_content_fingerprint) = 64
        AND rendered_content_fingerprint NOT GLOB '*[^0-9a-f]*'),
    snapshot_policy_id TEXT NOT NULL CHECK(snapshot_policy_id = 'project_context_budget_v1'),
    workspace_fingerprint TEXT NOT NULL CHECK(length(workspace_fingerprint) = 64
        AND workspace_fingerprint NOT GLOB '*[^0-9a-f]*'),
    selected_run_id TEXT,
    includes_derived_project_memory INTEGER NOT NULL CHECK(includes_derived_project_memory IN (0,1)),
    snapshot_estimated_tokens INTEGER NOT NULL CHECK(snapshot_estimated_tokens BETWEEN 1 AND 24000),
    rendered_estimated_tokens INTEGER NOT NULL CHECK(rendered_estimated_tokens > 0),
    rendered_text TEXT NOT NULL CHECK(length(CAST(rendered_text AS BLOB)) BETWEEN 1 AND 524288)
);
DROP TRIGGER IF EXISTS chat_project_context_reject_update;
CREATE TRIGGER chat_project_context_reject_update
BEFORE UPDATE ON chat_project_context_attachments
BEGIN
    SELECT RAISE(ABORT, 'chat_project_context_immutable');
END;
DROP TRIGGER IF EXISTS chat_project_context_validate_insert;
CREATE TRIGGER chat_project_context_validate_insert
BEFORE INSERT ON chat_project_context_attachments
WHEN NOT EXISTS(
    SELECT 1
    FROM chat_messages AS message
    JOIN chat_conversations AS conversation ON conversation.id = message.conversation_id
    WHERE message.id = NEW.assistant_message_id
      AND message.role = 'assistant'
      AND message.conversation_id = NEW.conversation_id
      AND conversation.project_id = NEW.project_id
)
BEGIN
    SELECT RAISE(ABORT, 'chat_project_context_invalid_identity');
END;
DROP TRIGGER IF EXISTS chat_project_context_message_role_guard;
CREATE TRIGGER chat_project_context_message_role_guard
BEFORE UPDATE OF role ON chat_messages
WHEN NEW.role NOT IN ('assistant','error')
 AND EXISTS(SELECT 1 FROM chat_project_context_attachments
            WHERE assistant_message_id = OLD.id)
BEGIN
    SELECT RAISE(ABORT, 'chat_project_context_message_role_guard');
END;
DROP TRIGGER IF EXISTS chat_project_context_message_identity_immutable;
CREATE TRIGGER chat_project_context_message_identity_immutable
BEFORE UPDATE OF conversation_id ON chat_messages
WHEN NEW.conversation_id IS NOT OLD.conversation_id
 AND EXISTS(SELECT 1 FROM chat_project_context_attachments
            WHERE assistant_message_id = OLD.id)
BEGIN
    SELECT RAISE(ABORT, 'chat_project_context_message_identity_immutable');
END;
PRAGMA user_version = 63;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v63_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 64) {
        // PC-4B: the Outcome Brief. outcome_brief_revisions is an APPEND-ONLY history (no UPDATE, no DELETE) of what the
        // Backend derived about one terminal outcome Run; outcome_brief_heads is the DERIVED pointer to the newest
        // revision of each Run. The tables are written by one transaction and guarded by triggers, so that neither a
        // second writer nor a hand-made statement can break the history: ordinals are 1, 2, 3... without gaps, a
        // revision never repeats the source state of the one before it, and the head can only point at the newest one.
        // That the revision's project is the project of its Run, and that the Run exists in the status the brief says,
        // is checked by the repository inside the write transaction - NOT by the schema. A trigger that reads `runs`,
        // or a composite foreign key onto an extra unique index of `runs`, makes the old v52 rebuild of that table
        // (DROP TABLE runs + RENAME) fail on a database that already carries it, and a database whose version is set
        // back re-runs every migration from there on (the v42 fixture of SqliteRepositoryTests does exactly that).
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS outcome_brief_revisions (
    id TEXT PRIMARY KEY NOT NULL CHECK(length(id) BETWEEN 1 AND 128),
    -- RESTRICT on purpose: a brief is evidence of what the Backend said about a Run and its project, so neither can
    -- be deleted (or renamed) from under it. The application never deletes Runs today; a future deletion must decide
    -- explicitly what happens to these rows.
    run_id TEXT NOT NULL REFERENCES runs(id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    project_id TEXT NOT NULL REFERENCES projects(id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    ordinal INTEGER NOT NULL CHECK(ordinal >= 1),
    -- The exact schema / policy spellings are enforced by validateOutcomeBrief, not here: a new policy is a new
    -- revision, never a migration.
    schema_version TEXT NOT NULL CHECK(length(schema_version) BETWEEN 1 AND 64),
    policy_id TEXT NOT NULL CHECK(length(policy_id) BETWEEN 1 AND 128),
    workflow_id TEXT NOT NULL CHECK(length(workflow_id) BETWEEN 1 AND 256),
    terminal_status TEXT NOT NULL CHECK(terminal_status IN ('completed','failed','cancelled','interrupted')),
    state TEXT NOT NULL CHECK(state IN ('resolved_no_change','awaiting_review','awaiting_apply','resolved_applied',
        'resolved_rejected','resolved_mixed','attention_required','awaiting_manual_resolution',
        'resolved_manually_applied','superseded_by_continuation','recovery_only')),
    reasons_json TEXT NOT NULL CHECK(substr(reasons_json, 1, 1) = '[' AND substr(reasons_json, -1, 1) = ']'),
    counts_json TEXT NOT NULL CHECK(substr(counts_json, 1, 1) = '{' AND substr(counts_json, -1, 1) = '}'),
    member_decisions_json TEXT NOT NULL CHECK(substr(member_decisions_json, 1, 1) = '['
        AND substr(member_decisions_json, -1, 1) = ']'),
    recovery_eligible INTEGER NOT NULL CHECK(recovery_eligible IN (0,1)),
    requires_inspection INTEGER NOT NULL CHECK(requires_inspection IN (0,1)),
    continuation_source_run_id TEXT CHECK(continuation_source_run_id IS NULL
        OR length(continuation_source_run_id) BETWEEN 1 AND 256),
    continuation_child_run_id TEXT CHECK(continuation_child_run_id IS NULL
        OR length(continuation_child_run_id) BETWEEN 1 AND 256),
    manual_handoff_id TEXT CHECK(manual_handoff_id IS NULL OR length(manual_handoff_id) BETWEEN 1 AND 256),
    source_state_json TEXT NOT NULL CHECK(length(CAST(source_state_json AS BLOB)) BETWEEN 1 AND 4194304),
    source_state_fingerprint TEXT NOT NULL CHECK(length(source_state_fingerprint) = 64
        AND source_state_fingerprint NOT GLOB '*[^0-9a-f]*'),
    rendered_text TEXT NOT NULL CHECK(length(CAST(rendered_text AS BLOB)) BETWEEN 1 AND 65536),
    content_fingerprint TEXT NOT NULL CHECK(length(content_fingerprint) = 64
        AND content_fingerprint NOT GLOB '*[^0-9a-f]*'),
    created_at_ms INTEGER NOT NULL,
    CHECK((state = 'attention_required') = (reasons_json <> '[]')),
    UNIQUE(run_id, ordinal)
);
CREATE INDEX IF NOT EXISTS outcome_brief_revisions_project_idx
    ON outcome_brief_revisions(project_id, run_id, ordinal);
CREATE TABLE IF NOT EXISTS outcome_brief_heads (
    run_id TEXT PRIMARY KEY NOT NULL REFERENCES runs(id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    revision_id TEXT NOT NULL REFERENCES outcome_brief_revisions(id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    updated_at_ms INTEGER NOT NULL
);
DROP TRIGGER IF EXISTS outcome_brief_revisions_validate_ordinal;
CREATE TRIGGER outcome_brief_revisions_validate_ordinal
BEFORE INSERT ON outcome_brief_revisions
WHEN NEW.ordinal IS NOT COALESCE((SELECT MAX(ordinal) FROM outcome_brief_revisions WHERE run_id = NEW.run_id), 0) + 1
BEGIN
    SELECT RAISE(ABORT, 'outcome_brief_ordinal_gap');
END;
DROP TRIGGER IF EXISTS outcome_brief_revisions_reject_repeat;
CREATE TRIGGER outcome_brief_revisions_reject_repeat
BEFORE INSERT ON outcome_brief_revisions
WHEN EXISTS(SELECT 1 FROM outcome_brief_revisions
            WHERE run_id = NEW.run_id AND ordinal = NEW.ordinal - 1
              AND source_state_fingerprint = NEW.source_state_fingerprint)
BEGIN
    SELECT RAISE(ABORT, 'outcome_brief_repeats_head');
END;
DROP TRIGGER IF EXISTS outcome_brief_revisions_reject_update;
CREATE TRIGGER outcome_brief_revisions_reject_update
BEFORE UPDATE ON outcome_brief_revisions
BEGIN
    SELECT RAISE(ABORT, 'outcome_brief_revision_immutable');
END;
DROP TRIGGER IF EXISTS outcome_brief_revisions_reject_delete;
CREATE TRIGGER outcome_brief_revisions_reject_delete
BEFORE DELETE ON outcome_brief_revisions
BEGIN
    SELECT RAISE(ABORT, 'outcome_brief_revision_immutable');
END;
DROP TRIGGER IF EXISTS outcome_brief_heads_validate_insert;
CREATE TRIGGER outcome_brief_heads_validate_insert
BEFORE INSERT ON outcome_brief_heads
WHEN NOT EXISTS(
    SELECT 1 FROM outcome_brief_revisions AS revision
    WHERE revision.id = NEW.revision_id
      AND revision.run_id = NEW.run_id
      AND revision.ordinal = (SELECT MAX(ordinal) FROM outcome_brief_revisions WHERE run_id = NEW.run_id)
)
BEGIN
    SELECT RAISE(ABORT, 'outcome_brief_head_not_latest');
END;
DROP TRIGGER IF EXISTS outcome_brief_heads_validate_update;
CREATE TRIGGER outcome_brief_heads_validate_update
BEFORE UPDATE ON outcome_brief_heads
WHEN NEW.run_id IS NOT OLD.run_id
BEGIN
    SELECT RAISE(ABORT, 'outcome_brief_head_identity_immutable');
END;
DROP TRIGGER IF EXISTS outcome_brief_heads_validate_move;
CREATE TRIGGER outcome_brief_heads_validate_move
BEFORE UPDATE ON outcome_brief_heads
WHEN NOT EXISTS(
    SELECT 1 FROM outcome_brief_revisions AS revision
    WHERE revision.id = NEW.revision_id
      AND revision.run_id = NEW.run_id
      AND revision.ordinal = (SELECT MAX(ordinal) FROM outcome_brief_revisions WHERE run_id = NEW.run_id)
)
BEGIN
    SELECT RAISE(ABORT, 'outcome_brief_head_not_latest');
END;
PRAGMA user_version = 64;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v64_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 65) {
        // ACD-2: duration-aware prompt-cache persistence and catalog prices.
        // Old rows remain explicitly unpartitioned: the aggregate cache-write
        // counter is preserved and every new detail column defaults to zero.
        const auto columnsOf = [&](const char* table)
            -> common::Result<std::unordered_set<std::string>> {
            std::unordered_set<std::string> columns;
            Statement info;
            const std::string pragma = std::string("PRAGMA table_info(") + table + ");";
            const auto prepared = prepare(impl->handle, pragma.c_str(), info);
            if (!prepared.success()) {
                return common::Result<std::unordered_set<std::string>>::failure(*prepared.error());
            }
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<std::unordered_set<std::string>>::failure(databaseError(
                        impl->handle, "migration.v65_failed", "Could not inspect table schema"));
                }
                columns.insert(columnText(info.get(), 1));
            }
            return common::Result<std::unordered_set<std::string>>::ok(std::move(columns));
        };
        const auto modelColumnsV65 = columnsOf("models");
        const auto runColumnsV65 = columnsOf("runs");
        const auto executionColumnsV65 = columnsOf("model_executions");
        if (!modelColumnsV65.success())
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*modelColumnsV65.error());
        if (!runColumnsV65.success())
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*runColumnsV65.error());
        if (!executionColumnsV65.success())
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*executionColumnsV65.error());

        struct AddedColumn { const char* table; const char* name; const char* definition; };
        static constexpr AddedColumn additions[] = {
            {"models", "cache_write_one_hour_price_micros_per_million_tokens",
                "INTEGER NOT NULL DEFAULT 0 CHECK(cache_write_one_hour_price_micros_per_million_tokens >= 0)"},
            {"models", "long_context_cache_write_one_hour_price_micros_per_million_tokens",
                "INTEGER NOT NULL DEFAULT 0 CHECK(long_context_cache_write_one_hour_price_micros_per_million_tokens >= 0)"},
            {"runs", "cache_creation_duration_partition_known",
                "INTEGER NOT NULL DEFAULT 0 CHECK(cache_creation_duration_partition_known IN (0,1))"},
            {"runs", "cache_creation_five_minute_input_tokens",
                "INTEGER NOT NULL DEFAULT 0 CHECK(cache_creation_five_minute_input_tokens >= 0)"},
            {"runs", "cache_creation_one_hour_input_tokens",
                "INTEGER NOT NULL DEFAULT 0 CHECK(cache_creation_one_hour_input_tokens >= 0)"},
            {"model_executions", "cache_creation_input_tokens",
                "INTEGER NOT NULL DEFAULT 0 CHECK(cache_creation_input_tokens >= 0)"},
            {"model_executions", "cache_read_input_tokens",
                "INTEGER NOT NULL DEFAULT 0 CHECK(cache_read_input_tokens >= 0)"},
            {"model_executions", "cache_creation_duration_partition_known",
                "INTEGER NOT NULL DEFAULT 0 CHECK(cache_creation_duration_partition_known IN (0,1))"},
            {"model_executions", "cache_creation_five_minute_input_tokens",
                "INTEGER NOT NULL DEFAULT 0 CHECK(cache_creation_five_minute_input_tokens >= 0)"},
            {"model_executions", "cache_creation_one_hour_input_tokens",
                "INTEGER NOT NULL DEFAULT 0 CHECK(cache_creation_one_hour_input_tokens >= 0)"},
        };
        const auto hasColumns = [&](const char* table) -> const std::unordered_set<std::string>& {
            if (std::string_view(table) == "models") return modelColumnsV65.value();
            if (std::string_view(table) == "runs") return runColumnsV65.value();
            return executionColumnsV65.value();
        };
        std::string migration = "BEGIN IMMEDIATE;\n";
        for (const auto& addition : additions) {
            if (!hasColumns(addition.table).contains(addition.name)) {
                migration += std::string("ALTER TABLE ") + addition.table + " ADD COLUMN "
                    + addition.name + ' ' + addition.definition + ";\n";
            }
        }
        migration += R"sql(
DROP TRIGGER IF EXISTS runs_cache_duration_validate_insert;
CREATE TRIGGER runs_cache_duration_validate_insert BEFORE INSERT ON runs
WHEN NOT ((NEW.cache_creation_duration_partition_known = 0
           AND NEW.cache_creation_five_minute_input_tokens = 0
           AND NEW.cache_creation_one_hour_input_tokens = 0)
       OR (NEW.cache_creation_duration_partition_known = 1
           AND NEW.cache_creation_five_minute_input_tokens <= NEW.cache_creation_input_tokens
           AND NEW.cache_creation_one_hour_input_tokens
               = NEW.cache_creation_input_tokens - NEW.cache_creation_five_minute_input_tokens))
BEGIN SELECT RAISE(ABORT, 'run_cache_duration_invalid'); END;
DROP TRIGGER IF EXISTS runs_cache_duration_validate_update;
CREATE TRIGGER runs_cache_duration_validate_update BEFORE UPDATE OF
    cache_creation_input_tokens,cache_creation_duration_partition_known,
    cache_creation_five_minute_input_tokens,cache_creation_one_hour_input_tokens ON runs
WHEN NOT ((NEW.cache_creation_duration_partition_known = 0
           AND NEW.cache_creation_five_minute_input_tokens = 0
           AND NEW.cache_creation_one_hour_input_tokens = 0)
       OR (NEW.cache_creation_duration_partition_known = 1
           AND NEW.cache_creation_five_minute_input_tokens <= NEW.cache_creation_input_tokens
           AND NEW.cache_creation_one_hour_input_tokens
               = NEW.cache_creation_input_tokens - NEW.cache_creation_five_minute_input_tokens))
BEGIN SELECT RAISE(ABORT, 'run_cache_duration_invalid'); END;
DROP TRIGGER IF EXISTS model_executions_cache_duration_validate_insert;
CREATE TRIGGER model_executions_cache_duration_validate_insert BEFORE INSERT ON model_executions
WHEN NEW.cache_creation_input_tokens > NEW.input_tokens
  OR NEW.cache_read_input_tokens > NEW.input_tokens - NEW.cache_creation_input_tokens
  OR NOT ((NEW.cache_creation_duration_partition_known = 0
           AND NEW.cache_creation_five_minute_input_tokens = 0
           AND NEW.cache_creation_one_hour_input_tokens = 0)
       OR (NEW.cache_creation_duration_partition_known = 1
           AND NEW.cache_creation_five_minute_input_tokens <= NEW.cache_creation_input_tokens
           AND NEW.cache_creation_one_hour_input_tokens
               = NEW.cache_creation_input_tokens - NEW.cache_creation_five_minute_input_tokens))
BEGIN SELECT RAISE(ABORT, 'model_execution_cache_duration_invalid'); END;
DROP TRIGGER IF EXISTS model_executions_cache_duration_validate_update;
CREATE TRIGGER model_executions_cache_duration_validate_update BEFORE UPDATE OF
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
BEGIN SELECT RAISE(ABORT, 'model_execution_cache_duration_invalid'); END;
)sql";
        const auto altered = execute(impl->handle, migration.c_str(), "migration.v65_failed");
        if (!altered.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*altered.error());
        }
        const auto invalidCount = [&](const char* sql) -> common::Result<std::int64_t> {
            Statement rows;
            const auto prepared = prepare(impl->handle, sql, rows);
            if (!prepared.success()) return common::Result<std::int64_t>::failure(*prepared.error());
            if (sqlite3_step(rows.get()) != SQLITE_ROW) {
                return common::Result<std::int64_t>::failure(databaseError(
                    impl->handle, "migration.v65_failed", "Could not verify cache-duration data"));
            }
            return common::Result<std::int64_t>::ok(sqlite3_column_int64(rows.get(), 0));
        };
        const auto invalidRuns = invalidCount(
            "SELECT COUNT(*) FROM runs WHERE NOT ((cache_creation_duration_partition_known=0 "
            "AND cache_creation_five_minute_input_tokens=0 AND cache_creation_one_hour_input_tokens=0) "
            "OR (cache_creation_duration_partition_known=1 "
            "AND cache_creation_five_minute_input_tokens<=cache_creation_input_tokens "
            "AND cache_creation_one_hour_input_tokens=cache_creation_input_tokens-cache_creation_five_minute_input_tokens));");
        const auto invalidExecutions = invalidCount(
            "SELECT COUNT(*) FROM model_executions WHERE cache_creation_input_tokens>input_tokens "
            "OR cache_read_input_tokens>input_tokens-cache_creation_input_tokens "
            "OR NOT ((cache_creation_duration_partition_known=0 "
            "AND cache_creation_five_minute_input_tokens=0 AND cache_creation_one_hour_input_tokens=0) "
            "OR (cache_creation_duration_partition_known=1 "
            "AND cache_creation_five_minute_input_tokens<=cache_creation_input_tokens "
            "AND cache_creation_one_hour_input_tokens=cache_creation_input_tokens-cache_creation_five_minute_input_tokens));");
        if (!invalidRuns.success() || !invalidExecutions.success()
            || invalidRuns.value() != 0 || invalidExecutions.value() != 0) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(error(
                "migration.v65_failed", "Existing cache-duration data is inconsistent"));
        }
        // v65 owns only the newly introduced one-hour price. Do not re-audit
        // unrelated legacy tier fields here: once a database has already
        // crossed v59, their corruption is isolated by readModel() instead of
        // blocking every later additive migration. A partially applied v65,
        // however, may already contain the new long-context rate, so refuse
        // the one new impossible combination before raising the version.
        const auto invalidDurationPrices = invalidCount(
            "SELECT COUNT(*) FROM models WHERE long_context_threshold_input_tokens=0 "
            "AND long_context_cache_write_one_hour_price_micros_per_million_tokens<>0;");
        if (!invalidDurationPrices.success() || invalidDurationPrices.value() != 0) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(
                error("migration.v65_failed",
                    "Existing one-hour long-context cache prices require a long-context threshold"));
        }
        const auto committed = execute(impl->handle,
            "PRAGMA user_version = 65;\nCOMMIT;", "migration.v65_failed");
        if (!committed.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*committed.error());
        }
    }

    if (schemaVersion < 66) {
        // PC-5B: the durable follow-up plan request, plus the two rebuilds the agreed design demands.
        //  1. follow_up_plan_requests: an immutable identity and quote, a small state machine whose only legal edges are the
        //     ones of transitionFollowUpPlanRequest (checked by a trigger as well as by the adapter), a claim stamp that
        //     separates a free request from a possibly charged one, and an immutable result. Like the outcome_brief_*
        //     tables it carries no trigger or key that reads `runs`: that the request's project is its Run's project is
        //     checked by the repository, inside the write transaction and on every read (the PC-4B lesson about the old
        //     `runs` rebuild).
        //  2. run_draft_groups is REBUILT: SQLite cannot alter a CHECK, and the origin list gains 'follow_up_request',
        //     the group gains a unique source_request_id, and a context fingerprint is now allowed for that origin too.
        //     Rows keep their rowid and every old column byte for byte; source_request_id is NULL for them.
        //  3. model_executions is REBUILT for the same reason: source_type gains 'follow_up'.
        // Both rebuilds follow the v13/v52 procedure (foreign keys off on this connection, one transaction, indexes and
        // triggers recreated before the commit). The one trigger that reads run_draft_groups from another table
        // (run_drafts_group_project_match) is dropped before the rename and created again, because SQLite re-parses
        // every trigger when a table is renamed. A rebuild is skipped when the stored definition already has the new
        // spelling, so re-running this step over a v66 schema (a rewound user_version) is a no-op.
        const auto storedTableSql = [&](const char* table) -> common::Result<std::string> {
            Statement statement;
            const auto prepared = prepare(
                impl->handle, "SELECT sql FROM sqlite_master WHERE type='table' AND name=?;", statement);
            if (!prepared.success()) return common::Result<std::string>::failure(*prepared.error());
            const auto bound = bindText(impl->handle, statement.get(), 1, table);
            if (!bound.success()) return common::Result<std::string>::failure(*bound.error());
            if (sqlite3_step(statement.get()) != SQLITE_ROW) {
                return common::Result<std::string>::failure(
                    error("migration.v66_failed", std::string("Table '") + table + "' is missing"));
            }
            return common::Result<std::string>::ok(columnText(statement.get(), 0));
        };
        const auto groupsSql = storedTableSql("run_draft_groups");
        if (!groupsSql.success()) return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*groupsSql.error());
        const auto executionsSql = storedTableSql("model_executions");
        if (!executionsSql.success()) return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*executionsSql.error());
        const bool rebuildGroups = groupsSql.value().find("'follow_up_request'") == std::string::npos;
        const bool rebuildExecutions = executionsSql.value().find("'follow_up'") == std::string::npos;
        const bool rebuilding = rebuildGroups || rebuildExecutions;

        std::string migration;
        if (rebuilding) migration += "PRAGMA foreign_keys = OFF;\n";
        migration += "\nBEGIN IMMEDIATE;\n";
        migration += followUpPlanRequestsObjectsSql();

        if (rebuildGroups) {
            migration += R"sql(
DROP TRIGGER IF EXISTS run_drafts_group_project_match;
DROP TRIGGER IF EXISTS follow_up_requests_confirm_needs_group;
CREATE TABLE run_draft_groups_v66 (
    id TEXT PRIMARY KEY NOT NULL CHECK(length(id) BETWEEN 1 AND 64),
    project_id TEXT NOT NULL REFERENCES projects(id) ON UPDATE CASCADE ON DELETE CASCADE,
    origin_kind TEXT NOT NULL CHECK(origin_kind IN ('chat_message','manual_import','follow_up_request')),
    idempotency_key TEXT NOT NULL UNIQUE CHECK(length(idempotency_key) = 64),
    plan_title TEXT NOT NULL CHECK(length(CAST(plan_title AS BLOB)) BETWEEN 1 AND 512),
    plan_fingerprint TEXT NOT NULL CHECK(length(plan_fingerprint) = 64),
    source_conversation_id TEXT,
    source_message_id TEXT,
    source_message_fingerprint TEXT,
    context_fingerprint TEXT CHECK(context_fingerprint IS NULL OR length(context_fingerprint) = 64),
    source_run_ids_json TEXT NOT NULL DEFAULT '[]',
    created_at_ms INTEGER NOT NULL,
    source_request_id TEXT UNIQUE CHECK(source_request_id IS NULL OR length(source_request_id) BETWEEN 1 AND 64)
        REFERENCES follow_up_plan_requests(id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    CHECK((origin_kind = 'chat_message') = (source_message_id IS NOT NULL)),
    CHECK((origin_kind = 'follow_up_request') = (source_request_id IS NOT NULL)),
    CHECK((source_message_id IS NULL) = (source_conversation_id IS NULL)),
    CHECK((source_message_id IS NULL) = (source_message_fingerprint IS NULL)),
    CHECK(source_message_fingerprint IS NULL OR length(source_message_fingerprint) = 64),
    CHECK(context_fingerprint IS NULL OR origin_kind IN ('chat_message','follow_up_request')),
    CHECK(origin_kind <> 'follow_up_request' OR context_fingerprint IS NOT NULL)
);
INSERT INTO run_draft_groups_v66 (
    rowid,id,project_id,origin_kind,idempotency_key,plan_title,plan_fingerprint,source_conversation_id,
    source_message_id,source_message_fingerprint,context_fingerprint,source_run_ids_json,created_at_ms)
SELECT
    rowid,id,project_id,origin_kind,idempotency_key,plan_title,plan_fingerprint,source_conversation_id,
    source_message_id,source_message_fingerprint,context_fingerprint,source_run_ids_json,created_at_ms
FROM run_draft_groups ORDER BY rowid;
DROP TABLE run_draft_groups;
ALTER TABLE run_draft_groups_v66 RENAME TO run_draft_groups;
)sql";
        }
        // Always (a trigger holds no data): the group's immutability now covers source_request_id, a follow-up group can
        // only be born from a request that is plan_ready with the same project, plan and sealed context, and a draft
        // still lands in its group's project.
        migration += R"sql(
DROP TRIGGER IF EXISTS run_draft_groups_reject_update;
CREATE TRIGGER run_draft_groups_reject_update
BEFORE UPDATE OF id, project_id, origin_kind, idempotency_key, plan_title, plan_fingerprint, source_conversation_id,
    source_message_id, source_message_fingerprint, context_fingerprint, source_run_ids_json, created_at_ms, source_request_id
ON run_draft_groups
BEGIN
    SELECT RAISE(ABORT, 'run_draft_group_immutable');
END;
DROP TRIGGER IF EXISTS run_draft_groups_follow_up_origin;
CREATE TRIGGER run_draft_groups_follow_up_origin
BEFORE INSERT ON run_draft_groups
WHEN NEW.origin_kind = 'follow_up_request'
 AND NOT EXISTS(SELECT 1 FROM follow_up_plan_requests AS request
                WHERE request.id = NEW.source_request_id AND request.project_id = NEW.project_id
                  AND request.state = 'plan_ready' AND request.plan_fingerprint = NEW.plan_fingerprint
                  AND request.context_fingerprint = NEW.context_fingerprint)
BEGIN
    SELECT RAISE(ABORT, 'run_draft_group_follow_up_not_confirmable');
END;
DROP TRIGGER IF EXISTS run_drafts_group_project_match;
CREATE TRIGGER run_drafts_group_project_match
BEFORE INSERT ON run_drafts
WHEN NEW.group_id IS NOT NULL
 AND NEW.project_id IS NOT (SELECT project_id FROM run_draft_groups WHERE id = NEW.group_id)
BEGIN
    SELECT RAISE(ABORT, 'run_draft_group_project_mismatch');
END;
)sql";
        migration += followUpConfirmNeedsGroupTriggerSql();

        if (rebuildExecutions) {
            migration += R"sql(
CREATE TABLE model_executions_v66 (
    id TEXT PRIMARY KEY NOT NULL,
    source_type TEXT NOT NULL CHECK(source_type IN ('run','comparison','chat','follow_up')),
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
    finished_at_ms INTEGER,
    cost_price_known INTEGER NOT NULL DEFAULT 1,
    cache_creation_input_tokens INTEGER NOT NULL DEFAULT 0 CHECK(cache_creation_input_tokens >= 0),
    cache_read_input_tokens INTEGER NOT NULL DEFAULT 0 CHECK(cache_read_input_tokens >= 0),
    cache_creation_duration_partition_known INTEGER NOT NULL DEFAULT 0 CHECK(cache_creation_duration_partition_known IN (0,1)),
    cache_creation_five_minute_input_tokens INTEGER NOT NULL DEFAULT 0 CHECK(cache_creation_five_minute_input_tokens >= 0),
    cache_creation_one_hour_input_tokens INTEGER NOT NULL DEFAULT 0 CHECK(cache_creation_one_hour_input_tokens >= 0),
    UNIQUE(source_type, source_id, position, attempt)
);
INSERT INTO model_executions_v66 (
    rowid,id,source_type,source_id,project_id,provider_id,model_id,role,position,attempt,status,request_hash,
    context_hash,output_artifact_id,output_content,output_preview,input_tokens,output_tokens,cost_micros,latency_ms,
    error_code,error_message,created_at_ms,started_at_ms,finished_at_ms,cost_price_known,cache_creation_input_tokens,
    cache_read_input_tokens,cache_creation_duration_partition_known,cache_creation_five_minute_input_tokens,
    cache_creation_one_hour_input_tokens)
SELECT
    rowid,id,source_type,source_id,project_id,provider_id,model_id,role,position,attempt,status,request_hash,
    context_hash,output_artifact_id,output_content,output_preview,input_tokens,output_tokens,cost_micros,latency_ms,
    error_code,error_message,created_at_ms,started_at_ms,finished_at_ms,cost_price_known,cache_creation_input_tokens,
    cache_read_input_tokens,cache_creation_duration_partition_known,cache_creation_five_minute_input_tokens,
    cache_creation_one_hour_input_tokens
FROM model_executions ORDER BY rowid;
DROP TABLE model_executions;
ALTER TABLE model_executions_v66 RENAME TO model_executions;
CREATE INDEX model_executions_source_idx
    ON model_executions(source_type,source_id,position,attempt);
CREATE INDEX model_executions_project_created_idx
    ON model_executions(project_id,created_at_ms,id);
CREATE TRIGGER model_executions_cache_duration_validate_insert BEFORE INSERT ON model_executions
WHEN NEW.cache_creation_input_tokens > NEW.input_tokens
  OR NEW.cache_read_input_tokens > NEW.input_tokens - NEW.cache_creation_input_tokens
  OR NOT ((NEW.cache_creation_duration_partition_known = 0
           AND NEW.cache_creation_five_minute_input_tokens = 0
           AND NEW.cache_creation_one_hour_input_tokens = 0)
       OR (NEW.cache_creation_duration_partition_known = 1
           AND NEW.cache_creation_five_minute_input_tokens <= NEW.cache_creation_input_tokens
           AND NEW.cache_creation_one_hour_input_tokens
               = NEW.cache_creation_input_tokens - NEW.cache_creation_five_minute_input_tokens))
BEGIN SELECT RAISE(ABORT, 'model_execution_cache_duration_invalid'); END;
CREATE TRIGGER model_executions_cache_duration_validate_update BEFORE UPDATE OF
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
BEGIN SELECT RAISE(ABORT, 'model_execution_cache_duration_invalid'); END;
)sql";
        }
        migration += "PRAGMA user_version = 66;\nCOMMIT;\n";
        if (rebuilding) migration += "PRAGMA foreign_keys = ON;\n";

        const auto migrated = execute(impl->handle, migration.c_str(), "migration.v66_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle,
                rebuilding ? "ROLLBACK; PRAGMA foreign_keys = ON;" : "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
        // The relationships this step touched, checked by name (not the whole database): a pre-existing violation
        // somewhere else must not block an unrelated upgrade, but none of ours may be left behind.
        for (const char* table : {"run_draft_groups", "run_drafts", "model_executions", "quality_evaluations",
                 "comparison_sessions", "follow_up_plan_requests"}) {
            Statement foreignKeyCheck;
            const std::string pragma = std::string("PRAGMA foreign_key_check(") + table + ");";
            const auto prepared = prepare(impl->handle, pragma.c_str(), foreignKeyCheck);
            if (!prepared.success() || sqlite3_step(foreignKeyCheck.get()) != SQLITE_DONE) {
                return common::Result<std::shared_ptr<SqliteDatabase>>::failure(error(
                    "migration.v66_foreign_key_check_failed",
                    std::string("The follow-up migration left an invalid foreign-key relationship in '") + table + "'"));
            }
        }
    }

    if (schemaVersion < 67) {
        // Repair of an UNRELEASED step: v66 was amended in place (the follow-up request table gained `accounting_json`, the state matrix
        // and the write-once trigger changed) while a database that had already run the first spelling of v66 stayed at user_version 66,
        // so the amended code failed there with "no such column: r.accounting_json". A table of the old shape is therefore rebuilt
        // here - the table, its indexes and every trigger on it, from the SAME SQL as step 66 - but ONLY when it holds no request: a
        // request is evidence of what was sent and paid for, so a non-empty table of the old shape is refused with its own error and
        // never dropped. A table that already has the column (every database created by the amended step 66) only gets the new version.
        bool hasAccounting = false;
        {
            Statement tableSql;
            const auto preparedSql = prepare(
                impl->handle, "SELECT sql FROM sqlite_master WHERE type='table' AND name='follow_up_plan_requests';", tableSql);
            if (!preparedSql.success()) return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*preparedSql.error());
            if (sqlite3_step(tableSql.get()) != SQLITE_ROW) {
                return common::Result<std::shared_ptr<SqliteDatabase>>::failure(
                    error("migration.v67_failed", "Table 'follow_up_plan_requests' is missing"));
            }
            hasAccounting = columnText(tableSql.get(), 0).find("accounting_json") != std::string::npos;
        }

        std::string migration = "BEGIN IMMEDIATE;\n";
        if (!hasAccounting) {
            sqlite3_int64 requests = 0;
            {
                Statement countStatement;
                const auto preparedCount = prepare(impl->handle, "SELECT COUNT(*) FROM follow_up_plan_requests;", countStatement);
                if (!preparedCount.success()) return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*preparedCount.error());
                if (sqlite3_step(countStatement.get()) != SQLITE_ROW) {
                    return common::Result<std::shared_ptr<SqliteDatabase>>::failure(error(
                        "migration.v67_failed", "The follow-up requests could not be counted before the repair"));
                }
                requests = sqlite3_column_int64(countStatement.get(), 0);
            }
            if (requests != 0) {
                return common::Result<std::shared_ptr<SqliteDatabase>>::failure(error("migration.v67_failed",
                    "The follow-up request table has the old shape and holds " + std::to_string(requests)
                        + " request(s): they are evidence of what was sent and paid for, so the table is not rebuilt automatically"));
            }
            // Dropping the table drops its indexes and every trigger ON it; the objects come back from step 66's own SQL.
            migration += "DROP TABLE follow_up_plan_requests;\n";
            migration += followUpPlanRequestsObjectsSql();
            migration += followUpConfirmNeedsGroupTriggerSql();
        }
        migration += "PRAGMA user_version = 67;\nCOMMIT;\n";

        const auto migrated = execute(impl->handle, migration.c_str(), "migration.v67_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
        Statement foreignKeyCheck;
        const auto preparedCheck = prepare(impl->handle, "PRAGMA foreign_key_check(follow_up_plan_requests);", foreignKeyCheck);
        if (!preparedCheck.success() || sqlite3_step(foreignKeyCheck.get()) != SQLITE_DONE) {
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(error(
                "migration.v67_foreign_key_check_failed", "The follow-up request repair left an invalid foreign-key relationship"));
        }
    }

    if (schemaVersion < 68) {
        // Run warnings: a durable note that a DIAGNOSTIC artifact of a Run (a report, a provenance note - nothing that is looked
        // up by name) could not be stored although the Run went on. A Run event is only a notification and the message of a
        // successful workflow is cleared, so without a table such a loss would leave no trace after a restart. The identity of
        // a warning is (run, stage, code, artifact name): recording the same failure twice leaves one row. The table is created
        // with IF NOT EXISTS and has no trigger that reads `runs` (see v64); a new step, never an edit of an applied one.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS run_warnings (
    id TEXT PRIMARY KEY NOT NULL CHECK(length(id) BETWEEN 1 AND 128),
    run_id TEXT NOT NULL REFERENCES runs(id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    stage_id TEXT CHECK(stage_id IS NULL OR length(stage_id) BETWEEN 1 AND 256),
    code TEXT NOT NULL CHECK(length(code) BETWEEN 1 AND 128),
    message TEXT NOT NULL CHECK(length(CAST(message AS BLOB)) BETWEEN 1 AND 1024),
    artifact_name TEXT NOT NULL CHECK(length(CAST(artifact_name AS BLOB)) <= 256),
    created_at_ms INTEGER NOT NULL
);
CREATE UNIQUE INDEX IF NOT EXISTS run_warnings_identity_idx
    ON run_warnings(run_id, ifnull(stage_id, ''), code, artifact_name);
CREATE INDEX IF NOT EXISTS run_warnings_run_idx ON run_warnings(run_id, created_at_ms, id);
PRAGMA user_version = 68;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v68_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 69) {
        // The folder for the temporary verification copies of a workspace: ONE per-installation setting (no value = the default,
        // "<TEMP>/aio-v"). A new table, because the application settings record is a versioned user-interface record with its own
        // revision rules and many call sites, and this value is validated against the file system by its own service. A new step,
        // created with IF NOT EXISTS, nothing applied is edited.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS verification_scratch_setting (
    id INTEGER PRIMARY KEY NOT NULL CHECK(id = 1),
    root_path TEXT CHECK(root_path IS NULL OR length(CAST(root_path AS BLOB)) BETWEEN 1 AND 2048),
    updated_at_ms INTEGER NOT NULL
);
PRAGMA user_version = 69;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v69_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 70) {
        // AG-18.4B-R1: the machine-local, per-project record of whether the project can be configured and built here - the
        // selected configure/build pair, the proven state, and what the proof was gathered with (configuration inputs, toolchain,
        // build directory). One row per project; a Ready row never speaks for another root, pair, input set or toolchain. A NEW
        // step with IF NOT EXISTS; no applied step is edited. Existing databases get no rows: a project is Unverified until it has
        // been checked.
        constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS project_verification_readiness (
    project_id TEXT PRIMARY KEY NOT NULL REFERENCES projects(id) ON UPDATE CASCADE ON DELETE CASCADE,
    project_root TEXT NOT NULL,
    configure_preset TEXT NOT NULL,
    build_preset TEXT NOT NULL,
    binary_dir TEXT NOT NULL,
    state TEXT NOT NULL CHECK(state IN ('unverified','ready','stale','failed')),
    inputs_fingerprint TEXT NOT NULL,
    toolchain_identity TEXT NOT NULL,
    targets_json TEXT NOT NULL,
    targets_complete INTEGER NOT NULL CHECK(targets_complete IN (0,1)),
    checked_at_ms INTEGER NOT NULL,
    last_success_at_ms INTEGER,
    failure_code TEXT NOT NULL,
    failure_message TEXT NOT NULL,
    CHECK(state <> 'ready' OR (configure_preset <> '' AND build_preset <> '' AND binary_dir <> ''
        AND inputs_fingerprint <> '' AND toolchain_identity <> '' AND last_success_at_ms IS NOT NULL))
);
PRAGMA user_version = 70;
COMMIT;
)sql";
        const auto migrated = execute(impl->handle, migration, "migration.v70_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(impl->handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<std::shared_ptr<SqliteDatabase>>::failure(*migrated.error());
        }
    }

    return common::Result<std::shared_ptr<SqliteDatabase>>::ok(
        std::shared_ptr<SqliteDatabase>(new SqliteDatabase(std::move(impl))));
}

const std::filesystem::path& SqliteDatabase::path() const noexcept
{
    return impl_->databasePath;
}

SqliteProjectRepository::SqliteProjectRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<Project> SqliteProjectRepository::insert(Project project)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO projects (id,name,description,root_path,status,created_at_ms,updated_at_ms) "
        "VALUES (?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<Project>::failure(*prepared.error());
    const auto bound = bindProject(handle, statement.get(), project);
    if (!bound.success()) return common::Result<Project>::failure(*bound.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        const int code = sqlite3_extended_errcode(handle);
        if (code == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<Project>::failure(
                error("project.duplicate_id", "Project id already exists"));
        }
        if (code == SQLITE_CONSTRAINT_UNIQUE) {
            return common::Result<Project>::failure(
                error("project.duplicate_path", "Project path is already registered"));
        }
        return common::Result<Project>::failure(
            databaseError(handle, "persistence.write_failed", "Could not insert project"));
    }
    return common::Result<Project>::ok(std::move(project));
}

common::Result<Project> SqliteProjectRepository::update(Project project)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "UPDATE projects SET name=?,description=?,root_path=?,status=?,created_at_ms=?,updated_at_ms=? "
        "WHERE id=?;",
        statement);
    if (!prepared.success()) return common::Result<Project>::failure(*prepared.error());

    const std::string path = pathToUtf8(project.rootPath);
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, project.name),
        bindText(handle, statement.get(), 2, project.description),
        bindText(handle, statement.get(), 3, path),
        bindText(handle, statement.get(), 4, application::toString(project.status)),
        bindInt64(handle, statement.get(), 5, toMilliseconds(project.createdAt)),
        bindInt64(handle, statement.get(), 6, toMilliseconds(project.updatedAt)),
        bindText(handle, statement.get(), 7, project.id)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<Project>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<Project>::failure(
            databaseError(handle, "persistence.write_failed", "Could not update project"));
    }
    if (sqlite3_changes(handle) == 0) {
        return common::Result<Project>::failure(error("project.not_found", "Project not found"));
    }
    return common::Result<Project>::ok(std::move(project));
}

common::Result<std::optional<Project>> SqliteProjectRepository::findById(
    const application::ProjectId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + projectColumns + " FROM projects WHERE id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<Project>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success()) return common::Result<std::optional<Project>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<Project>>::ok(std::nullopt);
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<Project>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read project"));
    }
    const auto project = readProject(statement.get());
    if (!project.success()) return common::Result<std::optional<Project>>::failure(*project.error());
    return common::Result<std::optional<Project>>::ok(project.value());
}

common::Result<std::optional<Project>> SqliteProjectRepository::findByRootPath(
    const std::filesystem::path& rootPath) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + projectColumns + " FROM projects WHERE root_path=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<Project>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, pathToUtf8(rootPath));
    if (!bound.success()) return common::Result<std::optional<Project>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<Project>>::ok(std::nullopt);
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<Project>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read project by root path"));
    }
    const auto project = readProject(statement.get());
    if (!project.success()) return common::Result<std::optional<Project>>::failure(*project.error());
    return common::Result<std::optional<Project>>::ok(project.value());
}

common::Result<std::vector<Project>> SqliteProjectRepository::listAll() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + projectColumns
        + " FROM projects ORDER BY created_at_ms,id;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<Project>>::failure(*prepared.error());

    std::vector<Project> projects;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<Project>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list projects"));
        }
        auto project = readProject(statement.get());
        if (!project.success()) return common::Result<std::vector<Project>>::failure(*project.error());
        projects.push_back(project.value());
    }
    return common::Result<std::vector<Project>>::ok(std::move(projects));
}

SqliteTaskRepository::SqliteTaskRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<ProjectTask> SqliteTaskRepository::insert(ProjectTask task)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO project_tasks "
        "(id,project_id,title,description,status,priority,requires_attention,due_at_ms,"
        "planned_run_at_ms,created_at_ms,updated_at_ms) VALUES (?,?,?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<ProjectTask>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, task.id),
        bindText(handle, statement.get(), 2, task.projectId),
        bindText(handle, statement.get(), 3, task.title),
        bindText(handle, statement.get(), 4, task.description),
        bindText(handle, statement.get(), 5, application::toString(task.status)),
        bindText(handle, statement.get(), 6, application::toString(task.priority)),
        bindInt64(handle, statement.get(), 7, task.requiresAttention ? 1 : 0),
        bindOptionalTime(handle, statement.get(), 8, task.dueAt),
        bindOptionalTime(handle, statement.get(), 9, task.plannedRunAt),
        bindInt64(handle, statement.get(), 10, toMilliseconds(task.createdAt)),
        bindInt64(handle, statement.get(), 11, toMilliseconds(task.updatedAt))};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<ProjectTask>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<ProjectTask>::failure(
            databaseError(handle, "persistence.write_failed", "Could not insert task"));
    }
    return common::Result<ProjectTask>::ok(std::move(task));
}

common::Result<ProjectTask> SqliteTaskRepository::update(ProjectTask task)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "UPDATE project_tasks SET title=?,description=?,status=?,priority=?,requires_attention=?,"
        "due_at_ms=?,planned_run_at_ms=?,updated_at_ms=? WHERE id=?;", statement);
    if (!prepared.success()) return common::Result<ProjectTask>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, task.title),
        bindText(handle, statement.get(), 2, task.description),
        bindText(handle, statement.get(), 3, application::toString(task.status)),
        bindText(handle, statement.get(), 4, application::toString(task.priority)),
        bindInt64(handle, statement.get(), 5, task.requiresAttention ? 1 : 0),
        bindOptionalTime(handle, statement.get(), 6, task.dueAt),
        bindOptionalTime(handle, statement.get(), 7, task.plannedRunAt),
        bindInt64(handle, statement.get(), 8, toMilliseconds(task.updatedAt)),
        bindText(handle, statement.get(), 9, task.id)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<ProjectTask>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<ProjectTask>::failure(
            databaseError(handle, "persistence.write_failed", "Could not update task"));
    }
    if (sqlite3_changes(handle) == 0) {
        return common::Result<ProjectTask>::failure(error("task.not_found", "Task not found"));
    }
    return common::Result<ProjectTask>::ok(std::move(task));
}

common::Result<std::optional<ProjectTask>> SqliteTaskRepository::findById(
    const application::TaskId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + taskColumns
        + " FROM project_tasks WHERE id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<ProjectTask>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success()) return common::Result<std::optional<ProjectTask>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<ProjectTask>>::ok(std::nullopt);
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<ProjectTask>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read task"));
    }
    auto task = readTask(statement.get());
    if (!task.success()) return common::Result<std::optional<ProjectTask>>::failure(*task.error());
    return common::Result<std::optional<ProjectTask>>::ok(task.value());
}

common::Result<std::vector<ProjectTask>> SqliteTaskRepository::list(
    const std::optional<application::ProjectId>& projectId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + taskColumns + " FROM project_tasks"
        + (projectId.has_value() ? " WHERE project_id=?" : "")
        + " ORDER BY updated_at_ms DESC,id;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<ProjectTask>>::failure(*prepared.error());
    if (projectId.has_value()) {
        const auto bound = bindText(handle, statement.get(), 1, *projectId);
        if (!bound.success()) return common::Result<std::vector<ProjectTask>>::failure(*bound.error());
    }
    std::vector<ProjectTask> tasks;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<ProjectTask>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list tasks"));
        }
        auto task = readTask(statement.get());
        if (!task.success()) return common::Result<std::vector<ProjectTask>>::failure(*task.error());
        tasks.push_back(task.value());
    }
    return common::Result<std::vector<ProjectTask>>::ok(std::move(tasks));
}

common::Result<void> SqliteTaskRepository::linkRun(
    const application::TaskId& taskId, const application::RunId& runId)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT OR REPLACE INTO task_run_links(task_id,run_id) VALUES (?,?);", statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const auto first = bindText(handle, statement.get(), 1, taskId);
    if (!first.success()) return common::Result<void>::failure(*first.error());
    const auto second = bindText(handle, statement.get(), 2, runId);
    if (!second.success()) return common::Result<void>::failure(*second.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not link task to run"));
    }
    return common::Result<void>::ok();
}

common::Result<std::vector<application::RunId>> SqliteTaskRepository::linkedRuns(
    const application::TaskId& taskId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT run_id FROM task_run_links WHERE task_id=? ORDER BY rowid;", statement);
    if (!prepared.success()) return common::Result<std::vector<application::RunId>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, taskId);
    if (!bound.success()) return common::Result<std::vector<application::RunId>>::failure(*bound.error());
    std::vector<application::RunId> runs;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<application::RunId>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list task runs"));
        }
        runs.push_back(columnText(statement.get(), 0));
    }
    return common::Result<std::vector<application::RunId>>::ok(std::move(runs));
}

SqliteRunRepository::SqliteRunRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<Run> SqliteRunRepository::insertRun(Run run)
{
    const auto validProviderUsage = validateProviderBudgetUsageForPersistence(
        run.providerBudgetUsage);
    if (!validProviderUsage.success()) {
        return common::Result<Run>::failure(*validProviderUsage.error());
    }
    const auto validProgress = validateProgress(run.progress, "Run");
    if (!validProgress.success()) return common::Result<Run>::failure(*validProgress.error());
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    const auto inserted = insertRunRow(handle, run);
    if (!inserted.success()) return common::Result<Run>::failure(*inserted.error());
    return common::Result<Run>::ok(std::move(run));
}

common::Result<Run> SqliteRunRepository::insertRunWithStages(
    Run run,
    std::vector<RunStage> stages)
{
    return insertRunWithStagesAndCopiedTaskLinks(
        std::move(run), std::move(stages), {});
}

common::Result<Run> SqliteRunRepository::insertRunWithStagesAndCopiedTaskLinks(
    Run run,
    std::vector<RunStage> stages,
    const application::RunId& taskLinkSourceRunId)
{
    // Validate all provider-neutral invariants before opening the transaction;
    // database constraints remain the final guard for duplicate identifiers.
    const auto validRunProgress = validateProgress(run.progress, "Run");
    if (!validRunProgress.success()) return common::Result<Run>::failure(*validRunProgress.error());
    for (const auto& stage : stages) {
        if (stage.runId != run.id) {
            return common::Result<Run>::failure(
                error("stage.run_mismatch", "Every stage must belong to the aggregate run"));
        }
        const auto validStageProgress = validateProgress(stage.progress, "Stage");
        if (!validStageProgress.success()) {
            return common::Result<Run>::failure(*validStageProgress.error());
        }
    }

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return common::Result<Run>::failure(*began.error());

    auto rollbackWith = [&](const common::Error& failure) {
        // Rollback is best-effort here; the original failure is the actionable
        // error the Application Layer must receive.
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return common::Result<Run>::failure(failure);
    };

    const auto insertedRun = insertRunRow(handle, run);
    if (!insertedRun.success()) return rollbackWith(*insertedRun.error());
    for (std::size_t index = 0; index < stages.size(); ++index) {
        const auto insertedStage = insertStageRow(handle, stages[index], index);
        if (!insertedStage.success()) return rollbackWith(*insertedStage.error());
    }
    // The immutable execution snapshot commits atomically with the run and
    // its stages: a Run row can never be observed with a partial or missing
    // snapshot. Empty for the Mock workflow (run.assignmentSnapshot is empty).
    for (const auto& assignment : run.assignmentSnapshot) {
        const auto insertedAssignment = insertRunStageAssignmentRow(handle, run.id, assignment);
        if (!insertedAssignment.success()) return rollbackWith(*insertedAssignment.error());
    }
    if (!taskLinkSourceRunId.empty()) {
        // Copy only links whose Task belongs to the new Run's Project. This
        // keeps Edit & rerun across Projects from importing foreign planning
        // state, while Retry in the same Project preserves every link.
        Statement copyLinks;
        const auto prepared = prepare(handle,
            "INSERT OR IGNORE INTO task_run_links(task_id,run_id) "
            "SELECT links.task_id,? FROM task_run_links AS links "
            "JOIN project_tasks AS tasks ON tasks.id=links.task_id "
            "WHERE links.run_id=? AND tasks.project_id=?;",
            copyLinks);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const common::Result<void> bindings[] = {
            bindText(handle, copyLinks.get(), 1, run.id),
            bindText(handle, copyLinks.get(), 2, taskLinkSourceRunId),
            bindText(handle, copyLinks.get(), 3, run.projectId)};
        for (const auto& binding : bindings) {
            if (!binding.success()) return rollbackWith(*binding.error());
        }
        if (sqlite3_step(copyLinks.get()) != SQLITE_DONE) {
            return rollbackWith(databaseError(
                handle, "persistence.write_failed",
                "Could not copy task links for rerun"));
        }
    }

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<Run>::ok(std::move(run));
}

common::Result<Run> SqliteRunRepository::insertRunWithStagesAndTaskLink(
    Run run,
    std::vector<RunStage> stages,
    const application::TaskId& taskId)
{
    const auto validRunProgress = validateProgress(run.progress, "Run");
    if (!validRunProgress.success()) return common::Result<Run>::failure(*validRunProgress.error());
    for (const auto& stage : stages) {
        if (stage.runId != run.id) {
            return common::Result<Run>::failure(
                error("stage.run_mismatch", "Every stage must belong to the aggregate run"));
        }
        const auto validStageProgress = validateProgress(stage.progress, "Stage");
        if (!validStageProgress.success()) {
            return common::Result<Run>::failure(*validStageProgress.error());
        }
    }

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return common::Result<Run>::failure(*began.error());

    auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return common::Result<Run>::failure(failure);
    };

    const auto insertedRun = insertRunRow(handle, run);
    if (!insertedRun.success()) return rollbackWith(*insertedRun.error());
    for (std::size_t index = 0; index < stages.size(); ++index) {
        const auto insertedStage = insertStageRow(handle, stages[index], index);
        if (!insertedStage.success()) return rollbackWith(*insertedStage.error());
    }
    for (const auto& assignment : run.assignmentSnapshot) {
        const auto insertedAssignment = insertRunStageAssignmentRow(handle, run.id, assignment);
        if (!insertedAssignment.success()) return rollbackWith(*insertedAssignment.error());
    }

    if (!taskId.empty()) {
        // Same statement SqliteTaskRepository::linkRun() runs standalone -
        // here it commits in the same transaction as the Run/stage insert so
        // a caller (RunManager) can never observe a persisted new Run
        // without its requested ProjectTask link, or vice versa.
        Statement link;
        const auto prepared = prepare(handle,
            "INSERT OR REPLACE INTO task_run_links(task_id,run_id) VALUES (?,?);", link);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const common::Result<void> bindings[] = {
            bindText(handle, link.get(), 1, taskId),
            bindText(handle, link.get(), 2, run.id)};
        for (const auto& binding : bindings) {
            if (!binding.success()) return rollbackWith(*binding.error());
        }
        if (sqlite3_step(link.get()) != SQLITE_DONE) {
            return rollbackWith(databaseError(
                handle, "persistence.write_failed", "Could not link task to new run"));
        }
    }

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<Run>::ok(std::move(run));
}

common::Result<Run> SqliteRunRepository::insertRunWithStagesAndLegacyConversion(
    Run run,
    std::vector<RunStage> stages,
    application::LegacyConversionRecord record,
    const std::optional<application::RunId>& taskLinkSourceRunId)
{
    // Every validation that does not need the database runs BEFORE the transaction opens, so a
    // refused record can never leave a Run (or half of one) behind.
    const auto validRunProgress = validateProgress(run.progress, "Run");
    if (!validRunProgress.success()) return common::Result<Run>::failure(*validRunProgress.error());
    for (const auto& stage : stages) {
        if (stage.runId != run.id) {
            return common::Result<Run>::failure(
                error("stage.run_mismatch", "Every stage must belong to the aggregate run"));
        }
        const auto validStageProgress = validateProgress(stage.progress, "Stage");
        if (!validStageProgress.success()) {
            return common::Result<Run>::failure(*validStageProgress.error());
        }
    }
    if (const auto problem = application::legacyConversionAggregateProblem(run, record)) {
        return common::Result<Run>::failure(
            error(application::kLegacyConversionRecordInvalidCode, *problem));
    }
    const auto droppedJson = application::legacyConversionDroppedToJson(record.dropped);
    if (!droppedJson.success()) return common::Result<Run>::failure(*droppedJson.error());

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return common::Result<Run>::failure(*began.error());

    auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return common::Result<Run>::failure(failure);
    };

    const auto insertedRun = insertRunRow(handle, run);
    if (!insertedRun.success()) return rollbackWith(*insertedRun.error());
    for (std::size_t index = 0; index < stages.size(); ++index) {
        const auto insertedStage = insertStageRow(handle, stages[index], index);
        if (!insertedStage.success()) return rollbackWith(*insertedStage.error());
    }
    for (const auto& assignment : run.assignmentSnapshot) {
        const auto insertedAssignment = insertRunStageAssignmentRow(handle, run.id, assignment);
        if (!insertedAssignment.success()) return rollbackWith(*insertedAssignment.error());
    }
    if (taskLinkSourceRunId.has_value() && !taskLinkSourceRunId->empty()) {
        // Same statement the other rerun aggregate uses: only links whose Task belongs to the
        // new Run's Project are copied.
        Statement copyLinks;
        const auto prepared = prepare(handle,
            "INSERT OR IGNORE INTO task_run_links(task_id,run_id) "
            "SELECT links.task_id,? FROM task_run_links AS links "
            "JOIN project_tasks AS tasks ON tasks.id=links.task_id "
            "WHERE links.run_id=? AND tasks.project_id=?;",
            copyLinks);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const common::Result<void> bindings[] = {
            bindText(handle, copyLinks.get(), 1, run.id),
            bindText(handle, copyLinks.get(), 2, *taskLinkSourceRunId),
            bindText(handle, copyLinks.get(), 3, run.projectId)};
        for (const auto& binding : bindings) {
            if (!binding.success()) return rollbackWith(*binding.error());
        }
        if (sqlite3_step(copyLinks.get()) != SQLITE_DONE) {
            return rollbackWith(databaseError(
                handle, "persistence.write_failed", "Could not copy task links for the conversion"));
        }
    }

    // The audit row commits (or rolls back) with everything above.
    Statement insertRecord;
    const auto preparedRecord = prepare(handle,
        "INSERT INTO run_legacy_conversions(run_id,source_run_id,source_workflow_id,policy_id,"
        "reason_code,source_snapshot_fingerprint,target_roster_fingerprint,"
        "confirmation_fingerprint,dropped_json,confirmation_mode,confirmed_at_ms,created_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?);",
        insertRecord);
    if (!preparedRecord.success()) return rollbackWith(*preparedRecord.error());
    const common::Result<void> recordBindings[] = {
        bindText(handle, insertRecord.get(), 1, record.runId),
        bindText(handle, insertRecord.get(), 2, record.sourceRunId),
        bindText(handle, insertRecord.get(), 3, record.sourceWorkflowId),
        bindText(handle, insertRecord.get(), 4, record.policyId),
        bindText(handle, insertRecord.get(), 5, record.reasonCode),
        bindText(handle, insertRecord.get(), 6, record.sourceSnapshotFingerprint),
        bindText(handle, insertRecord.get(), 7, record.targetRosterFingerprint),
        bindText(handle, insertRecord.get(), 8, record.confirmationFingerprint),
        bindText(handle, insertRecord.get(), 9, droppedJson.value()),
        bindText(handle, insertRecord.get(), 10,
            std::string(application::toString(record.confirmationMode))),
        bindOptionalTime(handle, insertRecord.get(), 11, record.confirmedAt),
        bindInt64(handle, insertRecord.get(), 12, toMilliseconds(record.createdAt))};
    for (const auto& binding : recordBindings) {
        if (!binding.success()) return rollbackWith(*binding.error());
    }
    if (sqlite3_step(insertRecord.get()) != SQLITE_DONE) {
        return rollbackWith(databaseError(
            handle, "persistence.write_failed", "Could not store the legacy conversion record"));
    }

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<Run>::ok(std::move(run));
}

common::Result<std::optional<application::LegacyConversionRecord>>
SqliteRunRepository::findLegacyConversion(const application::RunId& runId) const
{
    using Result = common::Result<std::optional<application::LegacyConversionRecord>>;
    // The Run is read first: findRun takes the database lock itself.
    const auto run = findRun(runId);
    if (!run.success()) return Result::failure(*run.error());

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT run_id,source_run_id,source_workflow_id,policy_id,reason_code,"
        "source_snapshot_fingerprint,target_roster_fingerprint,confirmation_fingerprint,"
        "dropped_json,confirmation_mode,confirmed_at_ms,created_at_ms "
        "FROM run_legacy_conversions WHERE run_id=?;",
        statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) return Result::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return Result::ok(std::nullopt);
    if (step != SQLITE_ROW) {
        return Result::failure(databaseError(
            handle, "persistence.read_failed", "Could not read the legacy conversion record"));
    }

    // From here on any defect is corruption: the record is never returned half trusted.
    const auto corrupt = [](std::string message) {
        return Result::failure(error(application::kLegacyConversionRecordCorruptCode, std::move(message)));
    };
    application::LegacyConversionRecord record;
    record.runId = columnText(statement.get(), 0);
    record.sourceRunId = columnText(statement.get(), 1);
    record.sourceWorkflowId = columnText(statement.get(), 2);
    record.policyId = columnText(statement.get(), 3);
    record.reasonCode = columnText(statement.get(), 4);
    record.sourceSnapshotFingerprint = columnText(statement.get(), 5);
    record.targetRosterFingerprint = columnText(statement.get(), 6);
    record.confirmationFingerprint = columnText(statement.get(), 7);
    const auto dropped = application::legacyConversionDroppedFromJson(columnText(statement.get(), 8));
    if (!dropped.success()) return Result::failure(*dropped.error());
    record.dropped = dropped.value();
    const auto mode = application::legacyConversionConfirmationModeFromString(
        columnText(statement.get(), 9));
    if (!mode.has_value()) return corrupt("The stored confirmation mode is not recognised");
    record.confirmationMode = *mode;
    record.confirmedAt = optionalTime(statement.get(), 10);
    record.createdAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 11));

    if (!run.value().has_value()) return corrupt("The stored conversion record has no Run");
    if (const auto problem = application::legacyConversionAggregateProblem(*run.value(), record)) {
        return corrupt(*problem);
    }
    return Result::ok(std::move(record));
}

common::Result<std::vector<application::LegacyConversionIndexEntry>>
SqliteRunRepository::listLegacyConversionIndex() const
{
    using Result = common::Result<std::vector<application::LegacyConversionIndexEntry>>;
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    // Only the key and the mode: the index is a marker, the full record is read (and validated)
    // by findLegacyConversion for the one Run the user opens.
    const auto prepared = prepare(handle,
        "SELECT run_id,confirmation_mode FROM run_legacy_conversions ORDER BY run_id;", statement);
    if (!prepared.success()) return Result::failure(*prepared.error());

    std::vector<application::LegacyConversionIndexEntry> index;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return Result::failure(databaseError(handle, "persistence.read_failed",
                "Could not read the legacy conversion index"));
        }
        index.push_back({columnText(statement.get(), 0),
            application::legacyConversionConfirmationModeFromString(columnText(statement.get(), 1))});
    }
    return Result::ok(std::move(index));
}

common::Result<Run> SqliteRunRepository::updateRun(Run run)
{
    const auto validProviderUsage = validateProviderBudgetUsageForPersistence(
        run.providerBudgetUsage);
    if (!validProviderUsage.success()) {
        return common::Result<Run>::failure(*validProviderUsage.error());
    }
    const auto validProgress = validateProgress(run.progress, "Run");
    if (!validProgress.success()) return common::Result<Run>::failure(*validProgress.error());
    const auto validCacheDuration = validateRunCacheDuration(run);
    if (!validCacheDuration.success()) {
        return common::Result<Run>::failure(*validCacheDuration.error());
    }
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "UPDATE runs SET project_id=?,parent_run_id=?,title=?,task=?,workflow_id=?,status=?,progress=?,"
        "current_stage_id=?,error_message=?,input_tokens=?,output_tokens=?,cache_creation_input_tokens=?,"
        "cache_read_input_tokens=?,cache_creation_duration_partition_known=?,"
        "cache_creation_five_minute_input_tokens=?,cache_creation_one_hour_input_tokens=?,"
        "created_at_ms=?,started_at_ms=?,finished_at_ms=?,max_parallelism=?,"
        "provider_budget_usage_json=?,revision=revision+1 "
        "WHERE id=? AND revision=?;",
        statement);
    if (!prepared.success()) return common::Result<Run>::failure(*prepared.error());

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, run.projectId),
        bindNullableText(handle, statement.get(), 2, run.parentRunId),
        bindText(handle, statement.get(), 3, run.title),
        bindText(handle, statement.get(), 4, run.task),
        bindText(handle, statement.get(), 5, run.workflowId),
        bindText(handle, statement.get(), 6, application::toString(run.status)),
        bindNullableText(handle, statement.get(), 8, run.currentStageId),
        bindText(handle, statement.get(), 9, run.errorMessage),
        bindUnsigned(handle, statement.get(), 10, run.usage.inputTokens),
        bindUnsigned(handle, statement.get(), 11, run.usage.outputTokens),
        bindUnsigned(handle, statement.get(), 12, run.usage.cacheCreationInputTokens),
        bindUnsigned(handle, statement.get(), 13, run.usage.cacheReadInputTokens),
        bindInt64(handle, statement.get(), 14,
            run.usage.cacheCreationDuration.known ? 1 : 0),
        bindUnsigned(handle, statement.get(), 15,
            run.usage.cacheCreationDuration.fiveMinuteTokens),
        bindUnsigned(handle, statement.get(), 16,
            run.usage.cacheCreationDuration.oneHourTokens),
        bindInt64(handle, statement.get(), 17, toMilliseconds(run.createdAt)),
        bindOptionalTime(handle, statement.get(), 18, run.startedAt),
        bindOptionalTime(handle, statement.get(), 19, run.finishedAt),
        bindUnsigned(handle, statement.get(), 20, run.maxParallelism),
        bindText(handle, statement.get(), 21, providerBudgetUsageJson(run.providerBudgetUsage)),
        bindText(handle, statement.get(), 22, run.id),
        bindUnsigned(handle, statement.get(), 23, run.revision)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<Run>::failure(*binding.error());
    }
    if (sqlite3_bind_double(statement.get(), 7, run.progress) != SQLITE_OK) {
        return common::Result<Run>::failure(
            databaseError(handle, "persistence.bind_failed", "Could not bind run progress"));
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<Run>::failure(
            databaseError(handle, "persistence.write_failed", "Could not update run"));
    }
    if (sqlite3_changes(handle) == 0) {
        // A zero-row UPDATE is ambiguous between "id does not exist" and "id
        // exists but the caller's revision is stale". Distinguish them with a
        // follow-up read so RunManager/OrchestrationService callers get the
        // correct typed error instead of a generic not-found.
        Statement exists;
        const auto existsPrepared = prepare(handle, "SELECT 1 FROM runs WHERE id=?;", exists);
        if (!existsPrepared.success()) return common::Result<Run>::failure(*existsPrepared.error());
        const auto existsBound = bindText(handle, exists.get(), 1, run.id);
        if (!existsBound.success()) return common::Result<Run>::failure(*existsBound.error());
        if (sqlite3_step(exists.get()) == SQLITE_ROW) {
            return common::Result<Run>::failure(
                error("run.revision_conflict", "Run was updated concurrently"));
        }
        return common::Result<Run>::failure(error("run.not_found", "Run not found"));
    }
    // Provider budget, verification-repair policy and interface contracts are immutable Run input:
    // the UPDATE above deliberately never touches these columns. Re-read the persisted values so a
    // stale/mutated caller copy cannot change them, matching InMemoryRunRepository::updateRun.
    Statement policyStatement;
    const auto policyPrepared = prepare(
        handle,
        "SELECT provider_budget_json,verification_repair_policy,run_interface_contracts_json "
        "FROM runs WHERE id=?;",
        policyStatement);
    if (!policyPrepared.success()) return common::Result<Run>::failure(*policyPrepared.error());
    const auto policyBound = bindText(handle, policyStatement.get(), 1, run.id);
    if (!policyBound.success()) return common::Result<Run>::failure(*policyBound.error());
    if (sqlite3_step(policyStatement.get()) != SQLITE_ROW) {
        return common::Result<Run>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read updated run's provider budget"));
    }
    const auto persistedPolicy = parseProviderBudgetPolicy(columnText(policyStatement.get(), 0));
    if (!persistedPolicy.success()) return common::Result<Run>::failure(*persistedPolicy.error());
    const auto persistedRepairPolicy = application::verificationRepairPolicyFromString(
        columnText(policyStatement.get(), 1));
    if (!persistedRepairPolicy.has_value()) {
        return common::Result<Run>::failure(error(
            "persistence.invalid_data",
            "Run row contains an unknown verification repair policy"));
    }
    const auto persistedContracts = application::parseRunInterfaceContracts(
        columnText(policyStatement.get(), 2));
    if (!persistedContracts.success())
        return common::Result<Run>::failure(*persistedContracts.error());
    run.providerBudget = persistedPolicy.value();
    run.verificationRepairPolicy = *persistedRepairPolicy;
    run.interfaceContracts = persistedContracts.value();
    ++run.revision;
    return common::Result<Run>::ok(std::move(run));
}

common::Result<std::optional<Run>> SqliteRunRepository::findRun(
    const application::RunId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + runColumns + " FROM runs WHERE id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<Run>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success()) return common::Result<std::optional<Run>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<Run>>::ok(std::nullopt);
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<Run>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read run"));
    }
    auto run = readRun(statement.get());
    if (!run.success()) return common::Result<std::optional<Run>>::failure(*run.error());
    const auto assignments = fetchAssignmentsForRun(handle, id);
    if (!assignments.success()) return common::Result<std::optional<Run>>::failure(*assignments.error());
    run.value().assignmentSnapshot = assignments.value();
    return common::Result<std::optional<Run>>::ok(run.value());
}

common::Result<std::vector<Run>> SqliteRunRepository::listRuns(
    const std::optional<application::ProjectId>& projectId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + runColumns + " FROM runs"
        + (projectId.has_value() ? " WHERE project_id=?" : "")
        + " ORDER BY created_at_ms,id;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<Run>>::failure(*prepared.error());
    if (projectId.has_value()) {
        const auto bound = bindText(handle, statement.get(), 1, *projectId);
        if (!bound.success()) return common::Result<std::vector<Run>>::failure(*bound.error());
    }

    std::vector<Run> runs;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<Run>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list runs"));
        }
        auto run = readRun(statement.get());
        if (!run.success()) return common::Result<std::vector<Run>>::failure(*run.error());
        runs.push_back(std::move(run.value()));
    }

    std::vector<std::string> runIds;
    runIds.reserve(runs.size());
    for (const auto& run : runs) runIds.push_back(run.id);
    const auto assignmentsByRun = fetchAssignmentsForRuns(handle, runIds);
    if (!assignmentsByRun.success()) return common::Result<std::vector<Run>>::failure(*assignmentsByRun.error());
    for (auto& run : runs) {
        const auto found = assignmentsByRun.value().find(run.id);
        if (found != assignmentsByRun.value().end()) run.assignmentSnapshot = found->second;
    }
    return common::Result<std::vector<Run>>::ok(std::move(runs));
}

common::Result<std::vector<Run>> SqliteRunRepository::listTerminalRunsPage(
    const application::ProjectId& projectId, const std::vector<std::string>& workflowIds,
    const std::optional<application::RunPageCursor>& after, std::size_t limit) const
{
    using Result = common::Result<std::vector<Run>>;
    if (limit == 0 || limit > application::kMaxRunPageSize) {
        return Result::failure(error("run.invalid_page", "The page size must be between 1 and 1000"));
    }

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    // (No workflow asked is an empty IN () list, which SQLite answers with no row.)
    // A keyset page on the existing (project_id, created_at_ms, id) index: strictly after the cursor in (created_at_ms DESC, id DESC).
    std::string sql = std::string("SELECT ") + runColumns
        + " FROM runs WHERE project_id=? AND status IN ('completed','failed','cancelled','interrupted') AND workflow_id IN (";
    for (std::size_t index = 0; index < workflowIds.size(); ++index) sql += index == 0 ? "?" : ",?";
    sql += ")";
    if (after.has_value()) sql += " AND (created_at_ms < ? OR (created_at_ms = ? AND id < ?))";
    sql += " ORDER BY created_at_ms DESC, id DESC LIMIT ?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    int index = 0;
    const auto bound = [&](const common::Result<void>& binding) { return binding.success(); };
    if (!bound(bindText(handle, statement.get(), ++index, projectId))) return Result::failure(error("persistence.bind_failed", "Could not bind the project"));
    for (const auto& workflowId : workflowIds) {
        if (!bound(bindText(handle, statement.get(), ++index, workflowId))) return Result::failure(error("persistence.bind_failed", "Could not bind a workflow"));
    }
    if (after.has_value()) {
        const std::int64_t cursorMs = toMilliseconds(after->createdAt);
        if (!bound(bindInt64(handle, statement.get(), ++index, cursorMs)) || !bound(bindInt64(handle, statement.get(), ++index, cursorMs))
            || !bound(bindText(handle, statement.get(), ++index, after->id))) {
            return Result::failure(error("persistence.bind_failed", "Could not bind the cursor"));
        }
    }
    if (!bound(bindInt64(handle, statement.get(), ++index, static_cast<std::int64_t>(limit)))) {
        return Result::failure(error("persistence.bind_failed", "Could not bind the limit"));
    }

    std::vector<Run> runs;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return Result::failure(databaseError(handle, "persistence.read_failed", "Could not page runs"));
        }
        auto run = readRun(statement.get());
        if (!run.success()) return Result::failure(*run.error());
        runs.push_back(std::move(run.value()));
    }

    std::vector<std::string> runIds;
    runIds.reserve(runs.size());
    for (const auto& run : runs) runIds.push_back(run.id);
    const auto assignmentsByRun = fetchAssignmentsForRuns(handle, runIds);
    if (!assignmentsByRun.success()) return Result::failure(*assignmentsByRun.error());
    for (auto& run : runs) {
        const auto found = assignmentsByRun.value().find(run.id);
        if (found != assignmentsByRun.value().end()) run.assignmentSnapshot = found->second;
    }
    return Result::ok(std::move(runs));
}

common::Result<std::vector<Run>> SqliteRunRepository::listUnfinishedRuns() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + runColumns
        + " FROM runs WHERE status NOT IN ('prepared','completed','failed','cancelled','interrupted') "
          "ORDER BY created_at_ms,id;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<Run>>::failure(*prepared.error());

    std::vector<Run> runs;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<Run>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list unfinished runs"));
        }
        auto run = readRun(statement.get());
        if (!run.success()) return common::Result<std::vector<Run>>::failure(*run.error());
        runs.push_back(std::move(run.value()));
    }

    std::vector<std::string> runIds;
    runIds.reserve(runs.size());
    for (const auto& run : runs) runIds.push_back(run.id);
    const auto assignmentsByRun = fetchAssignmentsForRuns(handle, runIds);
    if (!assignmentsByRun.success()) return common::Result<std::vector<Run>>::failure(*assignmentsByRun.error());
    for (auto& run : runs) {
        const auto found = assignmentsByRun.value().find(run.id);
        if (found != assignmentsByRun.value().end()) run.assignmentSnapshot = found->second;
    }
    return common::Result<std::vector<Run>>::ok(std::move(runs));
}

common::Result<void> SqliteRunRepository::replaceStages(
    const application::RunId& runId,
    std::vector<RunStage> stages)
{
    for (const auto& stage : stages) {
        if (stage.runId != runId) {
            return common::Result<void>::failure(
                error("stage.run_mismatch", "Every stage must belong to the replaced run"));
        }
        const auto validProgress = validateProgress(stage.progress, "Stage");
        if (!validProgress.success()) return validProgress;
    }

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    // Delete-then-insert must be one transaction: a reader (GUI thread listing
    // stages) must never observe a run with zero stages between the two
    // statements, and a crash mid-replace must not leave a partial stage list.
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return began;

    auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return common::Result<void>::failure(failure);
    };

    {
        Statement runExists;
        const auto existsPrepared = prepare(handle, "SELECT 1 FROM runs WHERE id=?;", runExists);
        if (!existsPrepared.success()) return rollbackWith(*existsPrepared.error());
        const auto existsBound = bindText(handle, runExists.get(), 1, runId);
        if (!existsBound.success()) return rollbackWith(*existsBound.error());
        if (sqlite3_step(runExists.get()) != SQLITE_ROW) {
            return rollbackWith(error("run.not_found", "Run not found"));
        }
    }

    {
        Statement remove;
        const auto removePrepared = prepare(handle, "DELETE FROM run_stages WHERE run_id=?;", remove);
        if (!removePrepared.success()) return rollbackWith(*removePrepared.error());
        const auto removeBound = bindText(handle, remove.get(), 1, runId);
        if (!removeBound.success()) return rollbackWith(*removeBound.error());
        if (sqlite3_step(remove.get()) != SQLITE_DONE) {
            return rollbackWith(
                databaseError(handle, "persistence.write_failed", "Could not replace run stages"));
        }
    }

    for (std::size_t index = 0; index < stages.size(); ++index) {
        const auto inserted = insertStageRow(handle, stages[index], index);
        if (!inserted.success()) return rollbackWith(*inserted.error());
    }

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<void>::ok();
}

common::Result<RunStage> SqliteRunRepository::updateStage(RunStage stage)
{
    const auto validProgress = validateProgress(stage.progress, "Stage");
    if (!validProgress.success()) return common::Result<RunStage>::failure(*validProgress.error());
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "UPDATE run_stages SET type=?,title=?,description=?,status=?,progress=?,provider_id=?,model_id=?,"
        "input_tokens=?,output_tokens=?,error_message=?,started_at_ms=?,finished_at_ms=? "
        "WHERE id=? AND run_id=?;",
        statement);
    if (!prepared.success()) return common::Result<RunStage>::failure(*prepared.error());

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, stage.type),
        bindText(handle, statement.get(), 2, stage.title),
        bindText(handle, statement.get(), 3, stage.description),
        bindText(handle, statement.get(), 4, application::toString(stage.status)),
        bindText(handle, statement.get(), 6, stage.providerId),
        bindText(handle, statement.get(), 7, stage.modelId),
        bindUnsigned(handle, statement.get(), 8, stage.usage.inputTokens),
        bindUnsigned(handle, statement.get(), 9, stage.usage.outputTokens),
        bindText(handle, statement.get(), 10, stage.errorMessage),
        bindOptionalTime(handle, statement.get(), 11, stage.startedAt),
        bindOptionalTime(handle, statement.get(), 12, stage.finishedAt),
        bindText(handle, statement.get(), 13, stage.id),
        bindText(handle, statement.get(), 14, stage.runId)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<RunStage>::failure(*binding.error());
    }
    if (sqlite3_bind_double(statement.get(), 5, stage.progress) != SQLITE_OK) {
        return common::Result<RunStage>::failure(
            databaseError(handle, "persistence.bind_failed", "Could not bind stage progress"));
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<RunStage>::failure(
            databaseError(handle, "persistence.write_failed", "Could not update stage"));
    }
    if (sqlite3_changes(handle) == 0) {
        // A plain WHERE match collapses "run has no stages yet" and "stage id
        // unknown for this run" into one case; both already report the same
        // code here, so this is not an observable behavior change.
        return common::Result<RunStage>::failure(error("stage.not_found", "Stage not found"));
    }
    return common::Result<RunStage>::ok(std::move(stage));
}

common::Result<std::vector<RunStage>> SqliteRunRepository::listStages(
    const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + stageColumns
        + " FROM run_stages WHERE run_id=? ORDER BY position;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<RunStage>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) return common::Result<std::vector<RunStage>>::failure(*bound.error());

    std::vector<RunStage> stages;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<RunStage>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list run stages"));
        }
        auto stage = readStage(statement.get());
        if (!stage.success()) return common::Result<std::vector<RunStage>>::failure(*stage.error());
        stages.push_back(stage.value());
    }
    return common::Result<std::vector<RunStage>>::ok(std::move(stages));
}

SqliteExecutionManifestRepository::SqliteExecutionManifestRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<application::ExecutionManifest>
SqliteExecutionManifestRepository::insert(application::ExecutionManifest manifest)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO execution_manifests "
        "(run_id,schema_version,manifest_json,created_at_ms) VALUES (?,?,?,?);",
        statement);
    if (!prepared.success())
        return common::Result<application::ExecutionManifest>::failure(*prepared.error());

    const std::string document = executionManifestToJson(manifest).dump();
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, manifest.runId),
        bindText(handle, statement.get(), 2, manifest.schemaVersion),
        bindText(handle, statement.get(), 3, document),
        bindInt64(handle, statement.get(), 4, toMilliseconds(manifest.createdAt))
    };
    for (const auto& binding : bindings) {
        if (!binding.success())
            return common::Result<application::ExecutionManifest>::failure(*binding.error());
    }

    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<application::ExecutionManifest>::failure(error(
                "manifest.already_frozen",
                "Execution manifest is already frozen for this Run"));
        }
        return common::Result<application::ExecutionManifest>::failure(databaseError(
            handle, "manifest.insert_failed", "Could not freeze execution manifest"));
    }
    return common::Result<application::ExecutionManifest>::ok(std::move(manifest));
}

common::Result<std::optional<application::ExecutionManifest>>
SqliteExecutionManifestRepository::findForRun(
    const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT schema_version,manifest_json,created_at_ms "
        "FROM execution_manifests WHERE run_id=?;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::optional<application::ExecutionManifest>>::failure(
            *prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::optional<application::ExecutionManifest>>::failure(
            *bound.error());
    }

    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return common::Result<std::optional<application::ExecutionManifest>>::ok(
            std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<application::ExecutionManifest>>::failure(
            databaseError(handle, "manifest.read_failed",
                "Could not read frozen execution manifest"));
    }
    auto parsed = executionManifestFromJson(
        columnText(statement.get(), 1),
        columnText(statement.get(), 0),
        fromMilliseconds(sqlite3_column_int64(statement.get(), 2)));
    if (!parsed.success()) {
        return common::Result<std::optional<application::ExecutionManifest>>::failure(
            *parsed.error());
    }
    if (parsed.value().runId != runId) {
        return common::Result<std::optional<application::ExecutionManifest>>::failure(error(
            "manifest.corrupt_snapshot", "Stored execution manifest Run id does not match its key"));
    }
    return common::Result<std::optional<application::ExecutionManifest>>::ok(
        std::move(parsed.value()));
}

namespace {

nlohmann::json workspaceScopeEntryToJson(const application::RunWorkspaceScopeEntry& entry)
{
    nlohmann::json j {
        {"relativePath", entry.relativePath},
        {"role", application::toString(entry.role)},
        {"reason", entry.reason},
        {"taskId", entry.taskId},
        {"origin", application::toString(entry.origin)}
    };
    j["baselineHash"] = entry.baselineHash.has_value()
        ? nlohmann::json(*entry.baselineHash) : nlohmann::json(nullptr);
    return j;
}

// AG-15: fails closed on any malformed entry - never silently drops or
// defaults a workspace-scope entry, matching AG-14.2's own array-parsing
// discipline.
common::Result<application::RunWorkspaceScopeEntry> workspaceScopeEntryFromJson(
    const nlohmann::json& j)
{
    const auto corrupt = [] {
        return common::Result<application::RunWorkspaceScopeEntry>::failure(
            error("workspace_scope.corrupt_entry", "Corrupt workspace scope entry"));
    };
    if (!j.is_object() || !j.contains("relativePath") || !j["relativePath"].is_string()
        || !j.contains("role") || !j["role"].is_string()
        || !j.contains("reason") || !j["reason"].is_string()
        || !j.contains("taskId") || !j["taskId"].is_string()
        || !j.contains("origin") || !j["origin"].is_string()
        || !j.contains("baselineHash")
        || !(j["baselineHash"].is_null() || j["baselineHash"].is_string())) {
        return corrupt();
    }
    application::RunWorkspaceScopeEntry entry;
    entry.relativePath = j["relativePath"].get<std::string>();
    const auto role = application::workspaceScopeRoleFromString(j["role"].get<std::string>());
    if (!role.has_value()) return corrupt();
    entry.role = *role;
    entry.reason = j["reason"].get<std::string>();
    entry.taskId = j["taskId"].get<std::string>();
    if (j["baselineHash"].is_string()) entry.baselineHash = j["baselineHash"].get<std::string>();
    const auto origin = application::workspaceScopeOriginFromString(j["origin"].get<std::string>());
    if (!origin.has_value()) return corrupt();
    entry.origin = *origin;
    return common::Result<application::RunWorkspaceScopeEntry>::ok(std::move(entry));
}

std::string workspaceScopeEntriesToJson(const std::vector<application::RunWorkspaceScopeEntry>& entries)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& entry : entries) array.push_back(workspaceScopeEntryToJson(entry));
    return array.dump();
}

common::Result<std::vector<application::RunWorkspaceScopeEntry>> workspaceScopeEntriesFromJson(
    const std::string& json)
{
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        return common::Result<std::vector<application::RunWorkspaceScopeEntry>>::failure(
            error("workspace_scope.corrupt_entry", "Corrupt workspace scope entries column"));
    }
    std::vector<application::RunWorkspaceScopeEntry> entries;
    entries.reserve(parsed.size());
    for (const auto& item : parsed) {
        auto entry = workspaceScopeEntryFromJson(item);
        if (!entry.success()) return common::Result<std::vector<application::RunWorkspaceScopeEntry>>::failure(
            *entry.error());
        entries.push_back(std::move(entry.value()));
    }
    return common::Result<std::vector<application::RunWorkspaceScopeEntry>>::ok(std::move(entries));
}

nlohmann::json workspaceScopeEdgeToJson(const application::WorkspaceScopeDependencyEdge& edge)
{
    return nlohmann::json {
        {"dependentTaskId", edge.dependentTaskId},
        {"providerTaskId", edge.providerTaskId},
        {"relativePath", edge.relativePath}
    };
}

common::Result<application::WorkspaceScopeDependencyEdge> workspaceScopeEdgeFromJson(
    const nlohmann::json& j)
{
    if (!j.is_object() || !j.contains("dependentTaskId") || !j["dependentTaskId"].is_string()
        || !j.contains("providerTaskId") || !j["providerTaskId"].is_string()
        || !j.contains("relativePath") || !j["relativePath"].is_string()) {
        return common::Result<application::WorkspaceScopeDependencyEdge>::failure(
            error("workspace_scope.corrupt_entry", "Corrupt workspace scope dependency edge"));
    }
    return common::Result<application::WorkspaceScopeDependencyEdge>::ok({
        j["dependentTaskId"].get<std::string>(),
        j["providerTaskId"].get<std::string>(),
        j["relativePath"].get<std::string>()});
}

std::string workspaceScopeEdgesToJson(
    const std::vector<application::WorkspaceScopeDependencyEdge>& edges)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& edge : edges) array.push_back(workspaceScopeEdgeToJson(edge));
    return array.dump();
}

common::Result<std::vector<application::WorkspaceScopeDependencyEdge>> workspaceScopeEdgesFromJson(
    const std::string& json)
{
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        return common::Result<std::vector<application::WorkspaceScopeDependencyEdge>>::failure(
            error("workspace_scope.corrupt_entry", "Corrupt workspace scope dependency edges column"));
    }
    std::vector<application::WorkspaceScopeDependencyEdge> edges;
    edges.reserve(parsed.size());
    for (const auto& item : parsed) {
        auto edge = workspaceScopeEdgeFromJson(item);
        if (!edge.success()) return common::Result<std::vector<application::WorkspaceScopeDependencyEdge>>::failure(
            *edge.error());
        edges.push_back(std::move(edge.value()));
    }
    return common::Result<std::vector<application::WorkspaceScopeDependencyEdge>>::ok(std::move(edges));
}

} // namespace

SqliteRunWorkspaceScopeRepository::SqliteRunWorkspaceScopeRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

SqliteManifestVerificationContinuationRepository::SqliteManifestVerificationContinuationRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteManifestVerificationContinuationRepository::insert(
    application::ManifestVerificationContinuationCheckpoint checkpoint)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO manifest_verification_continuations "
        "(source_run_id,id,schema_version,source_project_id,status,denial_code,"
        "denial_message,created_at_ms) VALUES (?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, checkpoint.sourceRunId),
        bindText(handle, statement.get(), 2, checkpoint.id),
        bindText(handle, statement.get(), 3, checkpoint.schemaVersion),
        bindText(handle, statement.get(), 4, checkpoint.sourceProjectId),
        bindText(handle, statement.get(), 5,
            application::toString(checkpoint.status)),
        bindText(handle, statement.get(), 6, checkpoint.denialCode),
        bindText(handle, statement.get(), 7, checkpoint.denialMessage),
        bindInt64(handle, statement.get(), 8, toMilliseconds(checkpoint.createdAt))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<void>::failure(error(
                "manifest_verification_continuation.already_exists",
                "A verification-continuation checkpoint already exists for this Run"));
        }
        return common::Result<void>::failure(databaseError(
            handle, "manifest_verification_continuation.write_failed",
            "Could not persist the verification-continuation checkpoint"));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<application::ManifestVerificationContinuationCheckpoint>>
SqliteManifestVerificationContinuationRepository::findBySourceRun(
    const application::RunId& sourceRunId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT id,schema_version,source_project_id,status,denial_code,denial_message,"
        "created_at_ms FROM manifest_verification_continuations WHERE source_run_id=?;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::optional<application::ManifestVerificationContinuationCheckpoint>>::
            failure(*prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, sourceRunId);
    if (!bound.success()) {
        return common::Result<std::optional<application::ManifestVerificationContinuationCheckpoint>>::
            failure(*bound.error());
    }
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return common::Result<std::optional<application::ManifestVerificationContinuationCheckpoint>>::
            ok(std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<application::ManifestVerificationContinuationCheckpoint>>::
            failure(databaseError(handle, "manifest_verification_continuation.read_failed",
                "Could not read the verification-continuation checkpoint"));
    }
    application::ManifestVerificationContinuationCheckpoint checkpoint;
    checkpoint.sourceRunId = sourceRunId;
    checkpoint.id = columnText(statement.get(), 0);
    checkpoint.schemaVersion = columnText(statement.get(), 1);
    checkpoint.sourceProjectId = columnText(statement.get(), 2);
    const auto status =
        application::manifestVerificationContinuationStatusFromString(columnText(statement.get(), 3));
    if (!status.has_value()) {
        return common::Result<std::optional<application::ManifestVerificationContinuationCheckpoint>>::
            failure(error("manifest_verification_continuation.corrupt_status",
                "The stored verification-continuation status is invalid"));
    }
    checkpoint.status = *status;
    checkpoint.denialCode = columnText(statement.get(), 4);
    checkpoint.denialMessage = columnText(statement.get(), 5);
    checkpoint.createdAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 6));
    return common::Result<std::optional<application::ManifestVerificationContinuationCheckpoint>>::
        ok(checkpoint);
}

namespace {

std::string serializeVerificationAttributions(
    const std::vector<application::ManifestVerificationFindingAttribution>& findings)
{
    nlohmann::json values = nlohmann::json::array();
    for (const auto& finding : findings) {
        nlohmann::json value {
            {"diagnosticCode", finding.diagnosticCode},
            {"requirementId", finding.requirementId},
            {"relativePath", finding.relativePath},
            {"line", finding.line},
            {"column", finding.column},
            {"tag", finding.tag},
            {"kind", application::toString(finding.kind)}};
        value["ownerTaskId"] = finding.ownerTaskId.has_value()
            ? nlohmann::json(*finding.ownerTaskId) : nlohmann::json(nullptr);
        value["ownerProposalId"] = finding.ownerProposalId.has_value()
            ? nlohmann::json(*finding.ownerProposalId) : nlohmann::json(nullptr);
        values.push_back(std::move(value));
    }
    return values.dump();
}

common::Result<std::vector<application::ManifestVerificationFindingAttribution>>
deserializeVerificationAttributions(const std::string& json)
{
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array() || parsed.size() > 256) {
        return common::Result<std::vector<application::ManifestVerificationFindingAttribution>>::failure(
            error("manifest_verification_attribution.corrupt_findings",
                "Stored verification attribution findings are invalid"));
    }
    std::vector<application::ManifestVerificationFindingAttribution> result;
    for (const auto& value : parsed) {
        if (!value.is_object() || !value.contains("diagnosticCode")
            || !value["diagnosticCode"].is_string() || !value.contains("requirementId")
            || !value["requirementId"].is_string() || !value.contains("relativePath")
            || !value["relativePath"].is_string() || !value.contains("line")
            || !value["line"].is_number_integer() || !value.contains("column")
            || !value["column"].is_number_integer() || !value.contains("tag")
            || !value["tag"].is_string() || !value.contains("kind")
            || !value["kind"].is_string()) {
            return common::Result<std::vector<application::ManifestVerificationFindingAttribution>>::failure(
                error("manifest_verification_attribution.corrupt_findings",
                    "Stored verification attribution findings are invalid"));
        }
        const auto kind = application::manifestVerificationFindingAttributionKindFromString(
            value["kind"].get<std::string>());
        if (!kind.has_value()) {
            return common::Result<std::vector<application::ManifestVerificationFindingAttribution>>::failure(
                error("manifest_verification_attribution.corrupt_findings",
                    "Stored verification attribution kind is invalid"));
        }
        application::ManifestVerificationFindingAttribution finding;
        finding.diagnosticCode = value["diagnosticCode"].get<std::string>();
        finding.requirementId = value["requirementId"].get<std::string>();
        finding.relativePath = value["relativePath"].get<std::string>();
        finding.line = value["line"].get<int>();
        finding.column = value["column"].get<int>();
        finding.tag = value["tag"].get<std::string>();
        finding.kind = *kind;
        if (value.contains("ownerTaskId") && value["ownerTaskId"].is_string())
            finding.ownerTaskId = value["ownerTaskId"].get<std::string>();
        if (value.contains("ownerProposalId") && value["ownerProposalId"].is_string())
            finding.ownerProposalId = value["ownerProposalId"].get<std::string>();
        result.push_back(std::move(finding));
    }
    return common::Result<std::vector<application::ManifestVerificationFindingAttribution>>::ok(
        std::move(result));
}

} // namespace

common::Result<void> SqliteManifestVerificationContinuationRepository::insertAttribution(
    application::ManifestVerificationAttributionCheckpoint checkpoint)
{
    const auto valid = application::validateManifestVerificationAttributionCheckpoint(checkpoint);
    if (!valid.success()) return valid;
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO manifest_verification_attributions "
        "(source_run_id,id,schema_version,source_project_id,status,block_code,block_message,"
        "baseline_fingerprint,source_overlay_fingerprint,findings_json,created_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?);", statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, checkpoint.sourceRunId),
        bindText(handle, statement.get(), 2, checkpoint.id),
        bindText(handle, statement.get(), 3, checkpoint.schemaVersion),
        bindText(handle, statement.get(), 4, checkpoint.sourceProjectId),
        bindText(handle, statement.get(), 5, application::toString(checkpoint.status)),
        bindText(handle, statement.get(), 6, checkpoint.blockCode),
        bindText(handle, statement.get(), 7, checkpoint.blockMessage),
        bindText(handle, statement.get(), 8, checkpoint.baselineFingerprint),
        bindText(handle, statement.get(), 9, checkpoint.sourceOverlayFingerprint),
        bindText(handle, statement.get(), 10, serializeVerificationAttributions(checkpoint.findings)),
        bindInt64(handle, statement.get(), 11, toMilliseconds(checkpoint.createdAt))};
    for (const auto& binding : bindings)
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<void>::failure({
                "manifest_verification_attribution.already_exists",
                "A verification-attribution checkpoint already exists for this Run"});
        }
        return common::Result<void>::failure(databaseError(handle,
            "manifest_verification_attribution.write_failed",
            "Could not persist the verification-attribution checkpoint"));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<application::ManifestVerificationAttributionCheckpoint>>
SqliteManifestVerificationContinuationRepository::findAttributionBySourceRun(
    const application::RunId& sourceRunId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT id,schema_version,source_project_id,status,block_code,block_message,"
        "baseline_fingerprint,source_overlay_fingerprint,findings_json,created_at_ms "
        "FROM manifest_verification_attributions WHERE source_run_id=?;", statement);
    if (!prepared.success()) return common::Result<std::optional<application::ManifestVerificationAttributionCheckpoint>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, sourceRunId);
    if (!bound.success()) return common::Result<std::optional<application::ManifestVerificationAttributionCheckpoint>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<application::ManifestVerificationAttributionCheckpoint>>::ok(std::nullopt);
    if (step != SQLITE_ROW) return common::Result<std::optional<application::ManifestVerificationAttributionCheckpoint>>::failure(
        databaseError(handle, "manifest_verification_attribution.read_failed",
            "Could not read the verification-attribution checkpoint"));
    application::ManifestVerificationAttributionCheckpoint checkpoint;
    checkpoint.sourceRunId = sourceRunId;
    checkpoint.id = columnText(statement.get(), 0);
    checkpoint.schemaVersion = columnText(statement.get(), 1);
    checkpoint.sourceProjectId = columnText(statement.get(), 2);
    const auto status = application::manifestVerificationAttributionStatusFromString(columnText(statement.get(), 3));
    if (!status.has_value() || checkpoint.schemaVersion != application::kManifestVerificationAttributionSchemaVersion) {
        return common::Result<std::optional<application::ManifestVerificationAttributionCheckpoint>>::failure({
            "manifest_verification_attribution.corrupt_identity",
            "Stored verification-attribution identity is invalid"});
    }
    checkpoint.status = *status;
    checkpoint.blockCode = columnText(statement.get(), 4);
    checkpoint.blockMessage = columnText(statement.get(), 5);
    checkpoint.baselineFingerprint = columnText(statement.get(), 6);
    checkpoint.sourceOverlayFingerprint = columnText(statement.get(), 7);
    const auto findings = deserializeVerificationAttributions(columnText(statement.get(), 8));
    if (!findings.success()) return common::Result<std::optional<application::ManifestVerificationAttributionCheckpoint>>::failure(*findings.error());
    checkpoint.findings = std::move(findings.value());
    checkpoint.createdAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 9));
    const auto valid = application::validateManifestVerificationAttributionCheckpoint(checkpoint);
    if (!valid.success()) {
        return common::Result<std::optional<application::ManifestVerificationAttributionCheckpoint>>::failure(
            {"manifest_verification_attribution.corrupt_identity",
                "Stored verification-attribution data violates its typed contract"});
    }
    return common::Result<std::optional<application::ManifestVerificationAttributionCheckpoint>>::ok(std::move(checkpoint));
}

common::Result<void> SqliteManifestVerificationContinuationRepository::insertInput(
    application::ManifestVerificationContinuationInput input)
{
    const auto valid = application::validateManifestVerificationContinuationInput(input);
    if (!valid.success()) return valid;
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO manifest_verification_continuation_inputs "
        "(source_run_id,child_run_id,schema_version,source_project_id,attribution_id,"
        "baseline_fingerprint,source_workspace_identity,source_overlay_fingerprint,"
        "proposal_ids_json,proposal_hashes_json,input_fingerprint,planner_requests,created_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?);", statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const std::string ids = nlohmann::json(input.orderedProposalIds).dump();
    const std::string hashes = nlohmann::json(input.orderedProposalContentHashes).dump();
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, input.sourceRunId),
        bindText(handle, statement.get(), 2, input.childRunId),
        bindText(handle, statement.get(), 3, input.schemaVersion),
        bindText(handle, statement.get(), 4, input.sourceProjectId),
        bindText(handle, statement.get(), 5, input.attributionId),
        bindText(handle, statement.get(), 6, input.baselineFingerprint),
        bindText(handle, statement.get(), 7, input.sourceWorkspaceIdentity),
        bindText(handle, statement.get(), 8, input.sourceOverlayFingerprint),
        bindText(handle, statement.get(), 9, ids),
        bindText(handle, statement.get(), 10, hashes),
        bindText(handle, statement.get(), 11, input.inputFingerprint),
        bindInt64(handle, statement.get(), 12, static_cast<std::int64_t>(input.plannerRequests)),
        bindInt64(handle, statement.get(), 13, toMilliseconds(input.createdAt))};
    for (const auto& binding : bindings)
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        const int extended = sqlite3_extended_errcode(handle);
        if (extended == SQLITE_CONSTRAINT_PRIMARYKEY || extended == SQLITE_CONSTRAINT_UNIQUE) {
            // Both immutable identities are unique. SQLite is free to report the
            // child_run_id index first when the exact same row is inserted again,
            // so the extended error code alone cannot distinguish idempotent
            // source duplication from a child-id collision. Resolve that ordering
            // ambiguity against the authoritative table while holding the same
            // repository lock.
            Statement existingSource;
            const auto sourcePrepared = prepare(handle,
                "SELECT 1 FROM manifest_verification_continuation_inputs "
                "WHERE source_run_id=? LIMIT 1;", existingSource);
            if (!sourcePrepared.success())
                return common::Result<void>::failure(*sourcePrepared.error());
            const auto sourceBound = bindText(handle, existingSource.get(), 1, input.sourceRunId);
            if (!sourceBound.success())
                return common::Result<void>::failure(*sourceBound.error());
            const bool sourceAlreadyExists = sqlite3_step(existingSource.get()) == SQLITE_ROW;
            return common::Result<void>::failure({
                sourceAlreadyExists
                    ? "manifest_verification_continuation.input_already_exists"
                    : "manifest_verification_continuation.child_already_reserved",
                "A continuation input or child reservation already exists"});
        }
        return common::Result<void>::failure(databaseError(handle,
            "manifest_verification_continuation.input_write_failed",
            "Could not persist the continuation input"));
    }
    return common::Result<void>::ok();
}

namespace {

common::Result<application::ManifestVerificationContinuationInput> readContinuationInput(
    sqlite3_stmt* statement)
{
    application::ManifestVerificationContinuationInput input;
    input.sourceRunId = columnText(statement, 0);
    input.childRunId = columnText(statement, 1);
    input.schemaVersion = columnText(statement, 2);
    input.sourceProjectId = columnText(statement, 3);
    input.attributionId = columnText(statement, 4);
    input.baselineFingerprint = columnText(statement, 5);
    input.sourceWorkspaceIdentity = columnText(statement, 6);
    input.sourceOverlayFingerprint = columnText(statement, 7);
    const auto ids = nlohmann::json::parse(columnText(statement, 8), nullptr, false);
    const auto hashes = nlohmann::json::parse(columnText(statement, 9), nullptr, false);
    if (ids.is_discarded() || hashes.is_discarded() || !ids.is_array() || !hashes.is_array()) {
        return common::Result<application::ManifestVerificationContinuationInput>::failure({
            "manifest_verification_continuation.corrupt_input",
            "Stored continuation proposal identity is invalid"});
    }
    try {
        input.orderedProposalIds = ids.get<std::vector<std::string>>();
        input.orderedProposalContentHashes = hashes.get<std::vector<std::string>>();
    } catch (...) {
        return common::Result<application::ManifestVerificationContinuationInput>::failure({
            "manifest_verification_continuation.corrupt_input",
            "Stored continuation proposal identity is invalid"});
    }
    input.inputFingerprint = columnText(statement, 10);
    const auto plannerRequests = sqlite3_column_int64(statement, 11);
    if (plannerRequests < 0) return common::Result<application::ManifestVerificationContinuationInput>::failure({
        "manifest_verification_continuation.corrupt_input",
        "Stored continuation request count is invalid"});
    input.plannerRequests = static_cast<std::uint64_t>(plannerRequests);
    input.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 12));
    const auto valid = application::validateManifestVerificationContinuationInput(input);
    if (!valid.success()) return common::Result<application::ManifestVerificationContinuationInput>::failure({
        "manifest_verification_continuation.corrupt_input",
        "Stored continuation input violates its typed contract"});
    return common::Result<application::ManifestVerificationContinuationInput>::ok(std::move(input));
}

constexpr const char* continuationInputColumns =
    "source_run_id,child_run_id,schema_version,source_project_id,attribution_id,"
    "baseline_fingerprint,source_workspace_identity,source_overlay_fingerprint,"
    "proposal_ids_json,proposal_hashes_json,input_fingerprint,planner_requests,created_at_ms";

} // namespace

common::Result<std::optional<application::ManifestVerificationContinuationInput>>
SqliteManifestVerificationContinuationRepository::findInputBySourceRun(
    const application::RunId& sourceRunId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + continuationInputColumns
        + " FROM manifest_verification_continuation_inputs WHERE source_run_id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<application::ManifestVerificationContinuationInput>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, sourceRunId);
    if (!bound.success()) return common::Result<std::optional<application::ManifestVerificationContinuationInput>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<application::ManifestVerificationContinuationInput>>::ok(std::nullopt);
    if (step != SQLITE_ROW) return common::Result<std::optional<application::ManifestVerificationContinuationInput>>::failure(
        databaseError(handle, "manifest_verification_continuation.input_read_failed", "Could not read continuation input"));
    auto input = readContinuationInput(statement.get());
    if (!input.success()) return common::Result<std::optional<application::ManifestVerificationContinuationInput>>::failure(*input.error());
    return common::Result<std::optional<application::ManifestVerificationContinuationInput>>::ok(std::move(input.value()));
}

common::Result<std::optional<application::ManifestVerificationContinuationInput>>
SqliteManifestVerificationContinuationRepository::findInputByChildRun(
    const application::RunId& childRunId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + continuationInputColumns
        + " FROM manifest_verification_continuation_inputs WHERE child_run_id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<application::ManifestVerificationContinuationInput>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, childRunId);
    if (!bound.success()) return common::Result<std::optional<application::ManifestVerificationContinuationInput>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<application::ManifestVerificationContinuationInput>>::ok(std::nullopt);
    if (step != SQLITE_ROW) return common::Result<std::optional<application::ManifestVerificationContinuationInput>>::failure(
        databaseError(handle, "manifest_verification_continuation.input_read_failed", "Could not read continuation input"));
    auto input = readContinuationInput(statement.get());
    if (!input.success()) return common::Result<std::optional<application::ManifestVerificationContinuationInput>>::failure(*input.error());
    return common::Result<std::optional<application::ManifestVerificationContinuationInput>>::ok(std::move(input.value()));
}

common::Result<std::vector<application::ManifestVerificationContinuationInput>>
SqliteManifestVerificationContinuationRepository::listInputs() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + continuationInputColumns
        + " FROM manifest_verification_continuation_inputs ORDER BY created_at_ms,source_run_id;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<application::ManifestVerificationContinuationInput>>::failure(*prepared.error());
    std::vector<application::ManifestVerificationContinuationInput> result;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) return common::Result<std::vector<application::ManifestVerificationContinuationInput>>::failure(
            databaseError(handle, "manifest_verification_continuation.input_read_failed", "Could not list continuation inputs"));
        auto input = readContinuationInput(statement.get());
        if (!input.success()) return common::Result<std::vector<application::ManifestVerificationContinuationInput>>::failure(*input.error());
        result.push_back(std::move(input.value()));
    }
    return common::Result<std::vector<application::ManifestVerificationContinuationInput>>::ok(std::move(result));
}

SqliteManifestVerificationManualHandoffRepository::
SqliteManifestVerificationManualHandoffRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteManifestVerificationManualHandoffRepository::insertHandoff(
    application::ManifestVerificationManualHandoff handoff)
{
    const auto valid = application::validateManifestVerificationManualHandoff(handoff);
    if (!valid.success()) return valid;
    const auto snapshotJson = application::WorkspaceBaselineProvenance::renderJson(
        handoff.originalSnapshot);
    if (!snapshotJson.success()) return common::Result<void>::failure(*snapshotJson.error());
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return common::Result<void>::failure(*began.error());
    auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return common::Result<void>::failure(failure);
    };

    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO manifest_verification_manual_handoffs "
        "(id,origin_manifest_run_id,verification_anchor_run_id,schema_version,source_project_id,"
        "storage_key,workspace_identity,baseline_fingerprint,source_overlay_fingerprint,"
        "frozen_members_json,allowed_paths_json,original_snapshot_json,created_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?);", statement);
    if (!prepared.success()) return rollbackWith(*prepared.error());
    nlohmann::json frozenMembersJson = nlohmann::json::array();
    for (const auto& member : handoff.frozenMembers) {
        frozenMembersJson.push_back({
            {"proposalId", member.proposalId},
            {"owningRunId", member.owningRunId},
            {"contentHash", member.contentHash},
            {"lineageRootProposalId", member.lineageRootProposalId}});
    }
    const std::string frozenMembers = frozenMembersJson.dump();
    nlohmann::json allowedPathsJson = nlohmann::json::array();
    for (const auto& allowed : handoff.allowedPaths) {
        allowedPathsJson.push_back({
            {"relativePath", allowed.relativePath},
            {"ownerProposalId", allowed.ownerProposalId},
            {"ownerTaskId", allowed.ownerTaskId}});
    }
    const std::string allowedPaths = allowedPathsJson.dump();
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, handoff.id),
        bindText(handle, statement.get(), 2, handoff.originManifestRunId),
        bindText(handle, statement.get(), 3, handoff.verificationAnchorRunId),
        bindText(handle, statement.get(), 4, handoff.schemaVersion),
        bindText(handle, statement.get(), 5, handoff.sourceProjectId),
        bindText(handle, statement.get(), 6, handoff.storageKey),
        bindText(handle, statement.get(), 7, handoff.workspaceIdentity),
        bindText(handle, statement.get(), 8, handoff.baselineFingerprint),
        bindText(handle, statement.get(), 9, handoff.sourceOverlayFingerprint),
        bindText(handle, statement.get(), 10, frozenMembers),
        bindText(handle, statement.get(), 11, allowedPaths),
        bindText(handle, statement.get(), 12, snapshotJson.value()),
        bindInt64(handle, statement.get(), 13, toMilliseconds(handoff.createdAt))};
    for (const auto& binding : bindings)
        if (!binding.success()) return rollbackWith(*binding.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        const int extended = sqlite3_extended_errcode(handle);
        if (extended == SQLITE_CONSTRAINT_PRIMARYKEY || extended == SQLITE_CONSTRAINT_UNIQUE) {
            return rollbackWith({
                "manifest_verification_manual_handoff.already_exists",
                "A manual handoff already exists for this verification anchor Run"});
        }
        return rollbackWith(databaseError(handle,
            "manifest_verification_manual_handoff.write_failed",
            "Could not persist the manual handoff"));
    }

    // Normalized, indexed member rows - what AG-16's reverse lookup actually
    // queries (see findHandoffByMemberProposalId). proposal_id's own UNIQUE
    // constraint is the durable, cross-handoff enforcement of "a proposal is
    // frozen into at most one handoff" - a SQLITE_CONSTRAINT_UNIQUE here
    // means some OTHER handoff already claimed one of these proposal ids.
    for (std::size_t ordinal = 0; ordinal < handoff.frozenMembers.size(); ++ordinal) {
        const auto& member = handoff.frozenMembers[ordinal];
        Statement memberStatement;
        const auto memberPrepared = prepare(handle,
            "INSERT INTO manifest_verification_manual_handoff_members "
            "(handoff_id,ordinal,proposal_id,owning_run_id,content_hash,lineage_root_proposal_id) "
            "VALUES (?,?,?,?,?,?);", memberStatement);
        if (!memberPrepared.success()) return rollbackWith(*memberPrepared.error());
        const common::Result<void> memberBindings[] = {
            bindText(handle, memberStatement.get(), 1, handoff.id),
            bindInt64(handle, memberStatement.get(), 2, static_cast<std::int64_t>(ordinal)),
            bindText(handle, memberStatement.get(), 3, member.proposalId),
            bindText(handle, memberStatement.get(), 4, member.owningRunId),
            bindText(handle, memberStatement.get(), 5, member.contentHash),
            bindText(handle, memberStatement.get(), 6, member.lineageRootProposalId)};
        for (const auto& binding : memberBindings)
            if (!binding.success()) return rollbackWith(*binding.error());
        if (sqlite3_step(memberStatement.get()) != SQLITE_DONE) {
            const int extended = sqlite3_extended_errcode(handle);
            if (extended == SQLITE_CONSTRAINT_PRIMARYKEY || extended == SQLITE_CONSTRAINT_UNIQUE) {
                return rollbackWith({
                    "manifest_verification_manual_handoff.member_already_claimed",
                    "A frozen member proposal is already claimed by a different manual handoff"});
            }
            return rollbackWith(databaseError(handle,
                "manifest_verification_manual_handoff.write_failed",
                "Could not persist a frozen manual-handoff member"));
        }
    }

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<void>::ok();
}

namespace {

constexpr const char* manualHandoffColumns =
    "id,origin_manifest_run_id,verification_anchor_run_id,schema_version,source_project_id,"
    "storage_key,workspace_identity,baseline_fingerprint,source_overlay_fingerprint,"
    "frozen_members_json,allowed_paths_json,original_snapshot_json,created_at_ms";

common::Result<application::ManifestVerificationManualHandoff> readManualHandoff(
    sqlite3_stmt* statement)
{
    application::ManifestVerificationManualHandoff handoff;
    handoff.id = columnText(statement, 0);
    handoff.originManifestRunId = columnText(statement, 1);
    handoff.verificationAnchorRunId = columnText(statement, 2);
    handoff.schemaVersion = columnText(statement, 3);
    handoff.sourceProjectId = columnText(statement, 4);
    handoff.storageKey = columnText(statement, 5);
    handoff.workspaceIdentity = columnText(statement, 6);
    handoff.baselineFingerprint = columnText(statement, 7);
    handoff.sourceOverlayFingerprint = columnText(statement, 8);
    const auto corrupt = [] {
        return common::Result<application::ManifestVerificationManualHandoff>::failure({
            "manifest_verification_manual_handoff.corrupt",
            "Stored manual-handoff identity is invalid"});
    };
    const auto frozenMembersJson = nlohmann::json::parse(columnText(statement, 9), nullptr, false);
    const auto allowedPathsJson = nlohmann::json::parse(columnText(statement, 10), nullptr, false);
    if (frozenMembersJson.is_discarded() || allowedPathsJson.is_discarded()
        || !frozenMembersJson.is_array() || !allowedPathsJson.is_array()) {
        return corrupt();
    }
    try {
        for (const auto& entry : frozenMembersJson) {
            if (!entry.is_object() || !entry.contains("proposalId")
                || !entry.contains("owningRunId") || !entry.contains("contentHash")
                || !entry.contains("lineageRootProposalId")) {
                return corrupt();
            }
            application::ManifestVerificationManualHandoffFrozenMember member;
            member.proposalId = entry.at("proposalId").get<std::string>();
            member.owningRunId = entry.at("owningRunId").get<std::string>();
            member.contentHash = entry.at("contentHash").get<std::string>();
            member.lineageRootProposalId = entry.at("lineageRootProposalId").get<std::string>();
            handoff.frozenMembers.push_back(std::move(member));
        }
        for (const auto& entry : allowedPathsJson) {
            if (!entry.is_object() || !entry.contains("relativePath")
                || !entry.contains("ownerProposalId") || !entry.contains("ownerTaskId")) {
                return corrupt();
            }
            application::ManifestVerificationManualHandoffAllowedPath allowed;
            allowed.relativePath = entry.at("relativePath").get<std::string>();
            allowed.ownerProposalId = entry.at("ownerProposalId").get<std::string>();
            allowed.ownerTaskId = entry.at("ownerTaskId").get<std::string>();
            handoff.allowedPaths.push_back(std::move(allowed));
        }
    } catch (...) {
        return corrupt();
    }
    const auto snapshot = application::WorkspaceBaselineProvenance::parseJson(
        columnText(statement, 11));
    if (!snapshot.success()) return corrupt();
    handoff.originalSnapshot = snapshot.value();
    handoff.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 12));
    const auto valid = application::validateManifestVerificationManualHandoff(handoff);
    if (!valid.success()) return corrupt();
    return common::Result<application::ManifestVerificationManualHandoff>::ok(
        std::move(handoff));
}

} // namespace

common::Result<std::optional<application::ManifestVerificationManualHandoff>>
SqliteManifestVerificationManualHandoffRepository::findHandoffByAnchorRun(
    const application::RunId& verificationAnchorRunId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + manualHandoffColumns
        + " FROM manifest_verification_manual_handoffs WHERE verification_anchor_run_id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, verificationAnchorRunId);
    if (!bound.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::ok(std::nullopt);
    if (step != SQLITE_ROW) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::failure(
        databaseError(handle, "manifest_verification_manual_handoff.read_failed", "Could not read manual handoff"));
    auto handoff = readManualHandoff(statement.get());
    if (!handoff.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::failure(*handoff.error());
    return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::ok(std::move(handoff.value()));
}

common::Result<std::optional<application::ManifestVerificationManualHandoff>>
SqliteManifestVerificationManualHandoffRepository::findHandoffById(
    const std::string& handoffId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + manualHandoffColumns
        + " FROM manifest_verification_manual_handoffs WHERE id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, handoffId);
    if (!bound.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::ok(std::nullopt);
    if (step != SQLITE_ROW) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::failure(
        databaseError(handle, "manifest_verification_manual_handoff.read_failed", "Could not read manual handoff"));
    auto handoff = readManualHandoff(statement.get());
    if (!handoff.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::failure(*handoff.error());
    return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::ok(std::move(handoff.value()));
}

common::Result<std::optional<application::ManifestVerificationManualHandoff>>
SqliteManifestVerificationManualHandoffRepository::findHandoffByMemberProposalId(
    const application::ChangeSetId& proposalId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ")
        + "h.id,h.origin_manifest_run_id,h.verification_anchor_run_id,h.schema_version,"
          "h.source_project_id,h.storage_key,h.workspace_identity,h.baseline_fingerprint,"
          "h.source_overlay_fingerprint,h.frozen_members_json,h.allowed_paths_json,"
          "h.original_snapshot_json,h.created_at_ms "
          "FROM manifest_verification_manual_handoff_members AS m "
          "JOIN manifest_verification_manual_handoffs AS h ON h.id=m.handoff_id "
          "WHERE m.proposal_id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, proposalId);
    if (!bound.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::ok(std::nullopt);
    if (step != SQLITE_ROW) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::failure(
        databaseError(handle, "manifest_verification_manual_handoff.read_failed", "Could not read manual handoff"));
    auto handoff = readManualHandoff(statement.get());
    if (!handoff.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::failure(*handoff.error());
    return common::Result<std::optional<application::ManifestVerificationManualHandoff>>::ok(std::move(handoff.value()));
}

common::Result<std::vector<application::ManifestVerificationManualHandoff>>
SqliteManifestVerificationManualHandoffRepository::listAllHandoffs() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + manualHandoffColumns
        + " FROM manifest_verification_manual_handoffs;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<application::ManifestVerificationManualHandoff>>::failure(*prepared.error());
    std::vector<application::ManifestVerificationManualHandoff> result;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) return common::Result<std::vector<application::ManifestVerificationManualHandoff>>::failure(
            databaseError(handle, "manifest_verification_manual_handoff.read_failed", "Could not list manual handoffs"));
        auto handoff = readManualHandoff(statement.get());
        if (!handoff.success()) return common::Result<std::vector<application::ManifestVerificationManualHandoff>>::failure(*handoff.error());
        result.push_back(std::move(handoff.value()));
    }
    return common::Result<std::vector<application::ManifestVerificationManualHandoff>>::ok(std::move(result));
}

common::Result<void> SqliteManifestVerificationManualHandoffRepository::discardHandoff(
    const std::string& handoffId)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    // A single DELETE on the parent row. The real schema's ON DELETE CASCADE
    // (foreign_keys=ON, established once at connection open - see
    // SqliteDatabase::open()) already removes every attempt AND member row
    // belonging to this handoff within the same implicit transaction; no
    // separate member/attempt cleanup statement is needed here, and adding
    // one would only risk drifting out of sync with the schema's own
    // cascade. Neither child table's own reject-update trigger fires on a
    // DELETE, only an UPDATE, so this cascade is never blocked by them.
    const auto prepared = prepare(
        handle, "DELETE FROM manifest_verification_manual_handoffs WHERE id=?;", statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, handoffId);
    if (!bound.success()) return common::Result<void>::failure(*bound.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(databaseError(
            handle, "manifest_verification_manual_handoff.delete_failed",
            "Could not delete the manual handoff"));
    }
    if (sqlite3_changes(handle) == 0) {
        return common::Result<void>::failure(error(
            "manifest_verification_manual_handoff.not_found",
            "No manual handoff exists with this id"));
    }
    return common::Result<void>::ok();
}

common::Result<void> SqliteManifestVerificationManualHandoffRepository::insertAttempt(
    application::ManifestVerificationManualHandoffAttempt attempt)
{
    const auto valid = application::validateManifestVerificationManualHandoffAttempt(attempt);
    if (!valid.success()) return valid;
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO manifest_verification_manual_handoff_attempts "
        "(id,handoff_id,verification_anchor_run_id,attempt_run_id,schema_version,attempt_ordinal,"
        "previous_attempt_id,edited_snapshot_fingerprint,created_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?,?);", statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, attempt.id),
        bindText(handle, statement.get(), 2, attempt.handoffId),
        bindText(handle, statement.get(), 3, attempt.verificationAnchorRunId),
        bindText(handle, statement.get(), 4, attempt.attemptRunId),
        bindText(handle, statement.get(), 5, attempt.schemaVersion),
        bindInt64(handle, statement.get(), 6, static_cast<std::int64_t>(attempt.attemptOrdinal)),
        bindNullableText(handle, statement.get(), 7, attempt.previousAttemptId),
        bindText(handle, statement.get(), 8, attempt.editedSnapshotFingerprint),
        bindInt64(handle, statement.get(), 9, toMilliseconds(attempt.createdAt))};
    for (const auto& binding : bindings)
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        const int extended = sqlite3_extended_errcode(handle);
        if (extended == SQLITE_CONSTRAINT_PRIMARYKEY || extended == SQLITE_CONSTRAINT_UNIQUE) {
            return common::Result<void>::failure({
                "manifest_verification_manual_handoff.attempt_already_exists",
                "A manual-handoff attempt already exists for this attempt Run or edited state"});
        }
        return common::Result<void>::failure(databaseError(handle,
            "manifest_verification_manual_handoff.attempt_write_failed",
            "Could not persist the manual-handoff attempt"));
    }
    return common::Result<void>::ok();
}

namespace {

constexpr const char* manualHandoffAttemptColumns =
    "id,handoff_id,verification_anchor_run_id,attempt_run_id,schema_version,attempt_ordinal,"
    "previous_attempt_id,edited_snapshot_fingerprint,created_at_ms";

common::Result<application::ManifestVerificationManualHandoffAttempt> readManualHandoffAttempt(
    sqlite3_stmt* statement)
{
    application::ManifestVerificationManualHandoffAttempt attempt;
    attempt.id = columnText(statement, 0);
    attempt.handoffId = columnText(statement, 1);
    attempt.verificationAnchorRunId = columnText(statement, 2);
    attempt.attemptRunId = columnText(statement, 3);
    attempt.schemaVersion = columnText(statement, 4);
    const auto ordinal = sqlite3_column_int64(statement, 5);
    if (ordinal < 0) {
        return common::Result<application::ManifestVerificationManualHandoffAttempt>::failure({
            "manifest_verification_manual_handoff.corrupt_attempt",
            "Stored manual-handoff attempt ordinal is invalid"});
    }
    attempt.attemptOrdinal = static_cast<std::uint64_t>(ordinal);
    if (sqlite3_column_type(statement, 6) != SQLITE_NULL) {
        attempt.previousAttemptId = columnText(statement, 6);
    }
    attempt.editedSnapshotFingerprint = columnText(statement, 7);
    attempt.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 8));
    const auto valid = application::validateManifestVerificationManualHandoffAttempt(attempt);
    if (!valid.success()) {
        return common::Result<application::ManifestVerificationManualHandoffAttempt>::failure({
            "manifest_verification_manual_handoff.corrupt_attempt",
            "Stored manual-handoff attempt violates its typed contract"});
    }
    return common::Result<application::ManifestVerificationManualHandoffAttempt>::ok(
        std::move(attempt));
}

} // namespace

common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>
SqliteManifestVerificationManualHandoffRepository::findAttemptByRunId(
    const application::RunId& attemptRunId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + manualHandoffAttemptColumns
        + " FROM manifest_verification_manual_handoff_attempts WHERE attempt_run_id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, attemptRunId);
    if (!bound.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>::ok(std::nullopt);
    if (step != SQLITE_ROW) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>::failure(
        databaseError(handle, "manifest_verification_manual_handoff.attempt_read_failed", "Could not read manual-handoff attempt"));
    auto attempt = readManualHandoffAttempt(statement.get());
    if (!attempt.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>::failure(*attempt.error());
    return common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>::ok(std::move(attempt.value()));
}

common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>
SqliteManifestVerificationManualHandoffRepository::findAttemptByFingerprint(
    const std::string& handoffId, const std::string& editedSnapshotFingerprint) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + manualHandoffAttemptColumns
        + " FROM manifest_verification_manual_handoff_attempts "
          "WHERE handoff_id=? AND edited_snapshot_fingerprint=? "
          "ORDER BY attempt_ordinal DESC LIMIT 1;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, handoffId),
        bindText(handle, statement.get(), 2, editedSnapshotFingerprint)};
    for (const auto& binding : bindings)
        if (!binding.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>::failure(*binding.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>::ok(std::nullopt);
    if (step != SQLITE_ROW) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>::failure(
        databaseError(handle, "manifest_verification_manual_handoff.attempt_read_failed", "Could not read manual-handoff attempt"));
    auto attempt = readManualHandoffAttempt(statement.get());
    if (!attempt.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>::failure(*attempt.error());
    return common::Result<std::optional<application::ManifestVerificationManualHandoffAttempt>>::ok(std::move(attempt.value()));
}

common::Result<std::vector<application::ManifestVerificationManualHandoffAttempt>>
SqliteManifestVerificationManualHandoffRepository::listAttemptsForHandoff(
    const std::string& handoffId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + manualHandoffAttemptColumns
        + " FROM manifest_verification_manual_handoff_attempts "
          "WHERE handoff_id=? ORDER BY attempt_ordinal;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<application::ManifestVerificationManualHandoffAttempt>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, handoffId);
    if (!bound.success()) return common::Result<std::vector<application::ManifestVerificationManualHandoffAttempt>>::failure(*bound.error());
    std::vector<application::ManifestVerificationManualHandoffAttempt> result;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) return common::Result<std::vector<application::ManifestVerificationManualHandoffAttempt>>::failure(
            databaseError(handle, "manifest_verification_manual_handoff.attempt_read_failed", "Could not list manual-handoff attempts"));
        auto attempt = readManualHandoffAttempt(statement.get());
        if (!attempt.success()) return common::Result<std::vector<application::ManifestVerificationManualHandoffAttempt>>::failure(*attempt.error());
        result.push_back(std::move(attempt.value()));
    }
    return common::Result<std::vector<application::ManifestVerificationManualHandoffAttempt>>::ok(std::move(result));
}

common::Result<std::vector<application::ManifestVerificationManualHandoffAttempt>>
SqliteManifestVerificationManualHandoffRepository::listAllAttempts() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + manualHandoffAttemptColumns
        + " FROM manifest_verification_manual_handoff_attempts;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<application::ManifestVerificationManualHandoffAttempt>>::failure(*prepared.error());
    std::vector<application::ManifestVerificationManualHandoffAttempt> result;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) return common::Result<std::vector<application::ManifestVerificationManualHandoffAttempt>>::failure(
            databaseError(handle, "manifest_verification_manual_handoff.attempt_read_failed", "Could not list manual-handoff attempts"));
        auto attempt = readManualHandoffAttempt(statement.get());
        if (!attempt.success()) return common::Result<std::vector<application::ManifestVerificationManualHandoffAttempt>>::failure(*attempt.error());
        result.push_back(std::move(attempt.value()));
    }
    return common::Result<std::vector<application::ManifestVerificationManualHandoffAttempt>>::ok(std::move(result));
}

namespace {

const char* manualHandoffAttemptResultKindToText(
    application::ManifestVerificationManualHandoffAttemptResultKind kind)
{
    switch (kind) {
        case application::ManifestVerificationManualHandoffAttemptResultKind::VerifiedSuccess:
            return "verified_success";
        case application::ManifestVerificationManualHandoffAttemptResultKind::VerifiedContentFailure:
            return "verified_content_failure";
        case application::ManifestVerificationManualHandoffAttemptResultKind::RejectedBeforeVerification:
            return "rejected_before_verification";
        case application::ManifestVerificationManualHandoffAttemptResultKind::Incomplete:
            return "incomplete";
    }
    return "incomplete";
}

// Fails closed on any value that is not one of the four the CHECK
// constraint itself already limits the column to - a corrupt or
// out-of-band row is never guessed into a default kind (see
// ManifestVerificationManualHandoffAttemptResultKind's own doc comment).
common::Result<application::ManifestVerificationManualHandoffAttemptResultKind>
manualHandoffAttemptResultKindFromText(const std::string& text)
{
    if (text == "verified_success") {
        return common::Result<application::ManifestVerificationManualHandoffAttemptResultKind>::ok(
            application::ManifestVerificationManualHandoffAttemptResultKind::VerifiedSuccess);
    }
    if (text == "verified_content_failure") {
        return common::Result<application::ManifestVerificationManualHandoffAttemptResultKind>::ok(
            application::ManifestVerificationManualHandoffAttemptResultKind::VerifiedContentFailure);
    }
    if (text == "rejected_before_verification") {
        return common::Result<application::ManifestVerificationManualHandoffAttemptResultKind>::ok(
            application::ManifestVerificationManualHandoffAttemptResultKind::RejectedBeforeVerification);
    }
    if (text == "incomplete") {
        return common::Result<application::ManifestVerificationManualHandoffAttemptResultKind>::ok(
            application::ManifestVerificationManualHandoffAttemptResultKind::Incomplete);
    }
    return common::Result<application::ManifestVerificationManualHandoffAttemptResultKind>::failure({
        "manifest_verification_manual_handoff.corrupt_attempt_result",
        "Stored manual-handoff attempt result kind is not one of the recognised typed values"});
}

} // namespace

common::Result<void> SqliteManifestVerificationManualHandoffRepository::insertAttemptResult(
    application::ManifestVerificationManualHandoffAttemptResult result)
{
    const auto valid = application::validateManifestVerificationManualHandoffAttemptResult(result);
    if (!valid.success()) return valid;
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO manifest_verification_manual_handoff_attempt_results "
        "(id,attempt_id,attempt_run_id,schema_version,kind,rejection_code,rejection_message,"
        "recorded_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?);", statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, result.id),
        bindText(handle, statement.get(), 2, result.attemptId),
        bindText(handle, statement.get(), 3, result.attemptRunId),
        bindText(handle, statement.get(), 4, result.schemaVersion),
        bindText(handle, statement.get(), 5, manualHandoffAttemptResultKindToText(result.kind)),
        bindNullableText(handle, statement.get(), 6, result.rejectionCode),
        bindNullableText(handle, statement.get(), 7, result.rejectionMessage),
        bindInt64(handle, statement.get(), 8, toMilliseconds(result.recordedAt))};
    for (const auto& binding : bindings)
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        const int extended = sqlite3_extended_errcode(handle);
        if (extended == SQLITE_CONSTRAINT_PRIMARYKEY || extended == SQLITE_CONSTRAINT_UNIQUE) {
            return common::Result<void>::failure({
                "manifest_verification_manual_handoff.attempt_result_already_exists",
                "A typed result already exists for this manual-handoff attempt"});
        }
        return common::Result<void>::failure(databaseError(handle,
            "manifest_verification_manual_handoff.attempt_result_write_failed",
            "Could not persist the manual-handoff attempt result"));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<application::ManifestVerificationManualHandoffAttemptResult>>
SqliteManifestVerificationManualHandoffRepository::findAttemptResultByAttemptId(
    const std::string& attemptId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT id,attempt_id,attempt_run_id,schema_version,kind,rejection_code,"
        "rejection_message,recorded_at_ms "
        "FROM manifest_verification_manual_handoff_attempt_results WHERE attempt_id=?;",
        statement);
    if (!prepared.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttemptResult>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, attemptId);
    if (!bound.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttemptResult>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttemptResult>>::ok(std::nullopt);
    if (step != SQLITE_ROW) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttemptResult>>::failure(
        databaseError(handle, "manifest_verification_manual_handoff.attempt_result_read_failed",
            "Could not read manual-handoff attempt result"));
    application::ManifestVerificationManualHandoffAttemptResult result;
    result.id = columnText(statement.get(), 0);
    result.attemptId = columnText(statement.get(), 1);
    result.attemptRunId = columnText(statement.get(), 2);
    result.schemaVersion = columnText(statement.get(), 3);
    const auto kind = manualHandoffAttemptResultKindFromText(columnText(statement.get(), 4));
    if (!kind.success()) return common::Result<std::optional<application::ManifestVerificationManualHandoffAttemptResult>>::failure(*kind.error());
    result.kind = kind.value();
    if (sqlite3_column_type(statement.get(), 5) != SQLITE_NULL) {
        result.rejectionCode = columnText(statement.get(), 5);
    }
    if (sqlite3_column_type(statement.get(), 6) != SQLITE_NULL) {
        result.rejectionMessage = columnText(statement.get(), 6);
    }
    result.recordedAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 7));
    const auto valid = application::validateManifestVerificationManualHandoffAttemptResult(result);
    if (!valid.success()) {
        return common::Result<std::optional<application::ManifestVerificationManualHandoffAttemptResult>>::failure({
            "manifest_verification_manual_handoff.corrupt_attempt_result",
            "Stored manual-handoff attempt result violates its typed contract"});
    }
    return common::Result<std::optional<application::ManifestVerificationManualHandoffAttemptResult>>::ok(
        std::move(result));
}

SqliteManifestVerificationBaselineNormalizationRepository::
SqliteManifestVerificationBaselineNormalizationRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteManifestVerificationBaselineNormalizationRepository::insert(
    application::ManifestVerificationBaselineNormalization normalization)
{
    const auto valid = application::validateManifestVerificationBaselineNormalization(normalization);
    if (!valid.success()) return valid;
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO manifest_verification_baseline_normalizations "
        "(source_run_id,id,schema_version,policy_id,durable_baseline_fingerprint,"
        "normalized_baseline_fingerprint,excluded_prefixes_json,normalized_paths_json,"
        "created_at_ms) VALUES (?,?,?,?,?,?,?,?,?);", statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, normalization.sourceRunId),
        bindText(handle, statement.get(), 2, normalization.id),
        bindText(handle, statement.get(), 3, normalization.schemaVersion),
        bindText(handle, statement.get(), 4, normalization.policyId),
        bindText(handle, statement.get(), 5, normalization.durableBaselineFingerprint),
        bindText(handle, statement.get(), 6, normalization.normalizedBaselineFingerprint),
        bindText(handle, statement.get(), 7, nlohmann::json(normalization.excludedPrefixes).dump()),
        bindText(handle, statement.get(), 8, nlohmann::json(normalization.normalizedPaths).dump()),
        bindInt64(handle, statement.get(), 9, toMilliseconds(normalization.createdAt))};
    for (const auto& binding : bindings)
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<void>::failure({
                "manifest_verification_baseline_normalization.already_exists",
                "A baseline-normalization proof already exists for this source Run"});
        }
        return common::Result<void>::failure(databaseError(handle,
            "manifest_verification_baseline_normalization.write_failed",
            "Could not persist the baseline-normalization proof"));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<application::ManifestVerificationBaselineNormalization>>
SqliteManifestVerificationBaselineNormalizationRepository::findBySourceRun(
    const application::RunId& sourceRunId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT id,schema_version,policy_id,durable_baseline_fingerprint,"
        "normalized_baseline_fingerprint,excluded_prefixes_json,normalized_paths_json,"
        "created_at_ms FROM manifest_verification_baseline_normalizations WHERE source_run_id=?;",
        statement);
    if (!prepared.success())
        return common::Result<std::optional<application::ManifestVerificationBaselineNormalization>>
            ::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, sourceRunId);
    if (!bound.success())
        return common::Result<std::optional<application::ManifestVerificationBaselineNormalization>>
            ::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE)
        return common::Result<std::optional<application::ManifestVerificationBaselineNormalization>>
            ::ok(std::nullopt);
    if (step != SQLITE_ROW)
        return common::Result<std::optional<application::ManifestVerificationBaselineNormalization>>
            ::failure(databaseError(handle,
                "manifest_verification_baseline_normalization.read_failed",
                "Could not read the baseline-normalization proof"));
    application::ManifestVerificationBaselineNormalization normalization;
    normalization.sourceRunId = sourceRunId;
    normalization.id = columnText(statement.get(), 0);
    normalization.schemaVersion = columnText(statement.get(), 1);
    normalization.policyId = columnText(statement.get(), 2);
    normalization.durableBaselineFingerprint = columnText(statement.get(), 3);
    normalization.normalizedBaselineFingerprint = columnText(statement.get(), 4);
    try {
        normalization.excludedPrefixes =
            nlohmann::json::parse(columnText(statement.get(), 5))
                .get<std::vector<std::string>>();
        normalization.normalizedPaths =
            nlohmann::json::parse(columnText(statement.get(), 6))
                .get<std::vector<std::string>>();
    } catch (const std::exception&) {
        return common::Result<std::optional<application::ManifestVerificationBaselineNormalization>>
            ::failure({"manifest_verification_baseline_normalization.corrupt_identity",
                "Stored baseline-normalization path lists are invalid"});
    }
    normalization.createdAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 7));
    const auto valid = application::validateManifestVerificationBaselineNormalization(normalization);
    if (!valid.success()) {
        return common::Result<std::optional<application::ManifestVerificationBaselineNormalization>>
            ::failure({"manifest_verification_baseline_normalization.corrupt_identity",
                "Stored baseline-normalization data violates its typed contract"});
    }
    return common::Result<std::optional<application::ManifestVerificationBaselineNormalization>>::ok(
        std::move(normalization));
}

common::Result<void> SqliteRunWorkspaceScopeRepository::freeze(application::RunWorkspaceScope scope)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO run_workspace_scopes "
        "(run_id,schema_version,manifest_schema_version,entries_json,dependency_edges_json,"
        "frozen_at_ms) VALUES (?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, scope.runId),
        bindText(handle, statement.get(), 2, scope.schemaVersion),
        bindText(handle, statement.get(), 3, scope.manifestSchemaVersion),
        bindText(handle, statement.get(), 4, workspaceScopeEntriesToJson(scope.entries)),
        bindText(handle, statement.get(), 5, workspaceScopeEdgesToJson(scope.dependencyEdges)),
        bindInt64(handle, statement.get(), 6, toMilliseconds(scope.frozenAt))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<void>::failure(error(
                "workspace_scope.already_frozen",
                "A workspace scope already exists for this Run"));
        }
        return common::Result<void>::failure(databaseError(
            handle, "workspace_scope.write_failed", "Could not freeze workspace scope"));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<application::RunWorkspaceScope>>
SqliteRunWorkspaceScopeRepository::findForRun(const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT schema_version,manifest_schema_version,entries_json,dependency_edges_json,"
        "frozen_at_ms FROM run_workspace_scopes WHERE run_id=?;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::optional<application::RunWorkspaceScope>>::failure(*prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::optional<application::RunWorkspaceScope>>::failure(*bound.error());
    }
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return common::Result<std::optional<application::RunWorkspaceScope>>::ok(std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<application::RunWorkspaceScope>>::failure(databaseError(
            handle, "workspace_scope.read_failed", "Could not read frozen workspace scope"));
    }
    application::RunWorkspaceScope scope;
    scope.runId = runId;
    scope.schemaVersion = columnText(statement.get(), 0);
    scope.manifestSchemaVersion = columnText(statement.get(), 1);
    auto entries = workspaceScopeEntriesFromJson(columnText(statement.get(), 2));
    if (!entries.success()) {
        return common::Result<std::optional<application::RunWorkspaceScope>>::failure(*entries.error());
    }
    scope.entries = std::move(entries.value());
    auto edges = workspaceScopeEdgesFromJson(columnText(statement.get(), 3));
    if (!edges.success()) {
        return common::Result<std::optional<application::RunWorkspaceScope>>::failure(*edges.error());
    }
    scope.dependencyEdges = std::move(edges.value());
    scope.frozenAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 4));
    return common::Result<std::optional<application::RunWorkspaceScope>>::ok(std::move(scope));
}

common::Result<void> SqliteRunWorkspaceScopeRepository::recordExpansion(
    application::RunWorkspaceScopeExpansion expansion)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO run_workspace_scope_expansions "
        "(id,run_id,task_id,relative_path,role,reason,baseline_hash,approved_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, expansion.id),
        bindText(handle, statement.get(), 2, expansion.runId),
        bindText(handle, statement.get(), 3, expansion.taskId),
        bindText(handle, statement.get(), 4, expansion.relativePath),
        bindText(handle, statement.get(), 5, application::toString(expansion.role)),
        bindText(handle, statement.get(), 6, expansion.reason),
        bindNullableText(handle, statement.get(), 7, expansion.baselineHash),
        bindInt64(handle, statement.get(), 8, toMilliseconds(expansion.approvedAt))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY
            || sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_UNIQUE) {
            return common::Result<void>::failure(error(
                "workspace_scope.expansion_already_recorded",
                "This path was already recorded as an expansion for this Run/task"));
        }
        return common::Result<void>::failure(databaseError(
            handle, "workspace_scope.write_failed", "Could not persist workspace scope expansion"));
    }
    return common::Result<void>::ok();
}

common::Result<std::vector<application::RunWorkspaceScopeExpansion>>
SqliteRunWorkspaceScopeRepository::listExpansionsForRun(const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT id,task_id,relative_path,role,reason,baseline_hash,approved_at_ms "
        "FROM run_workspace_scope_expansions WHERE run_id=? ORDER BY approved_at_ms, id;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::vector<application::RunWorkspaceScopeExpansion>>::failure(
            *prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::vector<application::RunWorkspaceScopeExpansion>>::failure(
            *bound.error());
    }
    std::vector<application::RunWorkspaceScopeExpansion> results;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<application::RunWorkspaceScopeExpansion>>::failure(
                databaseError(handle, "workspace_scope.read_failed",
                    "Could not list workspace scope expansions"));
        }
        application::RunWorkspaceScopeExpansion expansion;
        expansion.id = columnText(statement.get(), 0);
        expansion.runId = runId;
        expansion.taskId = columnText(statement.get(), 1);
        expansion.relativePath = columnText(statement.get(), 2);
        const auto role = application::workspaceScopeRoleFromString(columnText(statement.get(), 3));
        if (!role.has_value()) {
            return common::Result<std::vector<application::RunWorkspaceScopeExpansion>>::failure(
                error("workspace_scope.corrupt_entry", "Unknown workspace scope expansion role"));
        }
        expansion.role = *role;
        expansion.reason = columnText(statement.get(), 4);
        expansion.baselineHash = optionalText(statement.get(), 5);
        expansion.approvedAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 6));
        results.push_back(std::move(expansion));
    }
    return common::Result<std::vector<application::RunWorkspaceScopeExpansion>>::ok(std::move(results));
}

common::Result<void> SqliteRunWorkspaceScopeRepository::recordDiagnosticRead(
    application::RunWorkspaceScopeDiagnosticRead grant)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO run_workspace_scope_diagnostic_reads "
        "(id,run_id,task_id,relative_path,finding_code,reason,baseline_hash,granted_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, grant.id),
        bindText(handle, statement.get(), 2, grant.runId),
        bindText(handle, statement.get(), 3, grant.taskId),
        bindText(handle, statement.get(), 4, grant.relativePath),
        bindText(handle, statement.get(), 5, grant.findingCode),
        bindText(handle, statement.get(), 6, grant.reason),
        bindNullableText(handle, statement.get(), 7, grant.baselineHash),
        bindInt64(handle, statement.get(), 8, toMilliseconds(grant.grantedAt))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY
            || sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_UNIQUE) {
            return common::Result<void>::failure(error(
                "workspace_scope.diagnostic_read_already_recorded",
                "This path was already recorded as a diagnostic read for this Run/task"));
        }
        return common::Result<void>::failure(databaseError(
            handle, "workspace_scope.write_failed", "Could not persist diagnostic read grant"));
    }
    return common::Result<void>::ok();
}

common::Result<std::vector<application::RunWorkspaceScopeDiagnosticRead>>
SqliteRunWorkspaceScopeRepository::listDiagnosticReadsForRun(const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT id,task_id,relative_path,finding_code,reason,baseline_hash,granted_at_ms "
        "FROM run_workspace_scope_diagnostic_reads WHERE run_id=? ORDER BY granted_at_ms, id;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::vector<application::RunWorkspaceScopeDiagnosticRead>>::failure(
            *prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::vector<application::RunWorkspaceScopeDiagnosticRead>>::failure(
            *bound.error());
    }
    std::vector<application::RunWorkspaceScopeDiagnosticRead> results;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<application::RunWorkspaceScopeDiagnosticRead>>::failure(
                databaseError(handle, "workspace_scope.read_failed",
                    "Could not list diagnostic read grants"));
        }
        application::RunWorkspaceScopeDiagnosticRead grant;
        grant.id = columnText(statement.get(), 0);
        grant.runId = runId;
        grant.taskId = columnText(statement.get(), 1);
        grant.relativePath = columnText(statement.get(), 2);
        grant.findingCode = columnText(statement.get(), 3);
        grant.reason = columnText(statement.get(), 4);
        grant.baselineHash = optionalText(statement.get(), 5);
        grant.grantedAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 6));
        results.push_back(std::move(grant));
    }
    return common::Result<std::vector<application::RunWorkspaceScopeDiagnosticRead>>::ok(
        std::move(results));
}

namespace {

bool safeVerificationTarget(std::string_view value)
{
    return !value.empty() && value.size() <= 128
        && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.';
           });
}

bool safeVerificationRelativePath(std::string_view value)
{
    if (value.empty()
        || value.size() > application::kVerificationEnvironmentMaxRelativePathBytes
        || value.front() == '/' || value.back() == '/'
        || value.find('\\') != std::string_view::npos
        || value.find(':') != std::string_view::npos
        || value.find('$') != std::string_view::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find('/', start);
        const auto part = value.substr(start,
            (end == std::string_view::npos ? value.size() : end) - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (std::any_of(part.begin(), part.end(), [](unsigned char ch) {
                return ch < 0x20 || ch == 0x7f || ch == '*' || ch == '?' || ch == '['
                    || ch == ']' || ch == '{' || ch == '}';
            })) {
            return false;
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

nlohmann::json buildPresetToJson(const application::VerificationBuildPresetDescriptor& preset)
{
    return nlohmann::json {
        {"name", preset.name},
        {"displayName", preset.displayName},
        {"source", application::toString(preset.source)},
        {"declaredConfigurePreset", preset.declaredConfigurePreset},
        {"declaredTargets", preset.declaredTargets},
        {"allowedTargetsConfirmed", preset.allowedTargetsConfirmed},
        {"allowedTargets", preset.allowedTargets},
        {"allowedTargetsComplete", preset.allowedTargetsComplete}
    };
}

common::Result<application::VerificationBuildPresetDescriptor> buildPresetFromJson(
    const nlohmann::json& j)
{
    auto corrupt = [] {
        return common::Result<application::VerificationBuildPresetDescriptor>::failure(
            error("verification_environment.corrupt_entry",
                "Corrupt verification environment build preset"));
    };
    const bool hasAllowedTargetsConfirmed = j.contains("allowedTargetsConfirmed");
    const bool hasAllowedTargets = j.contains("allowedTargets");
    if (!j.is_object() || !j.contains("name") || !j["name"].is_string() || j["name"].get<std::string>().empty()
        || !j.contains("source") || !j["source"].is_string()
        || !j.contains("declaredTargets") || !j["declaredTargets"].is_array()
        || j["declaredTargets"].size() > application::kVerificationEnvironmentMaxTargetsPerPreset
        || hasAllowedTargetsConfirmed != hasAllowedTargets
        || (hasAllowedTargetsConfirmed && !j["allowedTargetsConfirmed"].is_boolean())
        || (j.contains("allowedTargetsComplete") && !j["allowedTargetsComplete"].is_boolean())
        || (hasAllowedTargets && (!j["allowedTargets"].is_array()
            || j["allowedTargets"].size() > application::kVerificationEnvironmentMaxTargetsPerPreset))) {
        return corrupt();
    }
    const auto source = application::verificationPresetFileSourceFromString(
        j["source"].get<std::string>());
    if (!source.has_value()) return corrupt();
    application::VerificationBuildPresetDescriptor preset;
    preset.name = j["name"].get<std::string>();
    if (j.contains("displayName") && j["displayName"].is_string())
        preset.displayName = j["displayName"].get<std::string>();
    preset.source = *source;
    if (j.contains("declaredConfigurePreset") && j["declaredConfigurePreset"].is_string())
        preset.declaredConfigurePreset = j["declaredConfigurePreset"].get<std::string>();
    for (const auto& target : j["declaredTargets"]) {
        if (!target.is_string()) return corrupt();
        preset.declaredTargets.push_back(target.get<std::string>());
    }
    // Rows written before the additive target-catalog fields existed remain
    // readable as an explicitly unconfirmed catalog. AG-18.2 must treat this
    // state as setup-required/blocked, never as an unrestricted allow-list.
    preset.allowedTargetsConfirmed = hasAllowedTargetsConfirmed
        && j["allowedTargetsConfirmed"].get<bool>();
    // AG-17.0: additive. A row written before the field existed carries no
    // completeness proof and reads as NOT complete (unknown), never complete.
    preset.allowedTargetsComplete = j.contains("allowedTargetsComplete")
        && j["allowedTargetsComplete"].get<bool>();
    if (preset.allowedTargetsComplete && !preset.allowedTargetsConfirmed) return corrupt();
    std::unordered_set<std::string> seenAllowedTargets;
    if (hasAllowedTargets) {
        for (const auto& target : j["allowedTargets"]) {
            if (!target.is_string() || !safeVerificationTarget(target.get<std::string>())) return corrupt();
            const auto value = target.get<std::string>();
            if (!seenAllowedTargets.insert(value).second) return corrupt();
            preset.allowedTargets.push_back(value);
        }
    }
    if (!preset.allowedTargetsConfirmed && !preset.allowedTargets.empty()) return corrupt();
    return common::Result<application::VerificationBuildPresetDescriptor>::ok(std::move(preset));
}

nlohmann::json buildPresetsToJson(
    const std::vector<application::VerificationBuildPresetDescriptor>& presets)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& preset : presets) array.push_back(buildPresetToJson(preset));
    return array;
}

common::Result<std::vector<application::VerificationBuildPresetDescriptor>>
buildPresetsFromJsonArray(const nlohmann::json& parsed, std::size_t maxCount)
{
    if (!parsed.is_array() || parsed.size() > maxCount) {
        return common::Result<std::vector<application::VerificationBuildPresetDescriptor>>::failure(
            error("verification_environment.corrupt_entry",
                "Corrupt or out-of-bound verification environment build presets"));
    }
    std::vector<application::VerificationBuildPresetDescriptor> presets;
    presets.reserve(parsed.size());
    for (const auto& item : parsed) {
        auto preset = buildPresetFromJson(item);
        if (!preset.success()) {
            return common::Result<std::vector<application::VerificationBuildPresetDescriptor>>::failure(
                *preset.error());
        }
        presets.push_back(std::move(preset.value()));
    }
    return common::Result<std::vector<application::VerificationBuildPresetDescriptor>>::ok(
        std::move(presets));
}

common::Result<std::vector<application::VerificationBuildPresetDescriptor>>
buildPresetsFromJson(const std::string& json, std::size_t maxCount)
{
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded()) {
        return common::Result<std::vector<application::VerificationBuildPresetDescriptor>>::failure(
            error("verification_environment.corrupt_entry",
                "Corrupt verification environment build presets column"));
    }
    return buildPresetsFromJsonArray(parsed, maxCount);
}

nlohmann::json profileToJson(const application::VerificationProfile& profile)
{
    return nlohmann::json {
        {"profileId", profile.profileId},
        {"configurePresetName", profile.configurePresetName},
        {"configureDisplayName", profile.configureDisplayName},
        {"configureSource", application::toString(profile.configureSource)},
        {"declaredBinaryDir", profile.declaredBinaryDir},
        {"resolvedBinaryDir", profile.resolvedBinaryDir},
        {"origin", application::toString(profile.origin)},
        {"buildPresetsConfirmed", profile.buildPresetsConfirmed},
        {"buildPresets", buildPresetsToJson(profile.buildPresets)}
    };
}

common::Result<application::VerificationProfile> profileFromJson(const nlohmann::json& j)
{
    auto corrupt = [] {
        return common::Result<application::VerificationProfile>::failure(
            error("verification_environment.corrupt_entry", "Corrupt verification environment profile"));
    };
    if (!j.is_object()
        || !j.contains("profileId") || !j["profileId"].is_string() || j["profileId"].get<std::string>().empty()
        || !j.contains("configurePresetName") || !j["configurePresetName"].is_string()
        || j["configurePresetName"].get<std::string>().empty()
        || !j.contains("configureSource") || !j["configureSource"].is_string()
        || !j.contains("origin") || !j["origin"].is_string()
        || !j.contains("buildPresetsConfirmed") || !j["buildPresetsConfirmed"].is_boolean()
        || !j.contains("buildPresets") || !j["buildPresets"].is_array()
        || j["buildPresets"].size() > application::kVerificationEnvironmentMaxBuildPresetsPerProfile) {
        return corrupt();
    }
    const auto source = application::verificationPresetFileSourceFromString(
        j["configureSource"].get<std::string>());
    const auto origin = application::verificationProfileOriginFromString(j["origin"].get<std::string>());
    if (!source.has_value() || !origin.has_value()) return corrupt();
    application::VerificationProfile profile;
    profile.profileId = j["profileId"].get<std::string>();
    profile.configurePresetName = j["configurePresetName"].get<std::string>();
    if (j.contains("configureDisplayName") && j["configureDisplayName"].is_string())
        profile.configureDisplayName = j["configureDisplayName"].get<std::string>();
    profile.configureSource = *source;
    if (j.contains("declaredBinaryDir") && j["declaredBinaryDir"].is_string())
        profile.declaredBinaryDir = j["declaredBinaryDir"].get<std::string>();
    if (j.contains("resolvedBinaryDir") && j["resolvedBinaryDir"].is_string())
        profile.resolvedBinaryDir = j["resolvedBinaryDir"].get<std::string>();
    if (!profile.resolvedBinaryDir.empty()
        && !safeVerificationRelativePath(profile.resolvedBinaryDir)) {
        return corrupt();
    }
    profile.origin = *origin;
    profile.buildPresetsConfirmed = j["buildPresetsConfirmed"].get<bool>();
    auto buildPresets = buildPresetsFromJsonArray(
        j["buildPresets"], application::kVerificationEnvironmentMaxBuildPresetsPerProfile);
    if (!buildPresets.success()) {
        return common::Result<application::VerificationProfile>::failure(*buildPresets.error());
    }
    profile.buildPresets = std::move(buildPresets.value());
    return common::Result<application::VerificationProfile>::ok(std::move(profile));
}

std::string profilesToJson(const std::vector<application::VerificationProfile>& profiles)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& profile : profiles) array.push_back(profileToJson(profile));
    return array.dump();
}

common::Result<std::vector<application::VerificationProfile>> profilesFromJson(const std::string& json)
{
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()
        || parsed.size() > application::kVerificationEnvironmentMaxProfiles) {
        return common::Result<std::vector<application::VerificationProfile>>::failure(
            error("verification_environment.corrupt_entry",
                "Corrupt or out-of-bound verification environment profiles column"));
    }
    std::vector<application::VerificationProfile> profiles;
    profiles.reserve(parsed.size());
    for (const auto& item : parsed) {
        auto profile = profileFromJson(item);
        if (!profile.success()) {
            return common::Result<std::vector<application::VerificationProfile>>::failure(*profile.error());
        }
        profiles.push_back(std::move(profile.value()));
    }
    return common::Result<std::vector<application::VerificationProfile>>::ok(std::move(profiles));
}

} // namespace

SqliteVerificationEnvironmentRepository::SqliteVerificationEnvironmentRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteVerificationEnvironmentRepository::recordSnapshot(
    application::VerificationEnvironmentSnapshot snapshot)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO verification_environment_snapshots "
        "(run_id,project_id,schema_version,baseline_fingerprint,state,"
        "profiles_json,unlinked_build_presets_json,diagnostic,discovered_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, snapshot.runId),
        bindText(handle, statement.get(), 2, snapshot.projectId),
        bindText(handle, statement.get(), 3, snapshot.schemaVersion),
        bindText(handle, statement.get(), 4, snapshot.baselineFingerprint),
        bindText(handle, statement.get(), 5, application::toString(snapshot.state)),
        bindText(handle, statement.get(), 6, profilesToJson(snapshot.profiles)),
        bindText(handle, statement.get(), 7, buildPresetsToJson(snapshot.unlinkedBuildPresets).dump()),
        bindText(handle, statement.get(), 8, snapshot.diagnostic),
        bindInt64(handle, statement.get(), 9, toMilliseconds(snapshot.discoveredAt))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<void>::failure(error(
                "verification_environment.already_recorded",
                "A verification environment snapshot already exists for this Run"));
        }
        return common::Result<void>::failure(databaseError(
            handle, "verification_environment.write_failed",
            "Could not record verification environment snapshot"));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<application::VerificationEnvironmentSnapshot>>
SqliteVerificationEnvironmentRepository::findForRun(const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT project_id,schema_version,baseline_fingerprint,state,"
        "profiles_json,unlinked_build_presets_json,diagnostic,discovered_at_ms "
        "FROM verification_environment_snapshots WHERE run_id=?;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::optional<application::VerificationEnvironmentSnapshot>>::failure(
            *prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::optional<application::VerificationEnvironmentSnapshot>>::failure(
            *bound.error());
    }
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return common::Result<std::optional<application::VerificationEnvironmentSnapshot>>::ok(
            std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<application::VerificationEnvironmentSnapshot>>::failure(
            databaseError(handle, "verification_environment.read_failed",
                "Could not read verification environment snapshot"));
    }
    auto corrupt = [] {
        return common::Result<std::optional<application::VerificationEnvironmentSnapshot>>::failure(
            error("verification_environment.corrupt_entry", "Corrupt verification environment snapshot row"));
    };
    application::VerificationEnvironmentSnapshot snapshot;
    snapshot.runId = runId;
    snapshot.projectId = columnText(statement.get(), 0);
    snapshot.schemaVersion = columnText(statement.get(), 1);
    // Fail closed on identity: a snapshot from an unrecognized schema
    // version, or with a blank required identity field, must never be
    // silently accepted as if it were today's contract.
    if (snapshot.schemaVersion != application::kVerificationEnvironmentSnapshotSchemaVersion
        || snapshot.projectId.empty()) {
        return corrupt();
    }
    snapshot.baselineFingerprint = columnText(statement.get(), 2);
    const auto state = application::verificationEnvironmentStateFromString(columnText(statement.get(), 3));
    if (!state.has_value()) return corrupt();
    snapshot.state = *state;
    auto profiles = profilesFromJson(columnText(statement.get(), 4));
    if (!profiles.success()) {
        return common::Result<std::optional<application::VerificationEnvironmentSnapshot>>::failure(
            *profiles.error());
    }
    snapshot.profiles = std::move(profiles.value());
    auto unlinkedBuildPresets = buildPresetsFromJson(
        columnText(statement.get(), 5), application::kVerificationEnvironmentMaxUnlinkedBuildPresets);
    if (!unlinkedBuildPresets.success()) {
        return common::Result<std::optional<application::VerificationEnvironmentSnapshot>>::failure(
            *unlinkedBuildPresets.error());
    }
    snapshot.unlinkedBuildPresets = std::move(unlinkedBuildPresets.value());
    snapshot.diagnostic = columnText(statement.get(), 6);
    snapshot.discoveredAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 7));
    // Fail closed on state/content consistency: a stored row claiming
    // ExistingProfile with no profiles, or a non-ExistingProfile state that
    // somehow carries profiles, is corrupt data, not a value this reader
    // silently trusts.
    const bool hasProfiles = !snapshot.profiles.empty();
    const bool claimsExistingProfile =
        snapshot.state == application::VerificationEnvironmentState::ExistingProfile;
    if (claimsExistingProfile != hasProfiles) return corrupt();
    return common::Result<std::optional<application::VerificationEnvironmentSnapshot>>::ok(
        std::move(snapshot));
}

namespace {

std::string stringArrayToJson(const std::vector<std::string>& values)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& value : values) array.push_back(value);
    return array.dump();
}

std::string pathArrayToJson(const std::vector<std::filesystem::path>& values)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& value : values) array.push_back(pathToUtf8(value));
    return array.dump();
}

common::Result<std::vector<std::string>> stringArrayFromJson(
    const std::string& json, std::size_t maxItems, std::string_view corruptCode)
{
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array() || parsed.size() > maxItems) {
        return common::Result<std::vector<std::string>>::failure(
            error(std::string(corruptCode), "Corrupt or out-of-bound bootstrap string array column"));
    }
    std::vector<std::string> values;
    values.reserve(parsed.size());
    for (const auto& item : parsed) {
        if (!item.is_string()) {
            return common::Result<std::vector<std::string>>::failure(
                error(std::string(corruptCode), "Corrupt bootstrap string array column"));
        }
        values.push_back(item.get<std::string>());
    }
    return common::Result<std::vector<std::string>>::ok(std::move(values));
}

common::Result<std::vector<std::filesystem::path>> pathArrayFromJson(
    const std::string& json, std::size_t maxItems, std::string_view corruptCode)
{
    auto strings = stringArrayFromJson(json, maxItems, corruptCode);
    if (!strings.success()) {
        return common::Result<std::vector<std::filesystem::path>>::failure(*strings.error());
    }
    std::vector<std::filesystem::path> paths;
    paths.reserve(strings.value().size());
    for (auto& value : strings.value()) paths.push_back(pathFromUtf8(value));
    return common::Result<std::vector<std::filesystem::path>>::ok(std::move(paths));
}

std::string candidateIdentityToJson(
    const std::optional<application::VerificationBootstrapCandidateIdentity>& identity)
{
    if (!identity.has_value()) return {};
    nlohmann::json j = {
        {"overlayFingerprint", identity->overlayFingerprint},
        {"candidateProfileId", identity->candidateProfileId},
        {"candidateBuildProfileId", identity->candidateBuildProfileId},
        {"candidateTargetId", identity->candidateTargetId}
    };
    return j.dump();
}

common::Result<std::optional<application::VerificationBootstrapCandidateIdentity>>
candidateIdentityFromJson(const std::string& json)
{
    if (json.empty()) {
        return common::Result<std::optional<application::VerificationBootstrapCandidateIdentity>>::ok(
            std::nullopt);
    }
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()
        || !parsed.contains("overlayFingerprint") || !parsed["overlayFingerprint"].is_string()
        || !parsed.contains("candidateProfileId") || !parsed["candidateProfileId"].is_string()
        || !parsed.contains("candidateBuildProfileId") || !parsed["candidateBuildProfileId"].is_string()
        || !parsed.contains("candidateTargetId") || !parsed["candidateTargetId"].is_string()) {
        return common::Result<std::optional<application::VerificationBootstrapCandidateIdentity>>::failure(
            error("verification_environment.corrupt_bootstrap_record",
                "Corrupt bootstrap candidate identity column"));
    }
    application::VerificationBootstrapCandidateIdentity identity;
    identity.overlayFingerprint = parsed["overlayFingerprint"].get<std::string>();
    identity.candidateProfileId = parsed["candidateProfileId"].get<std::string>();
    identity.candidateBuildProfileId = parsed["candidateBuildProfileId"].get<std::string>();
    identity.candidateTargetId = parsed["candidateTargetId"].get<std::string>();
    return common::Result<std::optional<application::VerificationBootstrapCandidateIdentity>>::ok(
        std::move(identity));
}

// Forward-declared here (defined below, after findDecisionForRun's public
// wrapper) so recordDecision's own duplicate-conflict check - which already
// holds database_->impl_->mutex - can call it directly instead of the
// locking public findDecisionForRun, which would deadlock on this
// non-recursive mutex.
common::Result<std::optional<application::VerificationBootstrapDecision>>
readDecisionForRunUnlocked(sqlite3* handle, const application::RunId& runId);

// Forward-declared for the same reason as readDecisionForRunUnlocked above
// - recordRetryLink's own duplicate-conflict check already holds the mutex.
common::Result<std::optional<application::VerificationBootstrapRetryLink>>
readRetryLinkForPredecessorUnlocked(sqlite3* handle, const application::RunId& predecessorRunId);

} // namespace

SqliteVerificationBootstrapRepository::SqliteVerificationBootstrapRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteVerificationBootstrapRepository::recordDecision(
    application::VerificationBootstrapDecision decision)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO verification_bootstrap_decisions "
        "(run_id,project_id,schema_version,baseline_fingerprint,"
        "verification_snapshot_schema_version,disposition,reason_code,message,"
        "bootstrap_setup_paths_json,owning_task_ids_json,predecessor_run_id,decided_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, decision.runId),
        bindText(handle, statement.get(), 2, decision.projectId),
        bindText(handle, statement.get(), 3, decision.schemaVersion),
        bindText(handle, statement.get(), 4, decision.baselineFingerprint),
        bindText(handle, statement.get(), 5, decision.verificationSnapshotSchemaVersion),
        bindText(handle, statement.get(), 6, application::toString(decision.disposition)),
        bindText(handle, statement.get(), 7, decision.reasonCode),
        bindText(handle, statement.get(), 8, decision.message),
        bindText(handle, statement.get(), 9, pathArrayToJson(decision.bootstrapSetupPaths)),
        bindText(handle, statement.get(), 10, stringArrayToJson(decision.owningTaskIds)),
        bindNullableText(handle, statement.get(), 11, decision.predecessorRunId),
        bindInt64(handle, statement.get(), 12, toMilliseconds(decision.decidedAt))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            // Idempotent only when the already-recorded decision agrees;
            // a genuinely conflicting duplicate must fail closed instead of
            // silently keeping whichever row happened to be inserted first.
            const auto existing = readDecisionForRunUnlocked(handle, decision.runId);
            if (existing.success() && existing.value().has_value()) {
                const auto& recorded = *existing.value();
                if (recorded.disposition == decision.disposition
                    && recorded.reasonCode == decision.reasonCode
                    && recorded.baselineFingerprint == decision.baselineFingerprint
                    && recorded.bootstrapSetupPaths == decision.bootstrapSetupPaths) {
                    return common::Result<void>::ok();
                }
            }
            return common::Result<void>::failure(error("verification_environment.decision_conflict",
                "A different verification bootstrap decision already exists for this Run"));
        }
        return common::Result<void>::failure(databaseError(
            handle, "verification_environment.write_failed",
            "Could not record verification bootstrap decision"));
    }
    return common::Result<void>::ok();
}

namespace {

// Shared by findDecisionForRun (which takes the lock itself) and
// recordDecision's own duplicate-conflict check (which already holds the
// lock when it needs to read the just-conflicting row back) - calling the
// locking public method from inside an already-locked method would
// deadlock on this non-recursive mutex.
common::Result<std::optional<application::VerificationBootstrapDecision>>
readDecisionForRunUnlocked(sqlite3* handle, const application::RunId& runId)
{
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT project_id,schema_version,baseline_fingerprint,"
        "verification_snapshot_schema_version,disposition,reason_code,message,"
        "bootstrap_setup_paths_json,owning_task_ids_json,predecessor_run_id,decided_at_ms "
        "FROM verification_bootstrap_decisions WHERE run_id=?;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::optional<application::VerificationBootstrapDecision>>::failure(
            *prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::optional<application::VerificationBootstrapDecision>>::failure(
            *bound.error());
    }
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return common::Result<std::optional<application::VerificationBootstrapDecision>>::ok(std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<application::VerificationBootstrapDecision>>::failure(
            databaseError(handle, "verification_environment.read_failed",
                "Could not read verification bootstrap decision"));
    }
    auto corrupt = [] {
        return common::Result<std::optional<application::VerificationBootstrapDecision>>::failure(
            error("verification_environment.corrupt_bootstrap_record",
                "Corrupt verification bootstrap decision row"));
    };
    application::VerificationBootstrapDecision decision;
    decision.runId = runId;
    decision.projectId = columnText(statement.get(), 0);
    decision.schemaVersion = columnText(statement.get(), 1);
    if (decision.schemaVersion != application::kVerificationBootstrapDecisionSchemaVersion
        || decision.projectId.empty()) {
        return corrupt();
    }
    decision.baselineFingerprint = columnText(statement.get(), 2);
    decision.verificationSnapshotSchemaVersion = columnText(statement.get(), 3);
    const auto disposition = application::verificationBootstrapDispositionFromString(
        columnText(statement.get(), 4));
    if (!disposition.has_value()) return corrupt();
    decision.disposition = *disposition;
    decision.reasonCode = columnText(statement.get(), 5);
    decision.message = columnText(statement.get(), 6);
    auto setupPaths = pathArrayFromJson(columnText(statement.get(), 7),
        application::kVerificationBootstrapMaxSetupPaths, "verification_environment.corrupt_bootstrap_record");
    if (!setupPaths.success()) {
        return common::Result<std::optional<application::VerificationBootstrapDecision>>::failure(
            *setupPaths.error());
    }
    decision.bootstrapSetupPaths = std::move(setupPaths.value());
    auto owningTaskIds = stringArrayFromJson(columnText(statement.get(), 8),
        application::kVerificationBootstrapMaxOwningTasks, "verification_environment.corrupt_bootstrap_record");
    if (!owningTaskIds.success()) {
        return common::Result<std::optional<application::VerificationBootstrapDecision>>::failure(
            *owningTaskIds.error());
    }
    decision.owningTaskIds = std::move(owningTaskIds.value());
    if (sqlite3_column_type(statement.get(), 9) != SQLITE_NULL) {
        decision.predecessorRunId = columnText(statement.get(), 9);
    }
    decision.decidedAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 10));
    // A BootstrapPending/BootstrapVerified/BootstrapFailed decision without
    // any declared setup path is corrupt data - only ActionRequired
    // legitimately has none.
    if (decision.disposition != application::VerificationBootstrapDisposition::ActionRequired
        && decision.bootstrapSetupPaths.empty()) {
        return corrupt();
    }
    return common::Result<std::optional<application::VerificationBootstrapDecision>>::ok(
        std::move(decision));
}

} // namespace

common::Result<std::optional<application::VerificationBootstrapDecision>>
SqliteVerificationBootstrapRepository::findDecisionForRun(const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    return readDecisionForRunUnlocked(database_->impl_->handle, runId);
}

namespace {

bool candidateIdentitiesEqual(
    const std::optional<application::VerificationBootstrapCandidateIdentity>& a,
    const std::optional<application::VerificationBootstrapCandidateIdentity>& b)
{
    if (a.has_value() != b.has_value()) return false;
    if (!a.has_value()) return true;
    return a->overlayFingerprint == b->overlayFingerprint
        && a->candidateProfileId == b->candidateProfileId
        && a->candidateBuildProfileId == b->candidateBuildProfileId
        && a->candidateTargetId == b->candidateTargetId;
}

// See InMemoryVerificationBootstrapRepository's identical helper doc
// comment: the target id is legitimately empty for a cmake_build-only
// bootstrap, so completeness never requires it.
bool candidateIdentityIsComplete(const application::VerificationBootstrapCandidateIdentity& identity)
{
    return !identity.overlayFingerprint.empty() && !identity.candidateProfileId.empty()
        && !identity.candidateBuildProfileId.empty();
}

} // namespace

common::Result<void> SqliteVerificationBootstrapRepository::appendTransition(
    application::VerificationBootstrapTransition transition)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    // AG-18.3 hardening (blocker 1): the owning decision must exist and be
    // BootstrapPending, and this Run's bootstrap lifecycle must not already
    // have a resolving transition - the same fail-closed semantics as the
    // InMemory implementation, expressed against SQLite directly.
    const auto owningDecision = readDecisionForRunUnlocked(handle, transition.runId);
    if (!owningDecision.success()) return common::Result<void>::failure(*owningDecision.error());
    if (!owningDecision.value().has_value()
        || owningDecision.value()->disposition
               != application::VerificationBootstrapDisposition::BootstrapPending) {
        return common::Result<void>::failure({"verification_environment.decision_conflict",
            "A verification bootstrap transition requires an owning BootstrapPending decision"});
    }
    if (transition.toDisposition != application::VerificationBootstrapDisposition::BootstrapVerified
        && transition.toDisposition != application::VerificationBootstrapDisposition::BootstrapFailed) {
        return common::Result<void>::failure({"verification_environment.decision_conflict",
            "Only BootstrapVerified or BootstrapFailed may be appended as a transition"});
    }
    if (transition.toDisposition == application::VerificationBootstrapDisposition::BootstrapVerified
        && (!transition.candidateIdentity.has_value()
            || !candidateIdentityIsComplete(*transition.candidateIdentity))) {
        return common::Result<void>::failure({"verification_environment.decision_conflict",
            "BootstrapVerified requires a complete candidate identity"});
    }
    if (transition.toDisposition == application::VerificationBootstrapDisposition::BootstrapFailed
        && transition.candidateIdentity.has_value()) {
        return common::Result<void>::failure({"verification_environment.decision_conflict",
            "BootstrapFailed must not carry a candidate identity"});
    }

    {
        Statement existingStatement;
        const auto preparedExisting = prepare(handle,
            "SELECT schema_version,to_disposition,reason_code,candidate_identity_json "
            "FROM verification_bootstrap_transitions WHERE id=?;",
            existingStatement);
        if (!preparedExisting.success()) {
            return common::Result<void>::failure(*preparedExisting.error());
        }
        const auto boundId = bindText(handle, existingStatement.get(), 1, transition.id);
        if (!boundId.success()) return common::Result<void>::failure(*boundId.error());
        const int existingStep = sqlite3_step(existingStatement.get());
        if (existingStep == SQLITE_ROW) {
            const bool schemaMatches = columnText(existingStatement.get(), 0) == transition.schemaVersion;
            const auto storedDisposition = application::verificationBootstrapDispositionFromString(
                columnText(existingStatement.get(), 1));
            const bool dispositionMatches = storedDisposition.has_value()
                && *storedDisposition == transition.toDisposition;
            const bool reasonMatches = columnText(existingStatement.get(), 2) == transition.reasonCode;
            const bool hasStoredCandidateJson =
                sqlite3_column_type(existingStatement.get(), 3) != SQLITE_NULL;
            auto storedCandidate = candidateIdentityFromJson(
                hasStoredCandidateJson ? columnText(existingStatement.get(), 3) : std::string {});
            const bool candidateMatches = storedCandidate.success()
                && candidateIdentitiesEqual(storedCandidate.value(), transition.candidateIdentity);
            if (schemaMatches && dispositionMatches && reasonMatches && candidateMatches) {
                return common::Result<void>::ok();
            }
            return common::Result<void>::failure({"verification_environment.decision_conflict",
                "A different verification bootstrap transition already exists with this identity"});
        }
        if (existingStep != SQLITE_DONE) {
            return common::Result<void>::failure(databaseError(handle,
                "verification_environment.read_failed",
                "Could not check for an existing verification bootstrap transition"));
        }
    }

    {
        Statement resolvedStatement;
        const auto preparedResolved = prepare(handle,
            "SELECT 1 FROM verification_bootstrap_transitions "
            "WHERE run_id=? AND to_disposition IN ('bootstrap_verified','bootstrap_failed') LIMIT 1;",
            resolvedStatement);
        if (!preparedResolved.success()) {
            return common::Result<void>::failure(*preparedResolved.error());
        }
        const auto boundRun = bindText(handle, resolvedStatement.get(), 1, transition.runId);
        if (!boundRun.success()) return common::Result<void>::failure(*boundRun.error());
        const int resolvedStep = sqlite3_step(resolvedStatement.get());
        if (resolvedStep == SQLITE_ROW) {
            return common::Result<void>::failure({"verification_environment.decision_conflict",
                "This Run's bootstrap lifecycle already has a resolving transition"});
        }
        if (resolvedStep != SQLITE_DONE) {
            return common::Result<void>::failure(databaseError(handle,
                "verification_environment.read_failed",
                "Could not check for an existing resolving verification bootstrap transition"));
        }
    }

    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO verification_bootstrap_transitions "
        "(id,run_id,schema_version,to_disposition,reason_code,message,"
        "candidate_identity_json,transitioned_at_ms) VALUES (?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    const std::string candidateJson = candidateIdentityToJson(transition.candidateIdentity);
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, transition.id),
        bindText(handle, statement.get(), 2, transition.runId),
        bindText(handle, statement.get(), 3, transition.schemaVersion),
        bindText(handle, statement.get(), 4, application::toString(transition.toDisposition)),
        bindText(handle, statement.get(), 5, transition.reasonCode),
        bindText(handle, statement.get(), 6, transition.message),
        bindNullableText(handle, statement.get(), 7,
            candidateJson.empty() ? std::optional<std::string>{} : std::optional<std::string>{candidateJson}),
        bindInt64(handle, statement.get(), 8, toMilliseconds(transition.transitionedAt))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(databaseError(
            handle, "verification_environment.write_failed",
            "Could not append verification bootstrap transition"));
    }
    return common::Result<void>::ok();
}

common::Result<std::vector<application::VerificationBootstrapTransition>>
SqliteVerificationBootstrapRepository::listTransitionsForRun(const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT id,schema_version,to_disposition,reason_code,message,"
        "candidate_identity_json,transitioned_at_ms "
        "FROM verification_bootstrap_transitions WHERE run_id=? ORDER BY transitioned_at_ms ASC;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::vector<application::VerificationBootstrapTransition>>::failure(
            *prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::vector<application::VerificationBootstrapTransition>>::failure(
            *bound.error());
    }
    std::vector<application::VerificationBootstrapTransition> transitions;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<application::VerificationBootstrapTransition>>::failure(
                databaseError(handle, "verification_environment.read_failed",
                    "Could not read verification bootstrap transitions"));
        }
        application::VerificationBootstrapTransition transition;
        transition.id = columnText(statement.get(), 0);
        transition.runId = runId;
        transition.schemaVersion = columnText(statement.get(), 1);
        if (transition.schemaVersion != application::kVerificationBootstrapTransitionSchemaVersion) {
            return common::Result<std::vector<application::VerificationBootstrapTransition>>::failure(
                error("verification_environment.corrupt_bootstrap_record",
                    "Corrupt verification bootstrap transition row"));
        }
        const auto disposition = application::verificationBootstrapDispositionFromString(
            columnText(statement.get(), 2));
        if (!disposition.has_value()) {
            return common::Result<std::vector<application::VerificationBootstrapTransition>>::failure(
                error("verification_environment.corrupt_bootstrap_record",
                    "Corrupt verification bootstrap transition disposition"));
        }
        transition.toDisposition = *disposition;
        transition.reasonCode = columnText(statement.get(), 3);
        transition.message = columnText(statement.get(), 4);
        const bool hasCandidateJson = sqlite3_column_type(statement.get(), 5) != SQLITE_NULL;
        auto candidate = candidateIdentityFromJson(
            hasCandidateJson ? columnText(statement.get(), 5) : std::string {});
        if (!candidate.success()) {
            return common::Result<std::vector<application::VerificationBootstrapTransition>>::failure(
                *candidate.error());
        }
        transition.candidateIdentity = std::move(candidate.value());
        transition.transitionedAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 6));
        transitions.push_back(std::move(transition));
    }
    return common::Result<std::vector<application::VerificationBootstrapTransition>>::ok(
        std::move(transitions));
}

common::Result<void> SqliteVerificationBootstrapRepository::recordRetryLink(
    application::VerificationBootstrapRetryLink link)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO verification_bootstrap_retry_links "
        "(predecessor_run_id,new_run_id,project_id,schema_version,reason,created_at_ms) "
        "VALUES (?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, link.predecessorRunId),
        bindText(handle, statement.get(), 2, link.newRunId),
        bindText(handle, statement.get(), 3, link.projectId),
        bindText(handle, statement.get(), 4, link.schemaVersion),
        bindText(handle, statement.get(), 5, link.reason),
        bindInt64(handle, statement.get(), 6, toMilliseconds(link.createdAt))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            // AG-18.3 hardening (blocker 4): the primary key already
            // existing must never itself mean success - re-read the stored
            // row and only treat a byte-identical repeat as idempotent; a
            // conflicting one (a different newRunId/projectId for the same
            // predecessor) fails closed.
            const auto existing = readRetryLinkForPredecessorUnlocked(handle, link.predecessorRunId);
            if (existing.success() && existing.value().has_value()
                && existing.value()->newRunId == link.newRunId
                && existing.value()->projectId == link.projectId) {
                return common::Result<void>::ok();
            }
            return common::Result<void>::failure({"verification_environment.decision_conflict",
                "A different verification bootstrap retry link already exists for this "
                "predecessor"});
        }
        return common::Result<void>::failure(databaseError(
            handle, "verification_environment.write_failed",
            "Could not record verification bootstrap retry link"));
    }
    return common::Result<void>::ok();
}

namespace {

// Mirrors readDecisionForRunUnlocked's own rationale: recordRetryLink's
// duplicate-conflict check already holds database_->impl_->mutex when it
// needs to compare against the just-conflicting row, so it must call this
// instead of the locking public findRetryLinkForPredecessor.
common::Result<std::optional<application::VerificationBootstrapRetryLink>>
readRetryLinkForPredecessorUnlocked(sqlite3* handle, const application::RunId& predecessorRunId)
{
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT new_run_id,project_id,schema_version,reason,created_at_ms "
        "FROM verification_bootstrap_retry_links WHERE predecessor_run_id=?;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::optional<application::VerificationBootstrapRetryLink>>::failure(
            *prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, predecessorRunId);
    if (!bound.success()) {
        return common::Result<std::optional<application::VerificationBootstrapRetryLink>>::failure(
            *bound.error());
    }
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return common::Result<std::optional<application::VerificationBootstrapRetryLink>>::ok(std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<application::VerificationBootstrapRetryLink>>::failure(
            databaseError(handle, "verification_environment.read_failed",
                "Could not read verification bootstrap retry link"));
    }
    application::VerificationBootstrapRetryLink link;
    link.predecessorRunId = predecessorRunId;
    link.newRunId = columnText(statement.get(), 0);
    link.projectId = columnText(statement.get(), 1);
    link.schemaVersion = columnText(statement.get(), 2);
    if (link.schemaVersion != application::kVerificationBootstrapRetryLinkSchemaVersion
        || link.newRunId.empty()) {
        return common::Result<std::optional<application::VerificationBootstrapRetryLink>>::failure(
            error("verification_environment.corrupt_bootstrap_record",
                "Corrupt verification bootstrap retry link row"));
    }
    link.reason = columnText(statement.get(), 3);
    link.createdAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 4));
    return common::Result<std::optional<application::VerificationBootstrapRetryLink>>::ok(std::move(link));
}

} // namespace

common::Result<std::optional<application::VerificationBootstrapRetryLink>>
SqliteVerificationBootstrapRepository::findRetryLinkForPredecessor(
    const application::RunId& predecessorRunId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    return readRetryLinkForPredecessorUnlocked(database_->impl_->handle, predecessorRunId);
}

SqliteManifestTaskExecutionRepository::SqliteManifestTaskExecutionRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteManifestTaskExecutionRepository::upsert(
    const application::RunId& runId, const std::string& taskId,
    const application::ManifestTaskState& state)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT OR REPLACE INTO manifest_task_executions "
        "(run_id,task_id,status,error_code,error_message,change_set_id,attempts,"
        "output_summary,inspected_relative_paths,updated_at_ms) VALUES (?,?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    // AG-15: same JSON-array-of-strings shape as
    // manifest_task_no_changes.inspected_relative_paths (v32).
    nlohmann::json inspectedArray = nlohmann::json::array();
    for (const auto& path : state.inspectedRelativePaths) inspectedArray.push_back(path);

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, runId),
        bindText(handle, statement.get(), 2, taskId),
        bindText(handle, statement.get(), 3, application::toString(state.status)),
        bindText(handle, statement.get(), 4, state.errorCode),
        bindText(handle, statement.get(), 5, state.errorMessage),
        bindNullableText(handle, statement.get(), 6, state.changeSetId),
        bindInt64(handle, statement.get(), 7, static_cast<std::int64_t>(state.attempts)),
        bindText(handle, statement.get(), 8, state.outputSummary),
        bindText(handle, statement.get(), 9, inspectedArray.dump()),
        bindInt64(handle, statement.get(), 10, toMilliseconds(std::chrono::system_clock::now()))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(databaseError(
            handle, "manifest_task.write_failed", "Could not persist manifest task execution state"));
    }
    return common::Result<void>::ok();
}

common::Result<std::vector<application::ManifestTaskExecutionRecord>>
SqliteManifestTaskExecutionRepository::listForRun(const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT task_id,status,error_code,error_message,change_set_id,attempts,"
        "output_summary,inspected_relative_paths,updated_at_ms FROM manifest_task_executions "
        "WHERE run_id=? ORDER BY task_id;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::vector<application::ManifestTaskExecutionRecord>>::failure(
            *prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::vector<application::ManifestTaskExecutionRecord>>::failure(
            *bound.error());
    }

    std::vector<application::ManifestTaskExecutionRecord> results;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<application::ManifestTaskExecutionRecord>>::failure(
                databaseError(handle, "manifest_task.read_failed",
                    "Could not read manifest task execution state"));
        }
        application::ManifestTaskExecutionRecord record;
        record.runId = runId;
        record.taskId = columnText(statement.get(), 0);
        record.state.status = application::manifestTaskStatusFromString(columnText(statement.get(), 1));
        record.state.errorCode = columnText(statement.get(), 2);
        record.state.errorMessage = columnText(statement.get(), 3);
        record.state.changeSetId = optionalText(statement.get(), 4);
        record.state.attempts = static_cast<std::size_t>(sqlite3_column_int64(statement.get(), 5));
        record.state.outputSummary = columnText(statement.get(), 6);
        {
            const auto parsed = nlohmann::json::parse(
                columnText(statement.get(), 7), nullptr, false);
            if (parsed.is_discarded() || !parsed.is_array()) {
                return common::Result<std::vector<application::ManifestTaskExecutionRecord>>::failure(
                    error("manifest_task.corrupt_inspected_paths",
                        "Stored inspected-path evidence is corrupt"));
            }
            for (const auto& item : parsed) {
                if (!item.is_string()) {
                    return common::Result<std::vector<application::ManifestTaskExecutionRecord>>::failure(
                        error("manifest_task.corrupt_inspected_paths",
                            "Stored inspected-path evidence contains an invalid item"));
                }
                record.state.inspectedRelativePaths.push_back(item.get<std::string>());
            }
        }
        record.updatedAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 8));
        results.push_back(std::move(record));
    }
    return common::Result<std::vector<application::ManifestTaskExecutionRecord>>::ok(std::move(results));
}

namespace {

std::string serializeNoChangesCriterionEvidence(
    const std::vector<application::ManifestWorkerCriterionEvidence>& evidence)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& item : evidence) {
        array.push_back(nlohmann::json {
            {"requirementId", item.requirementId},
            {"inspectedPath", item.inspectedPath},
            {"observation", item.observation}});
    }
    return array.dump();
}

std::vector<application::ManifestWorkerCriterionEvidence> deserializeNoChangesCriterionEvidence(
    const std::string& json)
{
    std::vector<application::ManifestWorkerCriterionEvidence> result;
    if (json.empty()) return result;
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) return result;
    for (const auto& item : parsed) {
        if (!item.is_object()) continue;
        application::ManifestWorkerCriterionEvidence evidence;
        if (item.contains("requirementId") && item["requirementId"].is_string())
            evidence.requirementId = item["requirementId"].get<std::string>();
        if (item.contains("inspectedPath") && item["inspectedPath"].is_string())
            evidence.inspectedPath = item["inspectedPath"].get<std::string>();
        if (item.contains("observation") && item["observation"].is_string())
            evidence.observation = item["observation"].get<std::string>();
        result.push_back(std::move(evidence));
    }
    return result;
}

std::string serializeNoChangesInspectedPaths(const std::vector<std::string>& paths)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& path : paths) array.push_back(path);
    return array.dump();
}

std::vector<std::string> deserializeNoChangesInspectedPaths(const std::string& json)
{
    std::vector<std::string> result;
    if (json.empty()) return result;
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) return result;
    for (const auto& item : parsed) {
        if (item.is_string()) result.push_back(item.get<std::string>());
    }
    return result;
}

} // namespace

common::Result<void> SqliteManifestTaskExecutionRepository::upsertNoChanges(
    const application::RunId& runId, const std::string& taskId,
    const application::ManifestWorkerNoChanges& evidence)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT OR REPLACE INTO manifest_task_no_changes "
        "(run_id,task_id,schema_version,reason,summary,criterion_evidence,"
        "inspected_relative_paths,updated_at_ms) VALUES (?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, runId),
        bindText(handle, statement.get(), 2, taskId),
        bindText(handle, statement.get(), 3, evidence.schemaVersion),
        bindText(handle, statement.get(), 4, application::toString(evidence.reason)),
        bindText(handle, statement.get(), 5, evidence.summary),
        bindText(handle, statement.get(), 6, serializeNoChangesCriterionEvidence(evidence.criterionEvidence)),
        bindText(handle, statement.get(), 7, serializeNoChangesInspectedPaths(evidence.inspectedRelativePaths)),
        bindInt64(handle, statement.get(), 8, toMilliseconds(std::chrono::system_clock::now()))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(databaseError(
            handle, "manifest_task.write_failed", "Could not persist manifest task no-op evidence"));
    }
    return common::Result<void>::ok();
}

common::Result<std::vector<application::ManifestTaskNoChangesRecord>>
SqliteManifestTaskExecutionRepository::listNoChangesForRun(const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT task_id,schema_version,reason,summary,criterion_evidence,"
        "inspected_relative_paths,updated_at_ms FROM manifest_task_no_changes WHERE run_id=? "
        "ORDER BY task_id;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::vector<application::ManifestTaskNoChangesRecord>>::failure(
            *prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::vector<application::ManifestTaskNoChangesRecord>>::failure(
            *bound.error());
    }

    std::vector<application::ManifestTaskNoChangesRecord> results;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<application::ManifestTaskNoChangesRecord>>::failure(
                databaseError(handle, "manifest_task.read_failed",
                    "Could not read manifest task no-op evidence"));
        }
        application::ManifestTaskNoChangesRecord record;
        record.runId = runId;
        record.taskId = columnText(statement.get(), 0);
        record.evidence.schemaVersion = columnText(statement.get(), 1);
        const auto reason = application::manifestWorkerNoChangesReasonFromString(
            columnText(statement.get(), 2));
        if (!reason.has_value()) {
            return common::Result<std::vector<application::ManifestTaskNoChangesRecord>>::failure(
                error("manifest_task.corrupt_no_changes", "Unknown no-op reason in stored evidence"));
        }
        record.evidence.reason = *reason;
        record.evidence.summary = columnText(statement.get(), 3);
        record.evidence.criterionEvidence =
            deserializeNoChangesCriterionEvidence(columnText(statement.get(), 4));
        record.evidence.inspectedRelativePaths =
            deserializeNoChangesInspectedPaths(columnText(statement.get(), 5));
        record.updatedAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 6));
        results.push_back(std::move(record));
    }
    return common::Result<std::vector<application::ManifestTaskNoChangesRecord>>::ok(std::move(results));
}

namespace {

std::string serializeTaskDispositions(
    const std::vector<application::ManifestWorkLedgerTaskDisposition>& dispositions)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& item : dispositions) {
        nlohmann::json entry {
            {"taskId", item.taskId},
            {"status", item.status}};
        entry["changeSetId"] = item.changeSetId.has_value()
            ? nlohmann::json(*item.changeSetId) : nlohmann::json(nullptr);
        entry["noChangesReason"] = item.noChangesReason.has_value()
            ? nlohmann::json(*item.noChangesReason) : nlohmann::json(nullptr);
        array.push_back(std::move(entry));
    }
    return array.dump();
}

// AG-14.1: a malformed/corrupt column now fails the whole row read
// (manifest_work_ledger.corrupt_entry) instead of silently substituting an
// empty vector, which was indistinguishable from "this Run genuinely has
// none." An individual array *item* that is itself malformed is still
// defensively skipped rather than failing the whole column - only total
// parse failure or the wrong root JSON type is treated as corruption.
common::Result<std::vector<application::ManifestWorkLedgerTaskDisposition>>
deserializeTaskDispositions(const std::string& json)
{
    std::vector<application::ManifestWorkLedgerTaskDisposition> result;
    if (json.empty()) {
        return common::Result<std::vector<application::ManifestWorkLedgerTaskDisposition>>::ok(result);
    }
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        return common::Result<std::vector<application::ManifestWorkLedgerTaskDisposition>>::failure(
            error("manifest_work_ledger.corrupt_entry", "Corrupt task_dispositions column"));
    }
    for (const auto& item : parsed) {
        if (!item.is_object()) continue;
        application::ManifestWorkLedgerTaskDisposition disposition;
        if (item.contains("taskId") && item["taskId"].is_string())
            disposition.taskId = item["taskId"].get<std::string>();
        if (item.contains("status") && item["status"].is_string())
            disposition.status = item["status"].get<std::string>();
        if (item.contains("changeSetId") && item["changeSetId"].is_string())
            disposition.changeSetId = item["changeSetId"].get<std::string>();
        if (item.contains("noChangesReason") && item["noChangesReason"].is_string())
            disposition.noChangesReason = item["noChangesReason"].get<std::string>();
        result.push_back(std::move(disposition));
    }
    return common::Result<std::vector<application::ManifestWorkLedgerTaskDisposition>>::ok(
        std::move(result));
}

std::string serializeStringArray(const std::vector<std::string>& values)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& value : values) array.push_back(value);
    return array.dump();
}

common::Result<std::vector<std::string>> deserializeStringArray(
    const std::string& json, const char* columnName)
{
    std::vector<std::string> result;
    if (json.empty()) return common::Result<std::vector<std::string>>::ok(result);
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        return common::Result<std::vector<std::string>>::failure(
            error("manifest_work_ledger.corrupt_entry",
                std::string("Corrupt ") + columnName + " column"));
    }
    for (const auto& item : parsed) {
        if (item.is_string()) result.push_back(item.get<std::string>());
    }
    return common::Result<std::vector<std::string>>::ok(std::move(result));
}

std::string serializeProposalLineage(
    const std::vector<application::ManifestWorkLedgerProposalLineage>& lineage)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& item : lineage) {
        nlohmann::json entry {
            {"changeSetId", item.changeSetId},
            {"state", item.state}};
        entry["supersedesChangeSetId"] = item.supersedesChangeSetId.has_value()
            ? nlohmann::json(*item.supersedesChangeSetId) : nlohmann::json(nullptr);
        array.push_back(std::move(entry));
    }
    return array.dump();
}

common::Result<std::vector<application::ManifestWorkLedgerProposalLineage>>
deserializeProposalLineage(const std::string& json)
{
    std::vector<application::ManifestWorkLedgerProposalLineage> result;
    if (json.empty()) {
        return common::Result<std::vector<application::ManifestWorkLedgerProposalLineage>>::ok(result);
    }
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        return common::Result<std::vector<application::ManifestWorkLedgerProposalLineage>>::failure(
            error("manifest_work_ledger.corrupt_entry", "Corrupt proposal_lineage column"));
    }
    for (const auto& item : parsed) {
        if (!item.is_object()) continue;
        application::ManifestWorkLedgerProposalLineage lineage;
        if (item.contains("changeSetId") && item["changeSetId"].is_string())
            lineage.changeSetId = item["changeSetId"].get<std::string>();
        if (item.contains("state") && item["state"].is_string())
            lineage.state = item["state"].get<std::string>();
        if (item.contains("supersedesChangeSetId") && item["supersedesChangeSetId"].is_string())
            lineage.supersedesChangeSetId = item["supersedesChangeSetId"].get<std::string>();
        result.push_back(std::move(lineage));
    }
    return common::Result<std::vector<application::ManifestWorkLedgerProposalLineage>>::ok(
        std::move(result));
}

// AG-14.1: thin wrappers around the shared ExecutionAcceptanceContract
// serialize/parse implementation - one field-by-field JSON shape, reused by
// both the manifest-verification-identity.v1.json artifact and this
// column, never re-derived twice. serializeVerificationIdentity/
// deserializeVerificationIdentity only add this column's own "empty means
// never ran" convention on top.
std::string serializeVerificationIdentity(const application::ManifestProposalSetVerification& v)
{
    return application::renderManifestProposalSetVerificationJson(v);
}

// AG-14.1: an empty column means "combined verification never ran for this
// Run" (a legitimate, common case - Planner no-op, early planning failure)
// and returns std::nullopt success. A non-empty but malformed column fails
// the whole row read, exactly like the other JSON columns above - it must
// never be silently reinterpreted as "never ran."
common::Result<std::optional<application::ManifestProposalSetVerification>>
deserializeVerificationIdentity(const std::string& json)
{
    if (json.empty()) {
        return common::Result<std::optional<application::ManifestProposalSetVerification>>::ok(std::nullopt);
    }
    auto parsed = application::parseManifestProposalSetVerificationJson(json);
    if (!parsed.success()) {
        return common::Result<std::optional<application::ManifestProposalSetVerification>>::failure(
            error("manifest_work_ledger.corrupt_entry", "Corrupt verification_identity_json column"));
    }
    return common::Result<std::optional<application::ManifestProposalSetVerification>>::ok(
        std::move(parsed.value()));
}

std::string serializeWorkspaceLedgerSnapshot(
    const application::ManifestWorkLedgerWorkspaceSnapshot& snapshot)
{
    nlohmann::json root;
    root["schemaVersion"] = snapshot.schemaVersion;
    root["entries"] = nlohmann::json::array();
    for (const auto& entry : snapshot.entries) {
        root["entries"].push_back({
            {"relativePath", entry.relativePath},
            {"role", application::toString(entry.role)},
            {"reason", entry.reason},
            {"taskId", entry.taskId},
            {"baselineHash", entry.baselineHash.has_value()
                ? nlohmann::json(*entry.baselineHash) : nlohmann::json(nullptr)},
            {"origin", application::toString(entry.origin)}});
    }

    root["expansions"] = nlohmann::json::array();
    for (const auto& expansion : snapshot.expansions) {
        root["expansions"].push_back({
            {"id", expansion.id}, {"runId", expansion.runId},
            {"taskId", expansion.taskId}, {"relativePath", expansion.relativePath},
            {"role", application::toString(expansion.role)}, {"reason", expansion.reason},
            {"baselineHash", expansion.baselineHash.has_value()
                ? nlohmann::json(*expansion.baselineHash) : nlohmann::json(nullptr)},
            {"approvedAtMs", toMilliseconds(expansion.approvedAt)}});
    }
    root["dependencyEdges"] = nlohmann::json::array();
    for (const auto& edge : snapshot.dependencyEdges) {
        root["dependencyEdges"].push_back({
            {"dependentTaskId", edge.dependentTaskId},
            {"providerTaskId", edge.providerTaskId},
            {"relativePath", edge.relativePath}});
    }
    root["taskActivities"] = nlohmann::json::array();
    for (const auto& activity : snapshot.taskActivities) {
        root["taskActivities"].push_back({
            {"taskId", activity.taskId},
            {"inspectedRelativePaths", activity.inspectedRelativePaths},
            {"proposedCreatedPaths", activity.proposedCreatedPaths},
            {"proposedModifiedPaths", activity.proposedModifiedPaths}});
    }
    return root.dump();
}

common::Result<std::optional<application::ManifestWorkLedgerWorkspaceSnapshot>>
deserializeWorkspaceLedgerSnapshot(const std::string& json)
{
    using Snapshot = application::ManifestWorkLedgerWorkspaceSnapshot;
    if (json.empty())
        return common::Result<std::optional<Snapshot>>::ok(std::nullopt);
    const auto root = nlohmann::json::parse(json, nullptr, false);
    const auto corrupt = [] {
        return common::Result<std::optional<Snapshot>>::failure(error(
            "manifest_work_ledger.corrupt_entry",
            "Corrupt workspace_scope_snapshot_json column"));
    };
    if (root.is_discarded() || !root.is_object()
        || root.value("schemaVersion", std::string {})
            != "manifest-workspace-ledger-snapshot.v1"
        || !root.contains("entries") || !root["entries"].is_array()
        || !root.contains("expansions") || !root["expansions"].is_array()
        || !root.contains("dependencyEdges") || !root["dependencyEdges"].is_array()
        || !root.contains("taskActivities") || !root["taskActivities"].is_array())
        return corrupt();

    Snapshot snapshot;
    try {
        for (const auto& item : root["entries"]) {
            if (!item.is_object()) return corrupt();
            application::RunWorkspaceScopeEntry entry;
            entry.relativePath = item.at("relativePath").get<std::string>();
            entry.reason = item.at("reason").get<std::string>();
            entry.taskId = item.at("taskId").get<std::string>();
            const auto role = application::workspaceScopeRoleFromString(
                item.at("role").get<std::string>());
            const auto origin = application::workspaceScopeOriginFromString(
                item.at("origin").get<std::string>());
            if (!role.has_value() || !origin.has_value()) return corrupt();
            entry.role = *role;
            entry.origin = *origin;
            if (!item.at("baselineHash").is_null())
                entry.baselineHash = item.at("baselineHash").get<std::string>();
            snapshot.entries.push_back(std::move(entry));
        }
        for (const auto& item : root["expansions"]) {
            if (!item.is_object()) return corrupt();
            application::RunWorkspaceScopeExpansion expansion;
            expansion.id = item.at("id").get<std::string>();
            expansion.runId = item.at("runId").get<std::string>();
            expansion.taskId = item.at("taskId").get<std::string>();
            expansion.relativePath = item.at("relativePath").get<std::string>();
            expansion.reason = item.at("reason").get<std::string>();
            const auto role = application::workspaceScopeRoleFromString(
                item.at("role").get<std::string>());
            if (!role.has_value()) return corrupt();
            expansion.role = *role;
            if (!item.at("baselineHash").is_null())
                expansion.baselineHash = item.at("baselineHash").get<std::string>();
            expansion.approvedAt = fromMilliseconds(item.at("approvedAtMs").get<std::int64_t>());
            snapshot.expansions.push_back(std::move(expansion));
        }
        for (const auto& item : root["dependencyEdges"]) {
            if (!item.is_object()) return corrupt();
            snapshot.dependencyEdges.push_back({
                item.at("dependentTaskId").get<std::string>(),
                item.at("providerTaskId").get<std::string>(),
                item.at("relativePath").get<std::string>()});
        }
        for (const auto& item : root["taskActivities"]) {
            if (!item.is_object()) return corrupt();
            application::ManifestWorkLedgerTaskWorkspaceActivity activity;
            activity.taskId = item.at("taskId").get<std::string>();
            activity.inspectedRelativePaths =
                item.at("inspectedRelativePaths").get<std::vector<std::string>>();
            activity.proposedCreatedPaths =
                item.at("proposedCreatedPaths").get<std::vector<std::string>>();
            activity.proposedModifiedPaths =
                item.at("proposedModifiedPaths").get<std::vector<std::string>>();
            snapshot.taskActivities.push_back(std::move(activity));
        }
    } catch (const std::exception&) {
        return corrupt();
    }
    if (snapshot.entries.size() > application::kRunWorkspaceScopeMaxEntries
        || snapshot.dependencyEdges.size() > application::kRunWorkspaceScopeMaxDependencyEdges)
        return corrupt();
    return common::Result<std::optional<Snapshot>>::ok(std::move(snapshot));
}

} // namespace

SqliteManifestWorkLedgerRepository::SqliteManifestWorkLedgerRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteManifestWorkLedgerRepository::insertEntry(
    application::ManifestWorkLedgerEntry entry)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    // AG-14.1: plain INSERT, not INSERT OR REPLACE - an entry is immutable
    // once written (ManifestWorkLedgerService::recordTerminalRun's own
    // "insert once, never replace" contract). A second insert for the same
    // run_id fails with manifest_work_ledger.duplicate_entry below instead
    // of silently clobbering a possibly-more-complete earlier entry.
    const auto prepared = prepare(handle,
        "INSERT INTO manifest_work_ledger_entries "
        "(run_id,project_id,schema_version,terminal_status,terminal_reason,"
        "baseline_fingerprint,manifest_schema_version,disposition,task_dispositions,"
        "proposal_ids,proposal_lineage,affected_relative_paths,verification_identity_json,"
        "verification_summary,remaining_work_note,workspace_scope_schema_version,"
        "workspace_scope_summary,planning_input_fingerprint,planning_failure_signature,"
        "workspace_scope_snapshot_json,created_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    const std::optional<std::string> verificationJson = entry.verification.has_value()
        ? std::optional<std::string>(serializeVerificationIdentity(*entry.verification))
        : std::nullopt;
    const std::optional<std::string> workspaceSnapshotJson = entry.workspaceSnapshot.has_value()
        ? std::optional<std::string>(serializeWorkspaceLedgerSnapshot(*entry.workspaceSnapshot))
        : std::nullopt;

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, entry.runId),
        bindText(handle, statement.get(), 2, entry.projectId),
        bindText(handle, statement.get(), 3, entry.schemaVersion),
        bindText(handle, statement.get(), 4, application::toString(entry.terminalStatus)),
        bindText(handle, statement.get(), 5, entry.terminalReason),
        bindText(handle, statement.get(), 6, entry.baselineFingerprint),
        bindText(handle, statement.get(), 7, entry.manifestSchemaVersion),
        bindText(handle, statement.get(), 8, application::toString(entry.disposition)),
        bindText(handle, statement.get(), 9, serializeTaskDispositions(entry.taskDispositions)),
        bindText(handle, statement.get(), 10, serializeStringArray(entry.proposalIds)),
        bindText(handle, statement.get(), 11, serializeProposalLineage(entry.proposalLineage)),
        bindText(handle, statement.get(), 12, serializeStringArray(entry.affectedRelativePaths)),
        bindNullableText(handle, statement.get(), 13, verificationJson),
        bindText(handle, statement.get(), 14, entry.verificationSummary),
        bindText(handle, statement.get(), 15, entry.remainingWorkNote),
        bindText(handle, statement.get(), 16, entry.workspaceScopeSchemaVersion),
        bindText(handle, statement.get(), 17, entry.workspaceScopeSummary),
        bindText(handle, statement.get(), 18, entry.planningInputFingerprint),
        bindText(handle, statement.get(), 19, entry.planningFailureSignature),
        bindNullableText(handle, statement.get(), 20, workspaceSnapshotJson),
        bindInt64(handle, statement.get(), 21, toMilliseconds(entry.createdAt))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<void>::failure(error(
                "manifest_work_ledger.duplicate_entry",
                "A Work Ledger entry already exists for this Run"));
        }
        return common::Result<void>::failure(databaseError(
            handle, "manifest_work_ledger.write_failed", "Could not persist Work Ledger entry"));
    }
    return common::Result<void>::ok();
}

namespace {

constexpr const char* kManifestWorkLedgerEntryColumns =
    "run_id,project_id,schema_version,terminal_status,terminal_reason,"
    "baseline_fingerprint,manifest_schema_version,disposition,task_dispositions,"
    "proposal_ids,proposal_lineage,affected_relative_paths,verification_identity_json,"
    "verification_summary,remaining_work_note,workspace_scope_schema_version,"
    "workspace_scope_summary,planning_input_fingerprint,planning_failure_signature,"
    "workspace_scope_snapshot_json,created_at_ms";

common::Result<application::ManifestWorkLedgerEntry> readManifestWorkLedgerEntryRow(
    sqlite3* handle, sqlite3_stmt* statement)
{
    application::ManifestWorkLedgerEntry entry;
    entry.runId = columnText(statement, 0);
    entry.projectId = columnText(statement, 1);
    entry.schemaVersion = columnText(statement, 2);
    const auto status = runStatusFromString(columnText(statement, 3));
    if (!status.has_value()) {
        return common::Result<application::ManifestWorkLedgerEntry>::failure(
            error("manifest_work_ledger.corrupt_entry", "Unknown terminal status in stored entry"));
    }
    entry.terminalStatus = *status;
    entry.terminalReason = columnText(statement, 4);
    entry.baselineFingerprint = columnText(statement, 5);
    entry.manifestSchemaVersion = columnText(statement, 6);
    const auto disposition = application::manifestWorkLedgerDispositionFromString(
        columnText(statement, 7));
    if (!disposition.has_value()) {
        return common::Result<application::ManifestWorkLedgerEntry>::failure(
            error("manifest_work_ledger.corrupt_entry", "Unknown disposition in stored entry"));
    }
    entry.disposition = *disposition;

    auto taskDispositions = deserializeTaskDispositions(columnText(statement, 8));
    if (!taskDispositions.success())
        return common::Result<application::ManifestWorkLedgerEntry>::failure(*taskDispositions.error());
    entry.taskDispositions = std::move(taskDispositions.value());

    auto proposalIds = deserializeStringArray(columnText(statement, 9), "proposal_ids");
    if (!proposalIds.success())
        return common::Result<application::ManifestWorkLedgerEntry>::failure(*proposalIds.error());
    entry.proposalIds = std::move(proposalIds.value());

    auto proposalLineage = deserializeProposalLineage(columnText(statement, 10));
    if (!proposalLineage.success())
        return common::Result<application::ManifestWorkLedgerEntry>::failure(*proposalLineage.error());
    entry.proposalLineage = std::move(proposalLineage.value());

    auto affectedRelativePaths = deserializeStringArray(columnText(statement, 11), "affected_relative_paths");
    if (!affectedRelativePaths.success())
        return common::Result<application::ManifestWorkLedgerEntry>::failure(*affectedRelativePaths.error());
    entry.affectedRelativePaths = std::move(affectedRelativePaths.value());

    const auto verificationColumn = optionalText(statement, 12);
    auto verification = deserializeVerificationIdentity(verificationColumn.value_or(std::string {}));
    if (!verification.success())
        return common::Result<application::ManifestWorkLedgerEntry>::failure(*verification.error());
    entry.verification = std::move(verification.value());

    entry.verificationSummary = columnText(statement, 13);
    entry.remainingWorkNote = columnText(statement, 14);
    entry.workspaceScopeSchemaVersion = columnText(statement, 15);
    entry.workspaceScopeSummary = columnText(statement, 16);
    entry.planningInputFingerprint = columnText(statement, 17);
    entry.planningFailureSignature = columnText(statement, 18);
    const auto workspaceSnapshotColumn = optionalText(statement, 19);
    auto workspaceSnapshot = deserializeWorkspaceLedgerSnapshot(
        workspaceSnapshotColumn.value_or(std::string {}));
    if (!workspaceSnapshot.success())
        return common::Result<application::ManifestWorkLedgerEntry>::failure(
            *workspaceSnapshot.error());
    entry.workspaceSnapshot = std::move(workspaceSnapshot.value());
    entry.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 20));
    static_cast<void>(handle);
    return common::Result<application::ManifestWorkLedgerEntry>::ok(std::move(entry));
}

} // namespace

common::Result<std::optional<application::ManifestWorkLedgerEntry>>
SqliteManifestWorkLedgerRepository::findEntry(const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + kManifestWorkLedgerEntryColumns
        + " FROM manifest_work_ledger_entries WHERE run_id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) {
        return common::Result<std::optional<application::ManifestWorkLedgerEntry>>::failure(
            *prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::optional<application::ManifestWorkLedgerEntry>>::failure(
            *bound.error());
    }
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return common::Result<std::optional<application::ManifestWorkLedgerEntry>>::ok(std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<application::ManifestWorkLedgerEntry>>::failure(
            databaseError(handle, "manifest_work_ledger.read_failed", "Could not read Work Ledger entry"));
    }
    const auto entry = readManifestWorkLedgerEntryRow(handle, statement.get());
    if (!entry.success()) {
        return common::Result<std::optional<application::ManifestWorkLedgerEntry>>::failure(
            *entry.error());
    }
    return common::Result<std::optional<application::ManifestWorkLedgerEntry>>::ok(entry.value());
}

common::Result<std::vector<application::ManifestWorkLedgerEntry>>
SqliteManifestWorkLedgerRepository::listRecentForProject(
    const application::ProjectId& projectId, std::size_t limit) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    // run_id is a stable secondary sort key: two entries sharing the same
    // created_at_ms millisecond must still produce a deterministic order
    // (AG-14.1) rather than depending on SQLite's unspecified tie order.
    const std::string sql = std::string("SELECT ") + kManifestWorkLedgerEntryColumns
        + " FROM manifest_work_ledger_entries WHERE project_id=? "
          "ORDER BY created_at_ms DESC, run_id DESC LIMIT ?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) {
        return common::Result<std::vector<application::ManifestWorkLedgerEntry>>::failure(
            *prepared.error());
    }
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, projectId),
        bindInt64(handle, statement.get(), 2, static_cast<std::int64_t>(limit))
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) {
            return common::Result<std::vector<application::ManifestWorkLedgerEntry>>::failure(
                *binding.error());
        }
    }
    std::vector<application::ManifestWorkLedgerEntry> results;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<application::ManifestWorkLedgerEntry>>::failure(
                databaseError(handle, "manifest_work_ledger.read_failed",
                    "Could not list Work Ledger entries"));
        }
        const auto entry = readManifestWorkLedgerEntryRow(handle, statement.get());
        if (!entry.success()) {
            return common::Result<std::vector<application::ManifestWorkLedgerEntry>>::failure(
                *entry.error());
        }
        results.push_back(entry.value());
    }
    return common::Result<std::vector<application::ManifestWorkLedgerEntry>>::ok(std::move(results));
}

common::Result<void> SqliteManifestWorkLedgerRepository::appendTransition(
    application::ManifestWorkLedgerReviewTransition transition)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO manifest_work_ledger_transitions "
        "(id,run_id,change_set_id,kind,occurred_at_ms,actor,reason,"
        "post_action_workspace_fingerprint) VALUES (?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, transition.id),
        bindText(handle, statement.get(), 2, transition.runId),
        bindText(handle, statement.get(), 3, transition.changeSetId),
        bindText(handle, statement.get(), 4, application::toString(transition.kind)),
        bindInt64(handle, statement.get(), 5, toMilliseconds(transition.occurredAt)),
        bindNullableText(handle, statement.get(), 6, transition.actor),
        bindNullableText(handle, statement.get(), 7, transition.reason),
        bindNullableText(handle, statement.get(), 8, transition.postActionWorkspaceFingerprint)
    };
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            // AG-14.2: deterministic transition ids (changeSetId + kind, see
            // ManifestWorkLedgerService::recordReviewTransition) make this a
            // benign idempotent duplicate, never a real conflict.
            return common::Result<void>::failure(error(
                "manifest_work_ledger.duplicate_transition",
                "A Work Ledger transition already exists for this change set and kind"));
        }
        return common::Result<void>::failure(databaseError(
            handle, "manifest_work_ledger.write_failed", "Could not persist Work Ledger transition"));
    }
    return common::Result<void>::ok();
}

common::Result<std::vector<application::ManifestWorkLedgerReviewTransition>>
SqliteManifestWorkLedgerRepository::listTransitionsForRun(const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT id,change_set_id,kind,occurred_at_ms,actor,reason,"
        "post_action_workspace_fingerprint FROM manifest_work_ledger_transitions "
        "WHERE run_id=? ORDER BY occurred_at_ms,id;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::vector<application::ManifestWorkLedgerReviewTransition>>::failure(
            *prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::vector<application::ManifestWorkLedgerReviewTransition>>::failure(
            *bound.error());
    }
    std::vector<application::ManifestWorkLedgerReviewTransition> results;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<application::ManifestWorkLedgerReviewTransition>>::failure(
                databaseError(handle, "manifest_work_ledger.read_failed",
                    "Could not read Work Ledger transitions"));
        }
        application::ManifestWorkLedgerReviewTransition transition;
        transition.id = columnText(statement.get(), 0);
        transition.runId = runId;
        transition.changeSetId = columnText(statement.get(), 1);
        const auto kind = application::manifestWorkLedgerTransitionKindFromString(
            columnText(statement.get(), 2));
        if (!kind.has_value()) {
            return common::Result<std::vector<application::ManifestWorkLedgerReviewTransition>>::failure(
                error("manifest_work_ledger.corrupt_transition", "Unknown transition kind in stored row"));
        }
        transition.kind = *kind;
        transition.occurredAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 3));
        transition.actor = optionalText(statement.get(), 4);
        transition.reason = optionalText(statement.get(), 5);
        transition.postActionWorkspaceFingerprint = optionalText(statement.get(), 6);
        results.push_back(std::move(transition));
    }
    return common::Result<std::vector<application::ManifestWorkLedgerReviewTransition>>::ok(
        std::move(results));
}

SqliteArtifactRepository::SqliteArtifactRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<Artifact> SqliteArtifactRepository::insertArtifact(Artifact artifact)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO artifacts (id,run_id,stage_id,kind,name,media_type,relative_path,byte_size,"
        "sha256,created_at_ms) VALUES (?,?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<Artifact>::failure(*prepared.error());
    const auto bound = bindArtifact(handle, statement.get(), artifact);
    if (!bound.success()) return common::Result<Artifact>::failure(*bound.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<Artifact>::failure(
                error("artifact.duplicate_id", "Artifact id already exists"));
        }
        return common::Result<Artifact>::failure(
            databaseError(handle, "persistence.write_failed", "Could not insert artifact"));
    }
    return common::Result<Artifact>::ok(std::move(artifact));
}

common::Result<std::optional<Artifact>> SqliteArtifactRepository::findArtifact(
    const application::ArtifactId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + artifactColumns + " FROM artifacts WHERE id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<Artifact>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success()) return common::Result<std::optional<Artifact>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<Artifact>>::ok(std::nullopt);
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<Artifact>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read artifact"));
    }
    const auto artifact = readArtifact(statement.get());
    if (!artifact.success()) return common::Result<std::optional<Artifact>>::failure(*artifact.error());
    return common::Result<std::optional<Artifact>>::ok(artifact.value());
}

common::Result<std::vector<Artifact>> SqliteArtifactRepository::listArtifacts(
    const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    // Deterministic order matches the IArtifactRepository contract every
    // adapter (including InMemoryArtifactRepository) must honor.
    const std::string sql = std::string("SELECT ") + artifactColumns
        + " FROM artifacts WHERE run_id=? ORDER BY created_at_ms,id;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<Artifact>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) return common::Result<std::vector<Artifact>>::failure(*bound.error());

    std::vector<Artifact> artifacts;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<Artifact>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list artifacts"));
        }
        auto artifact = readArtifact(statement.get());
        if (!artifact.success()) return common::Result<std::vector<Artifact>>::failure(*artifact.error());
        artifacts.push_back(artifact.value());
    }
    return common::Result<std::vector<Artifact>>::ok(std::move(artifacts));
}

SqliteRunWarningRepository::SqliteRunWarningRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<bool> SqliteRunWarningRepository::recordWarning(application::RunWarning warning)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    // Deliberately a plain INSERT: OR IGNORE would also swallow a CHECK violation (an empty or oversized message) and a
    // missing Run, and the caller has to hear about those.
    const auto prepared = prepare(handle,
        "INSERT INTO run_warnings (id,run_id,stage_id,code,message,artifact_name,created_at_ms) VALUES (?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<bool>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, warning.id),
        bindText(handle, statement.get(), 2, warning.runId),
        bindNullableText(handle, statement.get(), 3, warning.stageId),
        bindText(handle, statement.get(), 4, warning.code),
        bindText(handle, statement.get(), 5, warning.message),
        bindText(handle, statement.get(), 6, warning.artifactName),
        bindInt64(handle, statement.get(), 7, toMilliseconds(warning.createdAt))};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<bool>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        // The identity index is the only UNIQUE constraint besides the id (a fresh UUID): the same failure again.
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_UNIQUE) {
            return common::Result<bool>::ok(false);
        }
        return common::Result<bool>::failure(
            databaseError(handle, "persistence.write_failed", "Could not record run warning"));
    }
    return common::Result<bool>::ok(true);
}

common::Result<std::vector<application::RunWarning>> SqliteRunWarningRepository::listWarnings(
    const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT id,run_id,stage_id,code,message,artifact_name,created_at_ms FROM run_warnings "
        "WHERE run_id=? ORDER BY created_at_ms,id;",
        statement);
    if (!prepared.success()) {
        return common::Result<std::vector<application::RunWarning>>::failure(*prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, runId);
    if (!bound.success()) return common::Result<std::vector<application::RunWarning>>::failure(*bound.error());

    std::vector<application::RunWarning> warnings;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<application::RunWarning>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list run warnings"));
        }
        application::RunWarning warning;
        warning.id = columnText(statement.get(), 0);
        warning.runId = columnText(statement.get(), 1);
        warning.stageId = optionalText(statement.get(), 2);
        warning.code = columnText(statement.get(), 3);
        warning.message = columnText(statement.get(), 4);
        warning.artifactName = columnText(statement.get(), 5);
        warning.createdAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 6));
        warnings.push_back(std::move(warning));
    }
    return common::Result<std::vector<application::RunWarning>>::ok(std::move(warnings));
}

SqliteVerificationScratchSettingRepository::SqliteVerificationScratchSettingRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<std::optional<std::filesystem::path>> SqliteVerificationScratchSettingRepository::load() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle, "SELECT root_path FROM verification_scratch_setting WHERE id = 1;", statement);
    if (!prepared.success()) {
        return common::Result<std::optional<std::filesystem::path>>::failure(*prepared.error());
    }
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<std::filesystem::path>>::ok(std::nullopt);
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<std::filesystem::path>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read the verification folder setting"));
    }
    const auto text = optionalText(statement.get(), 0);
    if (!text.has_value()) return common::Result<std::optional<std::filesystem::path>>::ok(std::nullopt);
    return common::Result<std::optional<std::filesystem::path>>::ok(pathFromUtf8(*text));
}

common::Result<void> SqliteVerificationScratchSettingRepository::store(
    const std::optional<std::filesystem::path>& root)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO verification_scratch_setting (id,root_path,updated_at_ms) VALUES (1,?,?) "
        "ON CONFLICT(id) DO UPDATE SET root_path = excluded.root_path, updated_at_ms = excluded.updated_at_ms;",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    std::optional<std::string> text;
    if (root.has_value()) text = pathToUtf8(*root);
    const common::Result<void> bindings[] = {
        bindNullableText(handle, statement.get(), 1, text),
        bindInt64(handle, statement.get(), 2, toMilliseconds(std::chrono::system_clock::now()))};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not store the verification folder setting"));
    }
    return common::Result<void>::ok();
}

SqliteProjectReadinessRepository::SqliteProjectReadinessRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<std::optional<application::ProjectReadinessRecord>> SqliteProjectReadinessRepository::find(
    const application::ProjectId& projectId) const
{
    using Result = common::Result<std::optional<application::ProjectReadinessRecord>>;
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "SELECT project_root,configure_preset,build_preset,binary_dir,state,inputs_fingerprint,toolchain_identity,targets_json,"
        "targets_complete,checked_at_ms,last_success_at_ms,failure_code,failure_message "
        "FROM project_verification_readiness WHERE project_id = ?;",
        statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, projectId);
    if (!bound.success()) return Result::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return Result::ok(std::nullopt);
    if (step != SQLITE_ROW) {
        return Result::failure(databaseError(handle, "persistence.read_failed", "Could not read the project readiness"));
    }
    application::ProjectReadinessRecord record;
    record.projectId = projectId;
    record.projectRoot = columnText(statement.get(), 0);
    record.configurePreset = columnText(statement.get(), 1);
    record.buildPreset = columnText(statement.get(), 2);
    record.binaryDir = columnText(statement.get(), 3);
    const auto state = application::projectReadinessStateFromString(columnText(statement.get(), 4));
    if (!state.has_value()) {
        return Result::failure(error("persistence.invalid_data", "The stored project readiness state is unknown"));
    }
    record.state = *state;
    record.inputsFingerprint = columnText(statement.get(), 5);
    record.toolchainIdentity = columnText(statement.get(), 6);
    const auto targets = nlohmann::json::parse(columnText(statement.get(), 7), nullptr, false);
    if (targets.is_array()) {
        for (const auto& target : targets) {
            if (target.is_string()) record.targets.push_back(target.get<std::string>());
        }
    }
    record.targetsComplete = sqlite3_column_int(statement.get(), 8) != 0;
    record.checkedAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 9));
    if (sqlite3_column_type(statement.get(), 10) != SQLITE_NULL) {
        record.lastSuccessAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 10));
    }
    record.failureCode = columnText(statement.get(), 11);
    record.failureMessage = columnText(statement.get(), 12);
    return Result::ok(std::move(record));
}

common::Result<void> SqliteProjectReadinessRepository::upsert(const application::ProjectReadinessRecord& record)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO project_verification_readiness (project_id,project_root,configure_preset,build_preset,binary_dir,state,"
        "inputs_fingerprint,toolchain_identity,targets_json,targets_complete,checked_at_ms,last_success_at_ms,failure_code,"
        "failure_message) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(project_id) DO UPDATE SET project_root = excluded.project_root, configure_preset = excluded.configure_preset, "
        "build_preset = excluded.build_preset, binary_dir = excluded.binary_dir, state = excluded.state, "
        "inputs_fingerprint = excluded.inputs_fingerprint, toolchain_identity = excluded.toolchain_identity, "
        "targets_json = excluded.targets_json, targets_complete = excluded.targets_complete, "
        "checked_at_ms = excluded.checked_at_ms, last_success_at_ms = excluded.last_success_at_ms, "
        "failure_code = excluded.failure_code, failure_message = excluded.failure_message;",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    nlohmann::json targets = nlohmann::json::array();
    for (const auto& target : record.targets) targets.push_back(target);
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, record.projectId),
        bindText(handle, statement.get(), 2, record.projectRoot),
        bindText(handle, statement.get(), 3, record.configurePreset),
        bindText(handle, statement.get(), 4, record.buildPreset),
        bindText(handle, statement.get(), 5, record.binaryDir),
        bindText(handle, statement.get(), 6, application::toString(record.state)),
        bindText(handle, statement.get(), 7, record.inputsFingerprint),
        bindText(handle, statement.get(), 8, record.toolchainIdentity),
        bindText(handle, statement.get(), 9, targets.dump()),
        bindInt64(handle, statement.get(), 10, record.targetsComplete ? 1 : 0),
        bindInt64(handle, statement.get(), 11, toMilliseconds(record.checkedAt)),
        record.lastSuccessAt.has_value() ? bindInt64(handle, statement.get(), 12, toMilliseconds(*record.lastSuccessAt))
                                         : bindNullableText(handle, statement.get(), 12, std::nullopt),
        bindText(handle, statement.get(), 13, record.failureCode),
        bindText(handle, statement.get(), 14, record.failureMessage)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        const int code = sqlite3_extended_errcode(handle);
        if (code == SQLITE_CONSTRAINT_FOREIGNKEY) {
            return common::Result<void>::failure(error("project.not_found", "The project does not exist"));
        }
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not store the project readiness"));
    }
    return common::Result<void>::ok();
}

SqliteWorkspaceStateRepository::SqliteWorkspaceStateRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<std::optional<application::ProjectId>> SqliteWorkspaceStateRepository::activeProjectId() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(
        handle, "SELECT active_project_id FROM workspace_state WHERE id = 1;", statement);
    if (!prepared.success()) {
        return common::Result<std::optional<application::ProjectId>>::failure(*prepared.error());
    }
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        // The singleton row is always inserted by the v3 migration itself;
        // a database that could reach this code without that row would
        // already have failed open() at an older/newer schema version.
        return common::Result<std::optional<application::ProjectId>>::ok(std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<application::ProjectId>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read workspace state"));
    }
    return common::Result<std::optional<application::ProjectId>>::ok(optionalText(statement.get(), 0));
}

common::Result<void> SqliteWorkspaceStateRepository::setActiveProjectId(
    const std::optional<application::ProjectId>& projectId)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(
        handle, "UPDATE workspace_state SET active_project_id = ? WHERE id = 1;", statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const auto bound = bindNullableText(handle, statement.get(), 1, projectId);
    if (!bound.success()) return common::Result<void>::failure(*bound.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_FOREIGNKEY) {
            // Should not normally happen: WorkspaceContextService validates
            // the project exists/is Active before calling this. Still fails
            // closed with a typed code rather than a generic write error.
            return common::Result<void>::failure(
                error("workspace.project_not_found", "Project not found"));
        }
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not update workspace state"));
    }
    return common::Result<void>::ok();
}

SqliteProviderRepository::SqliteProviderRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<Provider> SqliteProviderRepository::insert(Provider provider)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO providers (id,name,kind,base_url,enabled,credential_reference,created_at_ms,"
        "updated_at_ms) VALUES (?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<Provider>::failure(*prepared.error());
    const auto bound = bindProvider(handle, statement.get(), provider);
    if (!bound.success()) return common::Result<Provider>::failure(*bound.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<Provider>::failure(
                error("provider.duplicate_id", "Provider id already exists"));
        }
        return common::Result<Provider>::failure(
            databaseError(handle, "persistence.write_failed", "Could not insert provider"));
    }
    return common::Result<Provider>::ok(std::move(provider));
}

common::Result<Provider> SqliteProviderRepository::update(Provider provider)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "UPDATE providers SET name=?,kind=?,base_url=?,enabled=?,credential_reference=?,"
        "created_at_ms=?,updated_at_ms=? WHERE id=?;",
        statement);
    if (!prepared.success()) return common::Result<Provider>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, provider.name),
        bindText(handle, statement.get(), 2, provider.kind),
        bindText(handle, statement.get(), 3, provider.baseUrl),
        bindInt64(handle, statement.get(), 4, provider.enabled ? 1 : 0),
        bindNullableText(handle, statement.get(), 5, provider.credentialReference),
        bindInt64(handle, statement.get(), 6, toMilliseconds(provider.createdAt)),
        bindInt64(handle, statement.get(), 7, toMilliseconds(provider.updatedAt)),
        bindText(handle, statement.get(), 8, provider.id)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<Provider>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<Provider>::failure(
            databaseError(handle, "persistence.write_failed", "Could not update provider"));
    }
    if (sqlite3_changes(handle) == 0) {
        return common::Result<Provider>::failure(error("provider.not_found", "Provider not found"));
    }
    return common::Result<Provider>::ok(std::move(provider));
}

common::Result<std::optional<Provider>> SqliteProviderRepository::findById(
    const application::ProviderId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + providerColumns + " FROM providers WHERE id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<Provider>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success()) return common::Result<std::optional<Provider>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<Provider>>::ok(std::nullopt);
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<Provider>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read provider"));
    }
    const auto provider = readProvider(statement.get());
    if (!provider.success()) return common::Result<std::optional<Provider>>::failure(*provider.error());
    return common::Result<std::optional<Provider>>::ok(provider.value());
}

common::Result<std::vector<Provider>> SqliteProviderRepository::listAll() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + providerColumns
        + " FROM providers ORDER BY created_at_ms,id;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<Provider>>::failure(*prepared.error());

    std::vector<Provider> providers;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<Provider>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list providers"));
        }
        auto provider = readProvider(statement.get());
        if (!provider.success()) return common::Result<std::vector<Provider>>::failure(*provider.error());
        providers.push_back(provider.value());
    }
    return common::Result<std::vector<Provider>>::ok(std::move(providers));
}

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

SqliteChatRepository::SqliteChatRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

namespace {
constexpr const char* chatConversationColumns =
    "id,project_id,provider_id,model_id,title,preview,pinned,archived,input_tokens,output_tokens,"
    "estimated_cost_micros,last_latency_ms,created_at_ms,updated_at_ms";
constexpr const char* chatMessageColumns =
    "m.id,m.conversation_id,m.role,m.status,m.content,m.input_tokens,m.output_tokens,m.cost_micros,m.latency_ms,"
    "m.created_at_ms,m.provider_id,m.model_id,a.schema_version,a.conversation_id,a.project_id,a.snapshot_fingerprint,"
    "a.rendered_content_fingerprint,a.snapshot_policy_id,a.workspace_fingerprint,a.selected_run_id,"
    "a.includes_derived_project_memory,a.snapshot_estimated_tokens,a.rendered_estimated_tokens,a.rendered_text";
constexpr const char* chatMessageJoin =
    " FROM chat_messages m LEFT JOIN chat_project_context_attachments a ON a.assistant_message_id=m.id ";

common::Result<ChatConversation> readChatConversation(sqlite3_stmt* s)
{
    ChatConversation value;
    value.id = columnText(s, 0); value.projectId = optionalText(s, 1);
    value.providerId = columnText(s, 2); value.modelId = columnText(s, 3);
    value.title = columnText(s, 4); value.preview = columnText(s, 5);
    value.pinned = sqlite3_column_int64(s, 6) != 0; value.archived = sqlite3_column_int64(s, 7) != 0;
    value.inputTokens = static_cast<std::uint64_t>(sqlite3_column_int64(s, 8));
    value.outputTokens = static_cast<std::uint64_t>(sqlite3_column_int64(s, 9));
    value.estimatedCostMicros = static_cast<std::uint64_t>(sqlite3_column_int64(s, 10));
    value.lastLatencyMs = static_cast<std::uint64_t>(sqlite3_column_int64(s, 11));
    value.createdAt = fromMilliseconds(sqlite3_column_int64(s, 12));
    value.updatedAt = fromMilliseconds(sqlite3_column_int64(s, 13));
    return common::Result<ChatConversation>::ok(std::move(value));
}

common::Result<ChatMessage> readChatMessage(sqlite3_stmt* s)
{
    const auto role = application::chatRoleFromString(columnText(s, 2));
    const auto status = application::chatMessageStatusFromString(columnText(s, 3));
    if (!role || !status) return common::Result<ChatMessage>::failure(error("persistence.invalid_chat_enum", "Invalid persisted chat role or status"));
    ChatMessage value;
    value.id = columnText(s, 0); value.conversationId = columnText(s, 1);
    value.role = *role; value.status = *status; value.content = columnText(s, 4);
    value.inputTokens = static_cast<std::uint64_t>(sqlite3_column_int64(s, 5));
    value.outputTokens = static_cast<std::uint64_t>(sqlite3_column_int64(s, 6));
    value.costMicros = static_cast<std::uint64_t>(sqlite3_column_int64(s, 7));
    value.latencyMs = static_cast<std::uint64_t>(sqlite3_column_int64(s, 8));
    value.createdAt = fromMilliseconds(sqlite3_column_int64(s, 9));
    value.providerId = columnText(s, 10); value.modelId = columnText(s, 11);
    if (sqlite3_column_type(s, 12) != SQLITE_NULL) {
        ChatProjectContextAttachment attachment;
        attachment.schemaVersion = columnText(s, 12);
        attachment.assistantMessageId = value.id;
        attachment.conversationId = columnText(s, 13);
        attachment.projectId = columnText(s, 14);
        attachment.snapshotFingerprint = columnText(s, 15);
        attachment.renderedContentFingerprint = columnText(s, 16);
        attachment.snapshotPolicyId = columnText(s, 17);
        attachment.workspaceFingerprint = columnText(s, 18);
        attachment.selectedRunId = optionalText(s, 19);
        attachment.includesDerivedProjectMemory = sqlite3_column_int64(s, 20) != 0;
        attachment.snapshotEstimatedTokens = static_cast<std::uint64_t>(sqlite3_column_int64(s, 21));
        attachment.renderedEstimatedTokens = static_cast<std::uint64_t>(sqlite3_column_int64(s, 22));
        attachment.renderedText = columnText(s, 23);
        // An attachment belongs to an Assistant turn. A failed generation turns that row into an Error row (Retry turns
        // it back), so both are valid carriers; a User or System row can never be.
        if (attachment.conversationId != value.conversationId
            || (value.role != application::ChatRole::Assistant && value.role != application::ChatRole::Error)) {
            return common::Result<ChatMessage>::failure(error(
                "chat_context.invalid_identity", "The stored project context does not belong to its Chat message"));
        }
        const auto valid = application::validateChatProjectContextAttachment(attachment);
        if (!valid.success()) return common::Result<ChatMessage>::failure(*valid.error());
        value.projectContext = std::move(attachment);
    }
    return common::Result<ChatMessage>::ok(std::move(value));
}

common::Result<void> bindChatMessage(sqlite3* handle, sqlite3_stmt* statement, const ChatMessage& value)
{
    const common::Result<void> bound[] = {
        bindText(handle, statement, 1, value.id), bindText(handle, statement, 2, value.conversationId),
        bindText(handle, statement, 3, std::string(application::toString(value.role))),
        bindText(handle, statement, 4, std::string(application::toString(value.status))),
        bindText(handle, statement, 5, value.content), bindUnsigned(handle, statement, 6, value.inputTokens),
        bindUnsigned(handle, statement, 7, value.outputTokens), bindUnsigned(handle, statement, 8, value.costMicros),
        bindUnsigned(handle, statement, 9, value.latencyMs),
        bindInt64(handle, statement, 10, toMilliseconds(value.createdAt)),
        bindNullableText(handle, statement, 11, value.providerId.empty()
            ? std::optional<std::string> {} : std::optional<std::string> {value.providerId}),
        bindNullableText(handle, statement, 12, value.modelId.empty()
            ? std::optional<std::string> {} : std::optional<std::string> {value.modelId})};
    for (const auto& result : bound) if (!result.success()) return result;
    return common::Result<void>::ok();
}

common::Result<void> insertChatMessage(sqlite3* handle, const ChatMessage& value)
{
    Statement statement;
    auto prepared = prepare(handle,
        "INSERT INTO chat_messages (id,conversation_id,role,status,content,input_tokens,output_tokens,cost_micros,"
        "latency_ms,created_at_ms,provider_id,model_id) VALUES (?,?,?,?,?,?,?,?,?,?,?,?);", statement);
    if (!prepared.success()) return prepared;
    auto bound = bindChatMessage(handle, statement.get(), value);
    if (!bound.success()) return bound;
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(databaseError(
            handle, "persistence.write_failed", "Could not insert chat message"));
    }
    return common::Result<void>::ok();
}

common::Result<void> insertChatProjectContext(sqlite3* handle, const ChatProjectContextAttachment& value)
{
    const auto valid = application::validateChatProjectContextAttachment(value);
    if (!valid.success()) return valid;
    Statement statement;
    auto prepared = prepare(handle,
        "INSERT INTO chat_project_context_attachments (assistant_message_id,conversation_id,project_id,"
        "schema_version,snapshot_fingerprint,rendered_content_fingerprint,snapshot_policy_id,workspace_fingerprint,"
        "selected_run_id,includes_derived_project_memory,snapshot_estimated_tokens,rendered_estimated_tokens,"
        "rendered_text) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?);", statement);
    if (!prepared.success()) return prepared;
    const common::Result<void> bound[] = {
        bindText(handle, statement.get(), 1, value.assistantMessageId),
        bindText(handle, statement.get(), 2, value.conversationId),
        bindText(handle, statement.get(), 3, value.projectId),
        bindText(handle, statement.get(), 4, value.schemaVersion),
        bindText(handle, statement.get(), 5, value.snapshotFingerprint),
        bindText(handle, statement.get(), 6, value.renderedContentFingerprint),
        bindText(handle, statement.get(), 7, value.snapshotPolicyId),
        bindText(handle, statement.get(), 8, value.workspaceFingerprint),
        bindNullableText(handle, statement.get(), 9, value.selectedRunId),
        bindInt64(handle, statement.get(), 10, value.includesDerivedProjectMemory),
        bindUnsigned(handle, statement.get(), 11, value.snapshotEstimatedTokens),
        bindUnsigned(handle, statement.get(), 12, value.renderedEstimatedTokens),
        bindText(handle, statement.get(), 13, value.renderedText)};
    for (const auto& result : bound) if (!result.success()) return result;
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(databaseError(
            handle, "persistence.write_failed", "Could not insert Chat project context"));
    }
    return common::Result<void>::ok();
}
}

common::Result<ChatConversation> SqliteChatRepository::insertConversation(ChatConversation v)
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h = database_->impl_->handle; Statement s;
    const auto p = prepare(h, "INSERT INTO chat_conversations VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?);", s);
    if (!p.success()) return common::Result<ChatConversation>::failure(*p.error());
    const common::Result<void> b[] = {bindText(h,s.get(),1,v.id),bindNullableText(h,s.get(),2,v.projectId),bindText(h,s.get(),3,v.providerId),bindText(h,s.get(),4,v.modelId),bindText(h,s.get(),5,v.title),bindText(h,s.get(),6,v.preview),bindInt64(h,s.get(),7,v.pinned),bindInt64(h,s.get(),8,v.archived),bindUnsigned(h,s.get(),9,v.inputTokens),bindUnsigned(h,s.get(),10,v.outputTokens),bindUnsigned(h,s.get(),11,v.estimatedCostMicros),bindUnsigned(h,s.get(),12,v.lastLatencyMs),bindInt64(h,s.get(),13,toMilliseconds(v.createdAt)),bindInt64(h,s.get(),14,toMilliseconds(v.updatedAt))};
    for (const auto& x:b) if(!x.success()) return common::Result<ChatConversation>::failure(*x.error());
    if(sqlite3_step(s.get())!=SQLITE_DONE) return common::Result<ChatConversation>::failure(databaseError(h,"persistence.write_failed","Could not insert conversation"));
    return common::Result<ChatConversation>::ok(std::move(v));
}

common::Result<ChatConversation> SqliteChatRepository::updateConversation(ChatConversation v)
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h=database_->impl_->handle; Statement s;
    const auto p=prepare(h,"UPDATE chat_conversations SET project_id=?,provider_id=?,model_id=?,title=?,preview=?,pinned=?,archived=?,input_tokens=?,output_tokens=?,estimated_cost_micros=?,last_latency_ms=?,created_at_ms=?,updated_at_ms=? WHERE id=?;",s);
    if(!p.success()) return common::Result<ChatConversation>::failure(*p.error());
    const common::Result<void> b[]={bindNullableText(h,s.get(),1,v.projectId),bindText(h,s.get(),2,v.providerId),bindText(h,s.get(),3,v.modelId),bindText(h,s.get(),4,v.title),bindText(h,s.get(),5,v.preview),bindInt64(h,s.get(),6,v.pinned),bindInt64(h,s.get(),7,v.archived),bindUnsigned(h,s.get(),8,v.inputTokens),bindUnsigned(h,s.get(),9,v.outputTokens),bindUnsigned(h,s.get(),10,v.estimatedCostMicros),bindUnsigned(h,s.get(),11,v.lastLatencyMs),bindInt64(h,s.get(),12,toMilliseconds(v.createdAt)),bindInt64(h,s.get(),13,toMilliseconds(v.updatedAt)),bindText(h,s.get(),14,v.id)};
    for(const auto& x:b) if(!x.success()) return common::Result<ChatConversation>::failure(*x.error());
    if(sqlite3_step(s.get())!=SQLITE_DONE) return common::Result<ChatConversation>::failure(databaseError(h,"persistence.write_failed","Could not update conversation"));
    if(sqlite3_changes(h)==0) return common::Result<ChatConversation>::failure(error("chat.not_found","Conversation not found"));
    return common::Result<ChatConversation>::ok(std::move(v));
}

common::Result<void> SqliteChatRepository::removeConversation(const application::ConversationId& id)
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h=database_->impl_->handle; Statement s;
    auto p=prepare(h,"DELETE FROM chat_conversations WHERE id=?;",s); if(!p.success()) return p;
    auto b=bindText(h,s.get(),1,id); if(!b.success()) return b;
    if(sqlite3_step(s.get())!=SQLITE_DONE) return common::Result<void>::failure(databaseError(h,"persistence.write_failed","Could not delete conversation"));
    if(sqlite3_changes(h)==0) return common::Result<void>::failure(error("chat.not_found","Conversation not found"));
    return common::Result<void>::ok();
}

common::Result<std::optional<ChatConversation>> SqliteChatRepository::findConversation(const application::ConversationId& id) const
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h=database_->impl_->handle; Statement s; const std::string sql=std::string("SELECT ")+chatConversationColumns+" FROM chat_conversations WHERE id=?;";
    auto p=prepare(h,sql.c_str(),s); if(!p.success()) return common::Result<std::optional<ChatConversation>>::failure(*p.error()); auto b=bindText(h,s.get(),1,id); if(!b.success()) return common::Result<std::optional<ChatConversation>>::failure(*b.error());
    int step=sqlite3_step(s.get()); if(step==SQLITE_DONE) return common::Result<std::optional<ChatConversation>>::ok(std::nullopt); if(step!=SQLITE_ROW) return common::Result<std::optional<ChatConversation>>::failure(databaseError(h,"persistence.read_failed","Could not read conversation"));
    auto v=readChatConversation(s.get()); if(!v.success()) return common::Result<std::optional<ChatConversation>>::failure(*v.error()); return common::Result<std::optional<ChatConversation>>::ok(v.value());
}

common::Result<std::vector<ChatConversation>> SqliteChatRepository::listConversations() const
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h=database_->impl_->handle; Statement s; const std::string sql=std::string("SELECT ")+chatConversationColumns+" FROM chat_conversations ORDER BY pinned DESC,updated_at_ms DESC,id;";
    auto p=prepare(h,sql.c_str(),s); if(!p.success()) return common::Result<std::vector<ChatConversation>>::failure(*p.error()); std::vector<ChatConversation> out;
    for(;;){int step=sqlite3_step(s.get()); if(step==SQLITE_DONE) break; if(step!=SQLITE_ROW) return common::Result<std::vector<ChatConversation>>::failure(databaseError(h,"persistence.read_failed","Could not list conversations")); auto v=readChatConversation(s.get()); if(!v.success()) return common::Result<std::vector<ChatConversation>>::failure(*v.error()); out.push_back(v.value());}
    return common::Result<std::vector<ChatConversation>>::ok(std::move(out));
}

common::Result<PreparedChatTurn> SqliteChatRepository::insertPreparedTurn(
    ChatConversation conversation, PreparedChatTurn turn)
{
    if (turn.userMessage.conversationId != conversation.id
        || turn.assistantMessage.conversationId != conversation.id
        || turn.userMessage.role != application::ChatRole::User
        || turn.assistantMessage.role != application::ChatRole::Assistant
        || turn.userMessage.projectContext.has_value()) {
        return common::Result<PreparedChatTurn>::failure(error(
            "chat.invalid_prepared_turn", "The prepared Chat turn identity or roles are invalid"));
    }
    if (turn.assistantMessage.projectContext.has_value()) {
        const auto& attachment = *turn.assistantMessage.projectContext;
        const auto valid = application::validateChatProjectContextAttachment(attachment);
        if (!valid.success()) return common::Result<PreparedChatTurn>::failure(*valid.error());
        if (attachment.assistantMessageId != turn.assistantMessage.id
            || attachment.conversationId != conversation.id
            || !conversation.projectId.has_value() || attachment.projectId != *conversation.projectId) {
            return common::Result<PreparedChatTurn>::failure(error(
                "chat_context.invalid_identity", "The context attachment does not belong to this prepared turn"));
        }
    }

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    auto begun = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!begun.success()) return common::Result<PreparedChatTurn>::failure(*begun.error());
    const auto rollback = [&] { static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed")); };
    const auto user = insertChatMessage(handle, turn.userMessage);
    if (!user.success()) { rollback(); return common::Result<PreparedChatTurn>::failure(*user.error()); }
    const auto assistant = insertChatMessage(handle, turn.assistantMessage);
    if (!assistant.success()) { rollback(); return common::Result<PreparedChatTurn>::failure(*assistant.error()); }
    if (turn.assistantMessage.projectContext.has_value()) {
        const auto attached = insertChatProjectContext(handle, *turn.assistantMessage.projectContext);
        if (!attached.success()) { rollback(); return common::Result<PreparedChatTurn>::failure(*attached.error()); }
    }
    Statement statement;
    auto prepared = prepare(handle,
        "UPDATE chat_conversations SET project_id=?,provider_id=?,model_id=?,title=?,preview=?,pinned=?,archived=?,"
        "input_tokens=?,output_tokens=?,estimated_cost_micros=?,last_latency_ms=?,created_at_ms=?,updated_at_ms=? "
        "WHERE id=?;", statement);
    if (!prepared.success()) { rollback(); return common::Result<PreparedChatTurn>::failure(*prepared.error()); }
    const common::Result<void> bound[] = {
        bindNullableText(handle,statement.get(),1,conversation.projectId),bindText(handle,statement.get(),2,conversation.providerId),
        bindText(handle,statement.get(),3,conversation.modelId),bindText(handle,statement.get(),4,conversation.title),
        bindText(handle,statement.get(),5,conversation.preview),bindInt64(handle,statement.get(),6,conversation.pinned),
        bindInt64(handle,statement.get(),7,conversation.archived),bindUnsigned(handle,statement.get(),8,conversation.inputTokens),
        bindUnsigned(handle,statement.get(),9,conversation.outputTokens),bindUnsigned(handle,statement.get(),10,conversation.estimatedCostMicros),
        bindUnsigned(handle,statement.get(),11,conversation.lastLatencyMs),bindInt64(handle,statement.get(),12,toMilliseconds(conversation.createdAt)),
        bindInt64(handle,statement.get(),13,toMilliseconds(conversation.updatedAt)),bindText(handle,statement.get(),14,conversation.id)};
    for (const auto& result : bound) {
        if (!result.success()) { rollback(); return common::Result<PreparedChatTurn>::failure(*result.error()); }
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE || sqlite3_changes(handle) == 0) {
        const auto cause = databaseError(handle, "persistence.write_failed", "Could not update prepared Chat conversation");
        rollback();
        return common::Result<PreparedChatTurn>::failure(cause);
    }
    const auto committed = execute(handle, "COMMIT;", "persistence.transaction_failed");
    if (!committed.success()) { rollback(); return common::Result<PreparedChatTurn>::failure(*committed.error()); }
    return common::Result<PreparedChatTurn>::ok(std::move(turn));
}

common::Result<ChatMessage> SqliteChatRepository::insertMessage(ChatMessage v)
{
    if (v.projectContext.has_value()) return common::Result<ChatMessage>::failure(error(
        "chat.context_requires_prepared_turn", "Project context must be inserted atomically with its Chat turn"));
    std::lock_guard lock(database_->impl_->mutex);
    auto inserted = insertChatMessage(database_->impl_->handle, v);
    if (!inserted.success()) return common::Result<ChatMessage>::failure(*inserted.error());
    return common::Result<ChatMessage>::ok(std::move(v));
}

common::Result<ChatMessage> SqliteChatRepository::updateMessage(ChatMessage v)
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h=database_->impl_->handle; Statement s; auto p=prepare(h,"UPDATE chat_messages SET conversation_id=?,role=?,status=?,content=?,input_tokens=?,output_tokens=?,cost_micros=?,latency_ms=?,created_at_ms=?,provider_id=?,model_id=? WHERE id=?;",s); if(!p.success()) return common::Result<ChatMessage>::failure(*p.error());
    const common::Result<void> b[]={bindText(h,s.get(),1,v.conversationId),bindText(h,s.get(),2,std::string(application::toString(v.role))),bindText(h,s.get(),3,std::string(application::toString(v.status))),bindText(h,s.get(),4,v.content),bindUnsigned(h,s.get(),5,v.inputTokens),bindUnsigned(h,s.get(),6,v.outputTokens),bindUnsigned(h,s.get(),7,v.costMicros),bindUnsigned(h,s.get(),8,v.latencyMs),bindInt64(h,s.get(),9,toMilliseconds(v.createdAt)),bindNullableText(h,s.get(),10,v.providerId.empty()?std::optional<std::string>{}:std::optional<std::string>{v.providerId}),bindNullableText(h,s.get(),11,v.modelId.empty()?std::optional<std::string>{}:std::optional<std::string>{v.modelId}),bindText(h,s.get(),12,v.id)}; for(const auto& x:b) if(!x.success()) return common::Result<ChatMessage>::failure(*x.error());
    if(sqlite3_step(s.get())!=SQLITE_DONE) return common::Result<ChatMessage>::failure(databaseError(h,"persistence.write_failed","Could not update chat message")); if(sqlite3_changes(h)==0) return common::Result<ChatMessage>::failure(error("chat.message_not_found","Message not found")); return common::Result<ChatMessage>::ok(std::move(v));
}

common::Result<std::optional<ChatMessage>> SqliteChatRepository::findMessage(const application::ChatMessageId& id) const
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h=database_->impl_->handle; Statement s; const std::string sql=std::string("SELECT ")+chatMessageColumns+chatMessageJoin+"WHERE m.id=?;"; auto p=prepare(h,sql.c_str(),s); if(!p.success()) return common::Result<std::optional<ChatMessage>>::failure(*p.error()); auto b=bindText(h,s.get(),1,id); if(!b.success()) return common::Result<std::optional<ChatMessage>>::failure(*b.error()); int step=sqlite3_step(s.get()); if(step==SQLITE_DONE) return common::Result<std::optional<ChatMessage>>::ok(std::nullopt); if(step!=SQLITE_ROW) return common::Result<std::optional<ChatMessage>>::failure(databaseError(h,"persistence.read_failed","Could not read chat message")); auto v=readChatMessage(s.get()); if(!v.success()) return common::Result<std::optional<ChatMessage>>::failure(*v.error()); return common::Result<std::optional<ChatMessage>>::ok(v.value());
}

common::Result<std::vector<ChatMessage>> SqliteChatRepository::listMessages(const application::ConversationId& id) const
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h=database_->impl_->handle; Statement s; const std::string sql=std::string("SELECT ")+chatMessageColumns+chatMessageJoin+"WHERE m.conversation_id=? ORDER BY m.created_at_ms,m.id;"; auto p=prepare(h,sql.c_str(),s); if(!p.success()) return common::Result<std::vector<ChatMessage>>::failure(*p.error()); auto b=bindText(h,s.get(),1,id); if(!b.success()) return common::Result<std::vector<ChatMessage>>::failure(*b.error()); std::vector<ChatMessage> out; for(;;){int step=sqlite3_step(s.get()); if(step==SQLITE_DONE) break; if(step!=SQLITE_ROW) return common::Result<std::vector<ChatMessage>>::failure(databaseError(h,"persistence.read_failed","Could not list chat messages")); auto v=readChatMessage(s.get()); if(!v.success()) return common::Result<std::vector<ChatMessage>>::failure(*v.error()); out.push_back(v.value());} return common::Result<std::vector<ChatMessage>>::ok(std::move(out));
}

namespace {
constexpr const char* chatMemorySegmentColumns =
    "id,conversation_id,first_message_id,last_message_id,summary,"
    "source_token_count,summary_token_count,created_at_ms";

common::Result<ChatMemorySegment> readChatMemorySegment(sqlite3_stmt* s)
{
    ChatMemorySegment value;
    value.id = columnText(s, 0);
    value.conversationId = columnText(s, 1);
    value.firstMessageId = columnText(s, 2);
    value.lastMessageId = columnText(s, 3);
    value.summary = columnText(s, 4);
    value.sourceTokenCount = static_cast<std::uint64_t>(sqlite3_column_int64(s, 5));
    value.summaryTokenCount = static_cast<std::uint64_t>(sqlite3_column_int64(s, 6));
    value.createdAt = fromMilliseconds(sqlite3_column_int64(s, 7));
    return common::Result<ChatMemorySegment>::ok(std::move(value));
}
}

SqliteChatMemoryRepository::SqliteChatMemoryRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<ChatMemorySegment> SqliteChatMemoryRepository::insertMemorySegment(ChatMemorySegment v)
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h = database_->impl_->handle; Statement s;
    const auto p = prepare(h,
        "INSERT INTO chat_memory_segments (id,conversation_id,first_message_id,last_message_id,"
        "summary,source_token_count,summary_token_count,created_at_ms) VALUES (?,?,?,?,?,?,?,?);", s);
    if (!p.success()) return common::Result<ChatMemorySegment>::failure(*p.error());
    const common::Result<void> b[] = {bindText(h,s.get(),1,v.id),bindText(h,s.get(),2,v.conversationId),
        bindText(h,s.get(),3,v.firstMessageId),bindText(h,s.get(),4,v.lastMessageId),
        bindText(h,s.get(),5,v.summary),bindUnsigned(h,s.get(),6,v.sourceTokenCount),
        bindUnsigned(h,s.get(),7,v.summaryTokenCount),bindInt64(h,s.get(),8,toMilliseconds(v.createdAt))};
    for (const auto& x : b) if (!x.success()) return common::Result<ChatMemorySegment>::failure(*x.error());
    if (sqlite3_step(s.get()) != SQLITE_DONE) {
        const int code = sqlite3_extended_errcode(h);
        if (code == SQLITE_CONSTRAINT_PRIMARYKEY)
            return common::Result<ChatMemorySegment>::failure(
                error("chat_memory.duplicate_id", "Memory segment id already exists"));
        if (code == SQLITE_CONSTRAINT_FOREIGNKEY)
            return common::Result<ChatMemorySegment>::failure(
                error("chat_memory.conversation_not_found", "Conversation does not exist"));
        return common::Result<ChatMemorySegment>::failure(
            databaseError(h, "persistence.write_failed", "Could not insert chat memory segment"));
    }
    return common::Result<ChatMemorySegment>::ok(std::move(v));
}

common::Result<std::vector<ChatMemorySegment>> SqliteChatMemoryRepository::listMemorySegments(
    const application::ConversationId& conversationId) const
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h = database_->impl_->handle; Statement s;
    // Stable chronological order: created_at_ms first, id as deterministic tiebreak.
    const std::string sql = std::string("SELECT ") + chatMemorySegmentColumns
        + " FROM chat_memory_segments WHERE conversation_id=? ORDER BY created_at_ms,id;";
    const auto p = prepare(h, sql.c_str(), s);
    if (!p.success()) return common::Result<std::vector<ChatMemorySegment>>::failure(*p.error());
    const auto b = bindText(h, s.get(), 1, conversationId);
    if (!b.success()) return common::Result<std::vector<ChatMemorySegment>>::failure(*b.error());
    std::vector<ChatMemorySegment> out;
    for (;;) {
        const int step = sqlite3_step(s.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW)
            return common::Result<std::vector<ChatMemorySegment>>::failure(
                databaseError(h, "persistence.read_failed", "Could not list chat memory segments"));
        auto v = readChatMemorySegment(s.get());
        if (!v.success()) return common::Result<std::vector<ChatMemorySegment>>::failure(*v.error());
        out.push_back(std::move(v.value()));
    }
    return common::Result<std::vector<ChatMemorySegment>>::ok(std::move(out));
}

common::Result<std::optional<ChatMemorySegment>> SqliteChatMemoryRepository::findMemorySegment(
    const application::ChatMemorySegmentId& id) const
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h = database_->impl_->handle; Statement s;
    const std::string sql = std::string("SELECT ") + chatMemorySegmentColumns
        + " FROM chat_memory_segments WHERE id=?;";
    const auto p = prepare(h, sql.c_str(), s);
    if (!p.success()) return common::Result<std::optional<ChatMemorySegment>>::failure(*p.error());
    const auto b = bindText(h, s.get(), 1, id);
    if (!b.success()) return common::Result<std::optional<ChatMemorySegment>>::failure(*b.error());
    const int step = sqlite3_step(s.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<ChatMemorySegment>>::ok(std::nullopt);
    if (step != SQLITE_ROW)
        return common::Result<std::optional<ChatMemorySegment>>::failure(
            databaseError(h, "persistence.read_failed", "Could not read chat memory segment"));
    auto v = readChatMemorySegment(s.get());
    if (!v.success()) return common::Result<std::optional<ChatMemorySegment>>::failure(*v.error());
    return common::Result<std::optional<ChatMemorySegment>>::ok(std::move(v.value()));
}

common::Result<void> SqliteChatMemoryRepository::removeMemorySegments(
    const application::ConversationId& conversationId)
{
    // Deleting zero rows is success: "no memory for this conversation" is a
    // valid state, unlike removeConversation's not-found contract.
    std::lock_guard lock(database_->impl_->mutex); sqlite3* h = database_->impl_->handle; Statement s;
    const auto p = prepare(h, "DELETE FROM chat_memory_segments WHERE conversation_id=?;", s);
    if (!p.success()) return p;
    const auto b = bindText(h, s.get(), 1, conversationId);
    if (!b.success()) return b;
    if (sqlite3_step(s.get()) != SQLITE_DONE)
        return common::Result<void>::failure(
            databaseError(h, "persistence.write_failed", "Could not remove chat memory segments"));
    return common::Result<void>::ok();
}

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

namespace {

std::string serializeSegmentIdList(const std::vector<application::ProjectMemorySegmentId>& ids)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& id : ids) array.push_back(id);
    return array.dump();
}

std::vector<application::ProjectMemorySegmentId> deserializeSegmentIdList(const std::string& json)
{
    std::vector<application::ProjectMemorySegmentId> result;
    if (json.empty()) return result;
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) return result;
    for (const auto& item : parsed) {
        if (item.is_string()) result.push_back(item.get<std::string>());
    }
    return result;
}

std::string projectMemoryCompactionStatusToString(ProjectMemoryCompactionStatus status)
{
    return application::toString(status);
}

std::optional<ProjectMemoryCompactionStatus> projectMemoryCompactionStatusFromString(
    std::string_view value)
{
    if (value == "completed") return ProjectMemoryCompactionStatus::Completed;
    if (value == "failed") return ProjectMemoryCompactionStatus::Failed;
    if (value == "cancelled") return ProjectMemoryCompactionStatus::Cancelled;
    return std::nullopt;
}

constexpr const char* projectMemoryCompactionRecordColumns =
    "id,project_id,provider_id,model_id,source_segment_ids,source_content_hash,"
    "result_segment_id,input_tokens,output_tokens,cost_micros,status,error_code,"
    "error_message,created_at_ms";

common::Result<void> insertProjectMemoryCompactionRecord(
    sqlite3* h, const ProjectMemoryCompactionRecord& record)
{
    Statement statement;
    const auto prepared = prepare(h,
        "INSERT INTO project_memory_compaction_records (id,project_id,provider_id,model_id,"
        "source_segment_ids,source_content_hash,result_segment_id,input_tokens,output_tokens,"
        "cost_micros,status,error_code,error_message,created_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const common::Result<void> binds[] = {
        bindText(h, statement.get(), 1, record.id),
        bindText(h, statement.get(), 2, record.projectId),
        bindText(h, statement.get(), 3, record.providerId),
        bindText(h, statement.get(), 4, record.modelId),
        bindText(h, statement.get(), 5, serializeSegmentIdList(record.sourceSegmentIds)),
        bindText(h, statement.get(), 6, record.sourceContentHash),
        bindNullableText(h, statement.get(), 7, record.resultSegmentId),
        bindUnsigned(h, statement.get(), 8, record.inputTokens),
        bindUnsigned(h, statement.get(), 9, record.outputTokens),
        bindUnsigned(h, statement.get(), 10, record.costMicros),
        bindText(h, statement.get(), 11, projectMemoryCompactionStatusToString(record.status)),
        bindText(h, statement.get(), 12, record.errorCode),
        bindText(h, statement.get(), 13, record.errorMessage),
        bindInt64(h, statement.get(), 14, toMilliseconds(record.createdAt))};
    for (const auto& bound : binds) {
        if (!bound.success()) return common::Result<void>::failure(*bound.error());
    }
    if (sqlite3_step(statement.get()) == SQLITE_DONE) return common::Result<void>::ok();

    const int code = sqlite3_extended_errcode(h);
    if (code == SQLITE_CONSTRAINT_PRIMARYKEY) {
        return common::Result<void>::failure(
            error("project_memory.duplicate_id", "Compaction record id already exists"));
    }
    if (code == SQLITE_CONSTRAINT_FOREIGNKEY) {
        return common::Result<void>::failure(
            error("project_memory.project_not_found", "Project does not exist"));
    }
    return common::Result<void>::failure(databaseError(
        h, "persistence.write_failed", "Could not insert project memory compaction record"));
}

common::Result<ProjectMemoryCompactionRecord> readProjectMemoryCompactionRecord(sqlite3_stmt* statement)
{
    ProjectMemoryCompactionRecord record;
    record.id = columnText(statement, 0);
    record.projectId = columnText(statement, 1);
    record.providerId = columnText(statement, 2);
    record.modelId = columnText(statement, 3);
    record.sourceSegmentIds = deserializeSegmentIdList(columnText(statement, 4));
    record.sourceContentHash = columnText(statement, 5);
    record.resultSegmentId = optionalText(statement, 6);
    record.inputTokens = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 7));
    record.outputTokens = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 8));
    record.costMicros = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 9));
    const auto status = projectMemoryCompactionStatusFromString(columnText(statement, 10));
    if (!status.has_value()) {
        return common::Result<ProjectMemoryCompactionRecord>::failure(
            {"persistence.read_failed", "Unknown project memory compaction status"});
    }
    record.status = *status;
    record.errorCode = columnText(statement, 11);
    record.errorMessage = columnText(statement, 12);
    record.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 13));
    return common::Result<ProjectMemoryCompactionRecord>::ok(std::move(record));
}

} // namespace

SqliteProjectMemoryCompactionRepository::SqliteProjectMemoryCompactionRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteProjectMemoryCompactionRepository::insertCompactionRecord(
    const ProjectMemoryCompactionRecord& record)
{
    if (record.status == ProjectMemoryCompactionStatus::Completed) {
        return common::Result<void>::failure(error(
            "project_memory.invalid_compaction_commit",
            "Completed compaction records require an atomic memory commit"));
    }
    std::lock_guard lock(database_->impl_->mutex);
    return insertProjectMemoryCompactionRecord(database_->impl_->handle, record);
}

common::Result<void> SqliteProjectMemoryCompactionRepository::commitSuccessfulCompaction(
    const ProjectMemoryCompactionRecord& record,
    ProjectMemorySegment resultSegment)
{
    if (record.status != ProjectMemoryCompactionStatus::Completed
        || !record.resultSegmentId.has_value() || *record.resultSegmentId != resultSegment.id
        || record.projectId != resultSegment.projectId || record.sourceSegmentIds.empty()) {
        return common::Result<void>::failure(error(
            "project_memory.invalid_compaction_commit",
            "Successful compaction commit fields are inconsistent"));
    }

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* h = database_->impl_->handle;
    const auto began = execute(h, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return began;

    auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(h, "ROLLBACK;", "persistence.rollback_failed"));
        return common::Result<void>::failure(failure);
    };

    // Requiring exactly one deleted row per source id is the optimistic
    // concurrency check. Another compaction or clear cannot make a stale
    // provider response overwrite the newer memory state.
    for (const auto& id : record.sourceSegmentIds) {
        Statement removed;
        const auto prepared = prepare(
            h, "DELETE FROM project_memory_segments WHERE id=? AND project_id=?;", removed);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const common::Result<void> binds[] = {
            bindText(h, removed.get(), 1, id), bindText(h, removed.get(), 2, record.projectId)};
        for (const auto& bound : binds) {
            if (!bound.success()) return rollbackWith(*bound.error());
        }
        if (sqlite3_step(removed.get()) != SQLITE_DONE) {
            return rollbackWith(databaseError(
                h, "persistence.write_failed", "Could not remove a source memory segment"));
        }
        if (sqlite3_changes(h) != 1) {
            return rollbackWith(error(
                "project_memory.compaction_conflict",
                "Project memory changed while compaction was running"));
        }
    }

    Statement inserted;
    const auto insertPrepared = prepare(h,
        "INSERT INTO project_memory_segments (id,project_id,run_id,"
        "summary,source_token_count,summary_token_count,created_at_ms) VALUES (?,?,?,?,?,?,?);",
        inserted);
    if (!insertPrepared.success()) return rollbackWith(*insertPrepared.error());
    const common::Result<void> insertBinds[] = {
        bindText(h, inserted.get(), 1, resultSegment.id),
        bindText(h, inserted.get(), 2, resultSegment.projectId),
        bindText(h, inserted.get(), 3, resultSegment.runId),
        bindText(h, inserted.get(), 4, resultSegment.summary),
        bindUnsigned(h, inserted.get(), 5, resultSegment.sourceTokenCount),
        bindUnsigned(h, inserted.get(), 6, resultSegment.summaryTokenCount),
        bindInt64(h, inserted.get(), 7, toMilliseconds(resultSegment.createdAt))};
    for (const auto& bound : insertBinds) {
        if (!bound.success()) return rollbackWith(*bound.error());
    }
    if (sqlite3_step(inserted.get()) != SQLITE_DONE) {
        const int code = sqlite3_extended_errcode(h);
        if (code == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return rollbackWith(error(
                "project_memory.duplicate_id", "Memory segment id already exists"));
        }
        if (code == SQLITE_CONSTRAINT_FOREIGNKEY) {
            return rollbackWith(error(
                "project_memory.project_not_found", "Project does not exist"));
        }
        return rollbackWith(databaseError(
            h, "persistence.write_failed", "Could not insert compacted memory segment"));
    }

    const auto recorded = insertProjectMemoryCompactionRecord(h, record);
    if (!recorded.success()) return rollbackWith(*recorded.error());

    const auto committed = execute(h, "COMMIT;", "persistence.transaction_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<void>::ok();
}

common::Result<std::vector<ProjectMemoryCompactionRecord>>
SqliteProjectMemoryCompactionRepository::listCompactionRecords(
    const application::ProjectId& projectId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* h = database_->impl_->handle;
    const std::string sql = std::string("SELECT ") + projectMemoryCompactionRecordColumns
        + " FROM project_memory_compaction_records WHERE project_id=? ORDER BY created_at_ms,id;";
    Statement s;
    const auto prepared = prepare(h, sql.c_str(), s);
    if (!prepared.success())
        return common::Result<std::vector<ProjectMemoryCompactionRecord>>::failure(*prepared.error());
    const auto bound = bindText(h, s.get(), 1, projectId);
    if (!bound.success())
        return common::Result<std::vector<ProjectMemoryCompactionRecord>>::failure(*bound.error());
    std::vector<ProjectMemoryCompactionRecord> out;
    for (;;) {
        const int step = sqlite3_step(s.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<ProjectMemoryCompactionRecord>>::failure(databaseError(
                h, "persistence.read_failed", "Could not list project memory compaction records"));
        }
        auto record = readProjectMemoryCompactionRecord(s.get());
        if (!record.success())
            return common::Result<std::vector<ProjectMemoryCompactionRecord>>::failure(*record.error());
        out.push_back(std::move(record.value()));
    }
    return common::Result<std::vector<ProjectMemoryCompactionRecord>>::ok(std::move(out));
}

SqliteExecutionProfileRepository::SqliteExecutionProfileRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<ExecutionProfile> SqliteExecutionProfileRepository::insert(ExecutionProfile profile)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return common::Result<ExecutionProfile>::failure(*began.error());

    auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return common::Result<ExecutionProfile>::failure(failure);
    };

    {
        Statement statement;
        const auto prepared = prepare(handle,
            "INSERT INTO execution_profiles (id,name,workflow_id,enabled,is_default,created_at_ms,"
            "updated_at_ms,provider_budget_json) VALUES (?,?,?,?,?,?,?,?);",
            statement);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const auto bound = bindExecutionProfile(handle, statement.get(), profile);
        if (!bound.success()) return rollbackWith(*bound.error());
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
                return rollbackWith(
                    error("execution_profile.duplicate_id", "Execution profile id already exists"));
            }
            return rollbackWith(
                databaseError(handle, "persistence.write_failed", "Could not insert execution profile"));
        }
    }

    for (const auto& assignment : profile.assignments) {
        Statement statement;
        const auto prepared = prepare(handle,
            "INSERT INTO execution_profile_assignments (profile_id,stage_type,provider_id,model_id,"
            "timeout_ms) VALUES (?,?,?,?,?);",
            statement);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const common::Result<void> bindings[] = {
            bindText(handle, statement.get(), 1, profile.id),
            bindText(handle, statement.get(), 2, assignment.stageType),
            bindText(handle, statement.get(), 3, assignment.providerId),
            bindText(handle, statement.get(), 4, assignment.modelId),
            bindInt64(handle, statement.get(), 5, static_cast<std::int64_t>(assignment.timeout.count()))};
        for (const auto& binding : bindings) {
            if (!binding.success()) return rollbackWith(*binding.error());
        }
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
                return rollbackWith(error(
                    "execution_profile.duplicate_stage_type",
                    "A stage type cannot appear more than once in one profile"));
            }
            return rollbackWith(databaseError(
                handle, "persistence.write_failed", "Could not insert execution profile assignment"));
        }
    }

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<ExecutionProfile>::ok(std::move(profile));
}

common::Result<ExecutionProfile> SqliteExecutionProfileRepository::update(ExecutionProfile profile)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return common::Result<ExecutionProfile>::failure(*began.error());

    auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return common::Result<ExecutionProfile>::failure(failure);
    };

    {
        // is_default is intentionally included as a straight passthrough
        // here (unlike Model), but callers always populate it from the
        // existing stored row before calling update() - only
        // setDefaultProfile() actually flips it.
        Statement statement;
        const auto prepared = prepare(handle,
            "UPDATE execution_profiles SET name=?,workflow_id=?,enabled=?,is_default=?,"
            "created_at_ms=?,updated_at_ms=?,provider_budget_json=? WHERE id=?;",
            statement);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const common::Result<void> bindings[] = {
            bindText(handle, statement.get(), 1, profile.name),
            bindText(handle, statement.get(), 2, profile.workflowId),
            bindInt64(handle, statement.get(), 3, profile.enabled ? 1 : 0),
            bindInt64(handle, statement.get(), 4, profile.isDefault ? 1 : 0),
            bindInt64(handle, statement.get(), 5, toMilliseconds(profile.createdAt)),
            bindInt64(handle, statement.get(), 6, toMilliseconds(profile.updatedAt)),
            bindText(handle, statement.get(), 7, providerBudgetOverridesJson(profile.providerBudget)),
            bindText(handle, statement.get(), 8, profile.id)};
        for (const auto& binding : bindings) {
            if (!binding.success()) return rollbackWith(*binding.error());
        }
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            return rollbackWith(
                databaseError(handle, "persistence.write_failed", "Could not update execution profile"));
        }
        if (sqlite3_changes(handle) == 0) {
            return rollbackWith(
                error("execution_profile.not_found", "Execution profile not found"));
        }
    }

    {
        Statement remove;
        const auto removePrepared = prepare(
            handle, "DELETE FROM execution_profile_assignments WHERE profile_id=?;", remove);
        if (!removePrepared.success()) return rollbackWith(*removePrepared.error());
        const auto removeBound = bindText(handle, remove.get(), 1, profile.id);
        if (!removeBound.success()) return rollbackWith(*removeBound.error());
        if (sqlite3_step(remove.get()) != SQLITE_DONE) {
            return rollbackWith(databaseError(
                handle, "persistence.write_failed", "Could not replace execution profile assignments"));
        }
    }

    for (const auto& assignment : profile.assignments) {
        Statement statement;
        const auto prepared = prepare(handle,
            "INSERT INTO execution_profile_assignments (profile_id,stage_type,provider_id,model_id,"
            "timeout_ms) VALUES (?,?,?,?,?);",
            statement);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const common::Result<void> bindings[] = {
            bindText(handle, statement.get(), 1, profile.id),
            bindText(handle, statement.get(), 2, assignment.stageType),
            bindText(handle, statement.get(), 3, assignment.providerId),
            bindText(handle, statement.get(), 4, assignment.modelId),
            bindInt64(handle, statement.get(), 5, static_cast<std::int64_t>(assignment.timeout.count()))};
        for (const auto& binding : bindings) {
            if (!binding.success()) return rollbackWith(*binding.error());
        }
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
                return rollbackWith(error(
                    "execution_profile.duplicate_stage_type",
                    "A stage type cannot appear more than once in one profile"));
            }
            return rollbackWith(databaseError(
                handle, "persistence.write_failed", "Could not insert execution profile assignment"));
        }
    }

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<ExecutionProfile>::ok(std::move(profile));
}

common::Result<std::optional<ExecutionProfile>> SqliteExecutionProfileRepository::findById(
    const application::ExecutionProfileId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + executionProfileColumns
        + " FROM execution_profiles WHERE id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) {
        return common::Result<std::optional<ExecutionProfile>>::failure(*prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success()) return common::Result<std::optional<ExecutionProfile>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<ExecutionProfile>>::ok(std::nullopt);
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<ExecutionProfile>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read execution profile"));
    }
    auto profile = readExecutionProfile(statement.get());
    if (!profile.success()) {
        return common::Result<std::optional<ExecutionProfile>>::failure(*profile.error());
    }

    const auto assignments = fetchAssignmentsForProfile(handle, id);
    if (!assignments.success()) {
        return common::Result<std::optional<ExecutionProfile>>::failure(*assignments.error());
    }
    profile.value().assignments = assignments.value();
    return common::Result<std::optional<ExecutionProfile>>::ok(profile.value());
}

common::Result<std::vector<ExecutionProfile>> SqliteExecutionProfileRepository::listAll() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + executionProfileColumns
        + " FROM execution_profiles ORDER BY created_at_ms,id;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<ExecutionProfile>>::failure(*prepared.error());

    std::vector<ExecutionProfile> profiles;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<ExecutionProfile>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list execution profiles"));
        }
        auto profile = readExecutionProfile(statement.get());
        if (!profile.success()) {
            return common::Result<std::vector<ExecutionProfile>>::failure(*profile.error());
        }
        profiles.push_back(profile.value());
    }

    for (auto& profile : profiles) {
        const auto assignments = fetchAssignmentsForProfile(handle, profile.id);
        if (!assignments.success()) {
            return common::Result<std::vector<ExecutionProfile>>::failure(*assignments.error());
        }
        profile.assignments = assignments.value();
    }
    return common::Result<std::vector<ExecutionProfile>>::ok(std::move(profiles));
}

common::Result<void> SqliteExecutionProfileRepository::setDefaultProfile(
    const application::ExecutionProfileId& profileId)
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
        const auto unset = execute(
            handle, "UPDATE execution_profiles SET is_default=0 WHERE is_default=1;",
            "persistence.write_failed");
        if (!unset.success()) return rollbackWith(*unset.error());
    }

    {
        Statement statement;
        const auto prepared = prepare(
            handle, "UPDATE execution_profiles SET is_default=1 WHERE id=?;", statement);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const auto bound = bindText(handle, statement.get(), 1, profileId);
        if (!bound.success()) return rollbackWith(*bound.error());
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            return rollbackWith(databaseError(
                handle, "persistence.write_failed", "Could not set default execution profile"));
        }
        if (sqlite3_changes(handle) == 0) {
            return rollbackWith(
                error("execution_profile.not_found", "Execution profile not found"));
        }
    }

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<void>::ok();
}

SqliteRunDraftRepository::SqliteRunDraftRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<RunDraft> SqliteRunDraftRepository::insert(RunDraft draft)
{
    // A draft that belongs to a group is written only together with its group row (insertGroup).
    if (draft.provenance.has_value()) {
        return common::Result<RunDraft>::failure(error("run_draft.group_insert_required",
            "A draft that belongs to a group is created through insertGroup"));
    }
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return common::Result<RunDraft>::failure(*began.error());

    auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return common::Result<RunDraft>::failure(failure);
    };

    {
        Statement statement;
        const std::string sql = std::string("INSERT INTO run_drafts (") + runDraftColumns
            + ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
        const auto prepared = prepare(handle, sql.c_str(), statement);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const auto bound = bindRunDraft(handle, statement.get(), draft);
        if (!bound.success()) return rollbackWith(*bound.error());
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
                return rollbackWith(error("run_draft.duplicate_id", "Run draft id already exists"));
            }
            return rollbackWith(
                databaseError(handle, "persistence.write_failed", "Could not insert run draft"));
        }
    }

    for (const auto& assignment : draft.roster) {
        const auto inserted = insertRunDraftAssignmentRow(handle, draft.id, assignment);
        if (!inserted.success()) return rollbackWith(*inserted.error());
    }

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<RunDraft>::ok(std::move(draft));
}

common::Result<RunDraft> SqliteRunDraftRepository::update(RunDraft draft)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return common::Result<RunDraft>::failure(*began.error());

    auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return common::Result<RunDraft>::failure(failure);
    };

    // The group and the plan provenance never change: read what is stored (inside the transaction, so nothing
    // can move between the check and the write) and refuse a draft that says otherwise. The UPDATE below does
    // not even name those columns, and the v62 triggers refuse them for any other writer.
    {
        Statement stored;
        const auto prepared = prepare(handle,
            "SELECT project_id, group_id, group_ordinal, plan_key, depends_on_json, created_content_fingerprint "
            "FROM run_drafts WHERE id=?;",
            stored);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const auto bound = bindText(handle, stored.get(), 1, draft.id);
        if (!bound.success()) return rollbackWith(*bound.error());
        const int step = sqlite3_step(stored.get());
        if (step == SQLITE_DONE) return rollbackWith(error("run_draft.not_found", "Run draft not found"));
        if (step != SQLITE_ROW) {
            return rollbackWith(databaseError(handle, "persistence.read_failed", "Could not read run draft"));
        }
        const auto provenance = readRunDraftProvenance(stored.get(), 1);
        if (!provenance.success()) return rollbackWith(*provenance.error());
        const auto unchanged = application::checkRunDraftProvenanceUnchanged(
            provenance.value(), columnText(stored.get(), 0), draft);
        if (!unchanged.success()) return rollbackWith(*unchanged.error());
    }

    {
        Statement statement;
        const auto prepared = prepare(handle,
            "UPDATE run_drafts SET name=?,project_id=?,title=?,task=?,workflow_id=?,launch_mode=?,"
            "execution_profile_id=?,max_parallelism=?,workspace_path=?,created_at_ms=?,updated_at_ms=?,"
            "verification_repair_policy=?,provider_budget_json=?,run_interface_contracts_json=? "
            "WHERE id=?;",
            statement);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const common::Result<void> bindings[] = {
            bindText(handle, statement.get(), 1, draft.name),
            bindText(handle, statement.get(), 2, draft.projectId),
            bindText(handle, statement.get(), 3, draft.title),
            bindText(handle, statement.get(), 4, draft.task),
            bindText(handle, statement.get(), 5, draft.workflowId),
            bindInt64(handle, statement.get(), 6, static_cast<std::int64_t>(draft.launchMode)),
            bindNullableText(handle, statement.get(), 7, draft.executionProfileId),
            bindUnsigned(handle, statement.get(), 8, draft.maxParallelism),
            bindNullableText(handle, statement.get(), 9,
                draft.workspacePath.has_value()
                    ? std::optional<std::string>(pathToUtf8(*draft.workspacePath))
                    : std::nullopt),
            bindInt64(handle, statement.get(), 10, toMilliseconds(draft.createdAt)),
            bindInt64(handle, statement.get(), 11, toMilliseconds(draft.updatedAt)),
            bindText(handle, statement.get(), 12,
                application::toString(draft.verificationRepairPolicy)),
            bindText(handle, statement.get(), 13, providerBudgetOverridesJson(draft.providerBudget)),
            bindText(handle, statement.get(), 14,
                application::serializeRunInterfaceContracts(draft.interfaceContracts)),
            bindText(handle, statement.get(), 15, draft.id)};
        for (const auto& binding : bindings) {
            if (!binding.success()) return rollbackWith(*binding.error());
        }
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            return rollbackWith(
                databaseError(handle, "persistence.write_failed", "Could not update run draft"));
        }
        if (sqlite3_changes(handle) == 0) {
            return rollbackWith(error("run_draft.not_found", "Run draft not found"));
        }
    }

    {
        Statement remove;
        const auto removePrepared = prepare(
            handle, "DELETE FROM run_draft_assignments WHERE draft_id=?;", remove);
        if (!removePrepared.success()) return rollbackWith(*removePrepared.error());
        const auto removeBound = bindText(handle, remove.get(), 1, draft.id);
        if (!removeBound.success()) return rollbackWith(*removeBound.error());
        if (sqlite3_step(remove.get()) != SQLITE_DONE) {
            return rollbackWith(databaseError(
                handle, "persistence.write_failed", "Could not replace run draft assignments"));
        }
    }

    for (const auto& assignment : draft.roster) {
        const auto inserted = insertRunDraftAssignmentRow(handle, draft.id, assignment);
        if (!inserted.success()) return rollbackWith(*inserted.error());
    }

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<RunDraft>::ok(std::move(draft));
}

common::Result<std::optional<RunDraft>> SqliteRunDraftRepository::findById(
    const application::RunDraftId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + runDraftColumns + " FROM run_drafts WHERE id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) {
        return common::Result<std::optional<RunDraft>>::failure(*prepared.error());
    }
    const auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success()) return common::Result<std::optional<RunDraft>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<RunDraft>>::ok(std::nullopt);
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<RunDraft>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read run draft"));
    }
    auto draft = readRunDraft(statement.get());
    if (!draft.success()) {
        return common::Result<std::optional<RunDraft>>::failure(*draft.error());
    }

    const auto roster = fetchAssignmentsForDraft(handle, id);
    if (!roster.success()) {
        return common::Result<std::optional<RunDraft>>::failure(*roster.error());
    }
    draft.value().roster = roster.value();
    return common::Result<std::optional<RunDraft>>::ok(draft.value());
}

common::Result<std::vector<RunDraft>> SqliteRunDraftRepository::listAll() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + runDraftColumns
        + " FROM run_drafts ORDER BY updated_at_ms DESC,id;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<RunDraft>>::failure(*prepared.error());

    std::vector<RunDraft> drafts;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<std::vector<RunDraft>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list run drafts"));
        }
        auto draft = readRunDraft(statement.get());
        if (!draft.success()) {
            return common::Result<std::vector<RunDraft>>::failure(*draft.error());
        }
        drafts.push_back(draft.value());
    }

    for (auto& draft : drafts) {
        const auto roster = fetchAssignmentsForDraft(handle, draft.id);
        if (!roster.success()) {
            return common::Result<std::vector<RunDraft>>::failure(*roster.error());
        }
        draft.roster = roster.value();
    }
    return common::Result<std::vector<RunDraft>>::ok(std::move(drafts));
}

common::Result<void> SqliteRunDraftRepository::remove(const application::RunDraftId& id)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    // One transaction: the draft, and - when it was the last one of its group - the group row. If the group
    // cannot be removed the draft stays too.
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return began;
    const auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return common::Result<void>::failure(failure);
    };

    std::optional<std::string> groupId;
    {
        Statement find;
        const auto prepared = prepare(handle, "SELECT group_id FROM run_drafts WHERE id=?;", find);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const auto bound = bindText(handle, find.get(), 1, id);
        if (!bound.success()) return rollbackWith(*bound.error());
        const int step = sqlite3_step(find.get());
        if (step == SQLITE_DONE) return rollbackWith(error("run_draft.not_found", "Run draft not found"));
        if (step != SQLITE_ROW) {
            return rollbackWith(databaseError(handle, "persistence.read_failed", "Could not read run draft"));
        }
        groupId = optionalText(find.get(), 0);
    }
    {
        Statement statement;
        const auto prepared = prepare(handle, "DELETE FROM run_drafts WHERE id=?;", statement);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const auto bound = bindText(handle, statement.get(), 1, id);
        if (!bound.success()) return rollbackWith(*bound.error());
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            return rollbackWith(databaseError(handle, "persistence.write_failed", "Could not remove run draft"));
        }
        if (sqlite3_changes(handle) == 0) return rollbackWith(error("run_draft.not_found", "Run draft not found"));
    }
    if (groupId.has_value()) {
        // The group row goes only when no draft references it any more.
        Statement statement;
        const auto prepared = prepare(handle,
            "DELETE FROM run_draft_groups WHERE id=? AND NOT EXISTS "
            "(SELECT 1 FROM run_drafts WHERE group_id=?);",
            statement);
        if (!prepared.success()) return rollbackWith(*prepared.error());
        const common::Result<void> bindings[] = {
            bindText(handle, statement.get(), 1, *groupId), bindText(handle, statement.get(), 2, *groupId)};
        for (const auto& binding : bindings) {
            if (!binding.success()) return rollbackWith(*binding.error());
        }
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            return rollbackWith(databaseError(handle, "persistence.write_failed", "Could not remove run draft group"));
        }
    }
    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<void>::ok();
}

namespace {

// The rows of ONE group and its drafts, written inside a transaction the CALLER owns (insertGroup's own, or the atomic
// Confirm of a follow-up request). The drafts must already be validated and sorted by ordinal.
common::Result<void> insertRunDraftGroupRows(
    sqlite3* handle, const application::RunDraftGroup& group, const std::vector<RunDraft>& drafts)
{
    {
        Statement statement;
        const std::string sql = std::string("INSERT INTO run_draft_groups (") + runDraftGroupColumns
            + ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?);";
        const auto prepared = prepare(handle, sql.c_str(), statement);
        if (!prepared.success()) return prepared;
        const auto bound = bindRunDraftGroup(handle, statement.get(), group);
        if (!bound.success()) return bound;
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            const int code = sqlite3_extended_errcode(handle);
            if (code == SQLITE_CONSTRAINT_PRIMARYKEY) {
                return common::Result<void>::failure(
                    error("run_draft_group.duplicate_id", "Run draft group id already exists"));
            }
            if (code == SQLITE_CONSTRAINT_UNIQUE) {
                // Two columns are unique: the idempotency key and (for a follow-up group) the request id.
                const std::string_view message(sqlite3_errmsg(handle));
                if (message.find("source_request_id") != std::string_view::npos) {
                    return common::Result<void>::failure(error("run_draft_group.duplicate_source_request",
                        "A group of this follow-up request already exists"));
                }
                return common::Result<void>::failure(error("run_draft_group.duplicate_idempotency_key",
                    "A group with this idempotency key already exists"));
            }
            return common::Result<void>::failure(
                databaseError(handle, "persistence.write_failed", "Could not insert run draft group"));
        }
    }

    for (const auto& draft : drafts) {
        {
            Statement statement;
            const std::string sql = std::string("INSERT INTO run_drafts (") + runDraftColumns
                + ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
            const auto prepared = prepare(handle, sql.c_str(), statement);
            if (!prepared.success()) return prepared;
            const auto bound = bindRunDraft(handle, statement.get(), draft);
            if (!bound.success()) return bound;
            if (sqlite3_step(statement.get()) != SQLITE_DONE) {
                if (sqlite3_extended_errcode(handle) == SQLITE_CONSTRAINT_PRIMARYKEY) {
                    return common::Result<void>::failure(error("run_draft.duplicate_id", "Run draft id already exists"));
                }
                return common::Result<void>::failure(
                    databaseError(handle, "persistence.write_failed", "Could not insert run draft"));
            }
        }
        for (const auto& assignment : draft.roster) {
            const auto inserted = insertRunDraftAssignmentRow(handle, draft.id, assignment);
            if (!inserted.success()) return inserted;
        }
    }
    return common::Result<void>::ok();
}

} // namespace

common::Result<std::vector<RunDraft>> SqliteRunDraftRepository::insertGroup(
    application::RunDraftGroup group, std::vector<RunDraft> drafts)
{
    using Drafts = std::vector<RunDraft>;
    // A follow-up group is born only from the Confirm of its request (IFollowUpPlanConfirmPort), never through this port.
    const auto allowed = application::checkRunDraftGroupPublicInsert(group);
    if (!allowed.success()) return common::Result<Drafts>::failure(*allowed.error());
    // Validated before the first write; a refusal never opens a transaction.
    const auto valid = application::validateRunDraftGroupInsertion(group, drafts);
    if (!valid.success()) return common::Result<Drafts>::failure(*valid.error());
    std::sort(drafts.begin(), drafts.end(), [](const RunDraft& left, const RunDraft& right) {
        return left.provenance->ordinal < right.provenance->ordinal;
    });

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    const auto began = execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) return common::Result<Drafts>::failure(*began.error());
    const auto rollbackWith = [&](const common::Error& failure) {
        static_cast<void>(execute(handle, "ROLLBACK;", "persistence.rollback_failed"));
        return common::Result<Drafts>::failure(failure);
    };

    const auto written = insertRunDraftGroupRows(handle, group, drafts);
    if (!written.success()) return rollbackWith(*written.error());

    const auto committed = execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) return rollbackWith(*committed.error());
    return common::Result<Drafts>::ok(std::move(drafts));
}

namespace {

common::Result<std::optional<application::RunDraftGroup>> findRunDraftGroupWhere(
    sqlite3* handle, const char* whereClause, const std::string& value)
{
    using Result = common::Result<std::optional<application::RunDraftGroup>>;
    Statement statement;
    const std::string sql = std::string("SELECT ") + runDraftGroupColumns + " FROM run_draft_groups WHERE "
        + whereClause + ";";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return Result::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, value);
    if (!bound.success()) return Result::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return Result::ok(std::nullopt);
    if (step != SQLITE_ROW) {
        return Result::failure(databaseError(handle, "persistence.read_failed", "Could not read run draft group"));
    }
    auto group = readRunDraftGroup(statement.get());
    if (!group.success()) return Result::failure(*group.error());
    return Result::ok(std::move(group.value()));
}

} // namespace

common::Result<std::optional<application::RunDraftGroup>> SqliteRunDraftRepository::findGroupById(
    const application::RunDraftGroupId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    return findRunDraftGroupWhere(database_->impl_->handle, "id=?", id);
}

common::Result<std::optional<application::RunDraftGroup>> SqliteRunDraftRepository::findGroupByIdempotencyKey(
    const std::string& idempotencyKey) const
{
    std::lock_guard lock(database_->impl_->mutex);
    return findRunDraftGroupWhere(database_->impl_->handle, "idempotency_key=?", idempotencyKey);
}

namespace {

// The drafts of one group (roster included) in ordinal order; the caller holds the database lock.
common::Result<std::vector<RunDraft>> readRunDraftGroupDrafts(sqlite3* handle, const application::RunDraftGroupId& id)
{
    using Drafts = std::vector<RunDraft>;
    Statement statement;
    const std::string sql = std::string("SELECT ") + runDraftColumns
        + " FROM run_drafts WHERE group_id=? ORDER BY group_ordinal;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<Drafts>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success()) return common::Result<Drafts>::failure(*bound.error());

    Drafts drafts;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            return common::Result<Drafts>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list run draft group"));
        }
        auto draft = readRunDraft(statement.get());
        if (!draft.success()) return common::Result<Drafts>::failure(*draft.error());
        drafts.push_back(std::move(draft.value()));
    }
    for (auto& draft : drafts) {
        const auto roster = fetchAssignmentsForDraft(handle, draft.id);
        if (!roster.success()) return common::Result<Drafts>::failure(*roster.error());
        draft.roster = roster.value();
    }
    return common::Result<Drafts>::ok(std::move(drafts));
}

} // namespace

common::Result<std::vector<RunDraft>> SqliteRunDraftRepository::listGroupDrafts(
    const application::RunDraftGroupId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    return readRunDraftGroupDrafts(database_->impl_->handle, id);
}

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

SqliteModelExecutionRepository::SqliteModelExecutionRepository(std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

namespace {

constexpr const char* modelExecutionColumns =
    "id,source_type,source_id,project_id,provider_id,model_id,role,position,attempt,status,"
    "request_hash,context_hash,output_artifact_id,output_content,output_preview,input_tokens,output_tokens,"
    "cost_micros,latency_ms,error_code,error_message,created_at_ms,started_at_ms,finished_at_ms,"
    "cost_price_known,cache_creation_input_tokens,cache_read_input_tokens,"
    "cache_creation_duration_partition_known,cache_creation_five_minute_input_tokens,"
    "cache_creation_one_hour_input_tokens";

common::Result<ModelExecution> readModelExecution(sqlite3_stmt* statement)
{
    const auto source = application::modelExecutionSourceFromString(columnText(statement, 1));
    const auto status = application::modelExecutionStatusFromString(columnText(statement, 9));
    if (!source || !status)
        return common::Result<ModelExecution>::failure(error("persistence.invalid_data", "Model execution contains an unknown enum value"));
    ModelExecution value;
    value.id = columnText(statement, 0);
    value.source = *source;
    value.sourceId = columnText(statement, 2);
    value.projectId = optionalText(statement, 3);
    value.providerId = columnText(statement, 4);
    value.modelId = columnText(statement, 5);
    value.role = columnText(statement, 6);
    value.position = static_cast<std::uint32_t>(sqlite3_column_int64(statement, 7));
    const auto attempt = sqlite3_column_int64(statement, 8);
    if (attempt <= 0) {
        return common::Result<ModelExecution>::failure(error(
            "persistence.invalid_data", "Model execution attempt must be positive"));
    }
    value.attempt = static_cast<std::uint32_t>(attempt);
    value.status = *status;
    value.requestHash = columnText(statement, 10);
    value.contextHash = columnText(statement, 11);
    value.outputArtifactId = optionalText(statement, 12);
    value.outputContent = columnText(statement, 13);
    value.outputPreview = columnText(statement, 14);
    value.inputTokens = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 15));
    value.outputTokens = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 16));
    value.costMicros = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 17));
    value.latencyMs = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 18));
    value.errorCode = columnText(statement, 19);
    value.errorMessage = columnText(statement, 20);
    value.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 21));
    value.startedAt = optionalTime(statement, 22);
    value.finishedAt = optionalTime(statement, 23);
    value.costPriceKnown = sqlite3_column_int(statement, 24) != 0;
    const std::int64_t cacheCreation = sqlite3_column_int64(statement, 25);
    const std::int64_t cacheRead = sqlite3_column_int64(statement, 26);
    const std::int64_t durationKnown = sqlite3_column_int64(statement, 27);
    const std::int64_t fiveMinute = sqlite3_column_int64(statement, 28);
    const std::int64_t oneHour = sqlite3_column_int64(statement, 29);
    if (cacheCreation < 0 || cacheRead < 0 || fiveMinute < 0 || oneHour < 0
        || (durationKnown != 0 && durationKnown != 1)) {
        return common::Result<ModelExecution>::failure(error(
            "persistence.invalid_data", "Model execution contains invalid cache usage"));
    }
    value.cacheCreationInputTokens = static_cast<std::uint64_t>(cacheCreation);
    value.cacheReadInputTokens = static_cast<std::uint64_t>(cacheRead);
    value.cacheCreationDuration.known = durationKnown != 0;
    value.cacheCreationDuration.fiveMinuteTokens = static_cast<std::uint64_t>(fiveMinute);
    value.cacheCreationDuration.oneHourTokens = static_cast<std::uint64_t>(oneHour);
    if (checkCacheCreationDurationUsage(
            value.cacheCreationInputTokens,
            value.cacheCreationDuration)
            != CacheCreationDurationUsageProblem::None
        || value.cacheCreationInputTokens > value.inputTokens
        || value.cacheReadInputTokens > value.inputTokens - value.cacheCreationInputTokens) {
        return common::Result<ModelExecution>::failure(error(
            "persistence.invalid_data", "Model execution contains inconsistent cache usage"));
    }
    return common::Result<ModelExecution>::ok(std::move(value));
}

common::Result<void> bindModelExecutionFields(sqlite3* handle, sqlite3_stmt* statement, const ModelExecution& value, int first)
{
    const auto durationProblem = checkCacheCreationDurationUsage(
        value.cacheCreationInputTokens, value.cacheCreationDuration);
    if (durationProblem != CacheCreationDurationUsageProblem::None) {
        return common::Result<void>::failure(error(
            cacheCreationDurationUsageProblemCode(durationProblem),
            cacheCreationDurationUsageProblemMessage(durationProblem)));
    }
    if (value.cacheCreationInputTokens > value.inputTokens
        || value.cacheReadInputTokens > value.inputTokens - value.cacheCreationInputTokens) {
        return common::Result<void>::failure(error(
            "model_execution.invalid_cache_usage",
            "Cache-creation and cache-read tokens must fit within input tokens"));
    }
    const common::Result<void> bindings[] = {
        bindText(handle, statement, first, std::string(application::toString(value.source))),
        bindText(handle, statement, first + 1, value.sourceId),
        bindNullableText(handle, statement, first + 2, value.projectId),
        bindText(handle, statement, first + 3, value.providerId),
        bindText(handle, statement, first + 4, value.modelId),
        bindText(handle, statement, first + 5, value.role),
        bindUnsigned(handle, statement, first + 6, value.position),
        bindUnsigned(handle, statement, first + 7, value.attempt),
        bindText(handle, statement, first + 8, std::string(application::toString(value.status))),
        bindText(handle, statement, first + 9, value.requestHash),
        bindText(handle, statement, first + 10, value.contextHash),
        bindNullableText(handle, statement, first + 11, value.outputArtifactId),
        bindText(handle, statement, first + 12, value.outputContent),
        bindText(handle, statement, first + 13, value.outputPreview),
        bindUnsigned(handle, statement, first + 14, value.inputTokens),
        bindUnsigned(handle, statement, first + 15, value.outputTokens),
        bindUnsigned(handle, statement, first + 16, value.costMicros),
        bindUnsigned(handle, statement, first + 17, value.latencyMs),
        bindText(handle, statement, first + 18, value.errorCode),
        bindText(handle, statement, first + 19, value.errorMessage),
        bindInt64(handle, statement, first + 20, toMilliseconds(value.createdAt)),
        bindOptionalTime(handle, statement, first + 21, value.startedAt),
        bindOptionalTime(handle, statement, first + 22, value.finishedAt),
        bindInt64(handle, statement, first + 23, value.costPriceKnown ? 1 : 0),
        bindUnsigned(handle, statement, first + 24, value.cacheCreationInputTokens),
        bindUnsigned(handle, statement, first + 25, value.cacheReadInputTokens),
        bindInt64(handle, statement, first + 26, value.cacheCreationDuration.known ? 1 : 0),
        bindUnsigned(handle, statement, first + 27,
            value.cacheCreationDuration.fiveMinuteTokens),
        bindUnsigned(handle, statement, first + 28,
            value.cacheCreationDuration.oneHourTokens)};
    for (const auto& binding : bindings) if (!binding.success()) return binding;
    return common::Result<void>::ok();
}

common::Result<QualityEvaluation> readQualityEvaluation(sqlite3_stmt* statement)
{
    const auto kind = application::qualityEvaluatorKindFromString(columnText(statement, 2));
    if (!kind) return common::Result<QualityEvaluation>::failure(error("persistence.invalid_data", "Quality evaluation contains an unknown evaluator kind"));
    QualityEvaluation value;
    value.id = columnText(statement, 0);
    value.executionId = columnText(statement, 1);
    value.evaluatorKind = *kind;
    value.evaluatorProviderId = optionalText(statement, 3);
    value.evaluatorModelId = optionalText(statement, 4);
    value.rubricVersion = columnText(statement, 5);
    value.scoreBasisPoints = static_cast<std::uint32_t>(sqlite3_column_int64(statement, 6));
    value.criteriaJson = columnText(statement, 7);
    value.explanation = columnText(statement, 8);
    value.evidence = columnText(statement, 9);
    value.createdAt = fromMilliseconds(sqlite3_column_int64(statement, 10));
    return common::Result<QualityEvaluation>::ok(std::move(value));
}

} // namespace

common::Result<ModelExecution> SqliteModelExecutionRepository::insertExecution(ModelExecution value)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle, "INSERT INTO model_executions (id,source_type,source_id,project_id,provider_id,model_id,role,position,attempt,status,request_hash,context_hash,output_artifact_id,output_content,output_preview,input_tokens,output_tokens,cost_micros,latency_ms,error_code,error_message,created_at_ms,started_at_ms,finished_at_ms,cost_price_known,cache_creation_input_tokens,cache_read_input_tokens,cache_creation_duration_partition_known,cache_creation_five_minute_input_tokens,cache_creation_one_hour_input_tokens) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);", statement);
    if (!prepared.success()) return common::Result<ModelExecution>::failure(*prepared.error());
    auto idBound = bindText(handle, statement.get(), 1, value.id);
    if (!idBound.success()) return common::Result<ModelExecution>::failure(*idBound.error());
    auto fieldsBound = bindModelExecutionFields(handle, statement.get(), value, 2);
    if (!fieldsBound.success()) return common::Result<ModelExecution>::failure(*fieldsBound.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE)
        return common::Result<ModelExecution>::failure(databaseError(handle, "persistence.write_failed", "Could not insert model execution"));
    return common::Result<ModelExecution>::ok(std::move(value));
}

common::Result<ModelExecution> SqliteModelExecutionRepository::updateExecution(ModelExecution value)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle, "UPDATE model_executions SET source_type=?,source_id=?,project_id=?,provider_id=?,model_id=?,role=?,position=?,attempt=?,status=?,request_hash=?,context_hash=?,output_artifact_id=?,output_content=?,output_preview=?,input_tokens=?,output_tokens=?,cost_micros=?,latency_ms=?,error_code=?,error_message=?,created_at_ms=?,started_at_ms=?,finished_at_ms=?,cost_price_known=?,cache_creation_input_tokens=?,cache_read_input_tokens=?,cache_creation_duration_partition_known=?,cache_creation_five_minute_input_tokens=?,cache_creation_one_hour_input_tokens=? WHERE id=?;", statement);
    if (!prepared.success()) return common::Result<ModelExecution>::failure(*prepared.error());
    auto fieldsBound = bindModelExecutionFields(handle, statement.get(), value, 1);
    if (!fieldsBound.success()) return common::Result<ModelExecution>::failure(*fieldsBound.error());
    auto idBound = bindText(handle, statement.get(), 30, value.id);
    if (!idBound.success()) return common::Result<ModelExecution>::failure(*idBound.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE)
        return common::Result<ModelExecution>::failure(databaseError(handle, "persistence.write_failed", "Could not update model execution"));
    if (sqlite3_changes(handle) == 0)
        return common::Result<ModelExecution>::failure(error("model_execution.not_found", "Execution not found"));
    return common::Result<ModelExecution>::ok(std::move(value));
}

common::Result<std::optional<ModelExecution>> SqliteModelExecutionRepository::findExecution(const application::ModelExecutionId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + modelExecutionColumns + " FROM model_executions WHERE id=?;";
    auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::optional<ModelExecution>>::failure(*prepared.error());
    auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success()) return common::Result<std::optional<ModelExecution>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return common::Result<std::optional<ModelExecution>>::ok(std::nullopt);
    if (step != SQLITE_ROW) return common::Result<std::optional<ModelExecution>>::failure(databaseError(handle, "persistence.read_failed", "Could not read model execution"));
    auto value = readModelExecution(statement.get());
    if (!value.success()) return common::Result<std::optional<ModelExecution>>::failure(*value.error());
    return common::Result<std::optional<ModelExecution>>::ok(value.value());
}

common::Result<std::vector<ModelExecution>> SqliteModelExecutionRepository::listExecutions(application::ModelExecutionSource source, const std::string& sourceId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + modelExecutionColumns + " FROM model_executions WHERE source_type=? AND source_id=? ORDER BY position,created_at_ms,id;";
    auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<ModelExecution>>::failure(*prepared.error());
    const common::Result<void> bindings[] = {bindText(handle, statement.get(), 1, std::string(application::toString(source))), bindText(handle, statement.get(), 2, sourceId)};
    for (const auto& binding : bindings) if (!binding.success()) return common::Result<std::vector<ModelExecution>>::failure(*binding.error());
    std::vector<ModelExecution> values;
    for (;;) { const int step = sqlite3_step(statement.get()); if (step == SQLITE_DONE) break; if (step != SQLITE_ROW) return common::Result<std::vector<ModelExecution>>::failure(databaseError(handle, "persistence.read_failed", "Could not list model executions")); auto value = readModelExecution(statement.get()); if (!value.success()) return common::Result<std::vector<ModelExecution>>::failure(*value.error()); values.push_back(std::move(value.value())); }
    return common::Result<std::vector<ModelExecution>>::ok(std::move(values));
}

common::Result<std::vector<ModelExecution>> SqliteModelExecutionRepository::listProjectExecutions(const application::ProjectId& projectId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + modelExecutionColumns + " FROM model_executions WHERE project_id=? ORDER BY created_at_ms,id;";
    auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<ModelExecution>>::failure(*prepared.error());
    auto bound = bindText(handle, statement.get(), 1, projectId);
    if (!bound.success()) return common::Result<std::vector<ModelExecution>>::failure(*bound.error());
    std::vector<ModelExecution> values;
    for (;;) { const int step = sqlite3_step(statement.get()); if (step == SQLITE_DONE) break; if (step != SQLITE_ROW) return common::Result<std::vector<ModelExecution>>::failure(databaseError(handle, "persistence.read_failed", "Could not list project executions")); auto value = readModelExecution(statement.get()); if (!value.success()) return common::Result<std::vector<ModelExecution>>::failure(*value.error()); values.push_back(std::move(value.value())); }
    return common::Result<std::vector<ModelExecution>>::ok(std::move(values));
}

common::Result<std::vector<ModelExecution>> SqliteModelExecutionRepository::listUnfinishedExecutions() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + modelExecutionColumns
        + " FROM model_executions WHERE status IN ('pending','running') ORDER BY created_at_ms,id;";
    auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success()) return common::Result<std::vector<ModelExecution>>::failure(*prepared.error());
    std::vector<ModelExecution> values;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW)
            return common::Result<std::vector<ModelExecution>>::failure(
                databaseError(handle, "persistence.read_failed", "Could not list unfinished model executions"));
        auto value = readModelExecution(statement.get());
        if (!value.success()) return common::Result<std::vector<ModelExecution>>::failure(*value.error());
        values.push_back(std::move(value.value()));
    }
    return common::Result<std::vector<ModelExecution>>::ok(std::move(values));
}

common::Result<QualityEvaluation> SqliteModelExecutionRepository::insertEvaluation(QualityEvaluation value)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    auto prepared = prepare(handle, "INSERT INTO quality_evaluations (id,execution_id,evaluator_kind,evaluator_provider_id,evaluator_model_id,rubric_version,score_basis_points,criteria_json,explanation,evidence,created_at_ms) VALUES (?,?,?,?,?,?,?,?,?,?,?);", statement);
    if (!prepared.success()) return common::Result<QualityEvaluation>::failure(*prepared.error());
    const common::Result<void> bindings[] = {bindText(handle,statement.get(),1,value.id),bindText(handle,statement.get(),2,value.executionId),bindText(handle,statement.get(),3,std::string(application::toString(value.evaluatorKind))),bindNullableText(handle,statement.get(),4,value.evaluatorProviderId),bindNullableText(handle,statement.get(),5,value.evaluatorModelId),bindText(handle,statement.get(),6,value.rubricVersion),bindUnsigned(handle,statement.get(),7,value.scoreBasisPoints),bindText(handle,statement.get(),8,value.criteriaJson),bindText(handle,statement.get(),9,value.explanation),bindText(handle,statement.get(),10,value.evidence),bindInt64(handle,statement.get(),11,toMilliseconds(value.createdAt))};
    for (const auto& binding : bindings) if (!binding.success()) return common::Result<QualityEvaluation>::failure(*binding.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) return common::Result<QualityEvaluation>::failure(databaseError(handle,"persistence.write_failed","Could not insert quality evaluation"));
    return common::Result<QualityEvaluation>::ok(std::move(value));
}

common::Result<std::optional<QualityEvaluation>> SqliteModelExecutionRepository::findEvaluation(const application::QualityEvaluationId& id) const
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* handle=database_->impl_->handle; Statement statement;
    auto prepared=prepare(handle,"SELECT id,execution_id,evaluator_kind,evaluator_provider_id,evaluator_model_id,rubric_version,score_basis_points,criteria_json,explanation,evidence,created_at_ms FROM quality_evaluations WHERE id=?;",statement); if(!prepared.success()) return common::Result<std::optional<QualityEvaluation>>::failure(*prepared.error()); auto bound=bindText(handle,statement.get(),1,id); if(!bound.success()) return common::Result<std::optional<QualityEvaluation>>::failure(*bound.error()); const int step=sqlite3_step(statement.get()); if(step==SQLITE_DONE) return common::Result<std::optional<QualityEvaluation>>::ok(std::nullopt); if(step!=SQLITE_ROW) return common::Result<std::optional<QualityEvaluation>>::failure(databaseError(handle,"persistence.read_failed","Could not read quality evaluation")); auto value=readQualityEvaluation(statement.get()); if(!value.success()) return common::Result<std::optional<QualityEvaluation>>::failure(*value.error()); return common::Result<std::optional<QualityEvaluation>>::ok(value.value());
}

common::Result<std::vector<QualityEvaluation>> SqliteModelExecutionRepository::listEvaluations(const application::ModelExecutionId& executionId) const
{
    std::lock_guard lock(database_->impl_->mutex); sqlite3* handle=database_->impl_->handle; Statement statement;
    // ORDER BY created_at_ms,rowid - not ,id. `id` is a random UUID
    // (generateUuid(), IdGenerator.cpp), which is not time-ordered; when two
    // evaluations for the same execution are inserted within the same
    // millisecond (common - two evaluations recorded back-to-back have no
    // reason to straddle a millisecond boundary), sorting by created_at_ms
    // then the random id ties on the first key and then orders by the
    // second key essentially randomly, silently reordering the two rows.
    // rowid is SQLite's own implicit, strictly-insertion-order column for
    // this ordinary (non-WITHOUT-ROWID) table - it is what "created_at_ms"
    // was actually trying to express as a tiebreaker, and a caller
    // reasonably expects listEvaluations() to come back in the order the
    // evaluations were actually added.
    auto prepared=prepare(handle,"SELECT id,execution_id,evaluator_kind,evaluator_provider_id,evaluator_model_id,rubric_version,score_basis_points,criteria_json,explanation,evidence,created_at_ms FROM quality_evaluations WHERE execution_id=? ORDER BY created_at_ms,rowid;",statement); if(!prepared.success()) return common::Result<std::vector<QualityEvaluation>>::failure(*prepared.error()); auto bound=bindText(handle,statement.get(),1,executionId); if(!bound.success()) return common::Result<std::vector<QualityEvaluation>>::failure(*bound.error()); std::vector<QualityEvaluation> values; for(;;){const int step=sqlite3_step(statement.get()); if(step==SQLITE_DONE) break; if(step!=SQLITE_ROW) return common::Result<std::vector<QualityEvaluation>>::failure(databaseError(handle,"persistence.read_failed","Could not list quality evaluations")); auto value=readQualityEvaluation(statement.get()); if(!value.success()) return common::Result<std::vector<QualityEvaluation>>::failure(*value.error()); values.push_back(std::move(value.value()));} return common::Result<std::vector<QualityEvaluation>>::ok(std::move(values));
}

SqliteComparisonSessionRepository::SqliteComparisonSessionRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<ComparisonSession> SqliteComparisonSessionRepository::insertSession(
    ComparisonSession value)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO comparison_sessions VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<ComparisonSession>::failure(*prepared.error());
    const std::string selection = serializeExecutionIds(value.synthesisSelection);
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, value.id),
        bindText(handle, statement.get(), 2, value.projectId),
        bindText(handle, statement.get(), 3, value.title),
        bindText(handle, statement.get(), 4, value.prompt),
        bindText(handle, statement.get(), 5, std::string(application::toString(value.status))),
        bindText(handle, statement.get(), 6, value.requestHash),
        bindText(handle, statement.get(), 7, value.contextHash),
        bindNullableText(handle, statement.get(), 8, value.preferredExecutionId),
        bindText(handle, statement.get(), 9, selection),
        bindNullableText(handle, statement.get(), 10, value.synthesisExecutionId),
        bindText(handle, statement.get(), 11, value.errorCode),
        bindText(handle, statement.get(), 12, value.errorMessage),
        bindInt64(handle, statement.get(), 13, toMilliseconds(value.createdAt)),
        bindOptionalTime(handle, statement.get(), 14, value.startedAt),
        bindOptionalTime(handle, statement.get(), 15, value.finishedAt),
        bindUnsigned(handle, statement.get(), 16, value.revision)};
    for (const auto& binding : bindings)
        if (!binding.success()) return common::Result<ComparisonSession>::failure(*binding.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<ComparisonSession>::failure(databaseError(
            handle, "persistence.write_failed", "Could not insert comparison session"));
    }
    return common::Result<ComparisonSession>::ok(std::move(value));
}

common::Result<ComparisonSession> SqliteComparisonSessionRepository::updateSession(
    ComparisonSession value)
{
    if (value.revision == 0) {
        return common::Result<ComparisonSession>::failure(
            error("comparison.revision_conflict", "Comparison revision did not advance"));
    }
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const auto prepared = prepare(handle,
        "UPDATE comparison_sessions SET project_id=?,title=?,prompt=?,status=?,"
        "request_hash=?,context_hash=?,preferred_execution_id=?,synthesis_selection_json=?,"
        "synthesis_execution_id=?,error_code=?,error_message=?,created_at_ms=?,started_at_ms=?,"
        "finished_at_ms=?,revision=? WHERE id=? AND revision=?;",
        statement);
    if (!prepared.success()) return common::Result<ComparisonSession>::failure(*prepared.error());
    const std::string selection = serializeExecutionIds(value.synthesisSelection);
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, value.projectId),
        bindText(handle, statement.get(), 2, value.title),
        bindText(handle, statement.get(), 3, value.prompt),
        bindText(handle, statement.get(), 4, std::string(application::toString(value.status))),
        bindText(handle, statement.get(), 5, value.requestHash),
        bindText(handle, statement.get(), 6, value.contextHash),
        bindNullableText(handle, statement.get(), 7, value.preferredExecutionId),
        bindText(handle, statement.get(), 8, selection),
        bindNullableText(handle, statement.get(), 9, value.synthesisExecutionId),
        bindText(handle, statement.get(), 10, value.errorCode),
        bindText(handle, statement.get(), 11, value.errorMessage),
        bindInt64(handle, statement.get(), 12, toMilliseconds(value.createdAt)),
        bindOptionalTime(handle, statement.get(), 13, value.startedAt),
        bindOptionalTime(handle, statement.get(), 14, value.finishedAt),
        bindUnsigned(handle, statement.get(), 15, value.revision),
        bindText(handle, statement.get(), 16, value.id),
        bindUnsigned(handle, statement.get(), 17, value.revision - 1)};
    for (const auto& binding : bindings)
        if (!binding.success()) return common::Result<ComparisonSession>::failure(*binding.error());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<ComparisonSession>::failure(databaseError(
            handle, "persistence.write_failed", "Could not update comparison session"));
    }
    if (sqlite3_changes(handle) != 1) {
        return common::Result<ComparisonSession>::failure(error(
            "comparison.revision_conflict", "Comparison session was changed concurrently or removed"));
    }
    return common::Result<ComparisonSession>::ok(std::move(value));
}

common::Result<std::optional<ComparisonSession>>
SqliteComparisonSessionRepository::findSession(
    const application::ComparisonSessionId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + comparisonSessionColumns
        + " FROM comparison_sessions WHERE id=?;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success())
        return common::Result<std::optional<ComparisonSession>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, id);
    if (!bound.success())
        return common::Result<std::optional<ComparisonSession>>::failure(*bound.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE)
        return common::Result<std::optional<ComparisonSession>>::ok(std::nullopt);
    if (step != SQLITE_ROW)
        return common::Result<std::optional<ComparisonSession>>::failure(databaseError(
            handle, "persistence.read_failed", "Could not read comparison session"));
    auto value = readComparisonSession(statement.get());
    if (!value.success())
        return common::Result<std::optional<ComparisonSession>>::failure(*value.error());
    return common::Result<std::optional<ComparisonSession>>::ok(value.value());
}

common::Result<std::vector<ComparisonSession>>
SqliteComparisonSessionRepository::listSessions(
    const std::optional<application::ProjectId>& projectId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + comparisonSessionColumns
        + (projectId.has_value()
            ? " FROM comparison_sessions WHERE project_id=? ORDER BY created_at_ms,id;"
            : " FROM comparison_sessions ORDER BY created_at_ms,id;");
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success())
        return common::Result<std::vector<ComparisonSession>>::failure(*prepared.error());
    if (projectId.has_value()) {
        const auto bound = bindText(handle, statement.get(), 1, *projectId);
        if (!bound.success())
            return common::Result<std::vector<ComparisonSession>>::failure(*bound.error());
    }
    std::vector<ComparisonSession> values;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW)
            return common::Result<std::vector<ComparisonSession>>::failure(databaseError(
                handle, "persistence.read_failed", "Could not list comparison sessions"));
        auto value = readComparisonSession(statement.get());
        if (!value.success())
            return common::Result<std::vector<ComparisonSession>>::failure(*value.error());
        values.push_back(std::move(value.value()));
    }
    return common::Result<std::vector<ComparisonSession>>::ok(std::move(values));
}

common::Result<std::vector<ComparisonSession>>
SqliteComparisonSessionRepository::listUnfinishedSessions() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    const std::string sql = std::string("SELECT ") + comparisonSessionColumns
        + " FROM comparison_sessions WHERE status IN ('pending','running') "
          "ORDER BY created_at_ms,id;";
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success())
        return common::Result<std::vector<ComparisonSession>>::failure(*prepared.error());
    std::vector<ComparisonSession> values;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW)
            return common::Result<std::vector<ComparisonSession>>::failure(databaseError(
                handle, "persistence.read_failed", "Could not list unfinished comparison sessions"));
        auto value = readComparisonSession(statement.get());
        if (!value.success())
            return common::Result<std::vector<ComparisonSession>>::failure(*value.error());
        values.push_back(std::move(value.value()));
    }
    return common::Result<std::vector<ComparisonSession>>::ok(std::move(values));
}

SqliteApplicationSettingsRepository::SqliteApplicationSettingsRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<application::ApplicationSettings>
SqliteApplicationSettingsRepository::load() const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    constexpr const char* sql =
        "SELECT theme_mode,interface_scale_percent,compact_mode,reduce_animations,"
        "run_context_policy,default_execution_profile_id,default_timeout_ms,"
        "default_max_parallel_agents,confirm_many_models,chat_context_policy,"
        "default_chat_provider_id,default_chat_model_id,chat_history_limit,revision,"
        "updated_at_ms FROM application_settings WHERE id=1;";
    const auto prepared = prepare(handle, sql, statement);
    if (!prepared.success())
        return common::Result<application::ApplicationSettings>::failure(*prepared.error());
    const int step = sqlite3_step(statement.get());
    if (step != SQLITE_ROW) {
        return common::Result<application::ApplicationSettings>::failure(databaseError(
            handle, "persistence.read_failed", "Could not read application settings"));
    }
    const auto theme = application::themePreferenceFromString(columnText(statement.get(), 0));
    const auto runPolicy = application::projectContextPolicyFromString(columnText(statement.get(), 4));
    const auto chatPolicy = application::projectContextPolicyFromString(columnText(statement.get(), 9));
    if (!theme.has_value() || !runPolicy.has_value() || !chatPolicy.has_value()) {
        return common::Result<application::ApplicationSettings>::failure(error(
            "persistence.invalid_data", "Application settings contain an unknown enum value"));
    }
    application::ApplicationSettings value;
    value.theme = *theme;
    value.interfaceScalePercent = static_cast<std::uint32_t>(sqlite3_column_int64(statement.get(), 1));
    value.compactMode = sqlite3_column_int(statement.get(), 2) != 0;
    value.reduceAnimations = sqlite3_column_int(statement.get(), 3) != 0;
    value.runContextPolicy = *runPolicy;
    value.defaultExecutionProfileId = optionalText(statement.get(), 5);
    value.defaultTimeoutMs = static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 6));
    value.defaultMaxParallelAgents = static_cast<std::uint32_t>(sqlite3_column_int64(statement.get(), 7));
    value.confirmManyModels = sqlite3_column_int(statement.get(), 8) != 0;
    value.chatContextPolicy = *chatPolicy;
    value.defaultChatProviderId = optionalText(statement.get(), 10);
    value.defaultChatModelId = optionalText(statement.get(), 11);
    value.chatHistoryLimit = static_cast<std::uint32_t>(sqlite3_column_int64(statement.get(), 12));
    value.revision = static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 13));
    value.updatedAt = fromMilliseconds(sqlite3_column_int64(statement.get(), 14));
    return common::Result<application::ApplicationSettings>::ok(std::move(value));
}

common::Result<application::ApplicationSettings>
SqliteApplicationSettingsRepository::save(application::ApplicationSettings value)
{
    if (value.revision == 0) {
        return common::Result<application::ApplicationSettings>::failure(error(
            "settings.revision_conflict", "Application settings revision did not advance"));
    }
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;
    Statement statement;
    constexpr const char* sql =
        "UPDATE application_settings SET theme_mode=?,interface_scale_percent=?,"
        "compact_mode=?,reduce_animations=?,run_context_policy=?,"
        "default_execution_profile_id=?,default_timeout_ms=?,"
        "default_max_parallel_agents=?,confirm_many_models=?,chat_context_policy=?,"
        "default_chat_provider_id=?,default_chat_model_id=?,chat_history_limit=?,"
        "revision=?,updated_at_ms=? WHERE id=1 AND revision=?;";
    const auto prepared = prepare(handle, sql, statement);
    if (!prepared.success())
        return common::Result<application::ApplicationSettings>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, std::string(application::toString(value.theme))),
        bindUnsigned(handle, statement.get(), 2, value.interfaceScalePercent),
        bindUnsigned(handle, statement.get(), 3, value.compactMode ? 1U : 0U),
        bindUnsigned(handle, statement.get(), 4, value.reduceAnimations ? 1U : 0U),
        bindText(handle, statement.get(), 5, std::string(application::toString(value.runContextPolicy))),
        bindNullableText(handle, statement.get(), 6, value.defaultExecutionProfileId),
        bindUnsigned(handle, statement.get(), 7, value.defaultTimeoutMs),
        bindUnsigned(handle, statement.get(), 8, value.defaultMaxParallelAgents),
        bindUnsigned(handle, statement.get(), 9, value.confirmManyModels ? 1U : 0U),
        bindText(handle, statement.get(), 10, std::string(application::toString(value.chatContextPolicy))),
        bindNullableText(handle, statement.get(), 11, value.defaultChatProviderId),
        bindNullableText(handle, statement.get(), 12, value.defaultChatModelId),
        bindUnsigned(handle, statement.get(), 13, value.chatHistoryLimit),
        bindUnsigned(handle, statement.get(), 14, value.revision),
        bindInt64(handle, statement.get(), 15, toMilliseconds(value.updatedAt)),
        bindUnsigned(handle, statement.get(), 16, value.revision - 1)};
    for (const auto& binding : bindings) {
        if (!binding.success())
            return common::Result<application::ApplicationSettings>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<application::ApplicationSettings>::failure(databaseError(
            handle, "persistence.write_failed", "Could not update application settings"));
    }
    if (sqlite3_changes(handle) != 1) {
        return common::Result<application::ApplicationSettings>::failure(error(
            "settings.revision_conflict", "Application settings were changed concurrently"));
    }
    return common::Result<application::ApplicationSettings>::ok(std::move(value));
}

// ============================================================
// SqliteChangeProposalRepository
// ============================================================

namespace {

// Read a ProposedChangeSet header row from a prepared statement.
// Column order must match changeSetColumns below.
ProposedChangeSet readChangeSet(sqlite3_stmt* stmt)
{
    ProposedChangeSet cs;
    cs.changeSetId           = columnText(stmt, 0);
    cs.runId                 = columnText(stmt, 1);
    cs.projectId             = columnText(stmt, 2);
    cs.providerId            = columnText(stmt, 3);
    cs.modelId               = columnText(stmt, 4);
    const std::string mexId  = columnText(stmt, 5);
    if (!mexId.empty()) cs.modelExecutionId = mexId;
    cs.agentRole             = columnText(stmt, 6);
    cs.summary               = columnText(stmt, 7);
    cs.baseRevision          = columnText(stmt, 8);
    cs.policySnapshotHash    = columnText(stmt, 9);
    cs.artifactRelativeRoot  = columnText(stmt, 10);
    cs.state                 = application::changeSetStateFromString(columnText(stmt, 11));
    cs.schemaVersion         = columnText(stmt, 12);
    cs.createdAt             = fromMilliseconds(sqlite3_column_int64(stmt, 13));
    cs.missingEvidenceCode    = optionalText(stmt, 14);
    cs.missingEvidenceMessage = optionalText(stmt, 15);
    cs.supersedesChangeSetId  = optionalText(stmt, 16);
    return cs;
}

constexpr const char* changeSetColumns =
    "id,run_id,project_id,provider_id,model_id,model_execution_id,"
    "agent_role,summary,base_revision,policy_snapshot_hash,"
    "artifact_relative_root,state,schema_version,created_at_ms,"
    "missing_evidence_code,missing_evidence_message,supersedes_change_set_id";

class ScopedTransactionRollback {
public:
    explicit ScopedTransactionRollback(sqlite3* handle)
        : handle_(handle)
    {
    }

    ~ScopedTransactionRollback()
    {
        if (active_) {
            static_cast<void>(
                execute(handle_, "ROLLBACK;", "persistence.rollback_failed"));
        }
    }

    void release() noexcept { active_ = false; }

private:
    sqlite3* handle_;
    bool active_ {true};
};

} // namespace

SqliteChangeProposalRepository::SqliteChangeProposalRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteChangeProposalRepository::insertChangeSet(
    const ProposedChangeSet& cs)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    const auto began =
        execute(handle, "BEGIN IMMEDIATE;", "persistence.transaction_failed");
    if (!began.success()) {
        return common::Result<void>::failure(*began.error());
    }
    ScopedTransactionRollback rollback {handle};

    // Insert the change-set header row.
    {
        Statement stmt;
        const auto prep = prepare(handle,
            "INSERT INTO proposed_change_sets "
            "(id,run_id,project_id,provider_id,model_id,model_execution_id,"
            "agent_role,summary,base_revision,policy_snapshot_hash,"
            "artifact_relative_root,state,schema_version,created_at_ms,"
            "missing_evidence_code,missing_evidence_message,supersedes_change_set_id) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
            stmt);
        if (!prep.success()) return common::Result<void>::failure(*prep.error());

        const std::string mexId = cs.modelExecutionId.has_value() ? *cs.modelExecutionId : "";
        const common::Result<void> bindings[] = {
            bindText(handle, stmt.get(), 1, cs.changeSetId),
            bindText(handle, stmt.get(), 2, cs.runId),
            bindText(handle, stmt.get(), 3, cs.projectId),
            bindText(handle, stmt.get(), 4, cs.providerId),
            bindText(handle, stmt.get(), 5, cs.modelId),
            bindText(handle, stmt.get(), 6, mexId),
            bindText(handle, stmt.get(), 7, cs.agentRole),
            bindText(handle, stmt.get(), 8, cs.summary),
            bindText(handle, stmt.get(), 9, cs.baseRevision),
            bindText(handle, stmt.get(), 10, cs.policySnapshotHash),
            bindText(handle, stmt.get(), 11, cs.artifactRelativeRoot),
            bindText(handle, stmt.get(), 12, application::toString(cs.state)),
            bindText(handle, stmt.get(), 13, cs.schemaVersion),
            bindInt64(handle, stmt.get(), 14, toMilliseconds(cs.createdAt)),
            bindNullableText(handle, stmt.get(), 15, cs.missingEvidenceCode),
            bindNullableText(handle, stmt.get(), 16, cs.missingEvidenceMessage),
            bindNullableText(handle, stmt.get(), 17, cs.supersedesChangeSetId)};
        for (const auto& b : bindings) {
            if (!b.success()) return common::Result<void>::failure(*b.error());
        }
        if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
            const int code = sqlite3_extended_errcode(handle);
            if (code == SQLITE_CONSTRAINT_PRIMARYKEY) {
                return common::Result<void>::failure(
                    error("proposal.duplicate_id", "Change set id already exists"));
            }
            return common::Result<void>::failure(
                databaseError(handle, "persistence.write_failed", "Could not insert change set"));
        }
    }

    // Insert per-change rows.
    for (std::size_t i = 0; i < cs.changes.size(); ++i) {
        const auto& ch = cs.changes[i];
        Statement stmt;
        const auto prep = prepare(handle,
            "INSERT INTO proposed_file_changes "
            "(id,change_set_id,change_id,operation,relative_path,source_path,expected_original_hash,ordinal) "
            "VALUES (?,?,?,?,?,?,?,?);",
            stmt);
        if (!prep.success()) return common::Result<void>::failure(*prep.error());

        const std::string srcPath = ch.sourcePath.has_value()
                                      ? pathToUtf8(*ch.sourcePath) : "";
        const std::string expHash = ch.expectedOriginalHash.value_or("");
        // Use changeSetId + ordinal as a globally-unique primary key.
        const std::string rowId = cs.changeSetId + "_" + std::to_string(i);
        const common::Result<void> bindings[] = {
            bindText(handle, stmt.get(), 1, rowId),
            bindText(handle, stmt.get(), 2, cs.changeSetId),
            bindText(handle, stmt.get(), 3, ch.changeId),
            bindText(handle, stmt.get(), 4, application::toString(ch.operation)),
            bindText(handle, stmt.get(), 5, pathToUtf8(ch.relativePath)),
            bindText(handle, stmt.get(), 6, srcPath),
            bindText(handle, stmt.get(), 7, expHash),
            bindInt64(handle, stmt.get(), 8, static_cast<std::int64_t>(i))};
        for (const auto& b : bindings) {
            if (!b.success()) return common::Result<void>::failure(*b.error());
        }
        if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
            return common::Result<void>::failure(
                databaseError(handle, "persistence.write_failed", "Could not insert file change"));
        }
    }

    // Insert validation findings.
    for (std::size_t i = 0; i < cs.validationFindings.size(); ++i) {
        const auto& f = cs.validationFindings[i];
        Statement stmt;
        const auto prep = prepare(handle,
            "INSERT INTO proposal_validation_findings "
            "(change_set_id,code,message,change_id,ordinal) VALUES (?,?,?,?,?);",
            stmt);
        if (!prep.success()) return common::Result<void>::failure(*prep.error());

        const common::Result<void> bindings[] = {
            bindText(handle, stmt.get(), 1, cs.changeSetId),
            bindText(handle, stmt.get(), 2, f.code),
            bindText(handle, stmt.get(), 3, f.message),
            bindText(handle, stmt.get(), 4, f.changeId),
            bindInt64(handle, stmt.get(), 5, static_cast<std::int64_t>(i))};
        for (const auto& b : bindings) {
            if (!b.success()) return common::Result<void>::failure(*b.error());
        }
        if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
            return common::Result<void>::failure(
                databaseError(handle, "persistence.write_failed", "Could not insert validation finding"));
        }
    }

    const auto committed =
        execute(handle, "COMMIT;", "persistence.commit_failed");
    if (!committed.success()) {
        return common::Result<void>::failure(*committed.error());
    }
    rollback.release();
    return common::Result<void>::ok();
}

common::Result<ProposedChangeSet> SqliteChangeProposalRepository::updateChangeSetState(
    const ChangeSetId& id, ChangeSetState newState)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    {
        Statement stmt;
        const auto prep = prepare(handle,
            "UPDATE proposed_change_sets SET state=? WHERE id=?;", stmt);
        if (!prep.success()) return common::Result<ProposedChangeSet>::failure(*prep.error());
        const common::Result<void> bindings[] = {
            bindText(handle, stmt.get(), 1, application::toString(newState)),
            bindText(handle, stmt.get(), 2, id)};
        for (const auto& b : bindings) {
            if (!b.success()) return common::Result<ProposedChangeSet>::failure(*b.error());
        }
        if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
            return common::Result<ProposedChangeSet>::failure(
                databaseError(handle, "persistence.write_failed", "Could not update change-set state"));
        }
        if (sqlite3_changes(handle) == 0) {
            return common::Result<ProposedChangeSet>::failure(
                error("proposal.not_found", "Change set not found: " + id));
        }
    }

    // Re-read the updated row.
    const std::string sql = std::string("SELECT ") + changeSetColumns
                          + " FROM proposed_change_sets WHERE id=?;";
    Statement stmt;
    const auto prep = prepare(handle, sql.c_str(), stmt);
    if (!prep.success()) return common::Result<ProposedChangeSet>::failure(*prep.error());
    const auto bound = bindText(handle, stmt.get(), 1, id);
    if (!bound.success()) return common::Result<ProposedChangeSet>::failure(*bound.error());
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return common::Result<ProposedChangeSet>::failure(
            error("proposal.not_found", "Change set disappeared after update: " + id));
    }
    return common::Result<ProposedChangeSet>::ok(readChangeSet(stmt.get()));
}

common::Result<void> SqliteChangeProposalRepository::markMissingEvidence(
    const ChangeSetId& id, std::string code, std::string message)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    Statement stmt;
    const auto prep = prepare(handle,
        "UPDATE proposed_change_sets SET missing_evidence_code=?,"
        "missing_evidence_message=? WHERE id=?;",
        stmt);
    if (!prep.success()) return common::Result<void>::failure(*prep.error());
    const common::Result<void> bindings[] = {
        bindText(handle, stmt.get(), 1, code),
        bindText(handle, stmt.get(), 2, message),
        bindText(handle, stmt.get(), 3, id)};
    for (const auto& b : bindings) {
        if (!b.success()) return common::Result<void>::failure(*b.error());
    }
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not record missing-evidence reason"));
    }
    if (sqlite3_changes(handle) == 0) {
        return common::Result<void>::failure(
            error("proposal.not_found", "Change set not found: " + id));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<ProposedChangeSet>> SqliteChangeProposalRepository::findChangeSet(
    const ChangeSetId& id) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    const std::string sql = std::string("SELECT ") + changeSetColumns
                          + " FROM proposed_change_sets WHERE id=?;";
    Statement stmt;
    const auto prep = prepare(handle, sql.c_str(), stmt);
    if (!prep.success()) {
        return common::Result<std::optional<ProposedChangeSet>>::failure(*prep.error());
    }
    const auto bound = bindText(handle, stmt.get(), 1, id);
    if (!bound.success()) {
        return common::Result<std::optional<ProposedChangeSet>>::failure(*bound.error());
    }
    const int step = sqlite3_step(stmt.get());
    if (step == SQLITE_DONE) {
        return common::Result<std::optional<ProposedChangeSet>>::ok(std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<ProposedChangeSet>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read change set"));
    }
    return common::Result<std::optional<ProposedChangeSet>>::ok(readChangeSet(stmt.get()));
}

common::Result<std::vector<ProposedChangeSet>> SqliteChangeProposalRepository::listForRun(
    const application::RunId& runId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    const std::string sql = std::string("SELECT ") + changeSetColumns
        + " FROM proposed_change_sets WHERE run_id=? ORDER BY created_at_ms ASC;";
    Statement stmt;
    const auto prep = prepare(handle, sql.c_str(), stmt);
    if (!prep.success()) {
        return common::Result<std::vector<ProposedChangeSet>>::failure(*prep.error());
    }
    const auto bound = bindText(handle, stmt.get(), 1, runId);
    if (!bound.success()) {
        return common::Result<std::vector<ProposedChangeSet>>::failure(*bound.error());
    }
    std::vector<ProposedChangeSet> result;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        result.push_back(readChangeSet(stmt.get()));
    }
    return common::Result<std::vector<ProposedChangeSet>>::ok(std::move(result));
}

common::Result<std::vector<ProposedChangeSet>> SqliteChangeProposalRepository::listForProject(
    const application::ProjectId& projectId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    const std::string sql = std::string("SELECT ") + changeSetColumns
        + " FROM proposed_change_sets WHERE project_id=? ORDER BY created_at_ms ASC;";
    Statement stmt;
    const auto prep = prepare(handle, sql.c_str(), stmt);
    if (!prep.success()) {
        return common::Result<std::vector<ProposedChangeSet>>::failure(*prep.error());
    }
    const auto bound = bindText(handle, stmt.get(), 1, projectId);
    if (!bound.success()) {
        return common::Result<std::vector<ProposedChangeSet>>::failure(*bound.error());
    }
    std::vector<ProposedChangeSet> result;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        result.push_back(readChangeSet(stmt.get()));
    }
    return common::Result<std::vector<ProposedChangeSet>>::ok(std::move(result));
}

common::Result<std::vector<ProposedFileChange>> SqliteChangeProposalRepository::listChanges(
    const ChangeSetId& changeSetId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    Statement stmt;
    const auto prep = prepare(handle,
        "SELECT change_id,change_set_id,operation,relative_path,source_path,"
        "expected_original_hash,ordinal "
        "FROM proposed_file_changes WHERE change_set_id=? ORDER BY ordinal ASC;",
        stmt);
    if (!prep.success()) {
        return common::Result<std::vector<ProposedFileChange>>::failure(*prep.error());
    }
    const auto bound = bindText(handle, stmt.get(), 1, changeSetId);
    if (!bound.success()) {
        return common::Result<std::vector<ProposedFileChange>>::failure(*bound.error());
    }

    std::vector<ProposedFileChange> result;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        ProposedFileChange ch;
        ch.changeId   = columnText(stmt.get(), 0);
        // column 1 is change_set_id (ignored)
        ch.operation  = application::fileChangeOperationFromString(columnText(stmt.get(), 2));
        ch.relativePath = pathFromUtf8(columnText(stmt.get(), 3));
        const std::string src = columnText(stmt.get(), 4);
        if (!src.empty()) ch.sourcePath = pathFromUtf8(src);
        const std::string hash = columnText(stmt.get(), 5);
        if (!hash.empty()) ch.expectedOriginalHash = hash;
        // proposedContent and capturedOriginalContent are NOT stored in SQLite.
        result.push_back(std::move(ch));
    }
    return common::Result<std::vector<ProposedFileChange>>::ok(std::move(result));
}

common::Result<std::vector<ValidationFinding>> SqliteChangeProposalRepository::listValidationFindings(
    const ChangeSetId& changeSetId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    Statement stmt;
    const auto prep = prepare(handle,
        "SELECT code,message,change_id FROM proposal_validation_findings "
        "WHERE change_set_id=? ORDER BY ordinal ASC;",
        stmt);
    if (!prep.success()) {
        return common::Result<std::vector<ValidationFinding>>::failure(*prep.error());
    }
    const auto bound = bindText(handle, stmt.get(), 1, changeSetId);
    if (!bound.success()) {
        return common::Result<std::vector<ValidationFinding>>::failure(*bound.error());
    }

    std::vector<ValidationFinding> result;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        ValidationFinding f;
        f.code     = columnText(stmt.get(), 0);
        f.message  = columnText(stmt.get(), 1);
        f.changeId = columnText(stmt.get(), 2);
        result.push_back(std::move(f));
    }
    return common::Result<std::vector<ValidationFinding>>::ok(std::move(result));
}

namespace {

// Conservative cap safely below any SQLite build's compiled-in
// SQLITE_LIMIT_VARIABLE_NUMBER (999 pre-3.32.0, 32766 by default since), so a
// batched IN(...) query below never risks "too many SQL variables" no matter
// how many ids a caller passes.
constexpr std::size_t kBatchedInQueryChunkSize = 500;

// Deduplicates (a repeated id must not produce repeated rows in the result,
// and would otherwise waste bound parameters/chunk headroom for nothing) and
// splits into chunks no larger than kBatchedInQueryChunkSize. A single id's
// rows always come from exactly one chunk's query - chunks partition the
// deduplicated id list - so each chunk's own "ORDER BY change_set_id,ordinal"
// is never interleaved with another chunk's rows for the same id.
std::vector<std::vector<ChangeSetId>> dedupedIdChunks(const std::vector<ChangeSetId>& ids)
{
    std::vector<ChangeSetId> unique;
    unique.reserve(ids.size());
    std::unordered_set<ChangeSetId> seen;
    seen.reserve(ids.size());
    for (const auto& id : ids) {
        if (seen.insert(id).second) unique.push_back(id);
    }
    std::vector<std::vector<ChangeSetId>> chunks;
    for (std::size_t offset = 0; offset < unique.size(); offset += kBatchedInQueryChunkSize) {
        const std::size_t count = std::min(kBatchedInQueryChunkSize, unique.size() - offset);
        chunks.emplace_back(
            unique.begin() + static_cast<std::ptrdiff_t>(offset),
            unique.begin() + static_cast<std::ptrdiff_t>(offset + count));
    }
    return chunks;
}

} // namespace

common::Result<std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>>>
SqliteChangeProposalRepository::listChangesForSets(
    const std::vector<ChangeSetId>& changeSetIds) const
{
    using ResultType = common::Result<std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>>>;
    std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>> byChangeSet;
    if (changeSetIds.empty()) {
        return ResultType::ok(std::move(byChangeSet));
    }

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    for (const auto& chunk : dedupedIdChunks(changeSetIds)) {
        std::string placeholders(chunk.size() * 2 - 1, ',');
        for (std::size_t i = 0; i < chunk.size(); ++i) placeholders[i * 2] = '?';

        Statement stmt;
        const std::string sql =
            "SELECT change_set_id,change_id,operation,relative_path,source_path,"
            "expected_original_hash,ordinal FROM proposed_file_changes WHERE change_set_id IN ("
            + placeholders + ") ORDER BY change_set_id,ordinal ASC;";
        const auto prep = prepare(handle, sql.c_str(), stmt);
        if (!prep.success()) return ResultType::failure(*prep.error());
        for (std::size_t i = 0; i < chunk.size(); ++i) {
            const auto bound = bindText(handle, stmt.get(), static_cast<int>(i) + 1, chunk[i]);
            if (!bound.success()) return ResultType::failure(*bound.error());
        }

        // Every non-ROW step() result must be checked explicitly: SQLITE_DONE
        // means the chunk finished cleanly, anything else (SQLITE_BUSY after
        // the busy-timeout elapses, SQLITE_IOERR, SQLITE_CORRUPT, ...) is a
        // real failure. Returning failure here discards this function's own
        // accumulated byChangeSet entirely - a caller never sees a partial
        // result silently reported as success.
        for (;;) {
            const int step = sqlite3_step(stmt.get());
            if (step == SQLITE_DONE) break;
            if (step != SQLITE_ROW) {
                return ResultType::failure(databaseError(
                    handle, "persistence.read_failed", "Could not read proposed file changes"));
            }
            const std::string changeSetId = columnText(stmt.get(), 0);
            ProposedFileChange ch;
            ch.changeId = columnText(stmt.get(), 1);
            ch.operation = application::fileChangeOperationFromString(columnText(stmt.get(), 2));
            ch.relativePath = pathFromUtf8(columnText(stmt.get(), 3));
            const std::string src = columnText(stmt.get(), 4);
            if (!src.empty()) ch.sourcePath = pathFromUtf8(src);
            const std::string hash = columnText(stmt.get(), 5);
            if (!hash.empty()) ch.expectedOriginalHash = hash;
            // proposedContent and capturedOriginalContent are NOT stored in SQLite.
            byChangeSet[changeSetId].push_back(std::move(ch));
        }
    }
    return ResultType::ok(std::move(byChangeSet));
}

common::Result<std::unordered_map<ChangeSetId, std::vector<ValidationFinding>>>
SqliteChangeProposalRepository::listValidationFindingsForSets(
    const std::vector<ChangeSetId>& changeSetIds) const
{
    using ResultType = common::Result<std::unordered_map<ChangeSetId, std::vector<ValidationFinding>>>;
    std::unordered_map<ChangeSetId, std::vector<ValidationFinding>> byChangeSet;
    if (changeSetIds.empty()) {
        return ResultType::ok(std::move(byChangeSet));
    }

    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    for (const auto& chunk : dedupedIdChunks(changeSetIds)) {
        std::string placeholders(chunk.size() * 2 - 1, ',');
        for (std::size_t i = 0; i < chunk.size(); ++i) placeholders[i * 2] = '?';

        Statement stmt;
        const std::string sql =
            "SELECT change_set_id,code,message,change_id FROM proposal_validation_findings "
            "WHERE change_set_id IN (" + placeholders + ") ORDER BY change_set_id,ordinal ASC;";
        const auto prep = prepare(handle, sql.c_str(), stmt);
        if (!prep.success()) return ResultType::failure(*prep.error());
        for (std::size_t i = 0; i < chunk.size(); ++i) {
            const auto bound = bindText(handle, stmt.get(), static_cast<int>(i) + 1, chunk[i]);
            if (!bound.success()) return ResultType::failure(*bound.error());
        }

        for (;;) {
            const int step = sqlite3_step(stmt.get());
            if (step == SQLITE_DONE) break;
            if (step != SQLITE_ROW) {
                return ResultType::failure(databaseError(
                    handle, "persistence.read_failed", "Could not read validation findings"));
            }
            const std::string changeSetId = columnText(stmt.get(), 0);
            ValidationFinding f;
            f.code = columnText(stmt.get(), 1);
            f.message = columnText(stmt.get(), 2);
            f.changeId = columnText(stmt.get(), 3);
            byChangeSet[changeSetId].push_back(std::move(f));
        }
    }
    return ResultType::ok(std::move(byChangeSet));
}

// ── SqliteChangeApprovalRepository ───────────────────────────────────────────

namespace {

std::string serializeApplyHashLog(const std::vector<ApplyHashEntry>& entries)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& entry : entries) {
        array.push_back({
            {"changeId", entry.changeId},
            {"relativePath", pathToUtf8(entry.relativePath)},
            {"originalHash", entry.originalHash.value_or(std::string {})},
            {"newHash", entry.newHash.value_or(std::string {})}
        });
    }
    return array.dump();
}

std::vector<ApplyHashEntry> deserializeApplyHashLog(const std::string& json)
{
    std::vector<ApplyHashEntry> result;
    if (json.empty()) return result;
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) return result;
    for (const auto& item : parsed) {
        ApplyHashEntry entry;
        entry.changeId = item.value("changeId", std::string {});
        entry.relativePath = pathFromUtf8(item.value("relativePath", std::string {}));
        const std::string originalHash = item.value("originalHash", std::string {});
        if (!originalHash.empty()) entry.originalHash = originalHash;
        const std::string newHash = item.value("newHash", std::string {});
        if (!newHash.empty()) entry.newHash = newHash;
        result.push_back(std::move(entry));
    }
    return result;
}

common::Result<ApprovalRecord> readApprovalRecord(sqlite3_stmt* statement)
{
    ApprovalRecord record;
    record.changeSetId = columnText(statement, 0);
    record.snapshotHash = columnText(statement, 1);
    record.approvedAt = fromMilliseconds(sqlite3_column_int64(statement, 2));
    record.approvedBy = optionalText(statement, 3);
    record.appliedAt = optionalTime(statement, 4);
    record.applyResult = optionalText(statement, 5);
    record.applyErrorCode = optionalText(statement, 6);
    record.applyHashLog = deserializeApplyHashLog(columnText(statement, 7));
    record.postApplyWorkspaceFingerprint = optionalText(statement, 8);
    return common::Result<ApprovalRecord>::ok(std::move(record));
}

constexpr const char* changeSetApprovalColumns =
    "change_set_id,snapshot_hash,approved_at_ms,approved_by,"
    "applied_at_ms,apply_result,apply_error_code,apply_hash_log,"
    "post_apply_workspace_fingerprint";

} // namespace

SqliteChangeApprovalRepository::SqliteChangeApprovalRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteChangeApprovalRepository::recordApproval(
    const ApprovalRecord& record)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO change_set_approvals "
        "(change_set_id,snapshot_hash,approved_at_ms,approved_by) VALUES (?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, record.changeSetId),
        bindText(handle, statement.get(), 2, record.snapshotHash),
        bindInt64(handle, statement.get(), 3, toMilliseconds(record.approvedAt)),
        bindNullableText(handle, statement.get(), 4, record.approvedBy)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        const int code = sqlite3_extended_errcode(handle);
        if (code == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<void>::failure(
                error("proposal.duplicate_id", "Change set already has an approval record"));
        }
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not insert approval record"));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<ApprovalRecord>> SqliteChangeApprovalRepository::findApproval(
    const ChangeSetId& changeSetId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    const std::string sql = std::string("SELECT ") + changeSetApprovalColumns
        + " FROM change_set_approvals WHERE change_set_id=?;";
    Statement statement;
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success())
        return common::Result<std::optional<ApprovalRecord>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, changeSetId);
    if (!bound.success())
        return common::Result<std::optional<ApprovalRecord>>::failure(*bound.error());

    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return common::Result<std::optional<ApprovalRecord>>::ok(std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<ApprovalRecord>>::failure(
            databaseError(handle, "persistence.read_failed", "Could not read approval record"));
    }
    auto record = readApprovalRecord(statement.get());
    if (!record.success())
        return common::Result<std::optional<ApprovalRecord>>::failure(*record.error());
    return common::Result<std::optional<ApprovalRecord>>::ok(std::move(record.value()));
}

common::Result<void> SqliteChangeApprovalRepository::recordApplyOutcome(
    const ChangeSetId& changeSetId,
    const std::string& result,
    const std::optional<std::string>& errorCode,
    const std::vector<ApplyHashEntry>& hashLog,
    const std::optional<std::string>& postApplyWorkspaceFingerprint)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    Statement statement;
    const auto prepared = prepare(handle,
        "UPDATE change_set_approvals SET applied_at_ms=?,apply_result=?,"
        "apply_error_code=?,apply_hash_log=?,post_apply_workspace_fingerprint=? "
        "WHERE change_set_id=?;",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindInt64(handle, statement.get(), 1, toMilliseconds(std::chrono::system_clock::now())),
        bindText(handle, statement.get(), 2, result),
        bindNullableText(handle, statement.get(), 3, errorCode),
        bindText(handle, statement.get(), 4, serializeApplyHashLog(hashLog)),
        bindNullableText(handle, statement.get(), 5, postApplyWorkspaceFingerprint),
        bindText(handle, statement.get(), 6, changeSetId)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return common::Result<void>::failure(
            databaseError(handle, "persistence.write_failed", "Could not record apply outcome"));
    }
    if (sqlite3_changes(handle) == 0) {
        return common::Result<void>::failure(
            error("proposal.not_found", "No approval record exists for this change set"));
    }
    return common::Result<void>::ok();
}

// ── SqliteProposalReviewRepository ───────────────────────────────────────────

namespace {

constexpr const char* proposalReviewRejectionColumns =
    "change_set_id,rejected_at_ms,rejected_by,reason";

common::Result<ProposalReviewRejection> readProposalReviewRejection(sqlite3_stmt* statement)
{
    ProposalReviewRejection record;
    record.changeSetId = columnText(statement, 0);
    record.rejectedAt = fromMilliseconds(sqlite3_column_int64(statement, 1));
    record.rejectedBy = optionalText(statement, 2);
    record.reason = optionalText(statement, 3);
    return common::Result<ProposalReviewRejection>::ok(std::move(record));
}

} // namespace

SqliteProposalReviewRepository::SqliteProposalReviewRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteProposalReviewRepository::recordRejection(
    const ProposalReviewRejection& record)
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    Statement statement;
    const auto prepared = prepare(handle,
        "INSERT INTO change_set_reviewer_rejections "
        "(change_set_id,rejected_at_ms,rejected_by,reason) VALUES (?,?,?,?);",
        statement);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
    const common::Result<void> bindings[] = {
        bindText(handle, statement.get(), 1, record.changeSetId),
        bindInt64(handle, statement.get(), 2, toMilliseconds(record.rejectedAt)),
        bindNullableText(handle, statement.get(), 3, record.rejectedBy),
        bindNullableText(handle, statement.get(), 4, record.reason)};
    for (const auto& binding : bindings) {
        if (!binding.success()) return common::Result<void>::failure(*binding.error());
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        const int code = sqlite3_extended_errcode(handle);
        if (code == SQLITE_CONSTRAINT_PRIMARYKEY) {
            return common::Result<void>::failure(
                error("proposal.duplicate_id",
                    "Change set already has a reviewer rejection record"));
        }
        return common::Result<void>::failure(databaseError(
            handle, "persistence.write_failed", "Could not insert reviewer rejection record"));
    }
    return common::Result<void>::ok();
}

common::Result<std::optional<ProposalReviewRejection>>
SqliteProposalReviewRepository::findRejection(const ChangeSetId& changeSetId) const
{
    std::lock_guard lock(database_->impl_->mutex);
    sqlite3* handle = database_->impl_->handle;

    const std::string sql = std::string("SELECT ") + proposalReviewRejectionColumns
        + " FROM change_set_reviewer_rejections WHERE change_set_id=?;";
    Statement statement;
    const auto prepared = prepare(handle, sql.c_str(), statement);
    if (!prepared.success())
        return common::Result<std::optional<ProposalReviewRejection>>::failure(*prepared.error());
    const auto bound = bindText(handle, statement.get(), 1, changeSetId);
    if (!bound.success())
        return common::Result<std::optional<ProposalReviewRejection>>::failure(*bound.error());

    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return common::Result<std::optional<ProposalReviewRejection>>::ok(std::nullopt);
    }
    if (step != SQLITE_ROW) {
        return common::Result<std::optional<ProposalReviewRejection>>::failure(databaseError(
            handle, "persistence.read_failed", "Could not read reviewer rejection record"));
    }
    auto record = readProposalReviewRejection(statement.get());
    if (!record.success())
        return common::Result<std::optional<ProposalReviewRejection>>::failure(*record.error());
    return common::Result<std::optional<ProposalReviewRejection>>::ok(std::move(record.value()));
}

// ── SqliteAgentToolSessionRepository ─────────────────────────────────────────

namespace {

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

std::string agentToolCallStateToString(application::AgentToolCallState s)
{
    switch (s) {
    case application::AgentToolCallState::Requested:  return "requested";
    case application::AgentToolCallState::Running:    return "running";
    case application::AgentToolCallState::Completed:  return "completed";
    case application::AgentToolCallState::Failed:     return "failed";
    case application::AgentToolCallState::Interrupted:return "interrupted";
    }
    return "requested";
}

std::optional<application::AgentToolCallState> agentToolCallStateFromString(
    std::string_view v)
{
    if (v == "requested")  return application::AgentToolCallState::Requested;
    if (v == "running")    return application::AgentToolCallState::Running;
    if (v == "completed")  return application::AgentToolCallState::Completed;
    if (v == "failed")     return application::AgentToolCallState::Failed;
    if (v == "interrupted")return application::AgentToolCallState::Interrupted;
    return std::nullopt;
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

namespace {
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

// ── SqliteAgentToolAuditRepository ───────────────────────────────────────────

SqliteAgentToolAuditRepository::SqliteAgentToolAuditRepository(
    std::shared_ptr<SqliteDatabase> database)
    : database_(std::move(database))
{
}

common::Result<void> SqliteAgentToolAuditRepository::insert(
    const application::AgentToolAuditRecord& record)
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(INSERT INTO agent_tool_calls
              (id,session_id,run_id,provider_call_id,tool_name,state,
               arguments_sha256,result_sha256,result_artifact_id,
               error_code,returned_bytes,truncated,created_at_ms,finished_at_ms)
              VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?))sql",
        stmt);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    auto* s = stmt.get();
    if (auto r = bindText(h,s,1,record.auditId);                              !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,2,record.sessionId);                            !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,3,record.runId);                                !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,4,record.providerCallId);                       !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,5,record.toolName);                             !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,6,agentToolCallStateToString(record.state));    !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,7,record.argumentsSha256);                      !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,8,record.resultSha256);                         !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindNullableText(h,s,9,record.resultArtifactId);             !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,10,record.errorCode);                           !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,11,static_cast<std::uint64_t>(record.returnedBytes)); !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindInt64(h,s,12,record.truncated ? 1 : 0);                  !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindInt64(h,s,13,toMilliseconds(record.createdAt));          !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindOptionalTime(h,s,14,record.finishedAt);                  !r.success()) return common::Result<void>::failure(*r.error());

    if (sqlite3_step(s) != SQLITE_DONE)
        return common::Result<void>::failure(
            databaseError(h, "persistence.insert_failed", "agent_tool_calls insert"));

    return common::Result<void>::ok();
}

common::Result<void> SqliteAgentToolAuditRepository::update(
    const application::AgentToolAuditRecord& record)
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(UPDATE agent_tool_calls SET
              session_id=?,run_id=?,provider_call_id=?,tool_name=?,state=?,
              arguments_sha256=?,result_sha256=?,result_artifact_id=?,
              error_code=?,returned_bytes=?,truncated=?,
              created_at_ms=?,finished_at_ms=?
              WHERE id=?)sql",
        stmt);
    if (!prepared.success()) return common::Result<void>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    auto* s = stmt.get();
    if (auto r = bindText(h,s,1,record.sessionId);                            !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,2,record.runId);                                !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,3,record.providerCallId);                       !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,4,record.toolName);                             !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,5,agentToolCallStateToString(record.state));    !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,6,record.argumentsSha256);                      !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,7,record.resultSha256);                         !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindNullableText(h,s,8,record.resultArtifactId);             !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,9,record.errorCode);                            !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindUnsigned(h,s,10,static_cast<std::uint64_t>(record.returnedBytes)); !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindInt64(h,s,11,record.truncated ? 1 : 0);                  !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindInt64(h,s,12,toMilliseconds(record.createdAt));          !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindOptionalTime(h,s,13,record.finishedAt);                  !r.success()) return common::Result<void>::failure(*r.error());
    if (auto r = bindText(h,s,14,record.auditId);                             !r.success()) return common::Result<void>::failure(*r.error());

    if (sqlite3_step(s) != SQLITE_DONE)
        return common::Result<void>::failure(
            databaseError(h, "persistence.update_failed", "agent_tool_calls update"));

    if (sqlite3_changes(h) == 0)
        return common::Result<void>::failure(
            error("agent_call.not_found", "No agent_tool_calls row with that id"));

    return common::Result<void>::ok();
}

namespace {
application::AgentToolAuditRecord readAgentToolAuditRecord(sqlite3_stmt* s)
{
    application::AgentToolAuditRecord r;
    r.auditId          = columnText(s, 0);
    r.sessionId        = columnText(s, 1);
    r.runId            = columnText(s, 2);
    r.providerCallId   = columnText(s, 3);
    r.toolName         = columnText(s, 4);
    const auto state   = agentToolCallStateFromString(columnText(s, 5));
    r.state            = state.value_or(application::AgentToolCallState::Requested);
    r.argumentsSha256  = columnText(s, 6);
    r.resultSha256     = columnText(s, 7);
    r.resultArtifactId = optionalText(s, 8);
    r.errorCode        = columnText(s, 9);
    r.returnedBytes    = static_cast<std::size_t>(sqlite3_column_int64(s, 10));
    r.truncated        = sqlite3_column_int64(s, 11) != 0;
    r.createdAt        = fromMilliseconds(sqlite3_column_int64(s, 12));
    r.finishedAt       = optionalTime(s, 13);
    return r;
}
} // namespace

common::Result<std::optional<application::AgentToolAuditRecord>>
SqliteAgentToolAuditRepository::find(const std::string& auditId) const
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(SELECT id,session_id,run_id,provider_call_id,tool_name,state,
                     arguments_sha256,result_sha256,result_artifact_id,
                     error_code,returned_bytes,truncated,created_at_ms,finished_at_ms
              FROM agent_tool_calls WHERE id=?)sql",
        stmt);
    if (!prepared.success())
        return common::Result<std::optional<application::AgentToolAuditRecord>>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    if (auto r = bindText(h, stmt.get(), 1, auditId); !r.success())
        return common::Result<std::optional<application::AgentToolAuditRecord>>::failure(*r.error());

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE)
        return common::Result<std::optional<application::AgentToolAuditRecord>>::ok(std::nullopt);
    if (rc != SQLITE_ROW)
        return common::Result<std::optional<application::AgentToolAuditRecord>>::failure(
            databaseError(h, "persistence.query_failed", "agent_tool_calls find"));

    return common::Result<std::optional<application::AgentToolAuditRecord>>::ok(
        readAgentToolAuditRecord(stmt.get()));
}

common::Result<std::vector<application::AgentToolAuditRecord>>
SqliteAgentToolAuditRepository::listForSession(const std::string& sessionId) const
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(SELECT id,session_id,run_id,provider_call_id,tool_name,state,
                     arguments_sha256,result_sha256,result_artifact_id,
                     error_code,returned_bytes,truncated,created_at_ms,finished_at_ms
              FROM agent_tool_calls WHERE session_id=? ORDER BY created_at_ms ASC)sql",
        stmt);
    if (!prepared.success())
        return common::Result<std::vector<application::AgentToolAuditRecord>>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    if (auto r = bindText(h, stmt.get(), 1, sessionId); !r.success())
        return common::Result<std::vector<application::AgentToolAuditRecord>>::failure(*r.error());

    std::vector<application::AgentToolAuditRecord> result;
    int rc;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW)
        result.push_back(readAgentToolAuditRecord(stmt.get()));

    if (rc != SQLITE_DONE)
        return common::Result<std::vector<application::AgentToolAuditRecord>>::failure(
            databaseError(h, "persistence.query_failed", "agent_tool_calls listForSession"));

    return common::Result<std::vector<application::AgentToolAuditRecord>>::ok(std::move(result));
}

common::Result<std::size_t> SqliteAgentToolAuditRepository::interruptRunning(
    const std::string& sessionId)
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    const std::int64_t nowMs = toMilliseconds(std::chrono::system_clock::now());
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(UPDATE agent_tool_calls
              SET state='interrupted', finished_at_ms=?
              WHERE session_id=? AND state IN ('requested','running'))sql",
        stmt);
    if (!prepared.success()) return common::Result<std::size_t>::failure(*prepared.error());

    auto* h = database_->impl_->handle;
    if (auto r = bindInt64(h, stmt.get(), 1, nowMs);       !r.success()) return common::Result<std::size_t>::failure(*r.error());
    if (auto r = bindText(h, stmt.get(), 2, sessionId);    !r.success()) return common::Result<std::size_t>::failure(*r.error());

    if (sqlite3_step(stmt.get()) != SQLITE_DONE)
        return common::Result<std::size_t>::failure(
            databaseError(h, "persistence.update_failed", "agent_tool_calls interruptRunning"));

    return common::Result<std::size_t>::ok(
        static_cast<std::size_t>(sqlite3_changes(h)));
}

common::Result<std::size_t> SqliteAgentToolAuditRepository::interruptAllRunning()
{
    std::lock_guard<std::mutex> lock(database_->impl_->mutex);
    const std::int64_t nowMs = toMilliseconds(std::chrono::system_clock::now());
    Statement stmt;
    auto prepared = prepare(database_->impl_->handle,
        R"sql(UPDATE agent_tool_calls
              SET state='interrupted', finished_at_ms=?
              WHERE state IN ('requested','running'))sql",
        stmt);
    if (!prepared.success()) return common::Result<std::size_t>::failure(*prepared.error());

    if (auto r = bindInt64(database_->impl_->handle, stmt.get(), 1, nowMs); !r.success())
        return common::Result<std::size_t>::failure(*r.error());

    if (sqlite3_step(stmt.get()) != SQLITE_DONE)
        return common::Result<std::size_t>::failure(
            databaseError(database_->impl_->handle,
                "persistence.update_failed", "agent_tool_calls interruptAllRunning"));

    return common::Result<std::size_t>::ok(
        static_cast<std::size_t>(sqlite3_changes(database_->impl_->handle)));
}

} // namespace ai::infrastructure
