#include "ai/application/planning/FollowUpPlan.hpp"

#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/ManifestVerificationContinuationPreparationService.hpp"
#include "ai/application/ManifestVerificationManualHandoffTypes.hpp"
#include "ai/application/planning/OutcomeBriefReconciler.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace ai::application;

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

template <typename T>
void requireFailure(const ai::common::Result<T>& result, std::string_view code)
{
    require(!result.success(), "Expected failure " + std::string(code));
    require(result.error()->code == code,
        "Expected " + std::string(code) + ", got " + result.error()->code);
}

FollowUpSourceFacts facts(
    RunStatus status,
    OutcomeBriefState state,
    bool continuation = false,
    bool handoff = false,
    FollowUpSourceWorkflow workflow = FollowUpSourceWorkflow::Manifest)
{
    return {workflow, status, state, continuation, handoff};
}

void testTextRoundTripsAndUnknownValuesFailClosed()
{
    const std::array kinds {FollowUpPlanKind::NextRun, FollowUpPlanKind::Recovery};
    for (const auto value : kinds) {
        const auto parsed = followUpPlanKindFromString(toString(value));
        require(parsed.has_value() && *parsed == value, "Kind round-trip");
    }
    const std::array hints {FollowUpSplitHint::One, FollowUpSplitHint::Many, FollowUpSplitHint::Auto};
    for (const auto value : hints) {
        const auto parsed = followUpSplitHintFromString(toString(value));
        require(parsed.has_value() && *parsed == value, "Split-hint round-trip");
    }
    const std::array states {
        FollowUpPlanRequestState::Quoted, FollowUpPlanRequestState::Authorized,
        FollowUpPlanRequestState::Composing, FollowUpPlanRequestState::PlanReady,
        FollowUpPlanRequestState::DraftsSaved, FollowUpPlanRequestState::Stale,
        FollowUpPlanRequestState::Expired, FollowUpPlanRequestState::Cancelled,
        FollowUpPlanRequestState::Discarded, FollowUpPlanRequestState::Interrupted,
        FollowUpPlanRequestState::Failed};
    for (const auto value : states) {
        const auto parsed = followUpPlanRequestStateFromString(toString(value));
        require(parsed.has_value() && *parsed == value, "State round-trip");
    }

    require(!followUpPlanKindFromString("NEXT_RUN").has_value(), "Kind parsing is exact");
    require(!followUpSplitHintFromString("two").has_value(), "Unknown split hint");
    require(!followUpPlanRequestStateFromString("retrying").has_value(), "Unknown state");
    require(std::string(toString(static_cast<FollowUpPlanKind>(99))) == "unknown", "Unknown kind text");
    require(std::string(toString(static_cast<FollowUpSplitHint>(99))) == "unknown", "Unknown hint text");
    require(std::string(toString(static_cast<FollowUpPlanRequestState>(99))) == "unknown", "Unknown state text");
    require(std::string(toString(FollowUpPlanEvent::CompositionFailed)) == "composition_failed",
        "Composition failure names both provider and invalid-plan failures without narrowing the contract");
    require(std::string(toString(static_cast<FollowUpPlanEvent>(99))) == "unknown", "Unknown event text");
}

