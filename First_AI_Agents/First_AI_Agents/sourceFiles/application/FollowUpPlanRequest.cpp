#include "ai/application/FollowUpPlanRequest.hpp"

#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/FollowUpPlanAdvisories.hpp"
#include "ai/application/ProjectContextSnapshot.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace ai::application {
namespace {

using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

constexpr std::uint64_t kMaxUnsigned = std::numeric_limits<std::int64_t>::max();

common::Result<void> invalid(std::string message)
{
    return common::Result<void>::failure({"follow_up.invalid_request", std::move(message)});
}

template <typename T>
common::Result<T> failWith(std::string code, std::string message)
{
    return common::Result<T>::failure({std::move(code), std::move(message)});
}

bool lowerHex64(std::string_view value) noexcept
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

bool hasControl(std::string_view text) noexcept
{
    return std::any_of(text.begin(), text.end(), [](char raw) {
        const auto ch = static_cast<unsigned char>(raw);
        return ch < 0x20 || ch == 0x7F;
    });
}

// Text that SQLite stores as TEXT and nlohmann can serialise without throwing: valid UTF-8, no NUL.
bool storableText(std::string_view text) noexcept
{
    return projectContextIsValidUtf8(text) && text.find('\0') == std::string_view::npos;
}

bool wholeMilliseconds(TimePoint value) noexcept
{
    return followUpFloorToMilliseconds(value) == value;
}

bool knownKind(FollowUpPlanKind kind) noexcept
{
    return followUpPlanKindFromString(toString(kind)).has_value();
}

bool knownSplitHint(FollowUpSplitHint hint) noexcept
{
    return followUpSplitHintFromString(toString(hint)).has_value();
}

bool knownState(FollowUpPlanRequestState state) noexcept
{
    return followUpPlanRequestStateFromString(toString(state)).has_value();
}

common::Result<void> validateFiles(const std::vector<FollowUpPinnedFile>& files)
{
    if (files.size() > kFollowUpComposerMaximumFiles) {
        return invalid("A request pins at most " + std::to_string(kFollowUpComposerMaximumFiles) + " files");
    }
    std::set<std::string> paths;
    for (const auto& file : files) {
        if (file.relativePath.empty() || file.relativePath.size() > kFollowUpComposerMaximumPathBytes
                || !storableText(file.relativePath) || hasControl(file.relativePath)) {
            return invalid("A pinned file needs a printable relative path of 1-1024 bytes");
        }
        if (!paths.insert(file.relativePath).second) return invalid("Pinned file paths are unique");
        if (!lowerHex64(file.contentSha256)) return invalid("A pinned file needs the 64-hex sha256 of its complete content");
        if (file.shownBytes > file.originalBytes || file.truncated != (file.shownBytes < file.originalBytes)) {
            return invalid("A pinned file's truncation flag must agree with its shown and original bytes");
        }
        if (file.estimatedTokens == 0 || file.estimatedTokens > kFollowUpComposerPerFileTokens) {
            return invalid("A pinned file's estimate is 1-1500 tokens");
        }
    }
    // At most 8 files of at most 1 500 tokens each can never exceed the 12 000-token excerpt share, so no sum is kept.
    return common::Result<void>::ok();
}

common::Result<void> validateQuote(const FollowUpComposerQuote& quote)
{
    const auto& worst = quote.worstCase;
    const auto& pricing = worst.pricingIdentity;
    const auto& recipient = quote.providerIdentity;

    if (quote.policyId != kFollowUpComposerPromptPolicyId) return invalid("The quote names another composer policy");
    if (!lowerHex64(quote.contextFingerprint)) return invalid("The quote needs the 64-hex sealed context fingerprint");

    // The recipient of the bytes.
    if (recipient.providerId.empty() || recipient.providerId.size() > 128 || !storableText(recipient.providerId)) {
        return invalid("The quote needs the recipient provider id");
    }
    if (recipient.kind.empty() || recipient.kind.size() > 64 || !storableText(recipient.kind) || !isKnownProviderKind(recipient.kind)) {
        return invalid("The quote needs a known recipient provider kind");
    }
    if (recipient.baseUrl.size() > 2048 || !storableText(recipient.baseUrl)) {
        return invalid("The recipient base URL is at most 2048 bytes of text");
    }
    if (recipient.credentialReference.has_value()
            && (recipient.credentialReference->size() > 1024 || !storableText(*recipient.credentialReference))) {
        return invalid("The recipient credential reference is at most 1024 bytes of text");
    }
    if (!lowerHex64(quote.providerIdentityFingerprint)
            || quote.providerIdentityFingerprint != followUpProviderExecutionIdentityFingerprint(recipient)) {
        return invalid("The recipient identity does not match its fingerprint");
    }

    // The pricing identity.
    if (pricing.providerId != recipient.providerId) return invalid("The pricing identity names another provider than the recipient");
    if (pricing.modelId.empty() || pricing.modelId.size() > 128 || !storableText(pricing.modelId)
            || pricing.apiModelId.size() > 256 || !storableText(pricing.apiModelId)) {
        return invalid("The pricing identity needs a model id and a text API model id");
    }
    if (!models::isKnown(pricing.promptCache.mode)
            || !(pricing.promptCache == followUpPromptCachePolicy(recipient.kind))) {
        return invalid("The cache policy must be the one the recipient's provider kind implies");
    }
    if (!lowerHex64(worst.pricingIdentityFingerprint)
            || worst.pricingIdentityFingerprint != providerRequestPricingIdentityFingerprint(pricing)) {
        return invalid("The pricing identity does not match its fingerprint");
    }

    // The numbers.
    if (!worst.priceKnown) return invalid("A stored quote has a known price");
    if (worst.estimatedInputTokens == 0 || worst.estimatedInputTokens > kFollowUpComposerTotalTokens) {
        return invalid("The estimated input is 1-48 000 tokens");
    }
    if (worst.cacheableInputTokens > worst.estimatedInputTokens
            || pricing.promptCache.enabled() != (worst.cacheableInputTokens > 0)) {
        return invalid("The cacheable prefix is part of the input and exists exactly when the cache policy is enabled");
    }
    if (worst.maximumOutputTokens < kFollowUpMinimumOutputTokens || worst.maximumOutputTokens > kFollowUpInitialMaxOutputTokens) {
        return invalid("The effective output limit is 4 096-16 384 tokens");
    }
    if (quote.capMicros == 0 || quote.capMicros > kFollowUpSystemCeilingMicros || worst.estimatedCostMicros > quote.capMicros) {
        return invalid("The worst-case cost must fit a cap of at most the system ceiling");
    }
    if (worst.maximumOutputTokens > pricing.modelMaxOutputTokens
            || pricing.contextWindowTokens < worst.estimatedInputTokens
            || worst.maximumOutputTokens > pricing.contextWindowTokens - worst.estimatedInputTokens) {
        return invalid("The input and the effective output must fit the model's window and output limit");
    }
    return common::Result<void>::ok();
}

common::Result<void> validateAnswer(const FollowUpPlanAnswer& answer)
{
    if (answer.rawReply.empty() || answer.rawReply.size() > kFollowUpMaxReplyBytes || !storableText(answer.rawReply)) {
        return invalid("A stored reply is 1-262 144 bytes of text");
    }
    if (!lowerHex64(answer.planFingerprint)) return invalid("A stored plan needs its 64-hex fingerprint");
    if (answer.advisoriesJson.size() > kFollowUpMaxAdvisoriesBytes || !storableText(answer.advisoriesJson)) {
        return invalid("The stored advisories are at most 65 536 bytes of text");
    }
    const auto parsed = nlohmann::json::parse(answer.advisoriesJson, nullptr, false);
    if (!parsed.is_array()) return invalid("The stored advisories are a JSON array");
    return common::Result<void>::ok();
}

common::Result<void> validateFailure(const FollowUpPlanFailure& failure)
{
    if (failure.code.empty() || failure.code.size() > kFollowUpMaxFailureCodeBytes || !storableText(failure.code)
            || hasControl(failure.code)) {
        return invalid("A failure has a code of 1-128 bytes");
    }
    if (failure.message.size() > kFollowUpMaxFailureMessageBytes || !storableText(failure.message)) {
        return invalid("A failure message is at most 4 096 bytes of text");
    }
    if (failure.rawReply.has_value()
            && (failure.rawReply->size() > kFollowUpMaxReplyBytes || !storableText(*failure.rawReply))) {
        return invalid("A stored invalid reply is at most 262 144 bytes of text");
    }
    return common::Result<void>::ok();
}

// What each state requires and forbids. `claimedAt` is the free-versus-charged stamp: it exists for every request that left
// the claim behind, and never for a request that could not have called the provider.
common::Result<void> validateLifecycle(const FollowUpPlanRequest& r)
{
    const bool authorized = r.authorizedAt.has_value();
    const bool claimed = r.claimedAt.has_value();
    const bool finished = r.finishedAt.has_value();
    const bool answered = r.answer.has_value();
    const bool failed = r.failure.has_value();
    const bool grouped = r.draftGroupId.has_value();
    // The accounting of the paid call: it exists exactly when a provider RESPONSE arrived (always for an answer, optionally for an
    // invalid answer or a stop after a complete reply), so it needs the claim and never exists before the request is settled.
    const bool accounted = r.accounting.has_value();
    using S = FollowUpPlanRequestState;

    bool ok = false;
    switch (r.state) {
    case S::Quoted: ok = !authorized && !claimed && !finished && !answered && !failed && !grouped && !accounted; break;
    case S::Authorized: ok = authorized && !claimed && !finished && !answered && !failed && !grouped && !accounted; break;
    case S::Composing: ok = authorized && claimed && !finished && !answered && !failed && !grouped && !accounted; break;
    case S::PlanReady: ok = authorized && claimed && !finished && answered && !failed && !grouped && accounted; break;
    case S::DraftsSaved: ok = authorized && claimed && finished && answered && !failed && grouped && accounted; break;
    case S::Failed: ok = authorized && claimed && finished && !answered && failed && !grouped; break;
    case S::Interrupted: ok = authorized && claimed && finished && !answered && !failed && !grouped && !accounted; break;
    case S::Stale:
    case S::Expired: ok = !claimed && finished && !answered && !failed && !grouped && !accounted; break;
    case S::Cancelled: ok = finished && !answered && !failed && !grouped && (!claimed || authorized) && (!accounted || claimed); break;
    // A discarded quote is free and was never authorised (quoted -> discarded); a discarded plan_ready plan went through the
    // whole claim, carries its answer and its accounting and is NOT free. No other shape has an edge that could have produced it.
    case S::Discarded:
        ok = finished && !failed && !grouped
            && ((!authorized && !claimed && !answered && !accounted) || (authorized && claimed && answered && accounted));
        break;
    }
    if (!ok) {
        return invalid(std::string("The fields of a '") + toString(r.state) + "' request do not match its state");
    }
    // The transitions set the stamp of a non-terminal step and the update time from ONE value, so in the state that step leaves
    // behind the two are equal. (plan_ready is later: its update time is the answer's stamp.)
    if ((r.state == S::Quoted && r.updatedAt != r.createdAt)
            || (r.state == S::Authorized && r.updatedAt != *r.authorizedAt)
            || (r.state == S::Composing && r.updatedAt != *r.claimedAt)) {
        return invalid(std::string("A '") + toString(r.state) + "' request was last updated by the step that made it so");
    }

    // The time chain.
    if (r.quoteExpiresAt != r.createdAt + kFollowUpQuoteLifetime) {
        return invalid("The quote expires exactly 15 minutes after it was created");
    }
    if (r.updatedAt < r.createdAt) return invalid("A request is not updated before it was created");
    if (r.authorizedAt.has_value() && (*r.authorizedAt < r.createdAt || *r.authorizedAt > r.updatedAt)) {
        return invalid("The authorisation time is between creation and the last update");
    }
    if (r.claimedAt.has_value() && (!r.authorizedAt.has_value() || *r.claimedAt < *r.authorizedAt || *r.claimedAt > r.updatedAt)) {
        return invalid("The claim follows the authorisation and precedes the last update");
    }
    const TimePoint lastStep = r.claimedAt.value_or(r.authorizedAt.value_or(r.createdAt));
    if (r.finishedAt.has_value() && (*r.finishedAt < lastStep || *r.finishedAt != r.updatedAt)) {
        return invalid("A terminal request is finished at its last update, after its last step");
    }

    if (r.answer.has_value()) {
        const auto checked = validateAnswer(*r.answer);
        if (!checked.success()) return checked;
        // A stored plan is read back as what it claims to be: ONE next-run-plan.v1 envelope that parses strictly, whose plan has the
        // stored fingerprint and a draft count inside the window the request authorised, with advisories that read back. The
        // parser of next-run-plan.v1 is FROZEN for that reason: a stricter rule is a new schema version, never an edit of v1, so a
        // stored row never becomes unreadable behind the user's back.
        const auto plan = parseFollowUpAnswerPlan(*r.answer, r.splitHint, r.maxDrafts);
        if (!plan.success()) return invalid("The stored answer is not one valid plan of the authorised size (" + plan.error()->code + ")");
        if (!parseFollowUpPlanAdvisories(r.answer->advisoriesJson).success()) {
            return invalid("The stored advisories do not read back");
        }
    }
    if (r.accounting.has_value()) {
        const auto& a = *r.accounting;
        for (const std::uint64_t value : {a.inputTokens, a.outputTokens, a.cacheCreationInputTokens, a.cacheReadInputTokens,
                 a.cacheCreationFiveMinuteTokens, a.cacheCreationOneHourTokens, a.costMicros, a.latencyMs}) {
            if (value > kMaxUnsigned) return invalid("A stored accounting figure is out of range");
        }
        // Exactly what the execution log accepts (ModelExecutionService::finishExecution), so that a stored accounting can always be
        // written to the log: the cache subsets fit the input, an unknown duration split is empty, a known one adds up.
        if (a.cacheCreationInputTokens > a.inputTokens || a.cacheReadInputTokens > a.inputTokens - a.cacheCreationInputTokens) {
            return invalid("The cache tokens of a stored accounting exceed its input tokens");
        }
        if (!a.cacheCreationDurationKnown
                && (a.cacheCreationFiveMinuteTokens != 0 || a.cacheCreationOneHourTokens != 0)) {
            return invalid("An unknown cache-creation duration split carries no tokens");
        }
        if (a.cacheCreationDurationKnown
                && (a.cacheCreationFiveMinuteTokens > a.cacheCreationInputTokens
                    || a.cacheCreationOneHourTokens != a.cacheCreationInputTokens - a.cacheCreationFiveMinuteTokens)) {
            return invalid("A known cache-creation duration split adds up to the cache-creation tokens");
        }
    }
    if (r.failure.has_value()) {
        const auto checked = validateFailure(*r.failure);
        if (!checked.success()) return checked;
    }
    if (r.draftGroupId.has_value()
            && (r.draftGroupId->empty() || r.draftGroupId->size() > 64 || !storableText(*r.draftGroupId))) {
        return invalid("The saved group id is 1-64 characters");
    }
    return common::Result<void>::ok();
}

} // namespace

