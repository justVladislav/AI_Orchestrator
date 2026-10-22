#include "FollowUpRequestTestSupport.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <set>

using namespace ai::application;
using namespace followup_test;

namespace {

using S = FollowUpPlanRequestState;
using E = FollowUpPlanEvent;

const Catalog kCatalog;

FollowUpPlanRequest baseRequest()
{
    return quotedRequest(kCatalog, "request-1", "project-1", "run-1");
}

FollowUpPlanRequest applied(const FollowUpPlanRequest& request, FollowUpTransition step, std::int64_t atMs)
{
    const auto next = applyFollowUpTransition(request, step, at(atMs));
    check(next.success(), std::string("setup: the step must succeed: ") + (next.success() ? "" : next.error()->code));
    return next.value();
}

// A request in each state, reached by the real transitions only.
FollowUpPlanRequest inState(S target)
{
    auto r = baseRequest();
    if (target == S::Quoted) return r;
    if (target == S::Stale) return applied(r, plainStep(E::MarkStale, S::Quoted), 500);
    if (target == S::Expired) return applied(r, plainStep(E::Expire, S::Quoted), kQuoteLifetimeMs);
    if (target == S::Cancelled) return applied(r, plainStep(E::Cancel, S::Quoted), 500);
    if (target == S::Discarded) return applied(r, plainStep(E::Discard, S::Quoted), 500);
    r = applied(r, authorizeStep(r), 1000);
    if (target == S::Authorized) return r;
    r = applied(r, plainStep(E::Claim, S::Authorized), 2000);
    if (target == S::Composing) return r;
    if (target == S::Failed) return applied(r, failStep({"provider.failed", "The provider failed", std::nullopt}), 3000);
    if (target == S::Interrupted) return applied(r, plainStep(E::Interrupt, S::Composing), 3000);
    r = applied(r, planStep(answerOf()), 3000);
    if (target == S::PlanReady) return r;
    const auto saved = applyFollowUpConfirm(r, "group-1", at(4000));
    check(saved.success(), "setup: the confirm must succeed");
    return saved.value();
}

const std::array<S, 11> kStates = {S::Quoted, S::Authorized, S::Composing, S::PlanReady, S::DraftsSaved, S::Stale,
    S::Expired, S::Cancelled, S::Discarded, S::Interrupted, S::Failed};

void expectInvalid(const FollowUpPlanRequest& request, const std::string& what)
{
    const auto gate = validateFollowUpPlanRequest(request);
    check(!gate.success() && gate.error()->code == "follow_up.invalid_request",
        what + ": the gate must refuse with follow_up.invalid_request (got " + (gate.success() ? "success" : gate.error()->code) + ")");
}

// ---------------------------------------------------------------------------------------------------------------

void testEveryRealStateIsAValidRecord()
{
    for (const S state : kStates) {
        const auto request = inState(state);
        check(request.state == state, std::string("setup reached ") + toString(state));
        const auto gate = validateFollowUpPlanRequest(request);
        check(gate.success(), std::string("a real '") + toString(state) + "' request passes the gate: "
            + (gate.success() ? "" : gate.error()->message));
    }
}

// The fields each state requires or forbids, written out independently of the gate: A authorized, C claimed, F finished,
// R answered, X failed, G grouped, U accounted (the usage and price of the paid response). All 128 combinations are tried for every
// state; only the listed ones may pass.
void testTheStateFieldMatrixIsExactlyTheAgreedOne()
{
    constexpr unsigned A = 1, C = 2, F = 4, R = 8, X = 16, G = 32, U = 64;
    const std::map<S, std::set<unsigned>> allowed = {
        {S::Quoted, {0}},
        {S::Authorized, {A}},
        {S::Composing, {A | C}},
        {S::PlanReady, {A | C | R | U}},
        {S::DraftsSaved, {A | C | F | R | G | U}},
        // A failure that followed a provider response carries its accounting; one that did not (the provider failed) has none.
        {S::Failed, {A | C | F | X, A | C | F | X | U}},
        {S::Interrupted, {A | C | F}},
        {S::Stale, {F, F | A}},
        {S::Expired, {F, F | A}},
        {S::Cancelled, {F, F | A, F | A | C, F | A | C | U}},
        // Only the two shapes with an edge into them: a free discarded quote, and a discarded plan_ready plan (claimed, answered).
        {S::Discarded, {F, F | A | C | R | U}}};
    for (const S state : kStates) {
        for (unsigned mask = 0; mask < 128; ++mask) {
            auto request = baseRequest();
            request.state = state;
            // The last step of a non-terminal state sets its stamp and the update time together (the time chain is tested apart).
            request.updatedAt = state == S::Quoted ? at(0) : state == S::Authorized ? at(1000) : state == S::Composing ? at(2000) : at(3000);
            if ((mask & A) != 0) request.authorizedAt = at(1000);
            if ((mask & C) != 0) request.claimedAt = at(2000);
            if ((mask & F) != 0) request.finishedAt = at(3000);
            if ((mask & R) != 0) request.answer = answerOf();
            if ((mask & X) != 0) request.failure = FollowUpPlanFailure {"provider.failed", "failed", std::nullopt};
            if ((mask & G) != 0) request.draftGroupId = "group-1";
            if ((mask & U) != 0) request.accounting = accountingOf();
            const bool passes = validateFollowUpPlanRequest(request).success();
            const bool expected = allowed.at(state).contains(mask);
            check(passes == expected, std::string("state '") + toString(state) + "' with fields mask " + std::to_string(mask)
                + (expected ? " must pass" : " must be refused"));
        }
    }
}

void testTheGateRefusesEveryBrokenField()
{
    struct Case {
        const char* name;
        std::function<void(FollowUpPlanRequest&)> edit;
    };
    const std::vector<Case> cases = {
        {"empty id", [](auto& r) { r.id.clear(); }},
        {"id of 65 characters", [](auto& r) { r.id = std::string(65, 'x'); }},
        {"id with a control character", [](auto& r) { r.id = "a\nb"; }},
        {"empty project", [](auto& r) { r.projectId.clear(); }},
        {"empty source run", [](auto& r) { r.sourceRunId.clear(); }},
        {"unknown kind", [](auto& r) { r.kind = static_cast<FollowUpPlanKind>(99); }},
        {"unknown split hint", [](auto& r) { r.splitHint = static_cast<FollowUpSplitHint>(99); }},
        {"unknown state", [](auto& r) { r.state = static_cast<S>(99); }},
        {"empty goal", [](auto& r) { r.goal.clear(); }},
        {"goal with a control character", [](auto& r) { r.goal = "a\x01" "b"; }},
        {"goal over 4 KiB", [](auto& r) { r.goal = std::string(4097, 'g'); }},
        {"maxDrafts 0", [](auto& r) { r.maxDrafts = 0; }},
        {"maxDrafts 5", [](auto& r) { r.maxDrafts = 5; }},
        {"many with maxDrafts 1", [](auto& r) { r.splitHint = FollowUpSplitHint::Many; r.maxDrafts = 1; }},
        {"outcome fingerprint of 63", [](auto& r) { r.outcomeSourceStateFingerprint = std::string(63, '1'); }},
        {"outcome fingerprint uppercase", [](auto& r) { r.outcomeSourceStateFingerprint = std::string(64, 'A'); }},
        {"snapshot fingerprint not hex", [](auto& r) { r.snapshotFingerprint = std::string(64, 'g'); }},
        {"nine files", [](auto& r) { r.files.clear(); for (int i = 0; i < 9; ++i) r.files.push_back(pinnedFile("f" + std::to_string(i) + ".cpp")); }},
        {"duplicate file path", [](auto& r) { r.files = {pinnedFile("a.cpp"), pinnedFile("a.cpp", 'c')}; }},
        {"file sha not hex", [](auto& r) { r.files[0].contentSha256 = "xyz"; }},
        {"file shown over original", [](auto& r) { r.files[0].shownBytes = 101; }},
        {"file truncation flag lies", [](auto& r) { r.files[0].truncated = true; }},
        {"file with zero tokens", [](auto& r) { r.files[0].estimatedTokens = 0; }},
        {"file over 1500 tokens", [](auto& r) { r.files[0].estimatedTokens = 1501; }},
        {"another composer policy", [](auto& r) { r.quote.policyId = "follow-up-composer.v2"; }},
        {"context fingerprint short", [](auto& r) { r.quote.contextFingerprint = "abc"; }},
        {"recipient kind unknown", [](auto& r) { r.quote.providerIdentity.kind = "mystery"; }},
        {"recipient fingerprint tampered", [](auto& r) { r.quote.providerIdentityFingerprint[0] = r.quote.providerIdentityFingerprint[0] == '0' ? '1' : '0'; }},
        {"recipient base url changed after sealing", [](auto& r) { r.quote.providerIdentity.baseUrl += "x"; }},
        {"recipient base url over 2048", [](auto& r) { r.quote.providerIdentity.baseUrl = std::string(2049, 'u'); r.quote.providerIdentityFingerprint = followUpProviderExecutionIdentityFingerprint(r.quote.providerIdentity); }},
        {"credential reference over 1024", [](auto& r) { r.quote.providerIdentity.credentialReference = std::string(1025, 'c'); r.quote.providerIdentityFingerprint = followUpProviderExecutionIdentityFingerprint(r.quote.providerIdentity); }},
        {"recipient with NUL", [](auto& r) { r.quote.providerIdentity.baseUrl = std::string("a\0b", 3); r.quote.providerIdentityFingerprint = followUpProviderExecutionIdentityFingerprint(r.quote.providerIdentity); }},
        {"pricing names another provider", [](auto& r) { r.quote.worstCase.pricingIdentity.providerId = "other"; r.quote.worstCase.pricingIdentityFingerprint = providerRequestPricingIdentityFingerprint(r.quote.worstCase.pricingIdentity); }},
        {"pricing fingerprint tampered", [](auto& r) { r.quote.worstCase.pricingIdentityFingerprint[0] = r.quote.worstCase.pricingIdentityFingerprint[0] == '0' ? '1' : '0'; }},
        {"pricing price changed after sealing", [](auto& r) { r.quote.worstCase.pricingIdentity.outputPriceMicrosPerMillionTokens += 1; }},
        {"cache policy the kind does not imply", [](auto& r) { r.quote.worstCase.pricingIdentity.promptCache.mode = ai::models::PromptCacheMode::Disabled; r.quote.worstCase.pricingIdentityFingerprint = providerRequestPricingIdentityFingerprint(r.quote.worstCase.pricingIdentity); }},
        {"cache policy the kind does not imply (kind changed, cache untouched)", [](auto& r) { r.quote.providerIdentity.kind = "openai"; r.quote.providerIdentityFingerprint = followUpProviderExecutionIdentityFingerprint(r.quote.providerIdentity); }},
        {"price not known",[](auto& r) { r.quote.worstCase.priceKnown = false; }},
        {"zero estimated input", [](auto& r) { r.quote.worstCase.estimatedInputTokens = 0; }},
        {"input over 48 000", [](auto& r) { r.quote.worstCase.estimatedInputTokens = 48001; }},
        {"cacheable over input", [](auto& r) { r.quote.worstCase.cacheableInputTokens = r.quote.worstCase.estimatedInputTokens + 1; }},
        {"enabled cache with nothing cacheable", [](auto& r) { r.quote.worstCase.cacheableInputTokens = 0; }},
        {"output below the minimum", [](auto& r) { r.quote.worstCase.maximumOutputTokens = 4095; }},
        {"output above 16 384", [](auto& r) { r.quote.worstCase.maximumOutputTokens = 16385; }},
        {"cap zero", [](auto& r) { r.quote.capMicros = 0; }},
        {"cap above the ceiling", [](auto& r) { r.quote.capMicros = kFollowUpSystemCeilingMicros + 1; }},
        {"cost above the cap", [](auto& r) { r.quote.worstCase.estimatedCostMicros = r.quote.capMicros + 1; }},
        {"output over the model limit", [](auto& r) { r.quote.worstCase.pricingIdentity.modelMaxOutputTokens = 4096; r.quote.worstCase.pricingIdentityFingerprint = providerRequestPricingIdentityFingerprint(r.quote.worstCase.pricingIdentity); }},
        {"input and output over the window", [](auto& r) { r.quote.worstCase.pricingIdentity.contextWindowTokens = r.quote.worstCase.estimatedInputTokens + 100; r.quote.worstCase.pricingIdentityFingerprint = providerRequestPricingIdentityFingerprint(r.quote.worstCase.pricingIdentity); }},
        {"expiry not 15 minutes", [](auto& r) { r.quoteExpiresAt = r.createdAt + std::chrono::minutes(14); }},
        {"updated before created", [](auto& r) { r.updatedAt = r.createdAt - std::chrono::milliseconds(1); }},
        {"time with sub-millisecond part", [](auto& r) { r.createdAt += std::chrono::microseconds(1); r.quoteExpiresAt = r.createdAt + kFollowUpQuoteLifetime; r.updatedAt = r.createdAt; }},
        {"answer on a quoted request", [](auto& r) { r.answer = answerOf(); }},
        {"failure on a quoted request", [](auto& r) { r.failure = FollowUpPlanFailure {"x", "y", std::nullopt}; }},
        {"group on a quoted request", [](auto& r) { r.draftGroupId = "group-1"; }},
        {"claim on a quoted request", [](auto& r) { r.claimedAt = r.createdAt; }},
    };
    for (const auto& item : cases) {
        auto request = baseRequest();
        item.edit(request);
        expectInvalid(request, item.name);
    }

    // The answer, failure and group fields of states that carry them are bounded too.
    const auto ready = inState(S::PlanReady);
    auto broken = ready;
    broken.answer->rawReply.clear();
    expectInvalid(broken, "empty reply");
    broken = ready;
    broken.answer->rawReply = std::string(kFollowUpMaxReplyBytes + 1, 'r');
    expectInvalid(broken, "reply over 256 KiB");
    broken = ready;
    broken.answer->rawReply = std::string("a\0b", 3);
    expectInvalid(broken, "reply with NUL");
    broken = ready;
    broken.answer->planFingerprint = "short";
    expectInvalid(broken, "plan fingerprint");
    broken = ready;
    broken.answer->advisoriesJson = "{}";
    expectInvalid(broken, "advisories not an array");
    broken = ready;
    broken.answer->advisoriesJson = "[not json";
    expectInvalid(broken, "advisories not json");
    broken = ready;
    broken.answer->advisoriesJson = "[" + std::string(kFollowUpMaxAdvisoriesBytes, ' ') + "]";
    expectInvalid(broken, "advisories over 64 KiB");

    const auto failed = inState(S::Failed);
    broken = failed;
    broken.failure->code.clear();
    expectInvalid(broken, "failure without a code");
    broken = failed;
    broken.failure->code = std::string(129, 'c');
    expectInvalid(broken, "failure code over 128");
    broken = failed;
    broken.failure->message = std::string(kFollowUpMaxFailureMessageBytes + 1, 'm');
    expectInvalid(broken, "failure message over 4 KiB");
    broken = failed;
    broken.failure->rawReply = std::string(kFollowUpMaxReplyBytes + 1, 'r');
    expectInvalid(broken, "invalid reply over 256 KiB");
    broken = inState(S::DraftsSaved);
    broken.draftGroupId = std::string(65, 'g');
    expectInvalid(broken, "group id over 64");

    // The time chain.
    broken = inState(S::Composing);
    broken.claimedAt = at(500);   // before the authorisation at 1000
    expectInvalid(broken, "claim before authorisation");
    broken = inState(S::Composing);
    broken.authorizedAt = at(100'000);   // after the last update
    expectInvalid(broken, "authorisation after the last update");
    broken = inState(S::Failed);
    broken.finishedAt = at(2999);   // not the last update
    expectInvalid(broken, "finished before the last update");
}

void testTheChargedStampIsTheClaim()
{
    for (const S state : kStates) {
        const auto request = inState(state);
        const bool expected = state == S::Composing || state == S::PlanReady || state == S::DraftsSaved
            || state == S::Failed || state == S::Interrupted;
        check(followUpRequestMayBeCharged(request) == expected,
            std::string("'") + toString(state) + "' is " + (expected ? "possibly charged" : "free"));
    }
    // cancelled and discarded exist on both sides of the claim.
    auto composing = inState(S::Composing);
    check(!followUpRequestMayBeCharged(inState(S::Cancelled)), "A cancelled quote is free");
    check(followUpRequestMayBeCharged(applied(composing, plainStep(E::Cancel, S::Composing), 3000)),
        "A cancel during the call is possibly charged");
    check(!followUpRequestMayBeCharged(inState(S::Discarded)), "A discarded quote is free");
    const auto discardedPlan = applied(inState(S::PlanReady), plainStep(E::Discard, S::PlanReady), 4000);
    check(followUpRequestMayBeCharged(discardedPlan) && discardedPlan.answer.has_value(),
        "A discarded ready plan paid for its call and keeps its answer");
    check(!followUpRequestMayBeCharged(applied(inState(S::Authorized), plainStep(E::Cancel, S::Authorized), 2000)),
        "A cancel before the claim is free");
    check(!followUpRequestMayBeCharged(applied(inState(S::Authorized), plainStep(E::MarkStale, S::Authorized), 2000)),
        "A stale authorised request is free");
}

// ---------------------------------------------------------------------------------------------------------------

void testTransitionsFollowTheFiveATableAndOnlyThat()
{
    for (const S from : kStates) {
        for (const E event : {E::Authorize, E::Claim, E::PlanSucceeded, E::CompositionFailed, E::MarkStale, E::Expire,
                 E::Cancel, E::Interrupt, E::ConfirmDrafts, E::Discard}) {
            const auto current = inState(from);
            FollowUpTransition step = plainStep(event, from);
            if (event == E::Authorize) step.contextFingerprint = current.quote.contextFingerprint;
            if (event == E::PlanSucceeded) {
                step.answer = answerOf();
                step.accounting = accountingOf();
            }
            if (event == E::CompositionFailed) step.failure = FollowUpPlanFailure {"provider.failed", "failed", std::nullopt};
            const auto now = event == E::Expire ? at(kQuoteLifetimeMs + 1) : at(10'000);
            const auto result = applyFollowUpTransition(current, step, now);
            const auto table = transitionFollowUpPlanRequest(from, event);
            const std::string name = std::string(toString(from)) + " + " + toString(event);
            if (event == E::ConfirmDrafts) {
                requireCode(result, "follow_up.confirm_requires_port", name);
            } else if (!table.success()) {
                requireCode(result, "follow_up.invalid_transition", name);
            } else {
                check(result.success(), name + " is an agreed edge: " + (result.success() ? "" : result.error()->code));
                check(result.value().state == table.value(), name + " lands where the table says");
                check(validateFollowUpPlanRequest(result.value()).success(), name + " produces a valid record");
            }
        }
    }
}

void testWhatEachTransitionWrites()
{
    const auto quoted = baseRequest();
    auto r = applied(quoted, authorizeStep(quoted), 1234);
    check(r.state == S::Authorized && r.authorizedAt == at(1234) && r.updatedAt == at(1234) && !r.claimedAt && !r.finishedAt,
        "authorize stamps the authorisation");
    r = applied(r, plainStep(E::Claim, S::Authorized), 2345);
    check(r.state == S::Composing && r.claimedAt == at(2345) && r.authorizedAt == at(1234) && !r.finishedAt,
        "claim stamps the claim and keeps the authorisation");
    const auto ready = applied(r, planStep(answerOf('d')), 3456);
    check(ready.state == S::PlanReady && ready.answer == std::optional<FollowUpPlanAnswer>(answerOf('d'))
            && ready.updatedAt == at(3456) && !ready.finishedAt && ready.claimedAt == at(2345),
        "a plan is stored byte for byte and is not terminal");
    const auto failed = applied(r, failStep({"follow_up.invalid_plan", "No envelope", std::string("raw reply")}), 3457);
    check(failed.state == S::Failed && failed.failure->rawReply == std::optional<std::string>("raw reply")
            && failed.finishedAt == at(3457) && !failed.answer,
        "an invalid answer is stored with its reply and finishes the request");
    const auto interrupted = applied(r, plainStep(E::Interrupt, S::Composing), 3458);
    check(interrupted.finishedAt == at(3458) && interrupted.claimedAt == at(2345), "interrupt finishes and keeps the stamp");

    // Time: floored to milliseconds, never before the last update.
    const auto floored = applyFollowUpTransition(quoted, authorizeStep(quoted), at(1000) + std::chrono::microseconds(900));
    check(floored.success() && floored.value().authorizedAt == at(1000), "the stamp is floored to whole milliseconds");
    const auto backwards = applyFollowUpTransition(
        applied(quoted, authorizeStep(quoted), 5000), plainStep(E::Claim, S::Authorized), at(1000));
    check(backwards.success() && backwards.value().claimedAt == at(5000) && backwards.value().updatedAt == at(5000),
        "a clock that went backwards never moves a request back in time");
}

void testTheCompareAndSetPayloadAndDeadlineRules()
{
    const auto quoted = baseRequest();
    requireCode(applyFollowUpTransition(quoted, plainStep(E::Cancel, S::Authorized), at(100)), "follow_up.state_changed",
        "the caller believed another state");
    auto wrong = authorizeStep(quoted);
    wrong.contextFingerprint = hex64('9');
    requireCode(applyFollowUpTransition(quoted, wrong, at(100)), "follow_up.context_changed", "another context");
    auto empty = authorizeStep(quoted);
    empty.contextFingerprint.clear();
    requireCode(applyFollowUpTransition(quoted, empty, at(100)), "follow_up.context_changed", "no context");

    // Payloads: exactly the event that takes one.
    auto extra = plainStep(E::Cancel, S::Quoted);
    extra.answer = answerOf();
    requireCode(applyFollowUpTransition(quoted, extra, at(100)), "follow_up.invalid_request", "an answer on cancel");
    extra = plainStep(E::Cancel, S::Quoted);
    extra.failure = FollowUpPlanFailure {"x", "y", std::nullopt};
    requireCode(applyFollowUpTransition(quoted, extra, at(100)), "follow_up.invalid_request", "a failure on cancel");
    extra = plainStep(E::Cancel, S::Quoted);
    extra.contextFingerprint = quoted.quote.contextFingerprint;
    requireCode(applyFollowUpTransition(quoted, extra, at(100)), "follow_up.invalid_request", "a fingerprint on cancel");
    const auto composing = inState(S::Composing);
    requireCode(applyFollowUpTransition(composing, plainStep(E::PlanSucceeded, S::Composing), at(5000)),
        "follow_up.invalid_request", "a plan without an answer");
    requireCode(applyFollowUpTransition(composing, plainStep(E::CompositionFailed, S::Composing), at(5000)),
        "follow_up.invalid_request", "a failure without its record");
    auto bothPayloads = planStep(answerOf());
    bothPayloads.failure = FollowUpPlanFailure {"x", "y", std::nullopt};
    requireCode(applyFollowUpTransition(composing, bothPayloads, at(5000)), "follow_up.invalid_request", "answer and failure");
    auto badAnswer = planStep(answerOf());
    badAnswer.answer->advisoriesJson = "{}";
    requireCode(applyFollowUpTransition(composing, badAnswer, at(5000)), "follow_up.invalid_request",
        "a result that fails the gate is refused, not stored");
    auto shortFingerprint = planStep(answerOf());
    shortFingerprint.answer->planFingerprint = "short";
    requireCode(applyFollowUpTransition(composing, shortFingerprint, at(5000)), "follow_up.answer_plan_mismatch",
        "a fingerprint that is not the reply's plan");

    // The ONE deadline: createdAt + 15 minutes.
    const auto deadline = kQuoteLifetimeMs;
    check(applyFollowUpTransition(quoted, authorizeStep(quoted), at(deadline - 1)).success(), "authorize just before the deadline");
    requireCode(applyFollowUpTransition(quoted, authorizeStep(quoted), at(deadline)), "follow_up.quote_expired", "authorize at the deadline");
    const auto authorized = inState(S::Authorized);
    check(applyFollowUpTransition(authorized, plainStep(E::Claim, S::Authorized), at(deadline - 1)).success(), "claim just before the deadline");
    requireCode(applyFollowUpTransition(authorized, plainStep(E::Claim, S::Authorized), at(deadline)), "follow_up.quote_expired", "claim at the deadline");
    requireCode(applyFollowUpTransition(quoted, plainStep(E::Expire, S::Quoted), at(deadline - 1)), "follow_up.not_expired", "expire early");
    check(applyFollowUpTransition(quoted, plainStep(E::Expire, S::Quoted), at(deadline)).success(), "expire at the deadline");
    const auto expiredAuthorized = applyFollowUpTransition(authorized, plainStep(E::Expire, S::Authorized), at(deadline + 5));
    check(expiredAuthorized.success() && expiredAuthorized.value().authorizedAt.has_value()
            && !followUpRequestMayBeCharged(expiredAuthorized.value()),
        "an authorised request that expires before the claim is free");
    // The deadline binds only the steps that start the paid call.
    check(applyFollowUpTransition(composing, plainStep(E::Cancel, S::Composing), at(deadline + 100)).success(),
        "a request already in the call can still be cancelled after the deadline");
    check(applyFollowUpTransition(inState(S::PlanReady), plainStep(E::Discard, S::PlanReady), at(deadline + 100)).success(),
        "a ready plan can be discarded after the deadline");
}

void testTheConfirmDecisionAndStep()
{
    const auto ready = inState(S::PlanReady);
    const auto bound = [&](const std::string& groupId) {
        RunDraftGroup group;
        group.id = groupId;
        group.projectId = ready.projectId;
        group.origin = RunDraftGroupOrigin::FollowUpRequest;
        group.idempotencyKey = hex64('e');
        group.planTitle = planOfAnswer(*ready.answer).title;
        group.planFingerprint = ready.answer->planFingerprint;
        group.sourceRequestId = ready.id;
        group.contextFingerprint = ready.quote.contextFingerprint;
        group.sourceRunIds = {ready.sourceRunId};
        group.createdAt = at(4000);
        return group;
    };
    const auto fingerprint = ready.answer->planFingerprint;
    const auto plan = planOfAnswer(*ready.answer);
    const auto drafts = draftsOfPlan(plan, "group-1", ready.projectId);
    const auto decision = planFollowUpConfirm(ready, fingerprint, bound("group-1"), drafts);
    check(decision.success() && decision.value() == FollowUpConfirmAction::SaveGroup, "A ready plan with its group is saved");

    requireCode(planFollowUpConfirm(ready, hex64('9'), bound("group-1"), drafts), "follow_up.plan_changed", "another plan fingerprint");
    requireCode(planFollowUpConfirm(ready, "short", bound("group-1"), drafts), "follow_up.plan_changed", "a malformed plan fingerprint");
    const std::vector<std::pair<const char*, std::function<void(RunDraftGroup&)>>> mismatches = {
        {"another origin", [](auto& g) { g.origin = RunDraftGroupOrigin::ManualImport; }},
        {"no request id", [](auto& g) { g.sourceRequestId.reset(); }},
        {"another request", [](auto& g) { g.sourceRequestId = "other"; }},
        {"another project", [](auto& g) { g.projectId = "project-2"; }},
        {"another sealed context", [](auto& g) { g.contextFingerprint = hex64('9'); }},
        {"no sealed context", [](auto& g) { g.contextFingerprint.reset(); }},
        {"another plan", [](auto& g) { g.planFingerprint = hex64('9'); }},
        {"without the source Run", [](auto& g) { g.sourceRunIds = {"another-run"}; }},
        {"no source Run at all", [](auto& g) { g.sourceRunIds.clear(); }}};
    for (const auto& [what, edit] : mismatches) {
        auto group = bound("group-1");
        edit(group);
        requireCode(planFollowUpConfirm(ready, fingerprint, group, drafts), "follow_up.group_mismatch", what);
    }
    auto withExtraRun = bound("group-1");
    withExtraRun.sourceRunIds = {"advisory-run", ready.sourceRunId};
    check(planFollowUpConfirm(ready, fingerprint, withExtraRun, drafts).success(),
        "Further source Runs may be named (their existence is the adapter's check); the source Run stays one of them");

    for (const S state : kStates) {
        if (state == S::PlanReady || state == S::DraftsSaved) continue;
        requireCode(planFollowUpConfirm(inState(state), fingerprint, bound("group-1"), drafts), "follow_up.not_plan_ready",
            std::string("a '") + toString(state) + "' request");
    }
    const auto saved = inState(S::DraftsSaved);
    const auto again = planFollowUpConfirm(saved, fingerprint, RunDraftGroup {}, {});
    check(again.success() && again.value() == FollowUpConfirmAction::ReturnExisting,
        "A saved request returns its group; the candidate group and drafts are not looked at");
    requireCode(planFollowUpConfirm(saved, hex64('9'), bound("group-1"), drafts), "follow_up.plan_changed", "a saved request, another plan");

    // P1: the Drafts must be the plan of the STORED reply, which is parsed again here.
    requireCode(planFollowUpConfirm(ready, fingerprint, bound("group-1"), {}), "follow_up.drafts_not_the_plan", "no drafts");
    auto otherTask = drafts;
    otherTask[1].task = "Something else";
    requireCode(planFollowUpConfirm(ready, fingerprint, bound("group-1"), otherTask), "follow_up.drafts_not_the_plan", "another task");
    const auto otherPlanDrafts = draftsOfPlan(planOfAnswer(answerOf('d')), "group-1", ready.projectId);
    requireCode(planFollowUpConfirm(ready, fingerprint, bound("group-1"), otherPlanDrafts), "follow_up.drafts_not_the_plan",
        "valid drafts of another plan under this plan's fingerprint");
    auto noEnvelope = ready;
    noEnvelope.answer->rawReply = "no plan here";
    requireCode(planFollowUpConfirm(noEnvelope, fingerprint, bound("group-1"), drafts), "next_run_plan.envelope_missing",
        "a stored reply that has no envelope");
    auto otherReply = ready;
    otherReply.answer->rawReply = answerOf('d').rawReply;
    requireCode(planFollowUpConfirm(otherReply, fingerprint, bound("group-1"), drafts), "follow_up.answer_plan_mismatch",
        "a stored reply whose plan is not the stored fingerprint");
    auto narrowed = ready;
    narrowed.splitHint = FollowUpSplitHint::One;
    requireCode(planFollowUpConfirm(narrowed, fingerprint, bound("group-1"), drafts), "follow_up.plan_draft_count_not_authorised",
        "two drafts where one was authorised");

    const auto next = applyFollowUpConfirm(ready, "group-1", at(4000) + std::chrono::microseconds(500));
    check(next.success() && next.value().state == S::DraftsSaved && next.value().draftGroupId == std::optional<std::string>("group-1")
            && next.value().finishedAt == at(4000) && next.value().updatedAt == at(4000) && next.value().answer == ready.answer
            && followUpRequestMayBeCharged(next.value()),
        "Confirm stamps the finish, links the group and keeps the answer");
    check(applyFollowUpConfirm(ready, "group-1", at(1)).value().updatedAt == ready.updatedAt,
        "Confirm never moves a request back in time");
    for (const S state : kStates) {
        if (state == S::PlanReady) continue;
        requireCode(applyFollowUpConfirm(inState(state), "group-1", at(5000)), "follow_up.not_plan_ready",
            std::string("confirm step from '") + toString(state) + "'");
    }
    requireCode(applyFollowUpConfirm(ready, "", at(5000)), "follow_up.invalid_request", "an empty group id");
}

// P1: an answer is stored only if its reply holds ONE valid plan, with the stored fingerprint, inside the authorised window.
void testAnAnswerIsOneValidPlanOfTheAuthorisedSize()
{
    const auto good = answerOf();
    const auto parsedPlan = parseFollowUpAnswerPlan(good, FollowUpSplitHint::Auto, 2);
    check(parsedPlan.success() && parsedPlan.value().drafts.size() == 2 && parsedPlan.value().fingerprint == good.planFingerprint,
        "A genuine answer yields its plan");

    const auto refused = [&](const char* what, FollowUpPlanAnswer answer, FollowUpSplitHint hint, std::uint32_t maxDrafts, std::string_view code) {
        requireCode(parseFollowUpAnswerPlan(answer, hint, maxDrafts), code, what);
    };
    auto text = good;
    text.rawReply = "just prose";
    refused("no envelope", text, FollowUpSplitHint::Auto, 2, "next_run_plan.envelope_missing");
    text.rawReply = good.rawReply + good.rawReply;
    refused("two envelopes", text, FollowUpSplitHint::Auto, 2, "next_run_plan.envelope_multiple");
    text.rawReply = replyOfPlanJson("{}");
    refused("an empty plan", text, FollowUpSplitHint::Auto, 2, "next_run_plan.schema_version");
    text.rawReply = replyOfPlanJson(R"({"schemaVersion":"next-run-plan.v1","title":"t","drafts":[]})");
    refused("a plan without drafts", text, FollowUpSplitHint::Auto, 2, "next_run_plan.no_drafts");
    auto foreign = good;
    foreign.planFingerprint = answerOf('d').planFingerprint;
    refused("the fingerprint of another plan", foreign, FollowUpSplitHint::Auto, 2, "follow_up.answer_plan_mismatch");
    refused("a window of one", good, FollowUpSplitHint::One, 2, "follow_up.plan_draft_count_not_authorised");
    refused("a window ending at one", good, FollowUpSplitHint::Auto, 1, "follow_up.plan_draft_count_not_authorised");
    refused("a request that authorises nothing", good, FollowUpSplitHint::Auto, 0, "follow_up.invalid_draft_limits");
    check(parseFollowUpAnswerPlan(answerOf('c', 3), FollowUpSplitHint::Many, 4).success(), "three drafts fit 'many' up to four");
    requireCode(parseFollowUpAnswerPlan(answerOf('c', 1), FollowUpSplitHint::Many, 4), "follow_up.plan_draft_count_not_authorised",
        "one draft where 'many' asks for at least two");

    // The same through the transition: the request stays composing, nothing is stored.
    const auto composing = inState(S::Composing);
    requireCode(applyFollowUpTransition(composing, planStep(text), at(5000)), "next_run_plan.no_drafts", "a transition with a plan without drafts");
    requireCode(applyFollowUpTransition(composing, planStep(answerOf('c', 3)), at(5000)), "follow_up.plan_draft_count_not_authorised",
        "a transition with three drafts for a window of two");
    check(applyFollowUpTransition(composing, planStep(good), at(5000)).success(), "...and the genuine answer goes through");
}

// P1: the Drafts to save are compared with the plan field by field.
void testTheDraftsMustBeThePlan()
{
    const auto plan = planOfAnswer(answerOf());
    RunDraftGroup group;
    group.id = "group-1";
    group.projectId = "project-1";
    group.planTitle = plan.title;
    const auto genuine = draftsOfPlan(plan, "group-1", "project-1");
    check(checkFollowUpDraftsAreThePlan(plan, group, genuine).success(), "The genuine Drafts are the plan");

    struct Case {
        const char* what;
        std::function<void(RunDraftGroup&, std::vector<RunDraft>&)> edit;
    };
    const std::vector<Case> cases = {
        {"group title", [](auto& g, auto&) { g.planTitle += "!"; }},
        {"count", [](auto&, auto& d) { d.pop_back(); }},
        {"title", [](auto&, auto& d) { d[0].title += "!"; }},
        {"name", [](auto&, auto& d) { d[0].name += "!"; }},
        {"task", [](auto&, auto& d) { d[0].task += "!"; }},
        {"plan key", [](auto&, auto& d) { d[0].provenance->planKey += "x"; }},
        {"dependencies", [](auto&, auto& d) { d[1].provenance->dependsOn.clear(); }},
        {"a contract added", [](auto&, auto& d) { d[0].interfaceContracts.push_back(RunInterfaceContract {"C1", RunContractKind::Constraint, "t", "x", {}}); }},
        {"content fingerprint", [](auto&, auto& d) { d[1].provenance->createdContentFingerprint = hex64('0'); }},
        {"workflow", [](auto&, auto& d) { d[0].workflowId = "other"; }},
        {"launch mode", [](auto&, auto& d) { d[0].launchMode = RunLaunchMode::Mock; }},
        {"group id", [](auto&, auto& d) { d[0].provenance->groupId = "group-2"; }},
        {"project", [](auto&, auto& d) { d[0].projectId = "project-2"; }},
        {"ordinal out of range", [](auto&, auto& d) { d[1].provenance->ordinal = 2; }},
        {"ordinal twice", [](auto&, auto& d) { d[1].provenance->ordinal = 0; }},
        // The first draft is genuine and its copy takes the slot of the second: only the duplicate check stops a null slot.
        {"the first draft twice", [](auto&, auto& d) { d[1] = d[0]; }},
        {"no provenance", [](auto&, auto& d) { d[0].provenance.reset(); }}};
    for (const auto& item : cases) {
        auto g = group;
        auto d = genuine;
        item.edit(g, d);
        requireCode(checkFollowUpDraftsAreThePlan(plan, g, d), "follow_up.drafts_not_the_plan", item.what);
    }
    auto reversed = genuine;
    std::reverse(reversed.begin(), reversed.end());
    check(checkFollowUpDraftsAreThePlan(plan, group, reversed).success(), "The order of the list does not matter, the ordinals do");

    // Contracts are compared field by field: a plan that has some (a shared one and the first draft's own).
    const auto rich = planOfAnswer(answerOf('c', 2, true));
    RunDraftGroup richGroup = group;
    richGroup.planTitle = rich.title;
    const auto richDrafts = draftsOfPlan(rich, "group-1", "project-1");
    check(richDrafts[0].interfaceContracts.size() == 2 && richDrafts[1].interfaceContracts.size() == 1,
        "setup: the first draft has the shared and its own contract, the second the shared one");
    check(checkFollowUpDraftsAreThePlan(rich, richGroup, richDrafts).success(), "A draft with its final contracts is the plan");
    const std::vector<Case> contractCases = {
        {"contract id", [](auto&, auto& d) { d[0].interfaceContracts[0].id = "S2"; }},
        {"contract kind", [](auto&, auto& d) { d[0].interfaceContracts[0].kind = RunContractKind::Constraint; }},
        {"contract title", [](auto&, auto& d) { d[0].interfaceContracts[0].title += "!"; }},
        {"contract text", [](auto&, auto& d) { d[0].interfaceContracts[0].text += "!"; }},
        {"contract scope", [](auto&, auto& d) { d[0].interfaceContracts[0].appliesToPaths = {"src/other"}; }},
        {"contract scope dropped", [](auto&, auto& d) { d[1].interfaceContracts[0].appliesToPaths.clear(); }},
        {"own contract text", [](auto&, auto& d) { d[0].interfaceContracts[1].text = "keep it big"; }},
        {"contracts reordered", [](auto&, auto& d) { std::swap(d[0].interfaceContracts[0], d[0].interfaceContracts[1]); }},
        {"a contract missing", [](auto&, auto& d) { d[0].interfaceContracts.pop_back(); }}};
    for (const auto& item : contractCases) {
        auto g = richGroup;
        auto d = richDrafts;
        item.edit(g, d);
        requireCode(checkFollowUpDraftsAreThePlan(rich, g, d), "follow_up.drafts_not_the_plan", item.what);
    }
}

// P4: every source Run of a group exists and belongs to the project.
void testTheSourceRunsOfAGroupAreTheProjects()
{
    RunDraftGroup group;
    group.sourceRunIds = {"run-1", "run-2"};
    std::vector<std::string> asked;
    const auto loader = [&](const RunId& id) {
        asked.push_back(id);
        if (id == "run-2") return ai::common::Result<std::optional<ProjectId>>::ok(std::optional<ProjectId>("project-2"));
        if (id == "ghost") return ai::common::Result<std::optional<ProjectId>>::ok(std::nullopt);
        if (id == "broken") return ai::common::Result<std::optional<ProjectId>>::failure({"persistence.query_failed", "boom"});
        return ai::common::Result<std::optional<ProjectId>>::ok(std::optional<ProjectId>("project-1"));
    };
    requireCode(checkFollowUpGroupSourceRuns(group, "project-1", loader), "follow_up.source_run_wrong_project", "another project's Run");
    group.sourceRunIds = {"run-1", "ghost"};
    requireCode(checkFollowUpGroupSourceRuns(group, "project-1", loader), "follow_up.source_run_not_found", "a Run that does not exist");
    group.sourceRunIds = {"broken", "ghost"};
    asked.clear();
    requireCode(checkFollowUpGroupSourceRuns(group, "project-1", loader), "persistence.query_failed", "a loader error is passed on");
    check(asked == std::vector<std::string> {"broken"}, "...and stops the walk");
    group.sourceRunIds = {"run-1"};
    check(checkFollowUpGroupSourceRuns(group, "project-1", loader).success(), "Runs of the project pass");
    group.sourceRunIds.clear();
    check(checkFollowUpGroupSourceRuns(group, "project-1", loader).success(), "No source Run is nothing to check here");
    group.sourceRunIds.assign(kRunDraftGroupMaxSourceRuns + 1, "run-1");
    asked.clear();
    requireCode(checkFollowUpGroupSourceRuns(group, "project-1", loader), "follow_up.group_mismatch", "too many ids");
    check(asked.empty(), "...refused before any lookup");
}

// P2: a stored row is exactly what the transitions write: the stamps of the last step equal the update time, and a discarded
// request has one of its two real shapes.
void testTheTimeChainIsTheOneTheTransitionsWrite()
{
    auto quoted = inState(S::Quoted);
    quoted.updatedAt = quoted.createdAt + std::chrono::milliseconds(1);
    expectInvalid(quoted, "a quoted request updated after it was created");
    auto authorized = inState(S::Authorized);
    authorized.updatedAt = *authorized.authorizedAt + std::chrono::milliseconds(1);
    expectInvalid(authorized, "an authorised request updated after its authorisation");
    auto composing = inState(S::Composing);
    composing.updatedAt = *composing.claimedAt + std::chrono::milliseconds(1);
    expectInvalid(composing, "a composing request updated after its claim");
    // plan_ready is later than its claim by design: the answer's stamp.
    check(validateFollowUpPlanRequest(inState(S::PlanReady)).success() && inState(S::PlanReady).updatedAt > *inState(S::PlanReady).claimedAt,
        "A ready plan is updated after its claim");

    // quoted -> discarded never authorises; authorized has no edge into discarded.
    auto discardedAuthorised = inState(S::Discarded);
    discardedAuthorised.authorizedAt = discardedAuthorised.createdAt;
    expectInvalid(discardedAuthorised, "a discarded request that was authorised but never claimed");
    requireCode(applyFollowUpTransition(inState(S::Authorized), plainStep(E::Discard, S::Authorized), at(5000)),
        "follow_up.invalid_transition", "authorized has no edge into discarded");
}

// PC-5C review: the accounting of the paid call rides with the step that settles it.
void testTheAccountingRidesWithTheStepThatSettles()
{
    const auto composing = inState(S::Composing);
    const auto stampAt = at(5000);

    auto withoutAccounting = plainStep(E::PlanSucceeded, S::Composing);
    withoutAccounting.answer = answerOf();
    requireCode(applyFollowUpTransition(composing, withoutAccounting, stampAt), "follow_up.invalid_request",
        "an answer without the accounting of the paid response");

    // The accounting reaches the stored request, for a plan, an invalid answer and a stop after a complete reply.
    const auto ready = applied(composing, planStep(answerOf()), 5000);
    check(ready.accounting == std::optional<FollowUpAccounting>(accountingOf()), "A stored plan carries the accounting of its call");
    auto invalidAnswer = failStep({"next_run_plan.envelope_missing", "No envelope", std::string("prose")});
    invalidAnswer.accounting = accountingOf();
    const auto failedWithResponse = applied(composing, invalidAnswer, 5000);
    check(failedWithResponse.state == S::Failed && failedWithResponse.accounting == std::optional<FollowUpAccounting>(accountingOf()),
        "A failure that followed a response carries it");
    const auto failedWithout = applied(composing, failStep({"provider.failed", "The provider failed", std::nullopt}), 5000);
    check(!failedWithout.accounting.has_value(), "...and one that did not has none");
    auto stopped = plainStep(E::Cancel, S::Composing);
    stopped.accounting = accountingOf();
    check(applied(composing, stopped, 5000).accounting.has_value(), "A stop after a complete reply carries it");
    check(applied(ready, plainStep(E::Discard, S::PlanReady), 6000).accounting == ready.accounting,
        "Discarding a plan keeps the accounting of the call that paid for it");
    const auto saved = applyFollowUpConfirm(ready, "group-1", at(6000));
    check(saved.success() && saved.value().accounting == ready.accounting, "...and so does the Confirm");

    // No other step may carry one.
    for (const auto& [event, from] : std::vector<std::pair<E, S>> {{E::Authorize, S::Quoted}, {E::Claim, S::Authorized}, {E::MarkStale, S::Quoted},
             {E::Expire, S::Quoted}, {E::Interrupt, S::Composing}, {E::Discard, S::Quoted}}) {
        auto step = plainStep(event, from);
        if (event == E::Authorize) step.contextFingerprint = inState(from).quote.contextFingerprint;
        step.accounting = accountingOf();
        requireCode(applyFollowUpTransition(inState(from), step, at(1'000'000 + 100)), "follow_up.invalid_request",
            std::string("an accounting on ") + toString(event));
    }
    // A stop before the claim has no call to account for: the gate refuses an accounting without the claim.
    auto early = plainStep(E::Cancel, S::Authorized);
    early.accounting = accountingOf();
    requireCode(applyFollowUpTransition(inState(S::Authorized), early, at(5000)), "follow_up.invalid_request", "an accounting before the claim");
}

void testAStoredAccountingIsCoherentAndStrict()
{
    const auto ready = inState(S::PlanReady);
    const auto refuses = [&](const char* what, const std::function<void(FollowUpAccounting&)>& edit) {
        auto request = ready;
        edit(*request.accounting);
        expectInvalid(request, what);
    };
    check(validateFollowUpPlanRequest(ready).success(), "setup: the control passes");
    refuses("cache tokens above the input", [](auto& a) { a.cacheReadInputTokens = a.inputTokens + 1; });
    refuses("cache creation plus read above the input", [](auto& a) { a.cacheCreationInputTokens = a.inputTokens; a.cacheReadInputTokens = 1; });
    refuses("a duration split that is not known but has tokens", [](auto& a) { a.cacheCreationInputTokens = 10; a.cacheCreationFiveMinuteTokens = 10; });
    refuses("a known duration split that does not add up", [](auto& a) {
        a.cacheCreationInputTokens = 100;
        a.cacheCreationDurationKnown = true;
        a.cacheCreationFiveMinuteTokens = 40;
        a.cacheCreationOneHourTokens = 40;
    });
    refuses("a figure beyond the storable range", [](auto& a) { a.costMicros = std::numeric_limits<std::uint64_t>::max(); });
    auto good = ready;
    good.accounting->cacheCreationInputTokens = 100;
    good.accounting->cacheCreationDurationKnown = true;
    good.accounting->cacheCreationFiveMinuteTokens = 40;
    good.accounting->cacheCreationOneHourTokens = 60;
    check(validateFollowUpPlanRequest(good).success(), "A known split that adds up is accepted");

    // The stored form.
    const auto original = good.accounting.value();
    const auto read = parseFollowUpAccounting(serializeFollowUpAccounting(original));
    check(read.success() && read.value() == original, "The accounting round-trips every field");
    auto unpriced = original;
    unpriced.priceKnown = false;
    unpriced.latencyMs = 77;
    unpriced.cacheReadInputTokens = 5;
    unpriced.costMicros = 123'456;
    const auto readUnpriced = parseFollowUpAccounting(serializeFollowUpAccounting(unpriced));
    check(readUnpriced.success() && readUnpriced.value() == unpriced, "...including an unknown price, the latency and the cache-read tokens");
    const std::vector<std::pair<const char*, std::string>> bad = {
        {"not JSON", "{not json"}, {"an array", "[]"},
        {"a missing field", R"({"inputTokens":1,"outputTokens":1,"cacheCreationInputTokens":0,"cacheReadInputTokens":0,"cacheCreationDurationKnown":false,"cacheCreationFiveMinuteTokens":0,"cacheCreationOneHourTokens":0,"costMicros":0,"priceKnown":true})"},
        {"an extra field", R"({"inputTokens":1,"outputTokens":1,"cacheCreationInputTokens":0,"cacheReadInputTokens":0,"cacheCreationDurationKnown":false,"cacheCreationFiveMinuteTokens":0,"cacheCreationOneHourTokens":0,"costMicros":0,"priceKnown":true,"latencyMs":0,"x":1})"},
        {"a negative figure", R"({"inputTokens":-1,"outputTokens":1,"cacheCreationInputTokens":0,"cacheReadInputTokens":0,"cacheCreationDurationKnown":false,"cacheCreationFiveMinuteTokens":0,"cacheCreationOneHourTokens":0,"costMicros":0,"priceKnown":true,"latencyMs":0})"},
        {"a mistyped flag", R"({"inputTokens":1,"outputTokens":1,"cacheCreationInputTokens":0,"cacheReadInputTokens":0,"cacheCreationDurationKnown":"no","cacheCreationFiveMinuteTokens":0,"cacheCreationOneHourTokens":0,"costMicros":0,"priceKnown":true,"latencyMs":0})"},
        {"a figure beyond the range", R"({"inputTokens":18446744073709551615,"outputTokens":1,"cacheCreationInputTokens":0,"cacheReadInputTokens":0,"cacheCreationDurationKnown":false,"cacheCreationFiveMinuteTokens":0,"cacheCreationOneHourTokens":0,"costMicros":0,"priceKnown":true,"latencyMs":0})"},
    };
    for (const auto& [what, text] : bad) {
        const auto refused = parseFollowUpAccounting(text);
        check(!refused.success() && refused.error()->code == "persistence.invalid_data", std::string("The reader refuses ") + what);
    }
}

// PC-5C review: a stored plan is read back as what it claims to be, not only checked for its form. The parser of next-run-plan.v1 is
// frozen, so this can never make an old row unreadable.
void testTheReadGateRefusesAStoredPlanThatIsNotThePlan()
{
    const auto ready = inState(S::PlanReady);
    check(validateFollowUpPlanRequest(ready).success(), "setup: the control passes");
    const auto refuses = [&](const char* what, const std::function<void(FollowUpPlanRequest&)>& edit) {
        auto request = ready;
        edit(request);
        expectInvalid(request, what);
    };
    refuses("a reply without an envelope", [](auto& r) { r.answer->rawReply = "prose only"; });
    refuses("two envelopes", [](auto& r) { r.answer->rawReply += r.answer->rawReply; });
    refuses("an envelope that is not a plan", [](auto& r) { r.answer->rawReply = replyOfPlanJson("{}"); });
    refuses("the fingerprint of another plan", [](auto& r) { r.answer->planFingerprint = answerOf('d').planFingerprint; });
    refuses("a reply that holds another plan", [](auto& r) { r.answer->rawReply = answerOf('d').rawReply; });
    refuses("three drafts for a window of two", [](auto& r) {
        r.answer = answerOf('c', 3);
    });
    refuses("advisories that do not read back", [](auto& r) { r.answer->advisoriesJson = "[1]"; });
    refuses("advisories with a missing field", [](auto& r) { r.answer->advisoriesJson = R"([{"code":"c"}])"; });
    auto narrowed = ready;
    narrowed.splitHint = FollowUpSplitHint::One;
    expectInvalid(narrowed, "a two-draft plan under the hint 'one'");
    // The saved and the discarded request keep their plan to the same standard.
    auto saved = inState(S::DraftsSaved);
    saved.answer->rawReply = "prose only";
    expectInvalid(saved, "a drafts_saved request whose reply is no plan");
    auto discarded = applied(ready, plainStep(E::Discard, S::PlanReady), 4000);
    check(validateFollowUpPlanRequest(discarded).success(), "A discarded plan is a valid record");
    discarded.answer->rawReply = "prose only";
    expectInvalid(discarded, "a discarded request whose reply is no plan");
}

void testTheOpenSlotRule()
{
    const auto incoming = baseRequest();
    auto open = [&](S state) { return std::optional<FollowUpPlanRequest>(inState(state)); };
    const auto none = planFollowUpQuotedInsert(incoming, std::nullopt);
    check(none.success() && !none.value().has_value(), "No open request: insert");
    const auto replaced = planFollowUpQuotedInsert(incoming, open(S::Quoted));
    check(replaced.success() && replaced.value() == std::optional<FollowUpPlanRequestId>("request-1"),
        "A quoted request is replaced");
    for (const S state : {S::Authorized, S::Composing, S::PlanReady}) {
        requireCode(planFollowUpQuotedInsert(incoming, open(state)), "follow_up.request_open",
            std::string("a '") + toString(state) + "' request is never displaced");
    }
    auto notQuoted = inState(S::Authorized);
    requireCode(planFollowUpQuotedInsert(notQuoted, std::nullopt), "follow_up.invalid_request", "only a quoted request is inserted");
}

void testTheAuthorityRules()
{
    const auto request = baseRequest();
    check(checkFollowUpRequestWriteAuthority(request, std::optional<ProjectId>("project-1")).success(), "write: all agree");
    requireCode(checkFollowUpRequestWriteAuthority(request, std::nullopt), "follow_up.run_not_found", "write: no Run");
    requireCode(checkFollowUpRequestWriteAuthority(request, std::optional<ProjectId>("project-2")), "follow_up.project_mismatch", "write: another project");
    check(checkFollowUpRequestReadAuthority(request, std::optional<ProjectId>("project-1")).success(), "read: all agree");
    requireCode(checkFollowUpRequestReadAuthority(request, std::nullopt), "persistence.invalid_data", "read: no Run");
    requireCode(checkFollowUpRequestReadAuthority(request, std::optional<ProjectId>("project-2")), "persistence.invalid_data", "read: another project");
    check(checkFollowUpListLimit(1).success() && checkFollowUpListLimit(kFollowUpMaxListLimit).success()
            && !checkFollowUpListLimit(0).success() && !checkFollowUpListLimit(kFollowUpMaxListLimit + 1).success(),
        "the list limit is 1..500");
}

void testTheStorageCodecsRoundTripAndAreStrict()
{
    const std::vector<FollowUpPinnedFile> files = {pinnedFile("a.cpp"), {"b/c.qml", hex64('d'), 9000, 4000, 1400, true}};
    const auto parsed = parseFollowUpPinnedFiles(serializeFollowUpPinnedFiles(files));
    check(parsed.success() && parsed.value() == files, "pinned files round-trip");
    check(parseFollowUpPinnedFiles("[]").success() && parseFollowUpPinnedFiles("[]").value().empty(), "an empty list");
    for (const char* bad : {"", "{}", "[1]", "[{}]", "not json",
             R"([{"relativePath":"a","contentSha256":"b","originalBytes":1,"shownBytes":1,"estimatedTokens":1}])",
             R"([{"relativePath":"a","contentSha256":"b","originalBytes":-1,"shownBytes":1,"estimatedTokens":1,"truncated":false}])",
             R"([{"relativePath":1,"contentSha256":"b","originalBytes":1,"shownBytes":1,"estimatedTokens":1,"truncated":false}])",
             R"([{"relativePath":"a","contentSha256":"b","originalBytes":1,"shownBytes":1,"estimatedTokens":1,"truncated":"no"}])",
             R"([{"relativePath":"a","contentSha256":"b","originalBytes":1,"shownBytes":1,"estimatedTokens":1,"truncated":false,"extra":1}])"}) {
        requireCode(parseFollowUpPinnedFiles(bad), "persistence.invalid_data", std::string("pinned files '") + bad + "'");
    }

    ProviderRequestPricingIdentity identity = baseRequest().quote.worstCase.pricingIdentity;
    identity.longContextPricing = {1000, 4'000'000, 20'000'000, 7'500'000, 400'000, 9'000'000};
    const auto text = serializeFollowUpPricingIdentity(identity);
    const auto back = parseFollowUpPricingIdentity(text);
    check(back.success() && back.value() == identity, "the pricing identity round-trips, tier included");
    check(providerRequestPricingIdentityFingerprint(back.value()) == providerRequestPricingIdentityFingerprint(identity),
        "...and keeps its fingerprint");
    for (const auto mode : {ai::models::PromptCacheMode::Disabled, ai::models::PromptCacheMode::EphemeralFiveMinutes,
             ai::models::PromptCacheMode::EphemeralOneHour}) {
        auto other = identity;
        other.promptCache.mode = mode;
        const auto roundTrip = parseFollowUpPricingIdentity(serializeFollowUpPricingIdentity(other));
        check(roundTrip.success() && roundTrip.value().promptCache.mode == mode, "every cache mode round-trips");
        check(followUpPromptCacheModeFromString(ai::models::toString(mode)) == std::optional<ai::models::PromptCacheMode>(mode),
            "every cache mode has a spelling");
    }
    check(!followUpPromptCacheModeFromString("").has_value() && !followUpPromptCacheModeFromString("EPHEMERAL_1H").has_value(),
        "only the exact spellings");
    auto json = nlohmann::json::parse(text);
    for (const char* key : {"providerId", "modelId", "apiModelId", "promptCache", "contextWindowTokens", "modelMaxOutputTokens",
             "inputPrice", "outputPrice", "cacheWritePrice", "cacheReadPrice", "cacheWriteOneHourPrice", "longContext"}) {
        auto missing = json;
        missing.erase(key);
        requireCode(parseFollowUpPricingIdentity(missing.dump()), "persistence.invalid_data", std::string("missing ") + key);
    }
    auto extra = json;
    extra["extra"] = 1;
    requireCode(parseFollowUpPricingIdentity(extra.dump()), "persistence.invalid_data", "an unknown key");
    auto wrongType = json;
    wrongType["inputPrice"] = "3";
    requireCode(parseFollowUpPricingIdentity(wrongType.dump()), "persistence.invalid_data", "a price as text");
    auto badMode = json;
    badMode["promptCache"] = "forever";
    requireCode(parseFollowUpPricingIdentity(badMode.dump()), "persistence.invalid_data", "an unknown cache mode");
    auto tierMissing = json;
    tierMissing["longContext"].erase("threshold");
    requireCode(parseFollowUpPricingIdentity(tierMissing.dump()), "persistence.invalid_data", "a tier without its threshold");
    auto tooBig = json;
    tooBig["inputPrice"] = std::numeric_limits<std::uint64_t>::max();
    requireCode(parseFollowUpPricingIdentity(tooBig.dump()), "persistence.invalid_data", "a price beyond the SQLite range");
    requireCode(parseFollowUpPricingIdentity("not json"), "persistence.invalid_data", "not json");
}

} // namespace

int main()
{
    int failures = 0;
    const auto run = [&failures](const char* name, void (*test)()) {
        try {
            test();
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAILED " << name << ": " << error.what() << '\n';
        }
    };
    run("testEveryRealStateIsAValidRecord", testEveryRealStateIsAValidRecord);
    run("testTheStateFieldMatrixIsExactlyTheAgreedOne", testTheStateFieldMatrixIsExactlyTheAgreedOne);
    run("testTheGateRefusesEveryBrokenField", testTheGateRefusesEveryBrokenField);
    run("testTheChargedStampIsTheClaim", testTheChargedStampIsTheClaim);
    run("testTransitionsFollowTheFiveATableAndOnlyThat", testTransitionsFollowTheFiveATableAndOnlyThat);
    run("testWhatEachTransitionWrites", testWhatEachTransitionWrites);
    run("testTheCompareAndSetPayloadAndDeadlineRules", testTheCompareAndSetPayloadAndDeadlineRules);
    run("testTheConfirmDecisionAndStep", testTheConfirmDecisionAndStep);
    run("testAnAnswerIsOneValidPlanOfTheAuthorisedSize", testAnAnswerIsOneValidPlanOfTheAuthorisedSize);
    run("testTheDraftsMustBeThePlan", testTheDraftsMustBeThePlan);
    run("testTheSourceRunsOfAGroupAreTheProjects", testTheSourceRunsOfAGroupAreTheProjects);
    run("testTheTimeChainIsTheOneTheTransitionsWrite", testTheTimeChainIsTheOneTheTransitionsWrite);
    run("testTheAccountingRidesWithTheStepThatSettles", testTheAccountingRidesWithTheStepThatSettles);
    run("testAStoredAccountingIsCoherentAndStrict", testAStoredAccountingIsCoherentAndStrict);
    run("testTheReadGateRefusesAStoredPlanThatIsNotThePlan", testTheReadGateRefusesAStoredPlanThatIsNotThePlan);
    run("testTheOpenSlotRule", testTheOpenSlotRule);
    run("testTheAuthorityRules", testTheAuthorityRules);
    run("testTheStorageCodecsRoundTripAndAreStrict", testTheStorageCodecsRoundTripAndAreStrict);
    if (failures != 0) {
        std::cerr << "FollowUpPlanRequest tests failed: " << failures << " test(s)\n";
        return 1;
    }
    std::cout << "FollowUpPlanRequest tests passed\n";
    return 0;
}