void testStateMachineHasOnlyTheAgreedEdges()
{
    using S = FollowUpPlanRequestState;
    using E = FollowUpPlanEvent;
    struct Edge { S from; E event; S to; };
    const std::vector<Edge> edges {
        {S::Quoted, E::Authorize, S::Authorized},
        {S::Quoted, E::MarkStale, S::Stale},
        {S::Quoted, E::Expire, S::Expired},
        {S::Quoted, E::Cancel, S::Cancelled},
        {S::Quoted, E::Discard, S::Discarded},
        {S::Authorized, E::Claim, S::Composing},
        {S::Authorized, E::MarkStale, S::Stale},
        {S::Authorized, E::Expire, S::Expired},
        {S::Authorized, E::Cancel, S::Cancelled},
        {S::Composing, E::PlanSucceeded, S::PlanReady},
        {S::Composing, E::CompositionFailed, S::Failed},
        {S::Composing, E::Cancel, S::Cancelled},
        {S::Composing, E::Interrupt, S::Interrupted},
        {S::PlanReady, E::ConfirmDrafts, S::DraftsSaved},
        {S::PlanReady, E::Discard, S::Discarded},
    };
    const std::array states {
        S::Quoted, S::Authorized, S::Composing, S::PlanReady, S::DraftsSaved,
        S::Stale, S::Expired, S::Cancelled, S::Discarded, S::Interrupted, S::Failed};
    const std::array events {
        E::Authorize, E::Claim, E::PlanSucceeded, E::CompositionFailed, E::MarkStale,
        E::Expire, E::Cancel, E::Interrupt, E::ConfirmDrafts, E::Discard};

    for (const auto state : states) {
        for (const auto event : events) {
            const auto expected = std::find_if(edges.begin(), edges.end(), [state, event](const Edge& edge) {
                return edge.from == state && edge.event == event;
            });
            const auto result = transitionFollowUpPlanRequest(state, event);
            if (expected == edges.end()) {
                requireFailure(result, "follow_up.invalid_transition");
            } else {
                require(result.success() && result.value() == expected->to,
                    "The agreed transition must produce its exact next state");
            }
        }
    }

    requireFailure(transitionFollowUpPlanRequest(static_cast<S>(99), E::Authorize),
        "follow_up.invalid_transition");
    requireFailure(transitionFollowUpPlanRequest(S::Quoted, static_cast<E>(99)),
        "follow_up.invalid_transition");
}

void testOpenAndTerminalStatesMatchTheFutureUniqueIndex()
{
    using S = FollowUpPlanRequestState;
    const std::array open {S::Quoted, S::Authorized, S::Composing, S::PlanReady};
    const std::array terminal {
        S::DraftsSaved, S::Stale, S::Expired, S::Cancelled, S::Discarded, S::Interrupted, S::Failed};
    for (const auto state : open) {
        require(isOpenFollowUpPlanRequestState(state), "Open state");
        require(!isTerminalFollowUpPlanRequestState(state), "Open is not terminal");
    }
    for (const auto state : terminal) {
        require(!isOpenFollowUpPlanRequestState(state), "Terminal is not open");
        require(isTerminalFollowUpPlanRequestState(state), "Terminal state");
    }
    const auto unknown = static_cast<S>(99);
    require(!isOpenFollowUpPlanRequestState(unknown), "Unknown is not open");
    require(!isTerminalFollowUpPlanRequestState(unknown), "Unknown is not terminal");
}

void testCompletedResolvedOutcomesAdmitOnlyNextRun()
{
    const std::array resolved {
        OutcomeBriefState::ResolvedNoChange, OutcomeBriefState::ResolvedApplied,
        OutcomeBriefState::ResolvedRejected, OutcomeBriefState::ResolvedMixed};
    for (const auto state : resolved) {
        require(checkFollowUpSourceEligibility(
                    FollowUpPlanKind::NextRun, facts(RunStatus::Completed, state)).success(),
            "A normal resolved outcome admits a next-Run plan");
        requireFailure(checkFollowUpSourceEligibility(
                           FollowUpPlanKind::Recovery, facts(RunStatus::Completed, state)),
            "follow_up.recovery_not_available");
        requireFailure(checkFollowUpSourceEligibility(
                           FollowUpPlanKind::NextRun, facts(RunStatus::Completed, state, true, false)),
            "follow_up.inconsistent_source");
        requireFailure(checkFollowUpSourceEligibility(
                           FollowUpPlanKind::NextRun, facts(RunStatus::Completed, state, false, true)),
            "follow_up.inconsistent_source");
        requireFailure(checkFollowUpSourceEligibility(
                           FollowUpPlanKind::NextRun, facts(RunStatus::Failed, state)),
            "follow_up.inconsistent_source");
    }
}