TimePoint followUpFloorToMilliseconds(TimePoint value) noexcept
{
    return std::chrono::floor<std::chrono::milliseconds>(value);
}

bool followUpRequestMayBeCharged(const FollowUpPlanRequest& request) noexcept
{
    return request.claimedAt.has_value();
}

common::Result<void> validateFollowUpPlanRequest(const FollowUpPlanRequest& request)
{
    if (request.id.empty() || request.id.size() > kFollowUpRequestMaxIdLength || !storableText(request.id)
            || hasControl(request.id)) {
        return invalid("A request id is 1-64 printable characters");
    }
    if (request.projectId.empty() || request.projectId.size() > 128 || !storableText(request.projectId)
            || request.sourceRunId.empty() || request.sourceRunId.size() > 128 || !storableText(request.sourceRunId)) {
        return invalid("A request names a project and a source Run");
    }
    if (!knownKind(request.kind) || !knownSplitHint(request.splitHint) || !knownState(request.state)) {
        return invalid("The kind, split hint or state of a request is unknown");
    }
    if (!followUpGoalIsValid(request.goal)) return invalid("The goal is not a valid composer goal");
    if (!followUpAuthorisedDraftLimits(request.splitHint, request.maxDrafts).success()) {
        return invalid("maxDrafts and the split hint do not describe an authorised draft window");
    }
    if (!lowerHex64(request.outcomeSourceStateFingerprint) || !lowerHex64(request.snapshotFingerprint)) {
        return invalid("A request pins the 64-hex Outcome state and snapshot fingerprints");
    }
    for (const auto point : {request.createdAt, request.quoteExpiresAt, request.updatedAt}) {
        if (!wholeMilliseconds(point)) return invalid("Times are whole milliseconds");
    }
    for (const auto& point : {request.authorizedAt, request.claimedAt, request.finishedAt}) {
        if (point.has_value() && !wholeMilliseconds(*point)) return invalid("Times are whole milliseconds");
    }
    if (const auto checked = validateFiles(request.files); !checked.success()) return checked;
    if (const auto checked = validateQuote(request.quote); !checked.success()) return checked;
    return validateLifecycle(request);
}

