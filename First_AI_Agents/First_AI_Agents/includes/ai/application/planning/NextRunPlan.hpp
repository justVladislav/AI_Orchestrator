#pragma once

#include "ai/application/execution/RunInterfaceContract.hpp"
#include "ai/common/Result.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

// PC-3A (gap MW-P10): the pure `next-run-plan.v1` contract of the Prompt Workshop. A plan is untrusted,
// model-authored text that a user explicitly turns into one or several reviewed Run Drafts. This header is the pure
// part: envelope scanning and extraction, strict parsing and validation, the shared-contract merge, the canonical
// fingerprints. It reads no repository, no file and no clock, calls no provider, writes nothing and knows nothing
// about Qt, a project or a roster: a plan has NO field for project, roster, provider, model, budget, workspace,
// launch or start, so it can neither choose cost nor start anything. Persistence, idempotency and the UI are 3B-3D.
inline constexpr std::string_view kNextRunPlanSchemaVersion = "next-run-plan.v1";

// Limits. The size limit applies to the JSON text BEFORE it is parsed.
inline constexpr std::size_t kNextRunPlanMaxBytes = 128 * 1024;
inline constexpr std::size_t kNextRunPlanMinDrafts = 1;
inline constexpr std::size_t kNextRunPlanMaxDrafts = 8;
inline constexpr std::size_t kNextRunPlanMaxTaskBytes = 16 * 1024;
inline constexpr std::size_t kNextRunPlanMaxKeyLength = 64;
inline constexpr std::size_t kNextRunPlanMaxTitleBytes = 512;
// The optional per-draft staffing HINT (design: CODEX_CONTEXT "Next-Run plan: staffing hint and file-name check").
inline constexpr std::size_t kNextRunPlanMaxSuggestedWorkers = 4;
inline constexpr std::size_t kNextRunPlanMaxStaffingFocusBytes = 200;
inline constexpr std::size_t kNextRunPlanMaxStaffingReasonBytes = 240;

// What a message holds, as far as the cheap line scan can tell. Computed by the Backend for the per-message action;
// QML never scans or parses text.
enum class NextRunPlanEnvelopeState {
    None,       // no fenced block tagged `next-run-plan.v1`
    One,        // exactly one (it may still be unterminated or invalid: only a full parse says)
    Multiple,   // two or more: ambiguous, never guessed
    TooLarge,   // exactly one, but its body is over kNextRunPlanMaxBytes
};

[[nodiscard]] const char* toString(NextRunPlanEnvelopeState state) noexcept;

// The envelope is a fenced code block (backtick fence of at least three, indented at most three spaces) whose WHOLE
// info string is `next-run-plan.v1`. Other fences (any other info string, extra words, `~~~` fences, an indented
// code block, text inside a longer outer fence) are opaque and never counted. An unterminated envelope fence counts
// as one envelope; extraction then fails with `next_run_plan.envelope_unterminated`. Prose has no effect.
// Bounded: the scan keeps at most kNextRunPlanMaxBytes + 1 bytes of an envelope body and never stores an info
// string, so its memory does not grow with the message however long a line is.
[[nodiscard]] NextRunPlanEnvelopeState scanNextRunPlanEnvelope(std::string_view messageContent);

// The JSON text of the single envelope of an Assistant message. Failure codes (all `next_run_plan.*`):
// envelope_missing, envelope_multiple, envelope_unterminated, too_large.
[[nodiscard]] common::Result<std::string> extractNextRunPlanJsonFromMessage(std::string_view messageContent);

// A manual paste: ONE fenced envelope (same rules as a message), or - when the text holds no envelope fence at all -
// the whole text as a bare JSON document (it must start with '{' after trimming whitespace). Same failure codes.
[[nodiscard]] common::Result<std::string> extractNextRunPlanJsonFromPaste(std::string_view pastedText);

// A model-free, advisory suggestion of who should work on a draft. It is NOT a roster: it names no provider, model, price
// or budget and the plan has no field for any of them. The lead Architect is implied and never listed. A `staffing` that
// breaks a rule is dropped by the parser (warning `next_run_plan.staffing_ignored`), it never fails the plan.
struct NextRunPlanStaffingWorker {
    std::string role;    // "implementation" | "documentation"
    std::string focus;   // one line, <= kNextRunPlanMaxStaffingFocusBytes: what this worker owns
};

