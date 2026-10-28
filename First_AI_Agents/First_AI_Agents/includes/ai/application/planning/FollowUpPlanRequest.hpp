#pragma once

#include "ai/application/planning/FollowUpComposerContext.hpp"
#include "ai/application/planning/FollowUpComposerQuote.hpp"
#include "ai/application/planning/FollowUpPlan.hpp"
#include "ai/application/planning/NextRunPlan.hpp"
#include "ai/application/planning/RunDraftTypes.hpp"
#include "ai/common/Result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

// PC-5B: the durable follow-up plan request and every RULE of its lifecycle, written once. The in-memory and the SQLite
// adapter only STORE what these functions decide; neither re-implements a rule. Nothing here reads a repository, a clock,
// a file or a provider, and nothing starts a Run.
using FollowUpPlanRequestId = std::string;

inline constexpr std::size_t kFollowUpRequestMaxIdLength = 64;
inline constexpr std::size_t kFollowUpMaxReplyBytes = 256 * 1024;
inline constexpr std::size_t kFollowUpMaxAdvisoriesBytes = 64 * 1024;
inline constexpr std::size_t kFollowUpMaxFailureCodeBytes = 128;
inline constexpr std::size_t kFollowUpMaxFailureMessageBytes = 4096;
inline constexpr std::size_t kFollowUpMaxListLimit = 500;
// A quote, and the authorisation made from it, live until createdAt + this (ONE deadline: a claim after it is a free stale).
inline constexpr std::chrono::milliseconds kFollowUpQuoteLifetime {std::chrono::minutes {15}};

// One file whose content went (or would go) to the provider, pinned by the sha256 of its COMPLETE bytes. The dialog lists
// every candidate; the request pins only the files that were selected and included.
struct FollowUpPinnedFile {
    std::string relativePath;
    std::string contentSha256;
    std::uint64_t originalBytes {0};
    std::uint64_t shownBytes {0};
    std::uint64_t estimatedTokens {0};
    bool truncated {false};

    friend bool operator==(const FollowUpPinnedFile&, const FollowUpPinnedFile&) = default;
};

// A composer answer that parsed as ONE valid plan. Stored immutably; the request becomes plan_ready.
struct FollowUpPlanAnswer {
    std::string rawReply;        // the provider's reply, byte for byte (1 .. kFollowUpMaxReplyBytes)
    std::string planFingerprint; // NextRunPlan::fingerprint, 64 lowercase hex
    std::string advisoriesJson;  // a JSON array: the advisory warnings computed over the plan and the frozen catalog

    friend bool operator==(const FollowUpPlanAnswer&, const FollowUpPlanAnswer&) = default;
};

// Why a claimed request failed: the provider failed, or the answer was invalid (then the reply and the typed parser
// errors stay readable). A retry is a NEW authorised request, never a repeat of this one.
struct FollowUpPlanFailure {
    std::string code;                    // 1 .. kFollowUpMaxFailureCodeBytes
    std::string message;                 // 0 .. kFollowUpMaxFailureMessageBytes
    std::optional<std::string> rawReply; // an invalid answer's text

    friend bool operator==(const FollowUpPlanFailure&, const FollowUpPlanFailure&) = default;
};

// What the provider reported for the ONE paid call of a request and what that usage costs, kept in the request itself and written
// in the SAME atomic step that settles the request. It exists because the execution log (a different repository) can fail to
// settle after the answer was paid for: this record is the durable copy from which the log is written again, with no second
// provider call (see FollowUpComposerProcessor::reconcileAccounting). Present exactly when a provider RESPONSE arrived: always for a
// stored plan, optionally for a failed (invalid answer) or cancelled request, never when no response did.
struct FollowUpAccounting {
    std::uint64_t inputTokens {0};
    std::uint64_t outputTokens {0};
    std::uint64_t cacheCreationInputTokens {0};
    std::uint64_t cacheReadInputTokens {0};
    // The duration split of the cache-creation tokens; when it is not `known` both counters are 0.
    bool cacheCreationDurationKnown {false};
    std::uint64_t cacheCreationFiveMinuteTokens {0};
    std::uint64_t cacheCreationOneHourTokens {0};
    std::uint64_t costMicros {0};
    bool priceKnown {true};
    std::uint64_t latencyMs {0};