common::Result<FollowUpPlanRequest> applyFollowUpTransition(
    const FollowUpPlanRequest& current,
    const FollowUpTransition& transition,
    TimePoint now)
{
    using Result = common::Result<FollowUpPlanRequest>;
    using E = FollowUpPlanEvent;
    if (transition.event == E::ConfirmDrafts) {
        return failWith<FollowUpPlanRequest>("follow_up.confirm_requires_port",
            "Saving the drafts is the atomic confirm operation, not a plain transition");
    }
    if (current.state != transition.expectedState) {
        return failWith<FollowUpPlanRequest>("follow_up.state_changed",
            std::string("The request is '") + toString(current.state) + "', not '" + toString(transition.expectedState) + "'");
    }
    const auto next = transitionFollowUpPlanRequest(current.state, transition.event);
    if (!next.success()) return Result::failure(*next.error());

    const auto badPayload = [](const char* what) {
        return failWith<FollowUpPlanRequest>("follow_up.invalid_request", std::string("The event takes ") + what);
    };
    if (transition.answer.has_value() != (transition.event == E::PlanSucceeded)) return badPayload("an answer exactly when a plan succeeded");
    if (transition.failure.has_value() != (transition.event == E::CompositionFailed)) {
        return badPayload("a failure exactly when composition failed");
    }
    if (transition.event != E::Authorize && !transition.contextFingerprint.empty()) {
        return badPayload("a context fingerprint only to authorise");
    }
    // The accounting of the paid call rides with the step that settles it: a stored answer always has one (it is a paid response),
    // an invalid answer or a stop after a complete reply may, nothing else does.
    const bool mayCarryAccounting = transition.event == E::PlanSucceeded || transition.event == E::CompositionFailed
        || transition.event == E::Cancel;
    if (transition.accounting.has_value() && !mayCarryAccounting) {
        return badPayload("an accounting only when a provider response arrived");
    }
    if (transition.event == E::PlanSucceeded && !transition.accounting.has_value()) {
        return badPayload("the accounting of the paid call with its answer");
    }
    // A stored answer IS one valid plan of the authorised size: a reply that is not never becomes plan_ready (the service
    // records it as a failed composition with the typed error instead).
    if (transition.event == E::PlanSucceeded) {
        const auto plan = parseFollowUpAnswerPlan(*transition.answer, current.splitHint, current.maxDrafts);
        if (!plan.success()) return Result::failure(*plan.error());
    }

    const TimePoint stamp = std::max(followUpFloorToMilliseconds(now), current.updatedAt);
    switch (transition.event) {
    case E::Authorize:
        if (transition.contextFingerprint != current.quote.contextFingerprint) {
            return failWith<FollowUpPlanRequest>("follow_up.context_changed",
                "The authorisation names another context than the quote");
        }
        [[fallthrough]];
    case E::Claim:
        if (stamp >= current.quoteExpiresAt) {
            return failWith<FollowUpPlanRequest>("follow_up.quote_expired", "The quote has expired");
        }
        break;
    case E::Expire:
        if (stamp < current.quoteExpiresAt) {
            return failWith<FollowUpPlanRequest>("follow_up.not_expired", "The quote has not expired yet");
        }
        break;
    default:
        break;
    }

    FollowUpPlanRequest updated = current;
    updated.state = next.value();
    updated.updatedAt = stamp;
    if (transition.event == E::Authorize) updated.authorizedAt = stamp;
    if (transition.event == E::Claim) updated.claimedAt = stamp;
    if (transition.event == E::PlanSucceeded) updated.answer = transition.answer;
    if (transition.event == E::CompositionFailed) updated.failure = transition.failure;
    if (mayCarryAccounting) updated.accounting = transition.accounting;
    if (isTerminalFollowUpPlanRequestState(updated.state)) updated.finishedAt = stamp;

    const auto gate = validateFollowUpPlanRequest(updated);
    if (!gate.success()) return Result::failure(*gate.error());
    return Result::ok(std::move(updated));
}