void testManualResolutionAdmitsARegularNextRunOnly()
{
    const auto valid = facts(RunStatus::Failed, OutcomeBriefState::ResolvedManuallyApplied, false, true);
    require(checkFollowUpSourceEligibility(FollowUpPlanKind::NextRun, valid).success(),
        "A fully applied manual resolution admits the next normal Run");
    requireFailure(checkFollowUpSourceEligibility(FollowUpPlanKind::Recovery, valid),
        "follow_up.recovery_not_available");
    requireFailure(checkFollowUpSourceEligibility(FollowUpPlanKind::NextRun,
                       facts(RunStatus::Failed, OutcomeBriefState::ResolvedManuallyApplied)),
        "follow_up.inconsistent_source");
    requireFailure(checkFollowUpSourceEligibility(FollowUpPlanKind::NextRun,
                       facts(RunStatus::Failed, OutcomeBriefState::ResolvedManuallyApplied, true, true)),
        "follow_up.inconsistent_source");
    requireFailure(checkFollowUpSourceEligibility(FollowUpPlanKind::NextRun,
                       facts(RunStatus::Completed, OutcomeBriefState::ResolvedManuallyApplied, false, true)),
        "follow_up.inconsistent_source");
}

void testRecoveryOnlyNeedsTheExplicitUnownedFailedPath()
{
    const auto valid = facts(RunStatus::Failed, OutcomeBriefState::RecoveryOnly);
    require(checkFollowUpSourceEligibility(FollowUpPlanKind::Recovery, valid).success(),
        "An unowned Failed recovery-only outcome admits an explicit recovery plan");
    requireFailure(checkFollowUpSourceEligibility(FollowUpPlanKind::NextRun, valid),
        "follow_up.recovery_flag_required");
    requireFailure(checkFollowUpSourceEligibility(FollowUpPlanKind::Recovery,
                       facts(RunStatus::Failed, OutcomeBriefState::RecoveryOnly, true, false)),
        "follow_up.recovery_owned");
    requireFailure(checkFollowUpSourceEligibility(FollowUpPlanKind::Recovery,
                       facts(RunStatus::Failed, OutcomeBriefState::RecoveryOnly, false, true)),
        "follow_up.recovery_owned");
    requireFailure(checkFollowUpSourceEligibility(FollowUpPlanKind::Recovery,
                       facts(RunStatus::Completed, OutcomeBriefState::RecoveryOnly)),
        "follow_up.inconsistent_source");
}

void testPendingAttentionAndSupersededOutcomesAreRefused()
{
    const std::array pending {
        OutcomeBriefState::AwaitingReview, OutcomeBriefState::AwaitingApply,
        OutcomeBriefState::AwaitingManualResolution};
    for (const auto state : pending) {
        requireFailure(checkFollowUpSourceEligibility(
                           FollowUpPlanKind::NextRun, facts(RunStatus::Completed, state)),
            "follow_up.outcome_pending");
    }
    requireFailure(checkFollowUpSourceEligibility(FollowUpPlanKind::NextRun,
                       facts(RunStatus::Completed, OutcomeBriefState::AttentionRequired)),
        "follow_up.outcome_attention_required");
    requireFailure(checkFollowUpSourceEligibility(FollowUpPlanKind::NextRun,
                       facts(RunStatus::Failed, OutcomeBriefState::SupersededByContinuation, true, false)),
        "follow_up.outcome_superseded");
}

void testRunStatusAndUnknownEnumsFailClosedBeforeAdmission()
{
    requireFailure(checkFollowUpSourceEligibility(FollowUpPlanKind::Recovery, FollowUpSourceFacts {}),
        "follow_up.unsupported_source_workflow");
    const std::array nonTerminal {
        RunStatus::Prepared, RunStatus::Queued, RunStatus::Running, RunStatus::Cancelling};
    for (const auto status : nonTerminal) {
        requireFailure(checkFollowUpSourceEligibility(FollowUpPlanKind::NextRun,
                           facts(status, OutcomeBriefState::ResolvedNoChange)),
            "follow_up.source_not_terminal");
    }
    requireFailure(checkFollowUpSourceEligibility(FollowUpPlanKind::NextRun,
                       facts(RunStatus::Cancelled, OutcomeBriefState::RecoveryOnly)),
        "follow_up.source_cancelled");
    requireFailure(checkFollowUpSourceEligibility(FollowUpPlanKind::Recovery,
                       facts(RunStatus::Interrupted, OutcomeBriefState::RecoveryOnly)),
        "follow_up.source_interrupted");
    requireFailure(checkFollowUpSourceEligibility(static_cast<FollowUpPlanKind>(99),
                       facts(RunStatus::Completed, OutcomeBriefState::ResolvedNoChange)),
        "follow_up.invalid_kind");
    requireFailure(checkFollowUpSourceEligibility(FollowUpPlanKind::NextRun,
                       facts(static_cast<RunStatus>(99), OutcomeBriefState::ResolvedNoChange)),
        "follow_up.invalid_run_status");
    requireFailure(checkFollowUpSourceEligibility(FollowUpPlanKind::NextRun,
                       facts(RunStatus::Completed, static_cast<OutcomeBriefState>(99))),
        "follow_up.invalid_outcome_state");
    requireFailure(checkFollowUpSourceEligibility(FollowUpPlanKind::NextRun,
                       facts(RunStatus::Completed, OutcomeBriefState::ResolvedNoChange,
                           false, false, FollowUpSourceWorkflow::Unknown)),
        "follow_up.unsupported_source_workflow");
    requireFailure(checkFollowUpSourceEligibility(FollowUpPlanKind::NextRun,
                       facts(RunStatus::Completed, OutcomeBriefState::ResolvedNoChange,
                           false, false, static_cast<FollowUpSourceWorkflow>(99))),
        "follow_up.unsupported_source_workflow");
}