    friend bool operator==(const FollowUpAccounting&, const FollowUpAccounting&) = default;
};

struct FollowUpPlanRequest {
    FollowUpPlanRequestId id;
    ProjectId projectId;
    RunId sourceRunId;
    FollowUpPlanKind kind {FollowUpPlanKind::NextRun};
    std::string goal;
    std::uint32_t maxDrafts {1};
    FollowUpSplitHint splitHint {FollowUpSplitHint::Auto};

    // What the quote was admitted against (checked again at the authorisation and at the claim by the service).
    std::string outcomeSourceStateFingerprint; // the Outcome Brief's sourceStateFingerprint
    std::string snapshotFingerprint;           // the PC-1 snapshot the context was built from
    std::vector<FollowUpPinnedFile> files;

    // The immutable quote: sealed context, recipient identity, pricing identity, effective output, worst-case cost, cap.
    FollowUpComposerQuote quote;

    // Lifecycle. Whether a terminal request was FREE is decided by where it left, not by its name: claimedAt is the
    // stamp of the claim (the call may have been made), absent for every request that never reached it.
    FollowUpPlanRequestState state {FollowUpPlanRequestState::Quoted};
    std::chrono::system_clock::time_point createdAt;      // whole milliseconds
    std::chrono::system_clock::time_point quoteExpiresAt; // createdAt + kFollowUpQuoteLifetime
    std::chrono::system_clock::time_point updatedAt;
    std::optional<std::chrono::system_clock::time_point> authorizedAt;
    std::optional<std::chrono::system_clock::time_point> claimedAt;
    std::optional<std::chrono::system_clock::time_point> finishedAt;

    std::optional<FollowUpPlanAnswer> answer;   // plan_ready / drafts_saved (and a discarded plan_ready)
    std::optional<FollowUpPlanFailure> failure; // failed only
    std::optional<RunDraftGroupId> draftGroupId; // drafts_saved only
    // Set together with the answer (required there), and optionally with a failure or a cancel after a response arrived.
    std::optional<FollowUpAccounting> accounting;
};

// The request left the machine's control at the claim: true when the provider call may have been made and billed.
[[nodiscard]] bool followUpRequestMayBeCharged(const FollowUpPlanRequest& request) noexcept;

[[nodiscard]] std::chrono::system_clock::time_point followUpFloorToMilliseconds(
    std::chrono::system_clock::time_point value) noexcept;

// The STORAGE gate: everything a stored request must satisfy, checked on every write and every read. It proves the record is
// STRUCTURALLY consistent (enums known, fingerprints well-formed and equal to what their identities hash to, the quote's
// numbers coherent and inside the system limits, the draft window valid, the exact fields each state requires or forbids,
// and the time chain: the stamps a state's last step sets are equal to the update time, exactly as the transitions write
// them). It does NOT prove that the quote came from quoteFollowUpComposerRequest or that the context bytes are the
// assembler's (the service assembles and re-quotes itself), and it does NOT parse a stored answer: that the reply holds ONE
// valid plan with the stored fingerprint inside the authorised draft window is checked when the answer is written
// (applyFollowUpTransition, PlanSucceeded) and again, from the stored reply, at the Confirm. Failure: follow_up.invalid_request.
[[nodiscard]] common::Result<void> validateFollowUpPlanRequest(const FollowUpPlanRequest& request);