common::Result<NextRunPlan> parseFollowUpAnswerPlan(
    const FollowUpPlanAnswer& answer, FollowUpSplitHint splitHint, std::uint32_t maxDrafts)
{
    using Result = common::Result<NextRunPlan>;
    const auto json = extractNextRunPlanJsonFromMessage(answer.rawReply);
    if (!json.success()) return Result::failure(*json.error());
    auto plan = parseNextRunPlan(json.value());
    if (!plan.success()) return plan;
    if (plan.value().fingerprint != answer.planFingerprint) {
        return failWith<NextRunPlan>("follow_up.answer_plan_mismatch",
            "The stored plan fingerprint is not the fingerprint of the plan in the stored reply");
    }
    const auto window = checkFollowUpPlanAgainstAuthorisation(plan.value(), splitHint, maxDrafts);
    if (!window.success()) return Result::failure(*window.error());
    return plan;
}

namespace {

bool sameContracts(const std::vector<RunInterfaceContract>& left, const std::vector<RunInterfaceContract>& right)
{
    return std::equal(left.begin(), left.end(), right.begin(), right.end(),
        [](const RunInterfaceContract& a, const RunInterfaceContract& b) {
            return a.id == b.id && a.kind == b.kind && a.title == b.title && a.text == b.text
                && a.appliesToPaths == b.appliesToPaths;
        });
}

common::Result<void> notThePlan(const std::string& what)
{
    return common::Result<void>::failure({"follow_up.drafts_not_the_plan",
        "The drafts to save are not the stored plan: " + what});
}

} // namespace