void testEligibilityIsExactlyTheFullWhitelistMatrix()
{
    const std::array workflows {
        FollowUpSourceWorkflow::Unknown,
        FollowUpSourceWorkflow::Manifest,
        FollowUpSourceWorkflow::VerificationContinuation,
        static_cast<FollowUpSourceWorkflow>(99)};
    const std::array statuses {
        RunStatus::Prepared, RunStatus::Queued, RunStatus::Running, RunStatus::Cancelling,
        RunStatus::Completed, RunStatus::Failed, RunStatus::Cancelled, RunStatus::Interrupted};
    const std::array states {
        OutcomeBriefState::ResolvedNoChange,
        OutcomeBriefState::AwaitingReview,
        OutcomeBriefState::AwaitingApply,
        OutcomeBriefState::ResolvedApplied,
        OutcomeBriefState::ResolvedRejected,
        OutcomeBriefState::ResolvedMixed,
        OutcomeBriefState::AttentionRequired,
        OutcomeBriefState::AwaitingManualResolution,
        OutcomeBriefState::ResolvedManuallyApplied,
        OutcomeBriefState::SupersededByContinuation,
        OutcomeBriefState::RecoveryOnly};
    const std::array kinds {FollowUpPlanKind::NextRun, FollowUpPlanKind::Recovery};

    const auto supportedWorkflow = [](FollowUpSourceWorkflow workflow) {
        return workflow == FollowUpSourceWorkflow::Manifest
            || workflow == FollowUpSourceWorkflow::VerificationContinuation;
    };
    const auto normallyResolved = [](OutcomeBriefState state) {
        return state == OutcomeBriefState::ResolvedNoChange
            || state == OutcomeBriefState::ResolvedApplied
            || state == OutcomeBriefState::ResolvedRejected
            || state == OutcomeBriefState::ResolvedMixed;
    };

    std::size_t combinations = 0;
    std::size_t admitted = 0;
    for (const auto workflow : workflows) {
        for (const auto status : statuses) {
            for (const auto state : states) {
                for (const auto kind : kinds) {
                    for (const bool continuation : {false, true}) {
                        for (const bool handoff : {false, true}) {
                            ++combinations;
                            const bool expected = supportedWorkflow(workflow)
                                && ((status == RunStatus::Completed && normallyResolved(state)
                                        && kind == FollowUpPlanKind::NextRun
                                        && !continuation && !handoff)
                                    || (status == RunStatus::Failed
                                        && state == OutcomeBriefState::ResolvedManuallyApplied
                                        && kind == FollowUpPlanKind::NextRun
                                        && !continuation && handoff)
                                    || (status == RunStatus::Failed
                                        && state == OutcomeBriefState::RecoveryOnly
                                        && kind == FollowUpPlanKind::Recovery
                                        && !continuation && !handoff));
                            const auto result = checkFollowUpSourceEligibility(
                                kind, facts(status, state, continuation, handoff, workflow));
                            require(result.success() == expected,
                                "Eligibility must equal the explicit whitelist for every fact tuple");
                            if (result.success()) ++admitted;
                        }
                    }
                }
            }
        }
    }

    require(combinations == 2816, "The full gate cross-product must be exercised");
    require(admitted == 12, "Only the twelve supported workflow/outcome tuples are admissible");
}

