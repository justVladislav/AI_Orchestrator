#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/planning/NextRunPlan.hpp"
#include "ai/application/OutcomeBrief.hpp"
#include "ai/common/Result.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace ai::application {

// PC-5A-1: the pure lifecycle and source-admission core of the opt-in paid
// next-Run composer. It reads no repository, file or clock, calls no provider,
// writes nothing and knows nothing about Qt. Persistence starts in PC-5B;
// context rendering, quotes and response policy are later PC-5A slices.
inline constexpr std::string_view kFollowUpPlanPolicyId = "follow_up_plan_policy_v1";

enum class FollowUpPlanKind {
    NextRun,
    Recovery,
};

enum class FollowUpSplitHint {
    One,
    Many,
    Auto,
};

enum class FollowUpSourceWorkflow {
    Unknown,
    Manifest,
    VerificationContinuation,
};

enum class FollowUpPlanRequestState {
    Quoted,
    Authorized,
    Composing,
    PlanReady,
    DraftsSaved,
    Stale,
    Expired,
    Cancelled,
    Discarded,
    Interrupted,
    Failed,
};

enum class FollowUpPlanEvent {
    Authorize,
    Claim,
    PlanSucceeded,
    CompositionFailed,
    MarkStale,
    Expire,
    Cancel,
    Interrupt,
    ConfirmDrafts,
    Discard,
};

[[nodiscard]] const char* toString(FollowUpPlanKind value) noexcept;
[[nodiscard]] const char* toString(FollowUpSplitHint value) noexcept;
[[nodiscard]] const char* toString(FollowUpPlanRequestState value) noexcept;
[[nodiscard]] const char* toString(FollowUpPlanEvent value) noexcept;

[[nodiscard]] std::optional<FollowUpPlanKind> followUpPlanKindFromString(std::string_view value) noexcept;
[[nodiscard]] std::optional<FollowUpSplitHint> followUpSplitHintFromString(std::string_view value) noexcept;
[[nodiscard]] std::optional<FollowUpPlanRequestState> followUpPlanRequestStateFromString(
    std::string_view value) noexcept;

// The partial unique index introduced in PC-5B covers exactly these states.
[[nodiscard]] bool isOpenFollowUpPlanRequestState(FollowUpPlanRequestState state) noexcept;
[[nodiscard]] bool isTerminalFollowUpPlanRequestState(FollowUpPlanRequestState state) noexcept;

// Applies one event to one state. No terminal state has an outgoing edge.
// Failure: follow_up.invalid_transition (including unknown enum values).
[[nodiscard]] common::Result<FollowUpPlanRequestState> transitionFollowUpPlanRequest(
    FollowUpPlanRequestState state,
    FollowUpPlanEvent event);

// The one mapping from a persisted Run::workflowId to the gate's workflow
// vocabulary. It classifies through the Outcome Brief's single table
// (outcomeWorkflowKind) and keeps no id list of its own: an ordinary Manifest
// Run and a paid verification-continuation child map to their gate values;
// every other id - Mock, execution-profile, legacy, the manual-handoff attempt
// workflow, an empty or decorated spelling - maps to Unknown, which the gate
// refuses. A workflow added to that table later is Unknown here (fail closed)
// until it is mapped on purpose; the test over outcomeWorkflows() fails first.
[[nodiscard]] FollowUpSourceWorkflow followUpSourceWorkflowFromId(std::string_view workflowId) noexcept;

// Typed facts already read from the authoritative Run, Outcome Brief and
// relation stores. The gate never reconstructs relations from parentRunId.
struct FollowUpSourceFacts {
    // Defaults are deliberately ineligible: an adapter that forgets to fill
    // any authoritative field must never manufacture a recovery path.
    // `workflow` comes from followUpSourceWorkflowFromId() and from nothing else.
    FollowUpSourceWorkflow workflow {FollowUpSourceWorkflow::Unknown};
    RunStatus terminalStatus {RunStatus::Prepared};
    OutcomeBriefState briefState {OutcomeBriefState::AttentionRequired};
    bool hasContinuationChild {false};
    bool hasManualHandoff {false};
};

// The whitelist agreed for PC-5. Normal resolved outcomes admit NextRun only;
// resolved_manually_applied is a resolved Failed anchor and also admits
// NextRun. recovery_only admits Recovery only for a Failed Run that no
// continuation or manual handoff owns. Cancelled / Interrupted, pending,
// attention-required, superseded and inconsistent combinations fail closed.
// Only ordinary Manifest Runs and paid verification-continuation children are
// eligible Outcome workflows; Mock, legacy and handoff-attempt Runs are not.
// Every failure code is stable and begins with follow_up.
[[nodiscard]] common::Result<void> checkFollowUpSourceEligibility(
    FollowUpPlanKind kind,
    const FollowUpSourceFacts& facts);

// PC-5A-3: what a request AUTHORISES. The user states `maxDrafts` and a split hint; the composer's answer is parsed by
// the shared next-run-plan parser, which accepts up to kNextRunPlanMaxDrafts (8) drafts for every other consumer. This
// window is the only thing that makes the user's limit binding on the returned plan.
inline constexpr std::uint32_t kFollowUpMaximumDrafts = 4;
inline constexpr std::uint32_t kFollowUpManyMinimumDrafts = 2;

struct FollowUpDraftLimits {
    std::uint32_t minimum {0};
    std::uint32_t maximum {0};
};

// The authorised draft-count window, inclusive:
//   auto: 1 .. maxDrafts      one: exactly 1      many: 2 .. maxDrafts
// An input that cannot describe a window is an invalid REQUEST, not a plan problem: maxDrafts outside 1..4, an unknown
// split hint, and `many` with maxDrafts 1 (it asks for two drafts and allows one).
// Failure: follow_up.invalid_draft_limits.
[[nodiscard]] common::Result<FollowUpDraftLimits> followUpAuthorisedDraftLimits(
    FollowUpSplitHint splitHint,
    std::uint32_t maxDrafts);

// A returned plan must have a draft count inside the window of the request it answers. A plan outside it stays an
// invalid answer (stored with this typed error, the request `failed`) and never reaches Drafts.
// Failure: follow_up.invalid_draft_limits (the request itself) or follow_up.plan_draft_count_not_authorised.
[[nodiscard]] common::Result<void> checkFollowUpPlanAgainstAuthorisation(
    const NextRunPlan& plan,
    FollowUpSplitHint splitHint,
    std::uint32_t maxDrafts);

} // namespace ai::application