common::Result<void> checkFollowUpDraftsAreThePlan(
    const NextRunPlan& plan, const RunDraftGroup& group, const std::vector<RunDraft>& drafts)
{
    if (group.planTitle != plan.title) return notThePlan("the group title is not the plan title");
    if (drafts.size() != plan.drafts.size()) return notThePlan("the number of drafts is not the number in the plan");

    // Drafts may arrive in any order; each ordinal 0..n-1 is taken by exactly one.
    std::vector<const RunDraft*> byOrdinal(plan.drafts.size(), nullptr);
    for (const auto& draft : drafts) {
        if (!draft.provenance.has_value() || draft.provenance->ordinal >= byOrdinal.size()
                || byOrdinal[draft.provenance->ordinal] != nullptr) {
            return notThePlan("the drafts do not take the ordinals 0..n-1 once each");
        }
        byOrdinal[draft.provenance->ordinal] = &draft;
    }
    for (std::size_t ordinal = 0; ordinal < plan.drafts.size(); ++ordinal) {
        const NextRunPlanDraft& planned = plan.drafts[ordinal];
        const RunDraft& draft = *byOrdinal[ordinal];
        const RunDraftProvenance& provenance = *draft.provenance;
        const std::string at = "draft " + std::to_string(ordinal) + " ('" + planned.key + "')";
        if (draft.projectId != group.projectId || provenance.groupId != group.id) {
            return notThePlan(at + " belongs to another project or group");
        }
        if (provenance.planKey != planned.key || provenance.dependsOn != planned.dependsOn) {
            return notThePlan(at + " has another plan key or other dependencies");
        }
        if (draft.title != planned.title || draft.name != planned.title || draft.task != planned.task) {
            return notThePlan(at + " has another title or task");
        }
        if (!sameContracts(draft.interfaceContracts, planned.contracts)) {
            return notThePlan(at + " has other contracts");
        }
        if (draft.workflowId != kExecutionManifestWorkflowId || draft.launchMode != RunLaunchMode::CustomRoster) {
            return notThePlan(at + " is not a per-Run roster draft of the Manifest workflow");
        }
        if (provenance.createdContentFingerprint != nextRunPlanDraftContentFingerprint(
                planned.title, planned.task, planned.contracts, planned.dependsOn)) {
            return notThePlan(at + " carries another content fingerprint");
        }
    }
    return common::Result<void>::ok();
}

common::Result<void> checkFollowUpGroupSourceRuns(
    const RunDraftGroup& group, const ProjectId& projectId, const FollowUpRunProjectLoader& loadProject)
{
    if (group.sourceRunIds.size() > kRunDraftGroupMaxSourceRuns) {
        return common::Result<void>::failure({"follow_up.group_mismatch", "The group names too many source Runs"});
    }
    for (const auto& runId : group.sourceRunIds) {
        const auto project = loadProject(runId);
        if (!project.success()) return common::Result<void>::failure(*project.error());
        if (!project.value().has_value()) {
            return common::Result<void>::failure({"follow_up.source_run_not_found", "A source Run of the group does not exist: " + runId});
        }
        if (*project.value() != projectId) {
            return common::Result<void>::failure({"follow_up.source_run_wrong_project",
                "A source Run of the group belongs to another project: " + runId});
        }
    }
    return common::Result<void>::ok();
}