void testWorkflowMappingIsDrivenByTheSharedOutcomeWorkflowTable()
{
    // Every workflow the Outcome Brief accepts, now or later, must be mapped here on purpose. A new entry of the
    // shared table fails this loop until the follow-up planner decides about it; at runtime it stays Unknown.
    std::set<FollowUpSourceWorkflow> mapped;
    for (const auto& entry : outcomeWorkflows()) {
        const auto workflow = followUpSourceWorkflowFromId(entry.id);
        require(workflow != FollowUpSourceWorkflow::Unknown,
            "An outcome workflow of the shared table is not mapped for follow-up planning: " + std::string(entry.id));
        require(mapped.insert(workflow).second, "Two outcome workflows must not share one follow-up workflow");
        require(isOutcomeBriefWorkflow(std::string(entry.id)), "The reconciler wrapper must accept every table id");
    }
    require(mapped.size() == outcomeWorkflows().size(), "Every table entry maps to its own follow-up workflow");

    require(followUpSourceWorkflowFromId(kExecutionManifestWorkflowId) == FollowUpSourceWorkflow::Manifest,
        "The ordinary Manifest workflow id maps to Manifest");
    require(followUpSourceWorkflowFromId(kManifestVerificationContinuationWorkflowId)
            == FollowUpSourceWorkflow::VerificationContinuation,
        "The paid continuation workflow id maps to VerificationContinuation");

    std::string embeddedNul = kExecutionManifestWorkflowId;
    embeddedNul.push_back('\0');
    embeddedNul += "x";
    const std::vector<std::string> unsupported {
        kMockWorkflowId,
        kExecutionProfileWorkflowId,
        std::string(kManifestVerificationManualHandoffWorkflowId),
        "read-only-analysis-v1",
        "",
        "execution-manifest",
        "execution-manifest-v1 ",
        " execution-manifest-v1",
        "EXECUTION-MANIFEST-V1",
        "execution-manifest-v1-extra",
        std::string(kManifestVerificationContinuationWorkflowId) + "\n",
        embeddedNul};
    for (const auto& id : unsupported) {
        require(followUpSourceWorkflowFromId(id) == FollowUpSourceWorkflow::Unknown,
            "Every other workflow id, and every decorated spelling, maps to Unknown");
        require(!isOutcomeBriefWorkflow(id), "The reconciler must refuse the same ids");
    }

    // The manual-handoff attempt Run is resolution evidence for its anchor, never a source.
    const auto handoff = followUpSourceWorkflowFromId(kManifestVerificationManualHandoffWorkflowId);
    requireFailure(checkFollowUpSourceEligibility(FollowUpPlanKind::NextRun,
                       facts(RunStatus::Completed, OutcomeBriefState::ResolvedNoChange, false, false, handoff)),
        "follow_up.unsupported_source_workflow");
}

