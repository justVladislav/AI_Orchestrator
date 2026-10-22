#include "ai/application/FollowUpPlan.hpp"

#include <string>
#include <utility>

namespace ai::application {
namespace {

template <typename T>
common::Result<T> fail(std::string code, std::string message)
{
    return common::Result<T>::failure({std::move(code), std::move(message)});
}

common::Result<void> refused(std::string code, std::string message)
{
    return common::Result<void>::failure({std::move(code), std::move(message)});
}

bool known(FollowUpPlanKind value) noexcept
{
    return value == FollowUpPlanKind::NextRun || value == FollowUpPlanKind::Recovery;
}

bool known(FollowUpSourceWorkflow value) noexcept
{
    return value == FollowUpSourceWorkflow::Manifest
        || value == FollowUpSourceWorkflow::VerificationContinuation;
}

bool known(FollowUpPlanRequestState value) noexcept
{
    switch (value) {
    case FollowUpPlanRequestState::Quoted:
    case FollowUpPlanRequestState::Authorized:
    case FollowUpPlanRequestState::Composing:
    case FollowUpPlanRequestState::PlanReady:
    case FollowUpPlanRequestState::DraftsSaved:
    case FollowUpPlanRequestState::Stale:
    case FollowUpPlanRequestState::Expired:
    case FollowUpPlanRequestState::Cancelled:
    case FollowUpPlanRequestState::Discarded:
    case FollowUpPlanRequestState::Interrupted:
    case FollowUpPlanRequestState::Failed:
        return true;
    }
    return false;
}

bool known(RunStatus value) noexcept
{
    switch (value) {
    case RunStatus::Prepared:
    case RunStatus::Queued:
    case RunStatus::Running:
    case RunStatus::Cancelling:
    case RunStatus::Completed:
    case RunStatus::Failed:
    case RunStatus::Cancelled:
    case RunStatus::Interrupted:
        return true;
    }
    return false;
}

bool known(OutcomeBriefState value) noexcept
{
    switch (value) {
    case OutcomeBriefState::ResolvedNoChange:
    case OutcomeBriefState::AwaitingReview:
    case OutcomeBriefState::AwaitingApply:
    case OutcomeBriefState::ResolvedApplied:
    case OutcomeBriefState::ResolvedRejected:
    case OutcomeBriefState::ResolvedMixed:
    case OutcomeBriefState::AttentionRequired:
    case OutcomeBriefState::AwaitingManualResolution:
    case OutcomeBriefState::ResolvedManuallyApplied:
    case OutcomeBriefState::SupersededByContinuation:
    case OutcomeBriefState::RecoveryOnly:
        return true;
    }
    return false;
}

bool known(FollowUpPlanEvent value) noexcept
{
    switch (value) {
    case FollowUpPlanEvent::Authorize:
    case FollowUpPlanEvent::Claim:
    case FollowUpPlanEvent::PlanSucceeded:
    case FollowUpPlanEvent::CompositionFailed:
    case FollowUpPlanEvent::MarkStale:
    case FollowUpPlanEvent::Expire:
    case FollowUpPlanEvent::Cancel:
    case FollowUpPlanEvent::Interrupt:
    case FollowUpPlanEvent::ConfirmDrafts:
    case FollowUpPlanEvent::Discard:
        return true;
    }
    return false;
}

bool terminalRunStatus(RunStatus status) noexcept
{
    return status == RunStatus::Completed || status == RunStatus::Failed
        || status == RunStatus::Cancelled || status == RunStatus::Interrupted;
}

bool normalResolvedState(OutcomeBriefState state) noexcept
{
    return state == OutcomeBriefState::ResolvedNoChange
        || state == OutcomeBriefState::ResolvedApplied
        || state == OutcomeBriefState::ResolvedRejected
        || state == OutcomeBriefState::ResolvedMixed;
}

} // namespace

const char* toString(FollowUpPlanKind value) noexcept
{
    switch (value) {
    case FollowUpPlanKind::NextRun: return "next_run";
    case FollowUpPlanKind::Recovery: return "recovery";
    }
    return "unknown";
}

const char* toString(FollowUpSplitHint value) noexcept
{
    switch (value) {
    case FollowUpSplitHint::One: return "one";
    case FollowUpSplitHint::Many: return "many";
    case FollowUpSplitHint::Auto: return "auto";
    }
    return "unknown";
}

const char* toString(FollowUpPlanRequestState value) noexcept
{
    switch (value) {
    case FollowUpPlanRequestState::Quoted: return "quoted";
    case FollowUpPlanRequestState::Authorized: return "authorized";
    case FollowUpPlanRequestState::Composing: return "composing";
    case FollowUpPlanRequestState::PlanReady: return "plan_ready";
    case FollowUpPlanRequestState::DraftsSaved: return "drafts_saved";
    case FollowUpPlanRequestState::Stale: return "stale";
    case FollowUpPlanRequestState::Expired: return "expired";
    case FollowUpPlanRequestState::Cancelled: return "cancelled";
    case FollowUpPlanRequestState::Discarded: return "discarded";
    case FollowUpPlanRequestState::Interrupted: return "interrupted";
    case FollowUpPlanRequestState::Failed: return "failed";
    }
    return "unknown";
}

const char* toString(FollowUpPlanEvent value) noexcept
{
    switch (value) {
    case FollowUpPlanEvent::Authorize: return "authorize";
    case FollowUpPlanEvent::Claim: return "claim";
    case FollowUpPlanEvent::PlanSucceeded: return "plan_succeeded";
    case FollowUpPlanEvent::CompositionFailed: return "composition_failed";
    case FollowUpPlanEvent::MarkStale: return "mark_stale";
    case FollowUpPlanEvent::Expire: return "expire";
    case FollowUpPlanEvent::Cancel: return "cancel";
    case FollowUpPlanEvent::Interrupt: return "interrupt";
    case FollowUpPlanEvent::ConfirmDrafts: return "confirm_drafts";
    case FollowUpPlanEvent::Discard: return "discard";
    }
    return "unknown";
}

std::optional<FollowUpPlanKind> followUpPlanKindFromString(std::string_view value) noexcept
{
    if (value == "next_run") return FollowUpPlanKind::NextRun;
    if (value == "recovery") return FollowUpPlanKind::Recovery;
    return std::nullopt;
}

std::optional<FollowUpSplitHint> followUpSplitHintFromString(std::string_view value) noexcept
{
    if (value == "one") return FollowUpSplitHint::One;
    if (value == "many") return FollowUpSplitHint::Many;
    if (value == "auto") return FollowUpSplitHint::Auto;
    return std::nullopt;
}

std::optional<FollowUpPlanRequestState> followUpPlanRequestStateFromString(std::string_view value) noexcept
{
    if (value == "quoted") return FollowUpPlanRequestState::Quoted;
    if (value == "authorized") return FollowUpPlanRequestState::Authorized;
    if (value == "composing") return FollowUpPlanRequestState::Composing;
    if (value == "plan_ready") return FollowUpPlanRequestState::PlanReady;
    if (value == "drafts_saved") return FollowUpPlanRequestState::DraftsSaved;
    if (value == "stale") return FollowUpPlanRequestState::Stale;
    if (value == "expired") return FollowUpPlanRequestState::Expired;
    if (value == "cancelled") return FollowUpPlanRequestState::Cancelled;
    if (value == "discarded") return FollowUpPlanRequestState::Discarded;
    if (value == "interrupted") return FollowUpPlanRequestState::Interrupted;
    if (value == "failed") return FollowUpPlanRequestState::Failed;
    return std::nullopt;
}

bool isOpenFollowUpPlanRequestState(FollowUpPlanRequestState state) noexcept
{
    return state == FollowUpPlanRequestState::Quoted
        || state == FollowUpPlanRequestState::Authorized
        || state == FollowUpPlanRequestState::Composing
        || state == FollowUpPlanRequestState::PlanReady;
}

bool isTerminalFollowUpPlanRequestState(FollowUpPlanRequestState state) noexcept
{
    return known(state) && !isOpenFollowUpPlanRequestState(state);
}

common::Result<FollowUpPlanRequestState> transitionFollowUpPlanRequest(
    FollowUpPlanRequestState state,
    FollowUpPlanEvent event)
{
    if (!known(state) || !known(event)) {
        return fail<FollowUpPlanRequestState>(
            "follow_up.invalid_transition", "The follow-up request state or event is unknown");
    }

    const auto moved = [](FollowUpPlanRequestState next) {
        return common::Result<FollowUpPlanRequestState>::ok(next);
    };
    switch (state) {
    case FollowUpPlanRequestState::Quoted:
        if (event == FollowUpPlanEvent::Authorize) return moved(FollowUpPlanRequestState::Authorized);
        if (event == FollowUpPlanEvent::MarkStale) return moved(FollowUpPlanRequestState::Stale);
        if (event == FollowUpPlanEvent::Expire) return moved(FollowUpPlanRequestState::Expired);
        if (event == FollowUpPlanEvent::Cancel) return moved(FollowUpPlanRequestState::Cancelled);
        if (event == FollowUpPlanEvent::Discard) return moved(FollowUpPlanRequestState::Discarded);
        break;
    case FollowUpPlanRequestState::Authorized:
        if (event == FollowUpPlanEvent::Claim) return moved(FollowUpPlanRequestState::Composing);
        if (event == FollowUpPlanEvent::MarkStale) return moved(FollowUpPlanRequestState::Stale);
        if (event == FollowUpPlanEvent::Expire) return moved(FollowUpPlanRequestState::Expired);
        if (event == FollowUpPlanEvent::Cancel) return moved(FollowUpPlanRequestState::Cancelled);
        break;
    case FollowUpPlanRequestState::Composing:
        if (event == FollowUpPlanEvent::PlanSucceeded) return moved(FollowUpPlanRequestState::PlanReady);
        if (event == FollowUpPlanEvent::CompositionFailed) return moved(FollowUpPlanRequestState::Failed);
        if (event == FollowUpPlanEvent::Cancel) return moved(FollowUpPlanRequestState::Cancelled);
        if (event == FollowUpPlanEvent::Interrupt) return moved(FollowUpPlanRequestState::Interrupted);
        break;
    case FollowUpPlanRequestState::PlanReady:
        if (event == FollowUpPlanEvent::ConfirmDrafts) return moved(FollowUpPlanRequestState::DraftsSaved);
        if (event == FollowUpPlanEvent::Discard) return moved(FollowUpPlanRequestState::Discarded);
        break;
    case FollowUpPlanRequestState::DraftsSaved:
    case FollowUpPlanRequestState::Stale:
    case FollowUpPlanRequestState::Expired:
    case FollowUpPlanRequestState::Cancelled:
    case FollowUpPlanRequestState::Discarded:
    case FollowUpPlanRequestState::Interrupted:
    case FollowUpPlanRequestState::Failed:
        break;
    }

    return fail<FollowUpPlanRequestState>(
        "follow_up.invalid_transition",
        "Event '" + std::string(toString(event)) + "' is not allowed from state '"
            + std::string(toString(state)) + "'");
}

FollowUpSourceWorkflow followUpSourceWorkflowFromId(std::string_view workflowId) noexcept
{
    const auto kind = outcomeWorkflowKind(workflowId);
    if (!kind.has_value()) return FollowUpSourceWorkflow::Unknown;
    switch (*kind) {
    case OutcomeWorkflowKind::OrdinaryManifest: return FollowUpSourceWorkflow::Manifest;
    case OutcomeWorkflowKind::VerificationContinuation: return FollowUpSourceWorkflow::VerificationContinuation;
    }
    // A future outcome workflow stays ineligible for a paid follow-up until it is mapped above on purpose.
    return FollowUpSourceWorkflow::Unknown;
}

common::Result<void> checkFollowUpSourceEligibility(
    FollowUpPlanKind kind,
    const FollowUpSourceFacts& facts)
{
    if (!known(kind)) {
        return refused("follow_up.invalid_kind", "The follow-up plan kind is unknown");
    }
    if (!known(facts.workflow)) {
        return refused(
            "follow_up.unsupported_source_workflow",
            "The source Run does not use a workflow that produces an eligible Outcome Brief");
    }
    if (!known(facts.terminalStatus)) {
        return refused("follow_up.invalid_run_status", "The source Run status is unknown");
    }
    if (!known(facts.briefState)) {
        return refused("follow_up.invalid_outcome_state", "The source Outcome Brief state is unknown");
    }
    if (!terminalRunStatus(facts.terminalStatus)) {
        return refused("follow_up.source_not_terminal", "The source Run is not terminal");
    }
    if (facts.terminalStatus == RunStatus::Cancelled) {
        return refused("follow_up.source_cancelled", "A user-cancelled Run cannot be a follow-up source");
    }
    if (facts.terminalStatus == RunStatus::Interrupted) {
        return refused("follow_up.source_interrupted", "An interrupted Run must be inspected before follow-up planning");
    }

    switch (facts.briefState) {
    case OutcomeBriefState::AwaitingReview:
    case OutcomeBriefState::AwaitingApply:
    case OutcomeBriefState::AwaitingManualResolution:
        return refused("follow_up.outcome_pending", "The source outcome is still awaiting resolution");
    case OutcomeBriefState::AttentionRequired:
        return refused("follow_up.outcome_attention_required", "The source outcome requires attention");
    case OutcomeBriefState::SupersededByContinuation:
        return refused("follow_up.outcome_superseded", "A continuation owns the source outcome");
    case OutcomeBriefState::ResolvedNoChange:
    case OutcomeBriefState::ResolvedApplied:
    case OutcomeBriefState::ResolvedRejected:
    case OutcomeBriefState::ResolvedMixed:
    case OutcomeBriefState::ResolvedManuallyApplied:
    case OutcomeBriefState::RecoveryOnly:
        break;
    }

    if (normalResolvedState(facts.briefState)) {
        if (facts.terminalStatus != RunStatus::Completed
            || facts.hasContinuationChild || facts.hasManualHandoff) {
            return refused("follow_up.inconsistent_source", "The resolved outcome facts are inconsistent");
        }
        if (kind != FollowUpPlanKind::NextRun) {
            return refused("follow_up.recovery_not_available", "This resolved outcome does not require a recovery plan");
        }
        return common::Result<void>::ok();
    }

    if (facts.briefState == OutcomeBriefState::ResolvedManuallyApplied) {
        if (facts.terminalStatus != RunStatus::Failed
            || !facts.hasManualHandoff || facts.hasContinuationChild) {
            return refused("follow_up.inconsistent_source", "The manually resolved outcome facts are inconsistent");
        }
        if (kind != FollowUpPlanKind::NextRun) {
            return refused("follow_up.recovery_not_available", "The manual resolution already settled this outcome");
        }
        return common::Result<void>::ok();
    }

    if (facts.briefState == OutcomeBriefState::RecoveryOnly) {
        if (facts.terminalStatus != RunStatus::Failed) {
            return refused("follow_up.inconsistent_source", "A recovery-only outcome must describe a Failed Run");
        }
        if (facts.hasContinuationChild || facts.hasManualHandoff) {
            return refused("follow_up.recovery_owned", "A continuation or manual handoff already owns recovery");
        }
        if (kind != FollowUpPlanKind::Recovery) {
            return refused("follow_up.recovery_flag_required", "Recovery-only outcomes require an explicit recovery plan");
        }
        return common::Result<void>::ok();
    }

    // Every state above is decided before this point, so this is unreachable
    // today; it keeps the gate closed by construction if the switch ever lags a
    // new OutcomeBriefState (the compiler only warns about that under /W4).
    return refused("follow_up.invalid_outcome_state", "The source Outcome Brief state is not admitted");
}

common::Result<FollowUpDraftLimits> followUpAuthorisedDraftLimits(
    FollowUpSplitHint splitHint,
    std::uint32_t maxDrafts)
{
    if (maxDrafts < 1 || maxDrafts > kFollowUpMaximumDrafts) {
        return fail<FollowUpDraftLimits>(
            "follow_up.invalid_draft_limits", "maxDrafts must be between 1 and " + std::to_string(kFollowUpMaximumDrafts));
    }
    switch (splitHint) {
    case FollowUpSplitHint::One:
        return common::Result<FollowUpDraftLimits>::ok({1, 1});
    case FollowUpSplitHint::Many:
        if (maxDrafts < kFollowUpManyMinimumDrafts) {
            return fail<FollowUpDraftLimits>(
                "follow_up.invalid_draft_limits",
                "A split into many drafts needs maxDrafts of at least " + std::to_string(kFollowUpManyMinimumDrafts));
        }
        return common::Result<FollowUpDraftLimits>::ok({kFollowUpManyMinimumDrafts, maxDrafts});
    case FollowUpSplitHint::Auto:
        return common::Result<FollowUpDraftLimits>::ok({1, maxDrafts});
    }
    return fail<FollowUpDraftLimits>("follow_up.invalid_draft_limits", "The split hint is unknown");
}

common::Result<void> checkFollowUpPlanAgainstAuthorisation(
    const NextRunPlan& plan,
    FollowUpSplitHint splitHint,
    std::uint32_t maxDrafts)
{
    const auto limits = followUpAuthorisedDraftLimits(splitHint, maxDrafts);
    if (!limits.success()) return common::Result<void>::failure(*limits.error());
    const std::size_t count = plan.drafts.size();
    if (count < limits.value().minimum || count > limits.value().maximum) {
        return refused(
            "follow_up.plan_draft_count_not_authorised",
            "The plan has " + std::to_string(count) + " drafts; the request authorised "
                + std::to_string(limits.value().minimum) + " to " + std::to_string(limits.value().maximum));
    }
    return common::Result<void>::ok();
}

} // namespace ai::application