struct NextRunPlanStaffing {
    std::vector<NextRunPlanStaffingWorker> workers;   // 1..kNextRunPlanMaxSuggestedWorkers
    std::size_t parallelism {1};                      // 1..workers.size()
    std::string reason;                               // one line, <= kNextRunPlanMaxStaffingReasonBytes
};

struct NextRunPlanDraft {
    // `[A-Za-z0-9][A-Za-z0-9_.-]{0,63}`, unique ignoring case; the handle other drafts depend on.
    std::string key;
    std::string title;   // trimmed, one line, <= kNextRunPlanMaxTitleBytes; the draft's `name` is set equal to it
    std::string task;    // line endings normalized to LF, <= kNextRunPlanMaxTaskBytes
    // Keys of drafts of the same plan that should be started first. Advisory: it never controls launching. Authoring
    // order, no duplicates, no self reference, no cycle; a forward reference is legal.
    std::vector<std::string> dependsOn;
    // The shared contracts this draft uses, in the order the draft lists them (exact ids, no duplicates).
    std::vector<std::string> sharedContractIds;
    // The final, canonical list the Draft will carry: the shared contracts it lists (in that order), then its own,
    // run through normalizeRunInterfaceContracts as ONE list (so every PC-2 limit applies to the merged result).
    std::vector<RunInterfaceContract> contracts;
    // How many leading entries of `contracts` came from the shared list (the rest are the draft's own).
    std::size_t sharedContractCount {0};
    // Present only when the plan carried a valid `staffing`; part of the plan fingerprint only then.
    std::optional<NextRunPlanStaffing> staffing;
};

struct NextRunPlanWarning {
    std::string code;     // `next_run_plan.*`
    std::string message;
};

struct NextRunPlan {
    std::string schemaVersion {std::string(kNextRunPlanSchemaVersion)};
    std::string title;
    // Canonical, each validated on its own and unique ignoring case; delivered only to drafts that list them. The
    // PC-2 count and total-size limits apply to each draft's merged list, not to this pool (the plan size bounds it).
    std::vector<RunInterfaceContract> sharedContracts;
    std::vector<NextRunPlanDraft> drafts;                // authoring order = ordinal
    // Findings that do not make the plan invalid (for example a shared contract no draft lists).
    std::vector<NextRunPlanWarning> warnings;
    // sha256 of the canonical serialization of the NORMALIZED plan: independent of JSON key order, whitespace and
    // line endings, and dependent on every normalized field of the plan except `warnings`. That includes the plan
    // title and any shared contract no draft lists (it reaches no Draft, but it is part of the plan). Not a Run,
    // project or message identity.
    std::string fingerprint;
};

// Strict parse + validation of the JSON text of one plan. Nothing is repaired: any violation fails the whole plan.
// Failure codes (all `next_run_plan.*` unless noted): too_large (checked before parsing), invalid_utf8,
// invalid_json, duplicate_json_key, too_deep, schema_version (missing or not next-run-plan.v1), unknown_field,
// missing_field, invalid_field (wrong type), invalid_title, invalid_task, task_too_long, invalid_key,
// duplicate_draft_key, no_drafts, too_many_drafts, unknown_dependency, self_dependency, duplicate_dependency,
// dependency_cycle, unknown_shared_contract, duplicate_shared_reference. Contract failures keep their
// `run_contracts.*` code (invalid_kind, invalid_scope, duplicate_id, too_many, ...) with the plan location in the
// message. The semantic errors name what is wrong and, where there is one, its location (for example
// `plan.drafts[1].dependsOn[0]`); no_drafts, too_many_drafts, duplicate_draft_key and dependency_cycle name the
// plan or a draft key instead. The syntactic and size errors say less: too_large, invalid_utf8 and too_deep state
// only the problem or the limit, invalid_json gives the byte offset of the first error, and duplicate_json_key
// gives the (truncated) text of the repeated key but not where it is.
[[nodiscard]] common::Result<NextRunPlan> parseNextRunPlan(std::string_view json);

// sha256 of the canonical title + task + FINAL contracts + dependency keys of ONE draft: the "created content" that
// the "edited since creation" indicator later recomputes from the current draft. Roster, budget and workspace are
// deliberately not part of it. `contracts` must already be in canonical form.
[[nodiscard]] std::string nextRunPlanDraftContentFingerprint(
    std::string_view title, std::string_view task, const std::vector<RunInterfaceContract>& contracts,
    const std::vector<std::string>& dependsOn);

} // namespace ai::application