// The claim (`authorized -> composing`) precedes the provider call and `plan_ready` exists only after it, so a request that never
// left `quoted` / `authorized` was free and every request that left `composing` / `plan_ready` may have been charged. This pins
// that fact on the real table: which terminal states can only be free, which can only be charged, and which are both - so a
// screen or a repository can never derive "free" from a terminal state's name alone.
void testStateNamesDoNotSayWhetherARequestWasFree()
{
    using S = FollowUpPlanRequestState;
    using E = FollowUpPlanEvent;
    const std::array states {
        S::Quoted, S::Authorized, S::Composing, S::PlanReady, S::DraftsSaved,
        S::Stale, S::Expired, S::Cancelled, S::Discarded, S::Interrupted, S::Failed};
    const std::array events {
        E::Authorize, E::Claim, E::PlanSucceeded, E::CompositionFailed, E::MarkStale,
        E::Expire, E::Cancel, E::Interrupt, E::ConfirmDrafts, E::Discard};

    const auto beforeClaim = [](S state) { return state == S::Quoted || state == S::Authorized; };
    std::array<bool, 11> fromFree {};
    std::array<bool, 11> fromCharged {};
    const auto indexOf = [&states](S state) {
        return static_cast<std::size_t>(std::find(states.begin(), states.end(), state) - states.begin());
    };
    for (const auto from : states) {
        for (const auto event : events) {
            const auto next = transitionFollowUpPlanRequest(from, event);
            if (!next.success() || !isTerminalFollowUpPlanRequestState(next.value())) continue;
            (beforeClaim(from) ? fromFree : fromCharged)[indexOf(next.value())] = true;
        }
    }

    for (const auto state : {S::Stale, S::Expired}) {
        require(fromFree[indexOf(state)] && !fromCharged[indexOf(state)],
            "stale and expired are reachable only before the claim, so they are always free");
    }
    for (const auto state : {S::DraftsSaved, S::Interrupted, S::Failed}) {
        require(!fromFree[indexOf(state)] && fromCharged[indexOf(state)],
            "drafts_saved, interrupted and failed are reachable only after the claim, so they are possibly charged");
    }
    for (const auto state : {S::Cancelled, S::Discarded}) {
        require(fromFree[indexOf(state)] && fromCharged[indexOf(state)],
            "cancelled and discarded are reachable both before and after the claim: the name alone never says free");
    }
    require(transitionFollowUpPlanRequest(S::Quoted, E::Discard).value() == S::Discarded
            && transitionFollowUpPlanRequest(S::PlanReady, E::Discard).value() == S::Discarded
            && !transitionFollowUpPlanRequest(S::Authorized, E::Discard).success(),
        "An explicit discard of a ready plan is a possibly-charged discard; only a replaced quote is a free one");
    for (const auto from : states) {
        for (const auto event : events) {
            const auto next = transitionFollowUpPlanRequest(from, event);
            if (next.success() && next.value() == S::PlanReady) {
                require(from == S::Composing, "plan_ready is reached only from composing, that is after the provider call");
            }
        }
    }
    require(transitionFollowUpPlanRequest(S::Composing, E::PlanSucceeded).success(),
        "plan_ready must be reachable from composing");
}

// The agreed matrix, written out cell by cell (not derived from a formula the code could share):
//   maxDrafts   auto      one      many
//   0, 5, ...   invalid   invalid  invalid
//   1           1..1      1..1     invalid (asks for two drafts, allows one)
//   2           1..2      1..1     2..2
//   3           1..3      1..1     2..3
//   4           1..4      1..1     2..4
struct Cell {
    FollowUpSplitHint hint;
    std::uint32_t maxDrafts;
    bool valid;
    std::uint32_t lo;
    std::uint32_t hi;
};

const std::vector<Cell>& matrix()
{
    using H = FollowUpSplitHint;
    static const std::vector<Cell> cells = {
        {H::Auto, 0, false, 0, 0}, {H::One, 0, false, 0, 0}, {H::Many, 0, false, 0, 0},
        {H::Auto, 1, true, 1, 1},  {H::One, 1, true, 1, 1},  {H::Many, 1, false, 0, 0},
        {H::Auto, 2, true, 1, 2},  {H::One, 2, true, 1, 1},  {H::Many, 2, true, 2, 2},
        {H::Auto, 3, true, 1, 3},  {H::One, 3, true, 1, 1},  {H::Many, 3, true, 2, 3},
        {H::Auto, 4, true, 1, 4},  {H::One, 4, true, 1, 1},  {H::Many, 4, true, 2, 4},
        {H::Auto, 5, false, 0, 0}, {H::One, 5, false, 0, 0}, {H::Many, 5, false, 0, 0},
        {H::Auto, 0xFFFFFFFFu, false, 0, 0}, {H::Many, 0xFFFFFFFFu, false, 0, 0}};
    return cells;
}

NextRunPlan planWithDrafts(std::size_t count)
{
    std::string drafts;
    for (std::size_t index = 1; index <= count; ++index) {
        if (index != 1) drafts += ",";
        drafts += "{\"key\":\"d" + std::to_string(index) + "\",\"title\":\"Draft " + std::to_string(index)
            + "\",\"task\":\"First inspect the files, then change them.\",\"dependsOn\":[],"
              "\"sharedContractIds\":[],\"contracts\":[]}";
    }
    const auto parsed = parseNextRunPlan(
        "{\"schemaVersion\":\"next-run-plan.v1\",\"title\":\"Plan\",\"sharedContracts\":[],\"drafts\":[" + drafts + "]}");
    require(parsed.success(), "A plan of " + std::to_string(count) + " drafts must parse");
    return parsed.value();
}

