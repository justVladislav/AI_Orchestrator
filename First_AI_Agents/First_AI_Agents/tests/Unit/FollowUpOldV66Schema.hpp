#pragma once

// PC-5D: the follow-up request table and everything on it EXACTLY as a database that ran the FIRST spelling of schema step 66
// (before the table was amended in place) still holds it - dumped byte for byte from such a database (sqlite_master.sql). Step 67
// repairs it; the tests rebuild this old shape to prove that. The old table has no `accounting_json`.
//
// Generated once (.tmp/tools/pc5/dump_old_v66.mjs); do not edit.

namespace followup_old_v66 {

inline constexpr const char* kObjects[] = {
    // table follow_up_plan_requests
    R"fixture(CREATE TABLE follow_up_plan_requests (
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
    CHECK((failure_code IS NULL) = (failure_message IS NULL)),
    CHECK(
        (state = 'quoted' AND authorized_at_ms IS NULL AND claimed_at_ms IS NULL AND finished_at_ms IS NULL
            AND reply_text IS NULL AND plan_fingerprint IS NULL AND failure_code IS NULL AND draft_group_id IS NULL)
     OR (state = 'authorized' AND authorized_at_ms IS NOT NULL AND claimed_at_ms IS NULL AND finished_at_ms IS NULL
            AND reply_text IS NULL AND plan_fingerprint IS NULL AND failure_code IS NULL AND draft_group_id IS NULL)
     OR (state = 'composing' AND authorized_at_ms IS NOT NULL AND claimed_at_ms IS NOT NULL AND finished_at_ms IS NULL
            AND reply_text IS NULL AND plan_fingerprint IS NULL AND failure_code IS NULL AND draft_group_id IS NULL)
     OR (state = 'plan_ready' AND authorized_at_ms IS NOT NULL AND claimed_at_ms IS NOT NULL AND finished_at_ms IS NULL
            AND plan_fingerprint IS NOT NULL AND failure_code IS NULL AND draft_group_id IS NULL)
     OR (state = 'drafts_saved' AND authorized_at_ms IS NOT NULL AND claimed_at_ms IS NOT NULL AND finished_at_ms IS NOT NULL
            AND plan_fingerprint IS NOT NULL AND failure_code IS NULL AND draft_group_id IS NOT NULL)
     OR (state = 'failed' AND authorized_at_ms IS NOT NULL AND claimed_at_ms IS NOT NULL AND finished_at_ms IS NOT NULL
            AND plan_fingerprint IS NULL AND failure_code IS NOT NULL AND draft_group_id IS NULL)
     OR (state = 'interrupted' AND authorized_at_ms IS NOT NULL AND claimed_at_ms IS NOT NULL AND finished_at_ms IS NOT NULL
            AND reply_text IS NULL AND plan_fingerprint IS NULL AND failure_code IS NULL AND draft_group_id IS NULL)
     OR (state IN ('stale','expired') AND claimed_at_ms IS NULL AND finished_at_ms IS NOT NULL
            AND reply_text IS NULL AND plan_fingerprint IS NULL AND failure_code IS NULL AND draft_group_id IS NULL)
     OR (state = 'cancelled' AND finished_at_ms IS NOT NULL
            AND reply_text IS NULL AND plan_fingerprint IS NULL AND failure_code IS NULL AND draft_group_id IS NULL)
     OR (state = 'discarded' AND finished_at_ms IS NOT NULL AND failure_code IS NULL AND draft_group_id IS NULL
            AND ((authorized_at_ms IS NULL AND claimed_at_ms IS NULL AND plan_fingerprint IS NULL AND reply_text IS NULL)
              OR (authorized_at_ms IS NOT NULL AND claimed_at_ms IS NOT NULL AND plan_fingerprint IS NOT NULL)))
    )
))fixture",
    // index follow_up_requests_one_open_per_run
    R"fixture(CREATE UNIQUE INDEX follow_up_requests_one_open_per_run
    ON follow_up_plan_requests(source_run_id) WHERE state IN ('quoted','authorized','composing','plan_ready'))fixture",
    // index follow_up_requests_project_created_idx
    R"fixture(CREATE INDEX follow_up_requests_project_created_idx
    ON follow_up_plan_requests(project_id, created_at_ms DESC, id DESC))fixture",
    // index follow_up_requests_state_created_idx
    R"fixture(CREATE INDEX follow_up_requests_state_created_idx
    ON follow_up_plan_requests(state, created_at_ms, id))fixture",
    // trigger follow_up_requests_confirm_needs_group
    R"fixture(CREATE TRIGGER follow_up_requests_confirm_needs_group
BEFORE UPDATE OF state ON follow_up_plan_requests
WHEN NEW.state = 'drafts_saved'
 AND NOT EXISTS(SELECT 1 FROM run_draft_groups
                WHERE id = NEW.draft_group_id AND source_request_id = NEW.id AND project_id = NEW.project_id)
BEGIN
    SELECT RAISE(ABORT, 'follow_up_request_confirm_needs_group');
END)fixture",
    // trigger follow_up_requests_identity_immutable
    R"fixture(CREATE TRIGGER follow_up_requests_identity_immutable
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
END)fixture",
    // trigger follow_up_requests_insert_quoted
    R"fixture(CREATE TRIGGER follow_up_requests_insert_quoted
BEFORE INSERT ON follow_up_plan_requests
WHEN NEW.state <> 'quoted'
BEGIN
    SELECT RAISE(ABORT, 'follow_up_request_must_start_quoted');
END)fixture",
    // trigger follow_up_requests_reject_delete
    R"fixture(CREATE TRIGGER follow_up_requests_reject_delete
BEFORE DELETE ON follow_up_plan_requests
BEGIN
    SELECT RAISE(ABORT, 'follow_up_request_immutable');
END)fixture",
    // trigger follow_up_requests_result_write_once
    R"fixture(CREATE TRIGGER follow_up_requests_result_write_once
BEFORE UPDATE OF authorized_at_ms, claimed_at_ms, finished_at_ms, reply_text, plan_fingerprint, advisories_json,
    failure_code, failure_message, draft_group_id
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
BEGIN
    SELECT RAISE(ABORT, 'follow_up_request_result_immutable');
END)fixture",
    // trigger follow_up_requests_transition
    R"fixture(CREATE TRIGGER follow_up_requests_transition
BEFORE UPDATE ON follow_up_plan_requests
WHEN NEW.state IS OLD.state
  OR NOT ((OLD.state = 'quoted' AND NEW.state IN ('authorized','stale','expired','cancelled','discarded'))
       OR (OLD.state = 'authorized' AND NEW.state IN ('composing','stale','expired','cancelled'))
       OR (OLD.state = 'composing' AND NEW.state IN ('plan_ready','failed','cancelled','interrupted'))
       OR (OLD.state = 'plan_ready' AND NEW.state IN ('drafts_saved','discarded')))
BEGIN
    SELECT RAISE(ABORT, 'follow_up_request_invalid_transition');
END)fixture",
};

} // namespace followup_old_v66