common::Result<FollowUpConfirmAction> planFollowUpConfirm(
    const FollowUpPlanRequest& request,
    const std::string& expectedPlanFingerprint,
    const RunDraftGroup& group,
    const std::vector<RunDraft>& drafts)
{
    using Result = common::Result<FollowUpConfirmAction>;
    if (request.state != FollowUpPlanRequestState::PlanReady && request.state != FollowUpPlanRequestState::DraftsSaved) {
        return failWith<FollowUpConfirmAction>("follow_up.not_plan_ready",
            std::string("Only a ready plan can be confirmed; the request is '") + toString(request.state) + "'");
    }
    if (!request.answer.has_value() || !lowerHex64(expectedPlanFingerprint)
            || request.answer->planFingerprint != expectedPlanFingerprint) {
        return failWith<FollowUpConfirmAction>("follow_up.plan_changed",
            "The plan being confirmed is not the stored plan of the request");
    }
    if (request.state == FollowUpPlanRequestState::DraftsSaved) {
        return Result::ok(FollowUpConfirmAction::ReturnExisting);
    }
    const bool bound = group.origin == RunDraftGroupOrigin::FollowUpRequest
        && group.sourceRequestId == std::optional<std::string>(request.id)
        && group.projectId == request.projectId
        && group.contextFingerprint == std::optional<std::string>(request.quote.contextFingerprint)
        && group.planFingerprint == request.answer->planFingerprint
        && std::find(group.sourceRunIds.begin(), group.sourceRunIds.end(), request.sourceRunId) != group.sourceRunIds.end();
    if (!bound) {
        return failWith<FollowUpConfirmAction>("follow_up.group_mismatch",
            "The group to save is not this request's group (origin, request, project, sealed context, plan or source Run)");
    }
    // The stored reply, not the caller, says what the plan is.
    const auto plan = parseFollowUpAnswerPlan(*request.answer, request.splitHint, request.maxDrafts);
    if (!plan.success()) return Result::failure(*plan.error());
    const auto same = checkFollowUpDraftsAreThePlan(plan.value(), group, drafts);
    if (!same.success()) return Result::failure(*same.error());
    return Result::ok(FollowUpConfirmAction::SaveGroup);
}

common::Result<FollowUpPlanRequest> applyFollowUpConfirm(
    const FollowUpPlanRequest& current,
    const RunDraftGroupId& groupId,
    TimePoint now)
{
    using Result = common::Result<FollowUpPlanRequest>;
    const auto next = transitionFollowUpPlanRequest(current.state, FollowUpPlanEvent::ConfirmDrafts);
    if (!next.success()) {
        return failWith<FollowUpPlanRequest>("follow_up.not_plan_ready", next.error()->message);
    }
    FollowUpPlanRequest updated = current;
    updated.state = next.value();
    updated.updatedAt = std::max(followUpFloorToMilliseconds(now), current.updatedAt);
    updated.finishedAt = updated.updatedAt;
    updated.draftGroupId = groupId;
    const auto gate = validateFollowUpPlanRequest(updated);
    if (!gate.success()) return Result::failure(*gate.error());
    return Result::ok(std::move(updated));
}

common::Result<std::optional<FollowUpPlanRequestId>> planFollowUpQuotedInsert(
    const FollowUpPlanRequest& incoming,
    const std::optional<FollowUpPlanRequest>& open)
{
    using Result = common::Result<std::optional<FollowUpPlanRequestId>>;
    if (incoming.state != FollowUpPlanRequestState::Quoted) {
        return failWith<std::optional<FollowUpPlanRequestId>>("follow_up.invalid_request", "A request is inserted as quoted");
    }
    if (!open.has_value()) return Result::ok(std::nullopt);
    if (open->state == FollowUpPlanRequestState::Quoted) return Result::ok(std::optional<FollowUpPlanRequestId>(open->id));
    return failWith<std::optional<FollowUpPlanRequestId>>("follow_up.request_open",
        std::string("The source Run already has a '") + toString(open->state) + "' request");
}

common::Result<void> checkFollowUpRequestWriteAuthority(
    const FollowUpPlanRequest& request, const std::optional<ProjectId>& runProject)
{
    if (!runProject.has_value()) {
        return common::Result<void>::failure({"follow_up.run_not_found", "The source Run does not exist"});
    }
    if (*runProject != request.projectId) {
        return common::Result<void>::failure({"follow_up.project_mismatch", "The source Run belongs to another project"});
    }
    return common::Result<void>::ok();
}

common::Result<void> checkFollowUpRequestReadAuthority(
    const FollowUpPlanRequest& request, const std::optional<ProjectId>& runProject)
{
    if (!runProject.has_value() || *runProject != request.projectId) {
        return common::Result<void>::failure({"persistence.invalid_data",
            "A follow-up request does not belong to the project of its source Run"});
    }
    return common::Result<void>::ok();
}