// The plan a composer answer carries, derived with the SAME functions that read any other plan: the one next-run-plan.v1
// envelope of the reply (extractNextRunPlanJsonFromMessage), the strict parse (parseNextRunPlan), the plan's own fingerprint
// equal to the answer's, and the draft count inside the window the request authorised (checkFollowUpPlanAgainstAuthorisation).
// Failure: the `next_run_plan.*` / `run_contracts.*` codes of the envelope and the parser, `follow_up.answer_plan_mismatch`
// (the fingerprint is not the plan's), `follow_up.invalid_draft_limits` / `follow_up.plan_draft_count_not_authorised`.
[[nodiscard]] common::Result<NextRunPlan> parseFollowUpAnswerPlan(
    const FollowUpPlanAnswer& answer, FollowUpSplitHint splitHint, std::uint32_t maxDrafts);

// The group and Drafts to save ARE this plan: the group's title, the number of Drafts, and for each ordinal the plan key,
// title (the Draft's name too), task, dependencies, the final contracts, the Manifest workflow with a per-Run roster, the group
// and project, and the content fingerprint the plan gives that Draft. Roster, budget and workspace are the user's choice and are
// not compared (insertGroup validates them). Failure: follow_up.drafts_not_the_plan.
[[nodiscard]] common::Result<void> checkFollowUpDraftsAreThePlan(
    const NextRunPlan& plan, const RunDraftGroup& group, const std::vector<RunDraft>& drafts);

// ---- Lifecycle rules ---------------------------------------------------------------------------------------------------------

// One compare-and-set step. `expectedState` is what the caller believed; the step happens only when the stored request is in
// that state (`follow_up.state_changed` otherwise). ConfirmDrafts is NOT a transition here: it is the atomic port.
struct FollowUpTransition {
    FollowUpPlanEvent event {FollowUpPlanEvent::Authorize};
    FollowUpPlanRequestState expectedState {FollowUpPlanRequestState::Quoted};
    std::string contextFingerprint;             // Authorize: must be the quote's, so a replaced quote cannot be authorised
    std::optional<FollowUpPlanAnswer> answer;   // PlanSucceeded, and only there
    std::optional<FollowUpPlanFailure> failure; // CompositionFailed, and only there
    // PlanSucceeded: REQUIRED (an answer is a paid response). CompositionFailed and Cancel: when a provider response arrived (an invalid
    // answer, a stop after a complete reply); absent when none did. Every other event: none.
    std::optional<FollowUpAccounting> accounting;
};

// The request after `transition`, computed from `current` and `now` (floored to milliseconds and never before the last
// update). Pure: it reads nothing and writes nothing; the adapter stores the result. Failure codes: follow_up.state_changed,
// follow_up.invalid_transition (the 5A-1 table), follow_up.context_changed (Authorize with another fingerprint),
// follow_up.quote_expired (Authorize or Claim at or after the deadline), follow_up.not_expired (Expire before it),
// follow_up.confirm_requires_port, follow_up.invalid_request (a payload the event does not take, or a result that fails the gate).
[[nodiscard]] common::Result<FollowUpPlanRequest> applyFollowUpTransition(
    const FollowUpPlanRequest& current,
    const FollowUpTransition& transition,
    std::chrono::system_clock::time_point now);

enum class FollowUpConfirmAction {
    SaveGroup,      // plan_ready: write the group, its drafts and drafts_saved together
    ReturnExisting, // drafts_saved with the same plan: return the SAME group and write nothing
};

// The decision of the atomic Confirm, before any write. plan_ready + the previewed plan fingerprint + a group that really is this
// request's (origin, request id, project, sealed context, plan, source Run) + Drafts that ARE the stored plan -> SaveGroup. The
// stored reply is parsed AGAIN here (parseFollowUpAnswerPlan), so Drafts can never be saved under the provenance of a plan they
// are not, whatever wrote the row. drafts_saved + the same plan fingerprint -> ReturnExisting (the candidate group and Drafts are
// not looked at). Anything else is refused: follow_up.not_plan_ready, follow_up.plan_changed, follow_up.group_mismatch,
// follow_up.drafts_not_the_plan, and the codes of parseFollowUpAnswerPlan (a stored reply that no longer parses is not confirmable).
[[nodiscard]] common::Result<FollowUpConfirmAction> planFollowUpConfirm(
    const FollowUpPlanRequest& request,
    const std::string& expectedPlanFingerprint,
    const RunDraftGroup& group,
    const std::vector<RunDraft>& drafts);

