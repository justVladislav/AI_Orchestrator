#include "SqliteSchema.hpp"
#include "SqlitePrimitives.hpp"

#include "ai/application/catalog/ProviderCatalogTypes.hpp"

#include <sqlite3.h>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace ai::infrastructure {

namespace {

using application::LongContextPricing;
using application::LongContextPricingProblem;

using sqlite_detail::Statement;
using sqlite_detail::error;
using sqlite_detail::databaseError;
using sqlite_detail::execute;
using sqlite_detail::prepare;
using sqlite_detail::columnText;
using sqlite_detail::bindText;

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

} // namespace

namespace sqlite_detail {

common::Result<void> ensureSchema(sqlite3* handle)
{
    int schemaVersion = 0;
    {
        // Finalize the PRAGMA statement before beginning a migration transaction.
        Statement versionStatement;
        const auto versionPrepared = prepare(handle, "PRAGMA user_version;", versionStatement);
        if (!versionPrepared.success()) {
            return common::Result<void>::failure(*versionPrepared.error());
        }
        if (sqlite3_step(versionStatement.get()) != SQLITE_ROW) {
            return common::Result<void>::failure(
                databaseError(handle, "migration.version_failed", "Could not read schema version"));
        }
        schemaVersion = sqlite3_column_int(versionStatement.get(), 0);
    }
    if (schemaVersion > supportedSchemaVersion) {
        return common::Result<void>::failure(error(
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
        const auto migrated = execute(handle, migration, "migration.v1_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v2_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v3_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v4_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v5_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v6_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v7_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v8_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v9_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v10_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v11_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v12_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
            handle, "PRAGMA foreign_keys = OFF;", "migration.v13_foreign_keys_off_failed");
        if (!foreignKeysOff.success()) {
            return common::Result<void>::failure(*foreignKeysOff.error());
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
        const auto migrated = execute(handle, migration, "migration.v13_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            static_cast<void>(execute(
                handle, "PRAGMA foreign_keys = ON;", "migration.foreign_keys_restore_failed"));
            return common::Result<void>::failure(*migrated.error());
        }
        const auto foreignKeysOn = execute(
            handle, "PRAGMA foreign_keys = ON;", "migration.v13_foreign_keys_on_failed");
        if (!foreignKeysOn.success()) {
            return common::Result<void>::failure(*foreignKeysOn.error());
        }
        Statement foreignKeyCheck;
        const auto foreignKeyCheckPrepared = prepare(
            handle, "PRAGMA foreign_key_check;", foreignKeyCheck);
        if (!foreignKeyCheckPrepared.success()) {
            return common::Result<void>::failure(
                *foreignKeyCheckPrepared.error());
        }
        const int foreignKeyCheckStep = sqlite3_step(foreignKeyCheck.get());
        if (foreignKeyCheckStep == SQLITE_ROW) {
            return common::Result<void>::failure(error(
                "migration.v13_foreign_key_check_failed",
                "Database contains an invalid foreign-key reference after migration"));
        }
        if (foreignKeyCheckStep != SQLITE_DONE) {
            return common::Result<void>::failure(databaseError(
                handle,
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
        const auto migrated = execute(handle, migration, "migration.v14_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v15_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v16_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v17_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v18_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
            const auto prepared = prepare(handle, "PRAGMA table_info(runs);", info);
            if (!prepared.success()) {
                return common::Result<void>::failure(*prepared.error());
            }
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<void>::failure(databaseError(
                        handle, "migration.v19_failed", "Could not inspect runs table schema"));
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
        const auto migrated = execute(handle, migration.c_str(), "migration.v19_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v20_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v21_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v22_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
            const auto prepared = prepare(handle, "PRAGMA table_info(run_stage_assignments);", info);
            if (!prepared.success()) {
                return common::Result<void>::failure(*prepared.error());
            }
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<void>::failure(databaseError(
                        handle, "migration.v23_failed",
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
        const auto migrated = execute(handle, migration.c_str(), "migration.v23_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v24_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v25_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v26_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
            const auto prepared = prepare(handle, "PRAGMA table_info(proposed_change_sets);", info);
            if (!prepared.success()) return common::Result<void>::failure(*prepared.error());
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<void>::failure(databaseError(
                        handle, "migration.v27_failed", "Could not inspect proposed_change_sets table schema"));
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
        const auto migrated = execute(handle, migration.c_str(), "migration.v27_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v28_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v29_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v30_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v31_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v32_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 33) {
        // Existing rows were created when evidence-backed planner no-op was
        // always accepted. Preserve their exact Retry semantics, while every
        // new Run explicitly writes the safe RequireManifest default.
        bool hasPlanningCompletionPolicyColumn = false;
        {
            Statement info;
            const auto prepared = prepare(handle, "PRAGMA table_info(runs);", info);
            if (!prepared.success()) {
                return common::Result<void>::failure(*prepared.error());
            }
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<void>::failure(databaseError(
                        handle, "migration.v33_failed",
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
            handle, migration.c_str(), "migration.v33_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v34_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
                handle, "PRAGMA table_info(manifest_work_ledger_entries);", info);
            if (!prepared.success()) {
                return common::Result<void>::failure(*prepared.error());
            }
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<void>::failure(databaseError(
                        handle, "migration.v35_failed",
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
        const auto migrated = execute(handle, migration.c_str(), "migration.v35_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
                handle, "PRAGMA table_info(change_set_approvals);", info);
            if (!prepared.success()) {
                return common::Result<void>::failure(*prepared.error());
            }
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<void>::failure(databaseError(
                        handle, "migration.v36_failed",
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
        const auto migrated = execute(handle, migration.c_str(), "migration.v36_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
                handle, "PRAGMA table_info(manifest_task_executions);", info);
            if (!prepared.success()) {
                return common::Result<void>::failure(*prepared.error());
            }
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<void>::failure(databaseError(
                        handle, "migration.v37_failed",
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
        const auto migrated = execute(handle, migration.c_str(), "migration.v37_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
                handle, "PRAGMA table_info(manifest_work_ledger_entries);", info);
            if (!prepared.success()) {
                return common::Result<void>::failure(*prepared.error());
            }
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<void>::failure(databaseError(
                        handle, "migration.v38_failed",
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
        const auto migrated = execute(handle, migration.c_str(), "migration.v38_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 39) {
        bool hasWorkspaceSnapshotColumn = false;
        {
            Statement info;
            const auto prepared = prepare(
                handle, "PRAGMA table_info(manifest_work_ledger_entries);", info);
            if (!prepared.success())
                return common::Result<void>::failure(*prepared.error());
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<void>::failure(databaseError(
                        handle, "migration.v39_failed",
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
        const auto migrated = execute(handle, migration.c_str(), "migration.v39_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration.c_str(), "migration.v40_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration.c_str(), "migration.v41_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration.c_str(), "migration.v42_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
            const auto prepared = prepare(handle, "PRAGMA table_info(runs);", info);
            if (!prepared.success())
                return common::Result<void>::failure(*prepared.error());
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<void>::failure(databaseError(
                        handle, "migration.v43_failed", "Could not inspect runs table schema"));
                }
                const auto name = columnText(info.get(), 1);
                runHasPolicy = runHasPolicy || name == "provider_budget_json";
                runHasUsage = runHasUsage || name == "provider_budget_usage_json";
            }
        }
        {
            Statement info;
            const auto prepared = prepare(
                handle, "PRAGMA table_info(execution_profiles);", info);
            if (!prepared.success())
                return common::Result<void>::failure(*prepared.error());
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<void>::failure(databaseError(
                        handle, "migration.v43_failed",
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
        const auto migrated = execute(handle, migration.c_str(), "migration.v43_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
            const auto prepared = prepare(handle, "PRAGMA table_info(runs);", info);
            if (!prepared.success())
                return common::Result<void>::failure(*prepared.error());
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<void>::failure(databaseError(
                        handle, "migration.v44_failed", "Could not inspect runs table schema"));
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
        const auto migrated = execute(handle, migration.c_str(), "migration.v44_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
            const auto prepared = prepare(handle, "PRAGMA table_info(models);", info);
            if (!prepared.success())
                return common::Result<void>::failure(*prepared.error());
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<void>::failure(databaseError(
                        handle, "migration.v45_failed", "Could not inspect models table schema"));
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
        const auto migrated = execute(handle, migration.c_str(), "migration.v45_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
            const auto prepared = prepare(handle, "PRAGMA table_info(model_executions);", info);
            if (!prepared.success())
                return common::Result<void>::failure(*prepared.error());
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<void>::failure(databaseError(
                        handle, "migration.v46_failed",
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
        const auto migrated = execute(handle, migration.c_str(), "migration.v46_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
                handle, "PRAGMA table_info(manifest_work_ledger_entries);", info);
            if (!prepared.success())
                return common::Result<void>::failure(*prepared.error());
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<void>::failure(databaseError(
                        handle, "migration.v47_failed",
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
        const auto migrated = execute(handle, migration.c_str(), "migration.v47_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration.c_str(), "migration.v48_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration.c_str(), "migration.v49_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration.c_str(), "migration.v50_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration.c_str(), "migration.v51_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v52_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle,
                "ROLLBACK; PRAGMA foreign_keys = ON;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
        }
        Statement foreignKeyCheck;
        const auto prepared = prepare(handle, "PRAGMA foreign_key_check;", foreignKeyCheck);
        if (!prepared.success() || sqlite3_step(foreignKeyCheck.get()) != SQLITE_DONE) {
            return common::Result<void>::failure(error(
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
            const auto existsPrepared = prepare(handle,
                "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?;", existsStatement);
            if (!existsPrepared.success()) {
                return common::Result<long long>::failure(*existsPrepared.error());
            }
            const auto existsBound = bindText(handle, existsStatement.get(), 1, tableName);
            if (!existsBound.success()) {
                return common::Result<long long>::failure(*existsBound.error());
            }
            if (sqlite3_step(existsStatement.get()) != SQLITE_ROW) {
                return common::Result<long long>::ok(0);
            }
            Statement countStatement;
            const std::string countSql = std::string("SELECT COUNT(*) FROM ") + tableName + ";";
            const auto countPrepared = prepare(handle, countSql.c_str(), countStatement);
            if (!countPrepared.success()) {
                return common::Result<long long>::failure(*countPrepared.error());
            }
            if (sqlite3_step(countStatement.get()) != SQLITE_ROW) {
                return common::Result<long long>::failure(databaseError(handle,
                    "migration.v55_count_failed",
                    "Could not count existing pre-v55 manual-handoff rows"));
            }
            return common::Result<long long>::ok(sqlite3_column_int64(countStatement.get(), 0));
        };
        const auto handoffRowCount = existingRowCount("manifest_verification_manual_handoffs");
        if (!handoffRowCount.success()) {
            return common::Result<void>::failure(*handoffRowCount.error());
        }
        const auto attemptRowCount = existingRowCount("manifest_verification_manual_handoff_attempts");
        if (!attemptRowCount.success()) {
            return common::Result<void>::failure(*attemptRowCount.error());
        }
        if (handoffRowCount.value() > 0 || attemptRowCount.value() > 0) {
            return common::Result<void>::failure(error(
                "migration.v55_refused_data_loss",
                "Refusing to upgrade to schema v55: the pre-v55 manual-handoff tables contain "
                "rows this migration cannot preserve. Discard them explicitly before upgrading."));
        }
        const auto migrated = execute(handle, migration, "migration.v55_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v56_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
            const auto prepared = prepare(handle, pragma.c_str(), info);
            if (!prepared.success()) return prepared;
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<void>::failure(databaseError(
                        handle, "migration.v57_failed",
                        std::string("Could not inspect ") + table + " table schema"));
                }
                found = found || columnText(info.get(), 1) == "verification_repair_policy";
            }
            return common::Result<void>::ok();
        };
        const auto inspectedRuns = inspectColumn("runs", runsHavePolicy);
        if (!inspectedRuns.success()) {
            return common::Result<void>::failure(
                *inspectedRuns.error());
        }
        const auto inspectedDrafts = inspectColumn("run_drafts", draftsHavePolicy);
        if (!inspectedDrafts.success()) {
            return common::Result<void>::failure(
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
            handle, migration.c_str(), "migration.v57_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v58_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
            const auto prepared = prepare(handle, "PRAGMA table_info(models);", info);
            if (!prepared.success()) {
                return common::Result<void>::failure(*prepared.error());
            }
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<void>::failure(databaseError(
                        handle, "migration.v59_failed", "Could not inspect models table schema"));
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
        const auto added = execute(handle, addColumns.c_str(), "migration.v59_failed");
        if (!added.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*added.error());
        }
        // Step 2: a partially migrated database may already hold tier values - for example long
        // prices that outlived a missing threshold column, which would now read back as FLAT
        // pricing, i.e. an understated cost. Check EVERY row against the shared rule and refuse the
        // whole migration on a violation: nothing is changed, the version is not raised and no
        // stored price is zeroed silently - the operator repairs the row and reopens.
        const auto verified = [&]() -> common::Result<void> {
            Statement rows;
            const auto prepared = prepare(handle,
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
                        handle, "migration.v59_failed",
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
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*verified.error());
        }
        // Step 3: record the version and commit.
        const auto committed = execute(handle,
            "PRAGMA user_version = 59;\nCOMMIT;", "migration.v59_failed");
        if (!committed.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*committed.error());
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
            const auto prepared = prepare(handle, "PRAGMA table_info(run_drafts);", info);
            if (!prepared.success()) {
                return common::Result<void>::failure(*prepared.error());
            }
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<void>::failure(databaseError(
                        handle, "migration.v60_failed",
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
        const auto migrated = execute(handle, migration.c_str(), "migration.v60_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
            const auto prepared = prepare(handle, pragma.c_str(), info);
            if (!prepared.success()) return prepared;
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<void>::failure(databaseError(
                        handle, "migration.v61_failed",
                        std::string("Could not inspect ") + table + " table schema"));
                }
                found = found || columnText(info.get(), 1) == "run_interface_contracts_json";
            }
            return common::Result<void>::ok();
        };
        const auto inspectedRuns = inspectColumn("runs", runsHaveContracts);
        if (!inspectedRuns.success()) {
            return common::Result<void>::failure(*inspectedRuns.error());
        }
        const auto inspectedDrafts = inspectColumn("run_drafts", draftsHaveContracts);
        if (!inspectedDrafts.success()) {
            return common::Result<void>::failure(*inspectedDrafts.error());
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
        const auto migrated = execute(handle, migration.c_str(), "migration.v61_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
            const auto prepared = prepare(handle, "PRAGMA table_info(run_drafts);", info);
            if (!prepared.success()) {
                return common::Result<void>::failure(*prepared.error());
            }
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<void>::failure(databaseError(
                        handle, "migration.v62_failed", "Could not inspect run_drafts table schema"));
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
        const auto migrated = execute(handle, migration.c_str(), "migration.v62_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v63_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v64_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
            const auto prepared = prepare(handle, pragma.c_str(), info);
            if (!prepared.success()) {
                return common::Result<std::unordered_set<std::string>>::failure(*prepared.error());
            }
            for (;;) {
                const int step = sqlite3_step(info.get());
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    return common::Result<std::unordered_set<std::string>>::failure(databaseError(
                        handle, "migration.v65_failed", "Could not inspect table schema"));
                }
                columns.insert(columnText(info.get(), 1));
            }
            return common::Result<std::unordered_set<std::string>>::ok(std::move(columns));
        };
        const auto modelColumnsV65 = columnsOf("models");
        const auto runColumnsV65 = columnsOf("runs");
        const auto executionColumnsV65 = columnsOf("model_executions");
        if (!modelColumnsV65.success())
            return common::Result<void>::failure(*modelColumnsV65.error());
        if (!runColumnsV65.success())
            return common::Result<void>::failure(*runColumnsV65.error());
        if (!executionColumnsV65.success())
            return common::Result<void>::failure(*executionColumnsV65.error());

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
        const auto altered = execute(handle, migration.c_str(), "migration.v65_failed");
        if (!altered.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*altered.error());
        }
        const auto invalidCount = [&](const char* sql) -> common::Result<std::int64_t> {
            Statement rows;
            const auto prepared = prepare(handle, sql, rows);
            if (!prepared.success()) return common::Result<std::int64_t>::failure(*prepared.error());
            if (sqlite3_step(rows.get()) != SQLITE_ROW) {
                return common::Result<std::int64_t>::failure(databaseError(
                    handle, "migration.v65_failed", "Could not verify cache-duration data"));
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
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(error(
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
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(
                error("migration.v65_failed",
                    "Existing one-hour long-context cache prices require a long-context threshold"));
        }
        const auto committed = execute(handle,
            "PRAGMA user_version = 65;\nCOMMIT;", "migration.v65_failed");
        if (!committed.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*committed.error());
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
                handle, "SELECT sql FROM sqlite_master WHERE type='table' AND name=?;", statement);
            if (!prepared.success()) return common::Result<std::string>::failure(*prepared.error());
            const auto bound = bindText(handle, statement.get(), 1, table);
            if (!bound.success()) return common::Result<std::string>::failure(*bound.error());
            if (sqlite3_step(statement.get()) != SQLITE_ROW) {
                return common::Result<std::string>::failure(
                    error("migration.v66_failed", std::string("Table '") + table + "' is missing"));
            }
            return common::Result<std::string>::ok(columnText(statement.get(), 0));
        };
        const auto groupsSql = storedTableSql("run_draft_groups");
        if (!groupsSql.success()) return common::Result<void>::failure(*groupsSql.error());
        const auto executionsSql = storedTableSql("model_executions");
        if (!executionsSql.success()) return common::Result<void>::failure(*executionsSql.error());
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

        const auto migrated = execute(handle, migration.c_str(), "migration.v66_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle,
                rebuilding ? "ROLLBACK; PRAGMA foreign_keys = ON;" : "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
        }
        // The relationships this step touched, checked by name (not the whole database): a pre-existing violation
        // somewhere else must not block an unrelated upgrade, but none of ours may be left behind.
        for (const char* table : {"run_draft_groups", "run_drafts", "model_executions", "quality_evaluations",
                 "comparison_sessions", "follow_up_plan_requests"}) {
            Statement foreignKeyCheck;
            const std::string pragma = std::string("PRAGMA foreign_key_check(") + table + ");";
            const auto prepared = prepare(handle, pragma.c_str(), foreignKeyCheck);
            if (!prepared.success() || sqlite3_step(foreignKeyCheck.get()) != SQLITE_DONE) {
                return common::Result<void>::failure(error(
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
                handle, "SELECT sql FROM sqlite_master WHERE type='table' AND name='follow_up_plan_requests';", tableSql);
            if (!preparedSql.success()) return common::Result<void>::failure(*preparedSql.error());
            if (sqlite3_step(tableSql.get()) != SQLITE_ROW) {
                return common::Result<void>::failure(
                    error("migration.v67_failed", "Table 'follow_up_plan_requests' is missing"));
            }
            hasAccounting = columnText(tableSql.get(), 0).find("accounting_json") != std::string::npos;
        }

        std::string migration = "BEGIN IMMEDIATE;\n";
        if (!hasAccounting) {
            sqlite3_int64 requests = 0;
            {
                Statement countStatement;
                const auto preparedCount = prepare(handle, "SELECT COUNT(*) FROM follow_up_plan_requests;", countStatement);
                if (!preparedCount.success()) return common::Result<void>::failure(*preparedCount.error());
                if (sqlite3_step(countStatement.get()) != SQLITE_ROW) {
                    return common::Result<void>::failure(error(
                        "migration.v67_failed", "The follow-up requests could not be counted before the repair"));
                }
                requests = sqlite3_column_int64(countStatement.get(), 0);
            }
            if (requests != 0) {
                return common::Result<void>::failure(error("migration.v67_failed",
                    "The follow-up request table has the old shape and holds " + std::to_string(requests)
                        + " request(s): they are evidence of what was sent and paid for, so the table is not rebuilt automatically"));
            }
            // Dropping the table drops its indexes and every trigger ON it; the objects come back from step 66's own SQL.
            migration += "DROP TABLE follow_up_plan_requests;\n";
            migration += followUpPlanRequestsObjectsSql();
            migration += followUpConfirmNeedsGroupTriggerSql();
        }
        migration += "PRAGMA user_version = 67;\nCOMMIT;\n";

        const auto migrated = execute(handle, migration.c_str(), "migration.v67_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
        }
        Statement foreignKeyCheck;
        const auto preparedCheck = prepare(handle, "PRAGMA foreign_key_check(follow_up_plan_requests);", foreignKeyCheck);
        if (!preparedCheck.success() || sqlite3_step(foreignKeyCheck.get()) != SQLITE_DONE) {
            return common::Result<void>::failure(error(
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
        const auto migrated = execute(handle, migration, "migration.v68_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v69_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
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
        const auto migrated = execute(handle, migration, "migration.v70_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
        }
    }

    if (schemaVersion < 71) {
        // P3a: the user's explicit approvals of output folders, per project and workspace root. A NEW step; no applied step is edited.
        // Existing databases get no rows. The step is repeatable over its own objects: a populated table that is EXACTLY this step's
        // table is kept (a rewound test database, a second run of the step) and only this step's own trigger is recreated. Anything
        // else of these names - a view, an index with another definition, a table that only has the same column names - refuses the
        // step; it is neither replaced nor "repaired", and the rows in it stay untouched.
        //
        // The table is accepted by its stored definition, because that is the only place its CHECK constraints can be read. SQLite
        // stores the statement from the object's name to its end, as written, so the text below is both what is executed and what
        // is compared (whitespace collapsed).
        const std::string tableBody = R"sql(project_output_approvals (
    id TEXT PRIMARY KEY NOT NULL CHECK(length(CAST(id AS BLOB)) BETWEEN 1 AND 64),
    project_id TEXT NOT NULL REFERENCES projects(id) ON UPDATE CASCADE ON DELETE CASCADE,
    workspace_root TEXT NOT NULL CHECK(length(CAST(workspace_root AS BLOB)) BETWEEN 1 AND 2048),
    workspace_key TEXT NOT NULL CHECK(length(CAST(workspace_key AS BLOB)) BETWEEN 1 AND 2048),
    relative_path TEXT NOT NULL CHECK(length(CAST(relative_path AS BLOB)) BETWEEN 1 AND 512),
    path_key TEXT NOT NULL CHECK(length(CAST(path_key AS BLOB)) BETWEEN 1 AND 512),
    inspection_complete INTEGER NOT NULL CHECK(inspection_complete IN (0,1)),
    nested_sources_acknowledged INTEGER NOT NULL CHECK(nested_sources_acknowledged IN (0,1)),
    approved_by TEXT NOT NULL DEFAULT '' CHECK(length(CAST(approved_by AS BLOB)) <= 256),
    approved_at_ms INTEGER NOT NULL,
    UNIQUE(project_id, workspace_key, path_key)
))sql";
        const std::string indexBody =
            "project_output_approvals_pair_idx ON project_output_approvals(project_id, workspace_key)";
        const auto collapse = [](const std::string& text) {
            std::string out;
            bool pendingSpace = false;
            for (const unsigned char c : text) {
                if (std::isspace(c) != 0) {
                    pendingSpace = !out.empty();
                    continue;
                }
                if (pendingSpace) out.push_back(' ');
                pendingSpace = false;
                out.push_back(static_cast<char>(c));
            }
            return out;
        };

        const auto begun = execute(handle, "BEGIN IMMEDIATE;", "migration.v71_failed");
        if (!begun.success()) return common::Result<void>::failure(*begun.error());
        const auto refuse = [handle](std::string message) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(error("migration.v71_failed", std::move(message)));
        };
        {
            Statement existing;
            const auto prepared = prepare(handle,
                "SELECT type, name, tbl_name, sql FROM sqlite_master "
                "WHERE name IN ('project_output_approvals', 'project_output_approvals_pair_idx');",
                existing);
            if (!prepared.success()) {
                static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
                return common::Result<void>::failure(*prepared.error());
            }
            int step = sqlite3_step(existing.get());
            for (; step == SQLITE_ROW; step = sqlite3_step(existing.get())) {
                const std::string type = columnText(existing.get(), 0);
                const std::string name = columnText(existing.get(), 1);
                const std::string owner = columnText(existing.get(), 2);
                const std::string definition = collapse(columnText(existing.get(), 3));
                if (name == "project_output_approvals") {
                    if (type != "table") {
                        return refuse("An object named project_output_approvals exists and is not a table; it is not replaced");
                    }
                    if (definition != collapse("CREATE TABLE " + tableBody)) {
                        return refuse("A table named project_output_approvals exists with another definition (columns, keys, "
                                      "constraints); it is not replaced");
                    }
                } else if (type != "index" || owner != "project_output_approvals"
                    || definition != collapse("CREATE INDEX " + indexBody)) {
                    return refuse("An object named project_output_approvals_pair_idx exists with another definition; it is not replaced");
                }
            }
            // A query that did not finish is not "nothing found".
            if (step != SQLITE_DONE) {
                static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
                return common::Result<void>::failure(
                    databaseError(handle, "migration.v71_failed", "Could not examine the existing objects of step 71"));
            }
        }
        const std::string migration = "CREATE TABLE IF NOT EXISTS " + tableBody + ";\n"
            "CREATE INDEX IF NOT EXISTS " + indexBody + ";\n" + R"sql(
-- A safety net next to the repository's transaction, not a replacement for it: at most 8 approvals per (project, workspace root).
DROP TRIGGER IF EXISTS project_output_approvals_limit;
CREATE TRIGGER project_output_approvals_limit
BEFORE INSERT ON project_output_approvals
WHEN (SELECT count(*) FROM project_output_approvals
      WHERE project_id = NEW.project_id AND workspace_key = NEW.workspace_key) >= 8
BEGIN
    SELECT RAISE(ABORT, 'output_approval_limit');
END;
PRAGMA user_version = 71;
COMMIT;
)sql";
        const auto migrated = execute(handle, migration.c_str(), "migration.v71_failed");
        if (!migrated.success()) {
            static_cast<void>(execute(handle, "ROLLBACK;", "migration.rollback_failed"));
            return common::Result<void>::failure(*migrated.error());
        }
    }

    return common::Result<void>::ok();
}

} // namespace sqlite_detail

} // namespace ai::infrastructure