void testTheAuthorisedDraftWindowIsTheAgreedMatrix()
{
    for (const auto& cell : matrix()) {
        const std::string name = std::string(toString(cell.hint)) + " with maxDrafts " + std::to_string(cell.maxDrafts);
        const auto limits = followUpAuthorisedDraftLimits(cell.hint, cell.maxDrafts);
        if (!cell.valid) {
            requireFailure(limits, "follow_up.invalid_draft_limits");
            continue;
        }
        require(limits.success(), name + " must describe a window");
        require(limits.value().minimum == cell.lo && limits.value().maximum == cell.hi,
            name + " must be " + std::to_string(cell.lo) + ".." + std::to_string(cell.hi) + ", got "
                + std::to_string(limits.value().minimum) + ".." + std::to_string(limits.value().maximum));
    }
    requireFailure(followUpAuthorisedDraftLimits(static_cast<FollowUpSplitHint>(99), 2), "follow_up.invalid_draft_limits");
    require(kFollowUpMaximumDrafts == 4 && kFollowUpManyMinimumDrafts == 2, "The agreed constants");
}

void testAReturnedPlanMustFitTheAuthorisedDraftWindow()
{
    // Every cell against every draft count the shared parser can deliver (1..8) - and the user's own scenario: one
    // draft allowed, eight returned by a parser that accepts eight.
    std::vector<NextRunPlan> plans;
    for (std::size_t count = 1; count <= kNextRunPlanMaxDrafts; ++count) plans.push_back(planWithDrafts(count));
    for (const auto& cell : matrix()) {
        for (std::size_t count = 1; count <= plans.size(); ++count) {
            const auto result = checkFollowUpPlanAgainstAuthorisation(plans[count - 1], cell.hint, cell.maxDrafts);
            const std::string name = std::string(toString(cell.hint)) + "/" + std::to_string(cell.maxDrafts) + " with "
                + std::to_string(count) + " drafts";
            if (!cell.valid) {
                // An unusable request is reported as such, even for a draft count that would have fitted.
                requireFailure(result, "follow_up.invalid_draft_limits");
            } else if (count >= cell.lo && count <= cell.hi) {
                require(result.success(), name + " must be accepted");
            } else {
                requireFailure(result, "follow_up.plan_draft_count_not_authorised");
            }
        }
    }
    const auto eight = checkFollowUpPlanAgainstAuthorisation(plans[7], FollowUpSplitHint::Auto, 1);
    require(!eight.success() && eight.error()->message.find('8') != std::string::npos
            && eight.error()->message.find("1 to 1") != std::string::npos,
        "The refusal says what was returned and what was authorised");

    NextRunPlan empty;
    requireFailure(checkFollowUpPlanAgainstAuthorisation(empty, FollowUpSplitHint::Auto, 4),
        "follow_up.plan_draft_count_not_authorised");
    NextRunPlan nine;
    nine.drafts.resize(9);
    requireFailure(checkFollowUpPlanAgainstAuthorisation(nine, FollowUpSplitHint::Auto, 4),
        "follow_up.plan_draft_count_not_authorised");
}

} // namespace

int main()
{
    try {
        testTextRoundTripsAndUnknownValuesFailClosed();
        testStateMachineHasOnlyTheAgreedEdges();
        testOpenAndTerminalStatesMatchTheFutureUniqueIndex();
        testCompletedResolvedOutcomesAdmitOnlyNextRun();
        testManualResolutionAdmitsARegularNextRunOnly();
        testRecoveryOnlyNeedsTheExplicitUnownedFailedPath();
        testPendingAttentionAndSupersededOutcomesAreRefused();
        testRunStatusAndUnknownEnumsFailClosedBeforeAdmission();
        testEligibilityIsExactlyTheFullWhitelistMatrix();
        testWorkflowMappingIsDrivenByTheSharedOutcomeWorkflowTable();
        testStateNamesDoNotSayWhetherARequestWasFree();
        testTheAuthorisedDraftWindowIsTheAgreedMatrix();
        testAReturnedPlanMustFitTheAuthorisedDraftWindow();
        std::cout << "FollowUpPlan tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FollowUpPlan test failure: " << error.what() << '\n';
        return 1;
    }
}