// The Runs a group names as its source are the project's Runs: each must exist and belong to `projectId` (an extra id that is
// a ghost, or another project's Run, would become trusted provenance). `loadProject` answers the project of a Run, empty when
// there is none, or a persistence error. Failure: follow_up.source_run_not_found, follow_up.source_run_wrong_project, or the
// loader's own error. At most kRunDraftGroupMaxSourceRuns ids are looked up; more is follow_up.group_mismatch.
using FollowUpRunProjectLoader = std::function<common::Result<std::optional<ProjectId>>(const RunId&)>;
[[nodiscard]] common::Result<void> checkFollowUpGroupSourceRuns(
    const RunDraftGroup& group, const ProjectId& projectId, const FollowUpRunProjectLoader& loadProject);

// plan_ready -> drafts_saved with the group id and the same time rules as applyFollowUpTransition.
[[nodiscard]] common::Result<FollowUpPlanRequest> applyFollowUpConfirm(
    const FollowUpPlanRequest& current,
    const RunDraftGroupId& groupId,
    std::chrono::system_clock::time_point now);

// ---- The one open request per source Run -------------------------------------------------------------------------------------

// `open` is the request of the same source Run that is in an open state (quoted | authorized | composing | plan_ready), if
// any. None: insert. A `quoted` one: it is replaced - the answer is its id and the adapter discards it (a free discard) in the
// SAME transaction as the insert. Anything further along is never displaced: follow_up.request_open.
[[nodiscard]] common::Result<std::optional<FollowUpPlanRequestId>> planFollowUpQuotedInsert(
    const FollowUpPlanRequest& incoming,
    const std::optional<FollowUpPlanRequest>& open);

// ---- Authority ---------------------------------------------------------------------------------------------------------------

// The Run row, not the request, says which project a Run belongs to. A write requires the Run to exist in the request's
// project (follow_up.run_not_found, follow_up.project_mismatch); EVERY read checks it again (persistence.invalid_data),
// because a row can be made behind the repository's back. `runProject` is the Run's project, empty when there is no such Run.
using FollowUpRunProjectLookup = std::function<std::optional<ProjectId>(const RunId&)>;
[[nodiscard]] common::Result<void> checkFollowUpRequestWriteAuthority(
    const FollowUpPlanRequest& request, const std::optional<ProjectId>& runProject);
[[nodiscard]] common::Result<void> checkFollowUpRequestReadAuthority(
    const FollowUpPlanRequest& request, const std::optional<ProjectId>& runProject);

[[nodiscard]] common::Result<void> checkFollowUpListLimit(std::size_t limit);

// ---- Storage codecs shared by every adapter ----------------------------------------------------------------------------------

[[nodiscard]] std::string serializeFollowUpPinnedFiles(const std::vector<FollowUpPinnedFile>& files);
[[nodiscard]] common::Result<std::vector<FollowUpPinnedFile>> parseFollowUpPinnedFiles(std::string_view stored);
[[nodiscard]] std::string serializeFollowUpAccounting(const FollowUpAccounting& accounting);
[[nodiscard]] common::Result<FollowUpAccounting> parseFollowUpAccounting(std::string_view stored);
[[nodiscard]] std::string serializeFollowUpPricingIdentity(const ProviderRequestPricingIdentity& identity);
[[nodiscard]] common::Result<ProviderRequestPricingIdentity> parseFollowUpPricingIdentity(std::string_view stored);

[[nodiscard]] std::optional<models::PromptCacheMode> followUpPromptCacheModeFromString(std::string_view value) noexcept;

} // namespace ai::application