common::Result<void> checkFollowUpListLimit(std::size_t limit)
{
    if (limit == 0 || limit > kFollowUpMaxListLimit) {
        return common::Result<void>::failure({"follow_up.invalid_request", "A list limit is 1-500"});
    }
    return common::Result<void>::ok();
}

// ---- codecs -----------------------------------------------------------------------------------------------------------------

std::string serializeFollowUpPinnedFiles(const std::vector<FollowUpPinnedFile>& files)
{
    try {
        nlohmann::json array = nlohmann::json::array();
        for (const auto& file : files) {
            array.push_back({{"relativePath", file.relativePath}, {"contentSha256", file.contentSha256},
                {"originalBytes", file.originalBytes}, {"shownBytes", file.shownBytes},
                {"estimatedTokens", file.estimatedTokens}, {"truncated", file.truncated}});
        }
        return array.dump();
    } catch (const std::exception&) {
        return {};
    }
}

common::Result<std::vector<FollowUpPinnedFile>> parseFollowUpPinnedFiles(std::string_view stored)
{
    using Result = common::Result<std::vector<FollowUpPinnedFile>>;
    const auto bad = [] {
        return failWith<std::vector<FollowUpPinnedFile>>("persistence.invalid_data", "The pinned files of a follow-up request are unreadable");
    };
    const auto parsed = nlohmann::json::parse(stored, nullptr, false);
    if (!parsed.is_array()) return bad();
    std::vector<FollowUpPinnedFile> files;
    for (const auto& item : parsed) {
        if (!item.is_object() || item.size() != 6) return bad();
        const auto path = item.find("relativePath");
        const auto sha = item.find("contentSha256");
        const auto original = item.find("originalBytes");
        const auto shown = item.find("shownBytes");
        const auto tokens = item.find("estimatedTokens");
        const auto truncated = item.find("truncated");
        if (path == item.end() || sha == item.end() || original == item.end() || shown == item.end()
                || tokens == item.end() || truncated == item.end() || !path->is_string() || !sha->is_string()
                || !original->is_number_unsigned() || !shown->is_number_unsigned() || !tokens->is_number_unsigned()
                || !truncated->is_boolean()) {
            return bad();
        }
        files.push_back({path->get<std::string>(), sha->get<std::string>(), original->get<std::uint64_t>(),
            shown->get<std::uint64_t>(), tokens->get<std::uint64_t>(), truncated->get<bool>()});
    }
    return Result::ok(std::move(files));
}

std::string serializeFollowUpAccounting(const FollowUpAccounting& accounting)
{
    try {
        const nlohmann::json json = {
            {"inputTokens", accounting.inputTokens},
            {"outputTokens", accounting.outputTokens},
            {"cacheCreationInputTokens", accounting.cacheCreationInputTokens},
            {"cacheReadInputTokens", accounting.cacheReadInputTokens},
            {"cacheCreationDurationKnown", accounting.cacheCreationDurationKnown},
            {"cacheCreationFiveMinuteTokens", accounting.cacheCreationFiveMinuteTokens},
            {"cacheCreationOneHourTokens", accounting.cacheCreationOneHourTokens},
            {"costMicros", accounting.costMicros},
            {"priceKnown", accounting.priceKnown},
            {"latencyMs", accounting.latencyMs}};
        return json.dump();
    } catch (const std::exception&) {
        return {};
    }
}

common::Result<FollowUpAccounting> parseFollowUpAccounting(std::string_view stored)
{
    using Result = common::Result<FollowUpAccounting>;
    const auto bad = [] {
        return failWith<FollowUpAccounting>("persistence.invalid_data", "The accounting of a follow-up request is unreadable");
    };
    const auto parsed = nlohmann::json::parse(stored, nullptr, false);
    if (!parsed.is_object() || parsed.size() != 10) return bad();
    const auto number = [&](const char* key, std::uint64_t& out) {
        const auto found = parsed.find(key);
        if (found == parsed.end() || !found->is_number_unsigned()) return false;
        out = found->get<std::uint64_t>();
        return out <= kMaxUnsigned;
    };
    const auto flag = [&](const char* key, bool& out) {
        const auto found = parsed.find(key);
        if (found == parsed.end() || !found->is_boolean()) return false;
        out = found->get<bool>();
        return true;
    };
    FollowUpAccounting accounting;
    if (!number("inputTokens", accounting.inputTokens) || !number("outputTokens", accounting.outputTokens)
            || !number("cacheCreationInputTokens", accounting.cacheCreationInputTokens)
            || !number("cacheReadInputTokens", accounting.cacheReadInputTokens)
            || !flag("cacheCreationDurationKnown", accounting.cacheCreationDurationKnown)
            || !number("cacheCreationFiveMinuteTokens", accounting.cacheCreationFiveMinuteTokens)
            || !number("cacheCreationOneHourTokens", accounting.cacheCreationOneHourTokens)
            || !number("costMicros", accounting.costMicros) || !flag("priceKnown", accounting.priceKnown)
            || !number("latencyMs", accounting.latencyMs)) {
        return bad();
    }
    return Result::ok(std::move(accounting));
}

std::string serializeFollowUpPricingIdentity(const ProviderRequestPricingIdentity& identity)
{
    try {
        const auto& tier = identity.longContextPricing;
        const nlohmann::json json = {
            {"providerId", identity.providerId},
            {"modelId", identity.modelId},
            {"apiModelId", identity.apiModelId},
            {"promptCache", std::string(models::toString(identity.promptCache.mode))},
            {"contextWindowTokens", identity.contextWindowTokens},
            {"modelMaxOutputTokens", identity.modelMaxOutputTokens},
            {"inputPrice", identity.inputPriceMicrosPerMillionTokens},
            {"outputPrice", identity.outputPriceMicrosPerMillionTokens},
            {"cacheWritePrice", identity.cacheWritePriceMicrosPerMillionTokens},
            {"cacheReadPrice", identity.cacheReadPriceMicrosPerMillionTokens},
            {"cacheWriteOneHourPrice", identity.cacheWriteOneHourPriceMicrosPerMillionTokens},
            {"longContext", {
                {"threshold", tier.thresholdInputTokens},
                {"inputPrice", tier.inputPriceMicrosPerMillionTokens},
                {"outputPrice", tier.outputPriceMicrosPerMillionTokens},
                {"cacheWritePrice", tier.cacheWritePriceMicrosPerMillionTokens},
                {"cacheReadPrice", tier.cacheReadPriceMicrosPerMillionTokens},
                {"cacheWriteOneHourPrice", tier.cacheWriteOneHourPriceMicrosPerMillionTokens}}}};
        return json.dump();
    } catch (const std::exception&) {
        return {};
    }
}

std::optional<models::PromptCacheMode> followUpPromptCacheModeFromString(std::string_view value) noexcept
{
    if (value == "disabled") return models::PromptCacheMode::Disabled;
    if (value == "ephemeral_5m") return models::PromptCacheMode::EphemeralFiveMinutes;
    if (value == "ephemeral_1h") return models::PromptCacheMode::EphemeralOneHour;
    return std::nullopt;
}

common::Result<ProviderRequestPricingIdentity> parseFollowUpPricingIdentity(std::string_view stored)
{
    using Result = common::Result<ProviderRequestPricingIdentity>;
    const auto bad = [] {
        return failWith<ProviderRequestPricingIdentity>("persistence.invalid_data", "The pricing identity of a follow-up request is unreadable");
    };
    const auto parsed = nlohmann::json::parse(stored, nullptr, false);
    if (!parsed.is_object() || parsed.size() != 12) return bad();
    const auto text = [&](const nlohmann::json& object, const char* key, std::string& out) {
        const auto found = object.find(key);
        if (found == object.end() || !found->is_string()) return false;
        out = found->get<std::string>();
        return true;
    };
    const auto number = [&](const nlohmann::json& object, const char* key, std::uint64_t& out) {
        const auto found = object.find(key);
        if (found == object.end() || !found->is_number_unsigned()) return false;
        out = found->get<std::uint64_t>();
        return out <= kMaxUnsigned;
    };
    ProviderRequestPricingIdentity identity;
    std::string cacheMode;
    if (!text(parsed, "providerId", identity.providerId) || !text(parsed, "modelId", identity.modelId)
            || !text(parsed, "apiModelId", identity.apiModelId) || !text(parsed, "promptCache", cacheMode)
            || !number(parsed, "contextWindowTokens", identity.contextWindowTokens)
            || !number(parsed, "modelMaxOutputTokens", identity.modelMaxOutputTokens)
            || !number(parsed, "inputPrice", identity.inputPriceMicrosPerMillionTokens)
            || !number(parsed, "outputPrice", identity.outputPriceMicrosPerMillionTokens)
            || !number(parsed, "cacheWritePrice", identity.cacheWritePriceMicrosPerMillionTokens)
            || !number(parsed, "cacheReadPrice", identity.cacheReadPriceMicrosPerMillionTokens)
            || !number(parsed, "cacheWriteOneHourPrice", identity.cacheWriteOneHourPriceMicrosPerMillionTokens)) {
        return bad();
    }
    const auto mode = followUpPromptCacheModeFromString(cacheMode);
    if (!mode.has_value()) return bad();
    identity.promptCache.mode = *mode;
    const auto tier = parsed.find("longContext");
    if (tier == parsed.end() || !tier->is_object() || tier->size() != 6) return bad();
    auto& long_ = identity.longContextPricing;
    if (!number(*tier, "threshold", long_.thresholdInputTokens) || !number(*tier, "inputPrice", long_.inputPriceMicrosPerMillionTokens)
            || !number(*tier, "outputPrice", long_.outputPriceMicrosPerMillionTokens)
            || !number(*tier, "cacheWritePrice", long_.cacheWritePriceMicrosPerMillionTokens)
            || !number(*tier, "cacheReadPrice", long_.cacheReadPriceMicrosPerMillionTokens)
            || !number(*tier, "cacheWriteOneHourPrice", long_.cacheWriteOneHourPriceMicrosPerMillionTokens)) {
        return bad();
    }
    return Result::ok(std::move(identity));
}

} // namespace ai::application
