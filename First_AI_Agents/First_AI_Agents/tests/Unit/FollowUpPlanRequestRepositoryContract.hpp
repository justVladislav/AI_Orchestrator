#pragma once

// PC-5B: ONE contract suite for IFollowUpPlanRequestRepository and IFollowUpPlanConfirmPort. The SQLite adapter and the
// in-memory adapter run exactly the same scenarios, so a difference between them is a test failure, not a surprise. Anything that
// depends on SQLite (the schema triggers and CHECKs, a damaged row, the rollback of a half-written Confirm, migrations) lives in
// the SQLite tests instead.

#include "FollowUpRequestTestSupport.hpp"
#include "RunDraftRepositoryContract.hpp"

#include "ai/application/planning/IFollowUpPlanRequestRepository.hpp"
#include "ai/application/planning/IRunDraftRepository.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace followup_contract {

using namespace followup_test;
using S = ai::application::FollowUpPlanRequestState;
using E = ai::application::FollowUpPlanEvent;

struct Env {
    std::shared_ptr<void> keepAlive;                 // owns the repositories and whatever backs them
    IFollowUpPlanRequestRepository* repository {nullptr};
    IFollowUpPlanConfirmPort* confirm {nullptr};
    IRunDraftRepository* drafts {nullptr};           // the Draft store the Confirm saves into
    Catalog catalog;                                 // an existing provider and model
    std::string projectId;                           // an existing project
    std::string otherProjectId;                      // a second existing project
    // Makes the Run exist in `projectId` (SQLite: a `runs` row; in-memory: what its Run lookup answers). Idempotent by Run id.
    std::function<void(const std::string& runId, const std::string& projectId)> ensureRun;
    // The Run changes project behind the repository's back.
    std::function<void(const std::string& runId, const std::string& projectId)> moveRun;
};
using EnvFactory = std::function<Env()>;

// ---- builders -----------------------------------------------------------------------------------------------------------------

inline FollowUpPlanRequest quoted(const Env& env, const std::string& id, const std::string& runId, std::int64_t createdMs = 0)
{
    if (env.ensureRun) env.ensureRun(runId, env.projectId);
    return quotedRequest(env.catalog, id, env.projectId, runId, at(createdMs));
}

inline FollowUpPlanRequest mustInsert(const Env& env, const FollowUpPlanRequest& request)
{
    const auto inserted = env.repository->insertQuoted(request);
    check(inserted.success(), "setup: insertQuoted must succeed: " + (inserted.success() ? std::string() : inserted.error()->code + " " + inserted.error()->message));
    return inserted.value().request;
}

inline FollowUpPlanRequest mustStep(const Env& env, const std::string& id, const FollowUpTransition& step, std::int64_t atMs)
{
    const auto next = env.repository->transition(id, step, at(atMs));
    check(next.success(), "setup: the step must succeed: " + (next.success() ? std::string() : next.error()->code + " " + next.error()->message));
    return next.value();
}

inline FollowUpPlanRequest stored(const Env& env, const std::string& id)
{
    const auto found = env.repository->findById(id);
    check(found.success() && found.value().has_value(), "setup: the request must be stored: " + id);
    return *found.value();
}

inline rundraft_contract::Env draftEnvOf(const Env& env)
{
    rundraft_contract::Env value;
    value.projectId = env.projectId;
    value.otherProjectId = env.otherProjectId;
    value.providerId = env.catalog.providerId;
    return value;
}

// The plan of the request's stored reply; a request without one (not yet plan_ready) is given the default fixture plan.
inline NextRunPlan planOfRequest(const FollowUpPlanRequest& request)
{
    return planOfAnswer(request.answer.has_value() ? *request.answer : answerOf());
}

inline RunDraftGroup groupFor(const FollowUpPlanRequest& request, const std::string& groupId, char keyDigit)
{
    const auto plan = planOfRequest(request);
    RunDraftGroup group;
    group.id = groupId;
    group.projectId = request.projectId;
    group.origin = RunDraftGroupOrigin::FollowUpRequest;
    group.idempotencyKey = hex64(keyDigit);
    group.planTitle = plan.title;
    group.planFingerprint = plan.fingerprint;
    group.sourceRequestId = request.id;
    group.contextFingerprint = request.quote.contextFingerprint;
    group.sourceRunIds = {request.sourceRunId};
    group.createdAt = at(50);
    return group;
}

// The Drafts of `request`'s plan, materialised the way RunDraftPlanService does it (title = name, the plan's contracts, a per-Run
// Manifest roster, the content fingerprint of the plan's draft). `request` null: the default fixture plan.
inline std::vector<RunDraft> draftsFor(const Env& env, const std::string& groupId, const std::string& prefix,
    const FollowUpPlanRequest* request = nullptr)
{
    const auto draftEnv = draftEnvOf(env);
    const auto plan = request != nullptr ? planOfRequest(*request) : planOfAnswer(answerOf());
    std::vector<RunDraft> drafts;
    for (std::size_t ordinal = 0; ordinal < plan.drafts.size(); ++ordinal) {
        const auto& planned = plan.drafts[ordinal];
        auto draft = rundraft_contract::groupDraft(draftEnv, groupId, prefix + "-" + planned.key,
            static_cast<std::uint32_t>(ordinal), planned.key, planned.dependsOn);
        draft.name = planned.title;
        draft.title = planned.title;
        draft.task = planned.task;
        draft.interfaceContracts = planned.contracts;
        rundraft_contract::reseal(draft);
        drafts.push_back(std::move(draft));
    }
    return drafts;
}

inline std::string keyDigitFor(const std::string& text)
{
    return ai::application::sha256Hex(text);
}

inline FollowUpConfirmResult mustConfirm(const Env& env, const FollowUpPlanRequest& request, std::int64_t atMs,
    const std::string& suffix = "")
{
    const std::string groupId = "group-" + request.id + suffix;
    auto group = groupFor(request, groupId, 'e');
    group.idempotencyKey = keyDigitFor(request.id + suffix);
    const auto result = env.confirm->confirmDrafts(
        request.id, request.answer->planFingerprint, group, draftsFor(env, groupId, "draft-" + request.id + suffix, &request), at(atMs));
    check(result.success(), "setup: the confirm must succeed: " + (result.success() ? std::string() : result.error()->code + " " + result.error()->message));
    return result.value();
}

// A request in `target`, reached through the repository only.
inline FollowUpPlanRequest drive(const Env& env, const std::string& id, const std::string& runId, S target, std::int64_t base = 0)
{
    auto request = mustInsert(env, quoted(env, id, runId, base));
    const auto step = [&](const FollowUpTransition& transition, std::int64_t ms) { return mustStep(env, id, transition, base + ms); };
    switch (target) {
    case S::Quoted: return request;
    case S::Stale: return step(plainStep(E::MarkStale, S::Quoted), 500);
    case S::Expired: return step(plainStep(E::Expire, S::Quoted), kQuoteLifetimeMs);
    case S::Cancelled: return step(plainStep(E::Cancel, S::Quoted), 500);
    case S::Discarded: return step(plainStep(E::Discard, S::Quoted), 500);
    default: break;
    }
    request = step(authorizeStep(request), 1000);
    if (target == S::Authorized) return request;
    request = step(plainStep(E::Claim, S::Authorized), 2000);
    if (target == S::Composing) return request;
    if (target == S::Failed) return step(failStep({"provider.failed", "The provider failed", std::nullopt}), 3000);
    if (target == S::Interrupted) return step(plainStep(E::Interrupt, S::Composing), 3000);
    request = step(planStep(answerOf()), 3000);
    if (target == S::PlanReady) return request;
    return mustConfirm(env, request, base + 4000).request;
}

inline const std::vector<S>& allStates()
{
    static const std::vector<S> states = {S::Quoted, S::Authorized, S::Composing, S::PlanReady, S::DraftsSaved, S::Stale,
        S::Expired, S::Cancelled, S::Discarded, S::Interrupted, S::Failed};
    return states;
}

inline std::size_t draftCount(const Env& env)
{
    const auto all = env.drafts->listAll();
    check(all.success(), "listAll must work");
    return all.value().size();
}

// ---- scenarios ----------------------------------------------------------------------------------------------------------------

inline void insertAndReadBack(const EnvFactory& make)
{
    const auto env = make();
    const auto request = quoted(env, "req-1", "run-1");
    const auto inserted = env.repository->insertQuoted(request);
    check(inserted.success() && !inserted.value().discardedQuoteId.has_value() && sameRequest(inserted.value().request, request),
        "A quoted request inserts and nothing was replaced");
    const auto found = env.repository->findById("req-1");
    check(found.success() && found.value().has_value() && sameRequest(*found.value(), request),
        "It reads back field for field: the whole quote, the recipient identity, the files, the times");
    const auto open = env.repository->findOpenForSourceRun("run-1");
    check(open.success() && open.value().has_value() && open.value()->id == "req-1", "It is the open request of its Run");
    const auto listed = env.repository->listForProject(env.projectId, 10);
    check(listed.success() && listed.value().size() == 1 && sameRequest(listed.value().front(), request), "It is listed for its project");
    const auto byState = env.repository->listByState(S::Quoted, 10);
    check(byState.success() && byState.value().size() == 1, "It is listed by its state");
    check(env.repository->listByState(S::Authorized, 10).value().empty(), "...and not by another state");
    check(env.repository->findById("nope").success() && !env.repository->findById("nope").value().has_value(), "An unknown id");
    check(env.repository->findOpenForSourceRun("run-2").success() && !env.repository->findOpenForSourceRun("run-2").value().has_value(),
        "A Run without a request");
    check(env.repository->listForProject(env.otherProjectId, 10).value().empty(), "Another project sees nothing");
    check(!followUpRequestMayBeCharged(request), "A new quote is free");
}

// PC-5D: the newest request of a Run in ANY state - one row whatever the history - and only of that Run.
inline void theLatestRequestOfARunIsTheNewestInAnyState(const EnvFactory& make)
{
    const auto env = make();
    check(env.repository->findLatestForSourceRun("run-1").success() && !env.repository->findLatestForSourceRun("run-1").value().has_value(),
        "A Run without a request has no latest one");
    mustInsert(env, quoted(env, "late-1", "run-1", 0));
    check(env.repository->findLatestForSourceRun("run-1").value()->id == "late-1", "A quoted request is the latest");
    check(env.repository->transition("late-1", plainStep(FollowUpPlanEvent::Cancel, S::Quoted), at(1)).success(), "setup: cancel");
    const auto cancelled = env.repository->findLatestForSourceRun("run-1");
    check(cancelled.success() && cancelled.value().has_value() && cancelled.value()->id == "late-1" && cancelled.value()->state == S::Cancelled,
        "A closed request is still the latest one (the open lookup no longer finds it)");
    check(!env.repository->findOpenForSourceRun("run-1").value().has_value(), "...which the open lookup does not");
    mustInsert(env, quoted(env, "late-2", "run-1", 10));
    check(env.repository->findLatestForSourceRun("run-1").value()->id == "late-2", "A newer request replaces it");
    mustInsert(env, quoted(env, "late-3", "run-2", 20));
    check(env.repository->findLatestForSourceRun("run-1").value()->id == "late-2", "A request of another Run is never mixed in");
    check(env.repository->findLatestForSourceRun("run-2").value()->id == "late-3", "...and that Run has its own");
    // Equal creation times: the larger id is the later (the same tie-break as every newest-first read).
    mustInsert(env, quoted(env, "tie-a", "run-3", 30));
    check(env.repository->transition("tie-a", plainStep(FollowUpPlanEvent::Cancel, S::Quoted), at(31)).success(), "setup: cancel tie-a");
    mustInsert(env, quoted(env, "tie-b", "run-3", 30));
    check(env.repository->findLatestForSourceRun("run-3").value()->id == "tie-b", "Equal creation times: the larger id is the later one");
}

// The shared gate is exercised field by field in FollowUpPlanRequest_tests; here each adapter must call it before storing.
inline void theGateRunsBeforeStorage(const EnvFactory& make)
{
    const auto env = make();
    auto broken = quoted(env, "req-bad-1", "run-1");
    broken.quote.providerIdentity.baseUrl += "x";   // the recipient no longer matches its fingerprint
    requireCode(env.repository->insertQuoted(broken), "follow_up.invalid_request", "a tampered recipient identity");
    broken = quoted(env, "req-bad-2", "run-1");
    broken.maxDrafts = 5;
    requireCode(env.repository->insertQuoted(broken), "follow_up.invalid_request", "an unauthorised draft window");
    broken = quoted(env, "req-bad-3", "run-1");
    broken.createdAt += std::chrono::microseconds(1);
    requireCode(env.repository->insertQuoted(broken), "follow_up.invalid_request", "a time that is not whole milliseconds");
    broken = quoted(env, "req-bad-4", "run-1");
    broken.state = S::Authorized;
    broken.authorizedAt = at(1);
    broken.updatedAt = at(1);
    requireCode(env.repository->insertQuoted(broken), "follow_up.invalid_request", "a request that does not start quoted");
    for (const char* id : {"req-bad-1", "req-bad-2", "req-bad-3", "req-bad-4"}) {
        check(!env.repository->findById(id).value().has_value(), std::string("A refused request is not stored: ") + id);
    }
    check(env.repository->listForProject(env.projectId, 10).value().empty(), "...and nothing at all was written");
}

inline void theRunAuthorityIsCheckedOnWrite(const EnvFactory& make)
{
    const auto env = make();
    auto request = quotedRequest(env.catalog, "req-1", env.projectId, "ghost-run", at(0));
    requireCode(env.repository->insertQuoted(request), "follow_up.run_not_found", "a Run that does not exist");
    env.ensureRun("run-elsewhere", env.otherProjectId);
    request = quotedRequest(env.catalog, "req-2", env.projectId, "run-elsewhere", at(0));
    requireCode(env.repository->insertQuoted(request), "follow_up.project_mismatch", "a Run of another project");
    check(!env.repository->findById("req-1").value().has_value() && !env.repository->findById("req-2").value().has_value(),
        "Nothing was written");
}

inline void aDuplicateIdIsRefusedAndHarmless(const EnvFactory& make)
{
    const auto env = make();
    const auto first = mustInsert(env, quoted(env, "req-1", "run-1"));
    // Same id, another Run: the id check cannot be confused with the one-open slot.
    requireCode(env.repository->insertQuoted(quoted(env, "req-1", "run-2")), "follow_up.duplicate_id", "the same id again");
    check(sameRequest(stored(env, "req-1"), first), "The first request is untouched");
    check(!env.repository->findOpenForSourceRun("run-2").value().has_value(), "...and the second Run has no request");
    // A duplicate id on the SAME Run must not cost the predecessor its place: the replacement fails as a whole.
    requireCode(env.repository->insertQuoted(quoted(env, "req-1", "run-1", 10)), "follow_up.duplicate_id", "the same id on the same Run");
    check(stored(env, "req-1").state == S::Quoted, "A failed replacement did not discard anything");
}

inline void oneOpenRequestPerSourceRun(const EnvFactory& make)
{
    const auto env = make();
    const auto first = mustInsert(env, quoted(env, "q-1", "run-1", 0));
    const auto replaced = env.repository->insertQuoted(quoted(env, "q-2", "run-1", 5000));
    check(replaced.success() && replaced.value().discardedQuoteId == std::optional<std::string>("q-1"),
        "A new quote replaces the quoted one and says so");
    const auto old = stored(env, "q-1");
    check(old.state == S::Discarded && !followUpRequestMayBeCharged(old) && old.finishedAt == at(5000) && old.updatedAt == at(5000)
            && !old.answer && !old.claimedAt,
        "...which is discarded for free, at the time of the new quote");
    check(env.repository->findOpenForSourceRun("run-1").value()->id == "q-2", "The new quote is the open one");
    check(env.repository->listByState(S::Discarded, 10).value().size() == 1, "The replaced quote stays as a record");
    check(first.id == "q-1", "setup");

    // Anything further along than quoted is never displaced - and the refusal changes nothing.
    for (const S state : {S::Authorized, S::Composing, S::PlanReady}) {
        const std::string id = std::string("open-") + toString(state);
        const std::string run = std::string("run-") + toString(state);
        const auto open = drive(env, id, run, state);
        requireCode(env.repository->insertQuoted(quoted(env, id + "-new", run, 9000)), "follow_up.request_open",
            std::string("a new quote while a '") + toString(state) + "' request is open");
        check(sameRequest(stored(env, id), open) && !env.repository->findById(id + "-new").value().has_value(),
            "...and the open request and the store are as they were");
    }

    // Every way a request ends frees the slot, with no discard of anything.
    for (const S state : {S::Stale, S::Expired, S::Cancelled, S::Discarded, S::Failed, S::Interrupted, S::DraftsSaved}) {
        const std::string run = std::string("free-") + toString(state);
        const auto done = drive(env, run + "-old", run, state);
        check(!env.repository->findOpenForSourceRun(run).value().has_value(),
            std::string("A '") + toString(state) + "' request is not open");
        const auto next = env.repository->insertQuoted(quoted(env, run + "-new", run, 20'000));
        check(next.success() && !next.value().discardedQuoteId.has_value(),
            std::string("After a '") + toString(state) + "' request the Run can be quoted again");
        check(sameRequest(stored(env, run + "-old"), done), "...and the old request is untouched");
    }

    // Another Run is independent.
    check(env.repository->insertQuoted(quoted(env, "other-run", "run-other", 0)).success(), "Another Run has its own slot");
    check(env.repository->findOpenForSourceRun("run-1").value()->id == "q-2", "...and does not disturb this one");
}

inline void transitionsFollowTheAgreedTableAndOnlyThat(const EnvFactory& make)
{
    const auto env = make();
    int index = 0;
    for (const S from : allStates()) {
        for (const E event : {E::Authorize, E::Claim, E::PlanSucceeded, E::CompositionFailed, E::MarkStale, E::Expire,
                 E::Cancel, E::Interrupt, E::ConfirmDrafts, E::Discard}) {
            const std::string id = "t-" + std::to_string(index);
            const std::string run = "run-t-" + std::to_string(index);
            ++index;
            const auto before = drive(env, id, run, from);
            FollowUpTransition step = plainStep(event, from);
            if (event == E::Authorize) step.contextFingerprint = before.quote.contextFingerprint;
            if (event == E::PlanSucceeded) {
                step.answer = answerOf();
                step.accounting = accountingOf();
            }
            if (event == E::CompositionFailed) step.failure = FollowUpPlanFailure {"provider.failed", "failed", std::nullopt};
            const auto now = event == E::Expire ? at(kQuoteLifetimeMs + 10) : at(10'000);
            const auto result = env.repository->transition(id, step, now);
            const auto table = transitionFollowUpPlanRequest(from, event);
            const std::string name = std::string(toString(from)) + " + " + toString(event);
            if (event == E::ConfirmDrafts) {
                requireCode(result, "follow_up.confirm_requires_port", name);
            } else if (!table.success()) {
                requireCode(result, "follow_up.invalid_transition", name);
            } else {
                check(result.success() && result.value().state == table.value(), name + " lands where the table says");
                check(sameRequest(stored(env, id), result.value()), name + ": the stored request is the one returned");
                continue;
            }
            check(sameRequest(stored(env, id), before), name + " is refused and writes nothing");
        }
    }
}

inline void whatEachStepStores(const EnvFactory& make)
{
    const auto env = make();
    auto request = mustInsert(env, quoted(env, "w-1", "run-1", 0));
    request = mustStep(env, "w-1", authorizeStep(request), 1234);
    check(request.authorizedAt == at(1234) && request.updatedAt == at(1234) && !request.claimedAt && !followUpRequestMayBeCharged(request),
        "authorize stamps the authorisation; still free");
    request = mustStep(env, "w-1", plainStep(E::Claim, S::Authorized), 2345);
    check(request.claimedAt == at(2345) && followUpRequestMayBeCharged(request) && !request.finishedAt, "claim stamps the claim; now possibly charged");
    request = mustStep(env, "w-1", planStep(answerOf('d')), 3456);
    check(request.answer == std::optional<FollowUpPlanAnswer>(answerOf('d')) && request.state == S::PlanReady && !request.finishedAt,
        "the plan is stored byte for byte");
    check(sameRequest(stored(env, "w-1"), request), "...and reads back the same");

    mustInsert(env, quoted(env, "w-2", "run-2", 0));
    auto second = stored(env, "w-2");
    second = mustStep(env, "w-2", authorizeStep(second), 1000);
    mustStep(env, "w-2", plainStep(E::Claim, S::Authorized), 2000);
    const auto failed = mustStep(env, "w-2", failStep({"follow_up.invalid_plan", "No envelope was found", std::string("the raw reply")}), 3000);
    check(failed.state == S::Failed && failed.failure->rawReply == std::optional<std::string>("the raw reply") && failed.finishedAt == at(3000),
        "an invalid answer is stored with its reply and the typed error");
    check(sameRequest(stored(env, "w-2"), failed), "...and reads back the same");
    check(failed.failure->message == "No envelope was found" && failed.failure->code == "follow_up.invalid_plan", "code and message");

    // An empty failure message is a message, not a missing one.
    mustInsert(env, quoted(env, "w-3", "run-3", 0));
    mustStep(env, "w-3", authorizeStep(stored(env, "w-3")), 1000);
    mustStep(env, "w-3", plainStep(E::Claim, S::Authorized), 2000);
    const auto bare = mustStep(env, "w-3", failStep({"provider.timeout", "", std::nullopt}), 3000);
    check(stored(env, "w-3").failure == bare.failure && !stored(env, "w-3").failure->rawReply.has_value(), "A failure with no message and no reply");
}

inline void aStepIsACompareAndSet(const EnvFactory& make)
{
    const auto env = make();
    const auto request = mustInsert(env, quoted(env, "c-1", "run-1", 0));
    requireCode(env.repository->transition("c-1", plainStep(E::Cancel, S::Authorized), at(100)), "follow_up.state_changed", "the caller believed another state");
    auto wrong = authorizeStep(request);
    wrong.contextFingerprint = hex64('9');
    requireCode(env.repository->transition("c-1", wrong, at(100)), "follow_up.context_changed", "authorising another context");
    check(sameRequest(stored(env, "c-1"), request), "A refused step writes nothing");
    requireCode(env.repository->transition("missing", authorizeStep(request), at(100)), "follow_up.not_found", "an unknown request");

    mustStep(env, "c-1", authorizeStep(request), 100);
    requireCode(env.repository->transition("c-1", authorizeStep(request), at(200)), "follow_up.state_changed", "authorising twice");
    mustStep(env, "c-1", plainStep(E::Claim, S::Authorized), 300);
    requireCode(env.repository->transition("c-1", plainStep(E::Claim, S::Authorized), at(400)), "follow_up.state_changed", "claiming twice");

    // A race for the claim: exactly one winner, and the loser learns it lost.
    mustInsert(env, quoted(env, "c-2", "run-2", 0));
    mustStep(env, "c-2", authorizeStep(stored(env, "c-2")), 100);
    std::atomic<int> winners {0};
    std::atomic<int> losers {0};
    std::vector<std::thread> threads;
    for (int index = 0; index < 8; ++index) {
        threads.emplace_back([&] {
            const auto result = env.repository->transition("c-2", plainStep(E::Claim, S::Authorized), at(500));
            if (result.success()) ++winners;
            else if (result.error()->code == "follow_up.state_changed") ++losers;
        });
    }
    for (auto& thread : threads) thread.join();
    check(winners == 1 && losers == 7, "Of eight racing claims exactly one wins (winners " + std::to_string(winners) + ")");
    check(stored(env, "c-2").state == S::Composing && stored(env, "c-2").claimedAt == at(500), "...and the request was claimed once");
}

inline void theDeadlineIsOneAndTheClockNeverRunsBackwards(const EnvFactory& make)
{
    const auto env = make();
    const auto request = mustInsert(env, quoted(env, "d-1", "run-1", 0));
    requireCode(env.repository->transition("d-1", authorizeStep(request), at(kQuoteLifetimeMs)), "follow_up.quote_expired", "authorising at the deadline");
    check(stored(env, "d-1").state == S::Quoted, "A refused authorisation writes nothing");
    requireCode(env.repository->transition("d-1", plainStep(E::Expire, S::Quoted), at(kQuoteLifetimeMs - 1)), "follow_up.not_expired", "expiring early");
    mustStep(env, "d-1", authorizeStep(request), kQuoteLifetimeMs - 1);
    requireCode(env.repository->transition("d-1", plainStep(E::Claim, S::Authorized), at(kQuoteLifetimeMs + 1)), "follow_up.quote_expired", "claiming after the deadline");
    const auto expired = mustStep(env, "d-1", plainStep(E::Expire, S::Authorized), kQuoteLifetimeMs + 1);
    check(expired.state == S::Expired && !followUpRequestMayBeCharged(expired) && expired.authorizedAt.has_value(),
        "An authorised request that was never claimed expires for free");

    mustInsert(env, quoted(env, "d-2", "run-2", 0));
    mustStep(env, "d-2", authorizeStep(stored(env, "d-2")), 5000);
    const auto claimed = mustStep(env, "d-2", plainStep(E::Claim, S::Authorized), 1000);   // the clock went back
    check(claimed.claimedAt == at(5000) && claimed.updatedAt == at(5000), "A clock that went backwards never moves a request back in time");
}

inline void freeAndChargedFollowTheClaim(const EnvFactory& make)
{
    const auto env = make();
    struct Row {
        S state;
        bool charged;
    };
    for (const Row row : {Row {S::Quoted, false}, Row {S::Authorized, false}, Row {S::Composing, true}, Row {S::PlanReady, true},
             Row {S::DraftsSaved, true}, Row {S::Stale, false}, Row {S::Expired, false}, Row {S::Cancelled, false},
             Row {S::Discarded, false}, Row {S::Interrupted, true}, Row {S::Failed, true}}) {
        const auto request = drive(env, std::string("f-") + toString(row.state), std::string("run-f-") + toString(row.state), row.state);
        check(followUpRequestMayBeCharged(request) == row.charged && followUpRequestMayBeCharged(stored(env, request.id)) == row.charged,
            std::string("'") + toString(row.state) + "' is " + (row.charged ? "possibly charged" : "free"));
    }
    // The states that exist on both sides of the claim.
    const auto cancelledInCall = mustStep(env, drive(env, "f-cc", "run-f-cc", S::Composing).id, plainStep(E::Cancel, S::Composing), 3000);
    check(cancelledInCall.state == S::Cancelled && followUpRequestMayBeCharged(cancelledInCall), "A cancel during the call is possibly charged");
    const auto cancelledAuthorized = mustStep(env, drive(env, "f-ca", "run-f-ca", S::Authorized).id, plainStep(E::Cancel, S::Authorized), 3000);
    check(cancelledAuthorized.state == S::Cancelled && !followUpRequestMayBeCharged(cancelledAuthorized), "A cancel before the claim is free");
    const auto discardedPlan = mustStep(env, drive(env, "f-dp", "run-f-dp", S::PlanReady).id, plainStep(E::Discard, S::PlanReady), 4000);
    check(discardedPlan.state == S::Discarded && followUpRequestMayBeCharged(discardedPlan) && discardedPlan.answer == std::optional<FollowUpPlanAnswer>(answerOf())
            && sameRequest(stored(env, "f-dp"), discardedPlan),
        "A discarded ready plan paid for its call: it is charged and keeps its answer");
    const auto staleAuthorized = mustStep(env, drive(env, "f-sa", "run-f-sa", S::Authorized).id, plainStep(E::MarkStale, S::Authorized), 3000);
    check(staleAuthorized.state == S::Stale && !followUpRequestMayBeCharged(staleAuthorized) && staleAuthorized.authorizedAt.has_value(),
        "A drift found after the authorisation but before the claim is free");
}

// The paging protocol of the startup recovery: a cursor (the key of the last row of the previous page) makes the next page start
// STRICTLY after it, so a walk makes progress whether or not the rows leave the state, ties in the creation time are ordered by id, and
// there is no ceiling on the number of rows.
inline void listByStateAfterWalksEveryRowOnce(const EnvFactory& make)
{
    const auto env = make();
    // Eleven requests: three share a creation time (ties), the rest are spread. Ids are chosen so that id order differs from time order.
    const std::vector<std::pair<std::string, std::int64_t>> rows = {{"p-09", 1000}, {"p-03", 1000}, {"p-05", 1000}, {"p-08", 500},
        {"p-01", 2000}, {"p-07", 3000}, {"p-02", 3000}, {"p-10", 4000}, {"p-04", 5000}, {"p-06", 6000}, {"p-00", 7000}};
    int index = 0;
    for (const auto& [id, createdMs] : rows) mustInsert(env, quoted(env, id, "run-p-" + std::to_string(index++), createdMs));
    const auto ids = [](const std::vector<FollowUpPlanRequest>& page) {
        std::vector<std::string> out;
        for (const auto& row : page) out.push_back(row.id);
        return out;
    };
    const std::vector<std::string> expectedOrder = {"p-08", "p-03", "p-05", "p-09", "p-01", "p-02", "p-07", "p-10", "p-04", "p-06", "p-00"};
    check(ids(env.repository->listByStateAfter(S::Quoted, 100, std::nullopt).value()) == expectedOrder,
        "Without a cursor the walk starts at the oldest row, ties by id");
    check(ids(env.repository->listByState(S::Quoted, 100).value()) == expectedOrder, "...exactly as listByState does");

    // A walk page by page, three at a time: every row exactly once, in order.
    std::vector<std::string> walked;
    std::optional<FollowUpStateCursor> cursor;
    for (int guard = 0; guard < 20; ++guard) {
        const auto page = env.repository->listByStateAfter(S::Quoted, 3, cursor);
        check(page.success(), "a page is read");
        if (page.value().empty()) break;
        for (const auto& row : page.value()) walked.push_back(row.id);
        cursor = FollowUpStateCursor {page.value().back().createdAt, page.value().back().id};
    }
    check(walked == expectedOrder, "Pages of three walk every row exactly once");

    // The cursor is a key, not a position: a row between the cursor and the next page that left the state does not break the walk, and
    // a row at the cursor's own creation time with a greater id is NOT skipped.
    const auto afterTie = env.repository->listByStateAfter(S::Quoted, 100, FollowUpStateCursor {at(1000), "p-03"});
    check(ids(afterTie.value()).front() == "p-05" && ids(afterTie.value()).size() == 9, "Strictly after (1000, p-03): the tie row p-05 comes next");
    const auto afterLast = env.repository->listByStateAfter(S::Quoted, 100, FollowUpStateCursor {at(7000), "p-00"});
    check(afterLast.success() && afterLast.value().empty(), "After the last row there is nothing");
    mustStep(env, "p-05", plainStep(E::Cancel, S::Quoted), 1500);
    check(ids(env.repository->listByStateAfter(S::Quoted, 100, FollowUpStateCursor {at(1000), "p-03"}).value()).front() == "p-09",
        "A row that left the state is not returned, and the walk goes on past it");
    check(ids(env.repository->listByStateAfter(S::Cancelled, 100, std::nullopt).value()) == (std::vector<std::string> {"p-05"}),
        "Another state has its own walk");
    requireCode(env.repository->listByStateAfter(S::Quoted, 0, std::nullopt), "follow_up.invalid_request", "limit 0");
    requireCode(env.repository->listByStateAfter(S::Quoted, kFollowUpMaxListLimit + 1, std::nullopt), "follow_up.invalid_request", "limit 501");
}

inline void readsAreOrderedAndBounded(const EnvFactory& make)
{
    const auto env = make();
    mustInsert(env, quoted(env, "r-a", "run-a", 1000));
    mustInsert(env, quoted(env, "r-b", "run-b", 3000));
    mustInsert(env, quoted(env, "r-c", "run-c", 3000));
    mustInsert(env, quoted(env, "r-d", "run-d", 2000));
    auto ids = [](const std::vector<FollowUpPlanRequest>& rows) {
        std::vector<std::string> out;
        for (const auto& row : rows) out.push_back(row.id);
        return out;
    };
    check(ids(env.repository->listForProject(env.projectId, 10).value()) == (std::vector<std::string> {"r-c", "r-b", "r-d", "r-a"}),
        "listForProject: newest first, ties by the greater id");
    check(ids(env.repository->listForProject(env.projectId, 2).value()) == (std::vector<std::string> {"r-c", "r-b"}), "...and bounded");
    check(ids(env.repository->listByState(S::Quoted, 10).value()) == (std::vector<std::string> {"r-a", "r-d", "r-b", "r-c"}),
        "listByState: oldest first, ties by the smaller id");
    check(ids(env.repository->listByState(S::Quoted, 3).value()) == (std::vector<std::string> {"r-a", "r-d", "r-b"}), "...and bounded");
    requireCode(env.repository->listForProject(env.projectId, 0), "follow_up.invalid_request", "limit 0");
    requireCode(env.repository->listForProject(env.projectId, kFollowUpMaxListLimit + 1), "follow_up.invalid_request", "limit 501");
    requireCode(env.repository->listByState(S::Quoted, 0), "follow_up.invalid_request", "limit 0 by state");
    requireCode(env.repository->listByState(S::Quoted, kFollowUpMaxListLimit + 1), "follow_up.invalid_request", "limit 501 by state");
    check(env.repository->listForProject(env.projectId, 1).success() && env.repository->listForProject(env.projectId, kFollowUpMaxListLimit).success(),
        "limits 1 and 500 are fine");
    mustStep(env, "r-a", authorizeStep(stored(env, "r-a")), 1500);
    check(ids(env.repository->listByState(S::Authorized, 10).value()) == (std::vector<std::string> {"r-a"}), "A request moves between state lists");
    check(env.repository->findOpenForSourceRun("run-a").value()->id == "r-a" && env.repository->findOpenForSourceRun("run-a").value()->state == S::Authorized,
        "...and findOpen follows it");
}

inline void aRunThatLeftItsProjectIsNeverRead(const EnvFactory& make)
{
    const auto env = make();
    const auto request = mustInsert(env, quoted(env, "m-1", "run-1", 0));
    const auto isInvalid = [](const auto& result) { return !result.success() && result.error()->code == "persistence.invalid_data"; };
    env.moveRun("run-1", env.otherProjectId);
    check(isInvalid(env.repository->findById("m-1")), "findById fails closed");
    check(isInvalid(env.repository->findOpenForSourceRun("run-1")), "findOpen fails closed");
    check(isInvalid(env.repository->findLatestForSourceRun("run-1")), "findLatest fails closed");
    check(isInvalid(env.repository->listForProject(env.projectId, 10)), "the list of the project the request NAMES fails closed");
    check(isInvalid(env.repository->listForProject(env.otherProjectId, 10)), "the list of the project the Run BELONGS to fails closed");
    check(isInvalid(env.repository->listByState(S::Quoted, 10)), "listByState fails closed");
    check(isInvalid(env.repository->transition("m-1", authorizeStep(request), at(100))), "no step is built on it");
    env.moveRun("run-1", env.projectId);
    check(env.repository->findById("m-1").value().has_value() && stored(env, "m-1").state == S::Quoted,
        "...and it read nothing and wrote nothing: with the Run back, the request is as it was");
}

// ---- Confirm ------------------------------------------------------------------------------------------------------------------

inline void confirmSavesTheGroupTheDraftsAndTheStateTogether(const EnvFactory& make)
{
    const auto env = make();
    const auto ready = drive(env, "k-1", "run-1", S::PlanReady);
    const auto result = mustConfirm(env, ready, 4000);
    check(result.created && result.request.state == S::DraftsSaved && result.request.draftGroupId == std::optional<std::string>("group-k-1")
            && result.request.finishedAt == at(4000) && result.request.answer == ready.answer && followUpRequestMayBeCharged(result.request),
        "A confirmed request is drafts_saved, linked to its group, finished and still charged");
    check(sameRequest(stored(env, "k-1"), result.request), "...and that is what is stored");

    const auto group = env.drafts->findGroupById("group-k-1");
    check(group.success() && group.value().has_value() && rundraft_contract::sameGroup(*group.value(), result.group), "The group is stored as returned");
    check(group.value()->origin == RunDraftGroupOrigin::FollowUpRequest && group.value()->sourceRequestId == std::optional<std::string>("k-1")
            && group.value()->contextFingerprint == std::optional<std::string>(ready.quote.contextFingerprint)
            && group.value()->planFingerprint == ready.answer->planFingerprint && group.value()->sourceRunIds == std::vector<std::string> {"run-1"},
        "...with the request as its origin, its sealed context, its plan and its source Run");
    const auto drafts = env.drafts->listGroupDrafts("group-k-1");
    check(drafts.success() && drafts.value().size() == 2 && result.drafts.size() == 2, "Both drafts are stored");
    for (std::size_t index = 0; index < 2; ++index) {
        check(rundraft_contract::sameDraft(drafts.value()[index], result.drafts[index]) && drafts.value()[index].provenance->ordinal == index,
            "...in ordinal order, as returned");
    }
    check(!env.repository->findOpenForSourceRun("run-1").value().has_value(), "The slot is free again");
    check(env.repository->insertQuoted(quoted(env, "k-2", "run-1", 10'000)).success(), "...so a new quote can be made");
}

inline void aSecondConfirmReturnsTheSameGroupAndWritesNothing(const EnvFactory& make)
{
    const auto env = make();
    const auto ready = drive(env, "k-1", "run-1", S::PlanReady);
    const auto first = mustConfirm(env, ready, 4000);
    const auto drafts = draftCount(env);
    const auto savedRequest = stored(env, "k-1");

    // Same plan, a freshly built candidate group with other ids: it is not looked at.
    const std::string otherGroup = "group-again";
    auto candidate = groupFor(ready, otherGroup, 'f');
    const auto again = env.confirm->confirmDrafts("k-1", ready.answer->planFingerprint, candidate, draftsFor(env, otherGroup, "again"), at(9000));
    check(again.success() && !again.value().created && rundraft_contract::sameGroup(again.value().group, first.group), "The same group comes back");
    check(again.value().drafts.size() == 2 && rundraft_contract::sameDraft(again.value().drafts[0], first.drafts[0])
            && rundraft_contract::sameDraft(again.value().drafts[1], first.drafts[1]),
        "...with the same drafts");
    check(sameRequest(again.value().request, savedRequest) && sameRequest(stored(env, "k-1"), savedRequest),
        "...and the request is not touched (its update time is the first confirm's)");
    check(draftCount(env) == drafts && !env.drafts->findGroupById(otherGroup).value().has_value(), "Nothing was written");

    // A junk candidate does not matter either; another plan does.
    const auto junk = env.confirm->confirmDrafts("k-1", ready.answer->planFingerprint, RunDraftGroup {}, {}, at(9000));
    check(junk.success() && !junk.value().created, "A repeat does not even need a usable candidate");
    requireCode(env.confirm->confirmDrafts("k-1", hex64('9'), candidate, draftsFor(env, otherGroup, "again"), at(9000)),
        "follow_up.plan_changed", "the same request, another plan");
}

inline void confirmRefusesAnythingButTheReadyPlan(const EnvFactory& make)
{
    const auto env = make();
    int index = 0;
    for (const S state : allStates()) {
        if (state == S::PlanReady || state == S::DraftsSaved) continue;
        const std::string id = "n-" + std::to_string(index);
        const auto request = drive(env, id, "run-n-" + std::to_string(index), state);
        ++index;
        const std::string groupId = "group-" + id;
        requireCode(env.confirm->confirmDrafts(id, hex64('c'), groupFor(request, groupId, 'e'), draftsFor(env, groupId, id), at(9000)),
            "follow_up.not_plan_ready", std::string("a '") + toString(state) + "' request");
        check(!env.drafts->findGroupById(groupId).value().has_value() && sameRequest(stored(env, id), request),
            "...and nothing was written");
    }
    requireCode(env.confirm->confirmDrafts("nope", hex64('c'), RunDraftGroup {}, {}, at(9000)), "follow_up.not_found", "an unknown request");
}

inline void confirmRefusesAGroupThatIsNotTheRequestsOwn(const EnvFactory& make)
{
    const auto env = make();
    const auto ready = drive(env, "k-1", "run-1", S::PlanReady);
    const std::vector<std::pair<const char*, std::function<void(RunDraftGroup&)>>> mismatches = {
        {"another origin", [](auto& g) { g.origin = RunDraftGroupOrigin::ManualImport; g.sourceRequestId.reset(); g.contextFingerprint.reset(); }},
        {"another request", [](auto& g) { g.sourceRequestId = "k-other"; }},
        {"another project", [&](auto& g) { g.projectId = env.otherProjectId; }},
        {"another sealed context", [](auto& g) { g.contextFingerprint = hex64('9'); }},
        {"another plan", [](auto& g) { g.planFingerprint = hex64('9'); }},
        {"another source Run", [](auto& g) { g.sourceRunIds = {"run-elsewhere"}; }}};
    for (const auto& [what, edit] : mismatches) {
        auto group = groupFor(ready, "group-k-1", 'e');
        edit(group);
        requireCode(env.confirm->confirmDrafts("k-1", ready.answer->planFingerprint, group, draftsFor(env, "group-k-1", "k"), at(9000)),
            "follow_up.group_mismatch", what);
        check(sameRequest(stored(env, "k-1"), ready) && draftCount(env) == 0 && !env.drafts->findGroupById("group-k-1").value().has_value(),
            std::string(what) + ": the request is still plan_ready and nothing was written");
    }
}

// Further source Runs are trusted provenance: each must exist and belong to the request's project, or the Confirm writes nothing.
inline void confirmChecksEverySourceRunOfTheGroup(const EnvFactory& make)
{
    const auto env = make();
    const auto ready = drive(env, "k-1", "run-1", S::PlanReady);
    if (env.ensureRun) {
        env.ensureRun("advisory-run", env.projectId);
        env.ensureRun("foreign-run", env.otherProjectId);
    }
    const auto confirmWith = [&](std::vector<std::string> runs) {
        auto group = groupFor(ready, "group-k-1", 'e');
        group.sourceRunIds = std::move(runs);
        return env.confirm->confirmDrafts("k-1", ready.answer->planFingerprint, group, draftsFor(env, "group-k-1", "k", &ready), at(9000));
    };
    const auto untouched = [&](const char* what) {
        check(sameRequest(stored(env, "k-1"), ready) && draftCount(env) == 0 && !env.drafts->findGroupById("group-k-1").value().has_value(),
            std::string(what) + ": the request is still plan_ready and nothing was written");
    };
    requireCode(confirmWith({"ghost-run", "run-1"}), "follow_up.source_run_not_found", "a source Run that does not exist");
    untouched("a ghost source Run");
    requireCode(confirmWith({"run-1", "foreign-run"}), "follow_up.source_run_wrong_project", "a source Run of another project");
    untouched("another project's source Run");
    // The group's own validation comes first - in both adapters, in the same order - then the Run lookups.
    std::vector<std::string> many = {"run-1"};
    for (std::size_t index = 0; index < kRunDraftGroupMaxSourceRuns; ++index) many.push_back("advisory-run");
    requireCode(confirmWith(many), "run_draft_group.invalid_group", "More source Runs than a group may name");
    untouched("too many source Runs");
    requireCode(confirmWith({"run-1", "ghost-run", "ghost-run"}), "run_draft_group.invalid_group", "A source Run named twice (before the ghost is looked up)");
    untouched("a source Run named twice");

    // An existing Run of the project may be added; the request's own Run stays one of them.
    const auto saved = confirmWith({"advisory-run", "run-1"});
    check(saved.success() && saved.value().group.sourceRunIds == std::vector<std::string>({"advisory-run", "run-1"}),
        "A group may name further source Runs that exist in the project");
}

// P1: the Drafts to save ARE the stored plan, not merely valid Drafts under its fingerprint.
inline void confirmRefusesDraftsThatAreNotTheStoredPlan(const EnvFactory& make)
{
    const auto env = make();
    const auto ready = drive(env, "k-1", "run-1", S::PlanReady);
    const std::string groupId = "group-k-1";
    const auto confirmWith = [&](const std::function<void(RunDraftGroup&, std::vector<RunDraft>&)>& edit) {
        auto group = groupFor(ready, groupId, 'e');
        auto drafts = draftsFor(env, groupId, "k", &ready);
        edit(group, drafts);
        return env.confirm->confirmDrafts("k-1", ready.answer->planFingerprint, group, drafts, at(9000));
    };
    const auto sealed = [](RunDraft& draft) { rundraft_contract::reseal(draft); };
    using Drafts = std::vector<RunDraft>;
    const std::vector<std::pair<const char*, std::function<void(RunDraftGroup&, Drafts&)>>> wrong = {
        {"another group title", [](auto& g, auto&) { g.planTitle = "Another title"; }},
        {"one draft too many", [&](auto&, auto& d) { d.push_back(rundraft_contract::groupDraft(draftEnvOf(env), "group-k-1", "k-x", 2, "k2", {"k1"})); }},
        {"one draft too few", [](auto&, auto& d) { d.pop_back(); }},
        {"another title", [&](auto&, auto& d) { d[0].title = "Different"; d[0].name = "Different"; sealed(d[0]); }},
        {"a name that is not the title", [](auto&, auto& d) { d[0].name = "Different"; }},
        {"another task", [&](auto&, auto& d) { d[1].task = "Do something else"; sealed(d[1]); }},
        {"another plan key", [&](auto&, auto& d) { d[0].provenance->planKey = "renamed"; sealed(d[0]); }},
        {"other dependencies", [&](auto&, auto& d) { d[1].provenance->dependsOn = {}; sealed(d[1]); }},
        {"an extra contract", [&](auto&, auto& d) {
             RunInterfaceContract contract;
             contract.id = "X1";
             contract.title = "Extra";
             contract.text = "Be extra.";
             d[0].interfaceContracts.push_back(contract);
             sealed(d[0]);
         }},
        {"a content fingerprint of other content", [](auto&, auto& d) { d[0].provenance->createdContentFingerprint = hex64('9'); }},
        {"a draft of another workflow", [](auto&, auto& d) { d[0].workflowId = "mock-workflow"; }},
        {"a draft that is not a per-Run roster draft", [](auto&, auto& d) { d[0].launchMode = RunLaunchMode::Mock; }},
        {"a draft of another group", [](auto&, auto& d) { d[0].provenance->groupId = "elsewhere"; }},
        {"a draft of another project", [&](auto&, auto& d) { d[0].projectId = env.otherProjectId; }},
        {"the ordinals swapped onto the wrong drafts", [](auto&, auto& d) { std::swap(d[0].provenance->ordinal, d[1].provenance->ordinal); }},
        {"an ordinal taken twice", [](auto&, auto& d) { d[1].provenance->ordinal = 0; }},
        {"a draft without provenance", [](auto&, auto& d) { d[0].provenance.reset(); }},
        {"the valid drafts of ANOTHER plan under the stored fingerprint", [&](auto&, auto& d) {
             auto otherPlan = ready;
             otherPlan.answer = answerOf('d');
             d = draftsFor(env, groupId, "k", &otherPlan);
         }},
    };
    for (const auto& [what, edit] : wrong) {
        requireCode(confirmWith(edit), "follow_up.drafts_not_the_plan", what);
        check(sameRequest(stored(env, "k-1"), ready) && draftCount(env) == 0 && !env.drafts->findGroupById(groupId).value().has_value(),
            std::string(what) + ": the request is still plan_ready and nothing was written");
    }
    // Drafts may arrive in any order: the ordinals, not the position, say which is which.
    const auto shuffled = confirmWith([](auto&, auto& d) { std::swap(d[0], d[1]); });
    check(shuffled.success() && shuffled.value().drafts[0].provenance->ordinal == 0 && shuffled.value().drafts[1].provenance->ordinal == 1,
        "The genuine drafts are accepted in any order and come back in ordinal order");
}

// P1: a reply that is not one valid plan of the authorised size never becomes a stored plan.
inline void anAnswerThatIsNotTheAuthorisedPlanIsNeverStored(const EnvFactory& make)
{
    const auto env = make();
    const auto composing = drive(env, "k-1", "run-1", S::Composing);
    const auto good = answerOf();
    const auto refuse = [&](const char* what, FollowUpPlanAnswer answer, std::string_view code) {
        requireCode(env.repository->transition("k-1", planStep(std::move(answer)), at(3000)), code, what);
        check(sameRequest(stored(env, "k-1"), composing), std::string(what) + ": the request is still composing");
    };
    auto noEnvelope = good;
    noEnvelope.rawReply = "I would split this in two, but here is no plan.";
    refuse("a reply without an envelope", noEnvelope, "next_run_plan.envelope_missing");
    auto emptyPlan = good;
    emptyPlan.rawReply = replyOfPlanJson("{}");
    refuse("an envelope that holds no plan", emptyPlan, "next_run_plan.schema_version");
    auto twice = good;
    twice.rawReply = good.rawReply + good.rawReply;
    refuse("two envelopes", twice, "next_run_plan.envelope_multiple");
    auto foreign = good;
    foreign.planFingerprint = answerOf('d').planFingerprint;
    refuse("the fingerprint of another plan", foreign, "follow_up.answer_plan_mismatch");
    refuse("three drafts where two were authorised", answerOf('c', 3), "follow_up.plan_draft_count_not_authorised");
    check(env.repository->transition("k-1", planStep(good), at(3000)).success(), "...while the genuine answer is accepted");
}

inline void aConfirmThatCannotSaveTheGroupLeavesEverythingAsItWas(const EnvFactory& make)
{
    const auto env = make();
    const auto ready = drive(env, "k-1", "run-1", S::PlanReady);
    const std::string groupId = "group-k-1";
    const auto untouched = [&](const char* what) {
        check(sameRequest(stored(env, "k-1"), ready), std::string(what) + ": the request is still the ready plan");
        check(!env.drafts->findGroupById(groupId).value().has_value() && env.drafts->listGroupDrafts(groupId).value().empty(),
            std::string(what) + ": no group and no draft was left behind");
    };

    // Drafts that are not the plan (the ordinals of a group are exactly 0..n-1; a plan has at least one draft).
    auto gappy = draftsFor(env, groupId, "k");
    gappy[1].provenance->ordinal = 2;
    requireCode(env.confirm->confirmDrafts("k-1", ready.answer->planFingerprint, groupFor(ready, groupId, 'e'), gappy, at(9000)),
        "follow_up.drafts_not_the_plan", "ordinals 0 and 2");
    untouched("invalid drafts");

    requireCode(env.confirm->confirmDrafts("k-1", ready.answer->planFingerprint, groupFor(ready, groupId, 'e'), {}, at(9000)),
        "follow_up.drafts_not_the_plan", "no drafts at all");
    untouched("no drafts");

    // An idempotency key that another group already holds.
    const auto draftEnv = draftEnvOf(env);
    auto manual = rundraft_contract::makeGroup(draftEnv, "manual-group", 'a');
    check(env.drafts->insertGroup(manual, {rundraft_contract::groupDraft(draftEnv, "manual-group", "manual-draft", 0, "only")}).success(),
        "setup: another group holds the key");
    const auto before = draftCount(env);
    auto sameKey = groupFor(ready, groupId, 'a');
    requireCode(env.confirm->confirmDrafts("k-1", ready.answer->planFingerprint, sameKey, draftsFor(env, groupId, "k"), at(9000)),
        "run_draft_group.duplicate_idempotency_key", "a key that is already used");
    untouched("a used key");
    check(draftCount(env) == before, "...and no draft was added to the other group's store");

    // A draft id that already exists: the group row must not stay behind.
    auto clashing = draftsFor(env, groupId, "k");
    clashing[1].id = "manual-draft";
    requireCode(env.confirm->confirmDrafts("k-1", ready.answer->planFingerprint, groupFor(ready, groupId, 'e'), clashing, at(9000)),
        "run_draft.duplicate_id", "a draft id that is taken");
    untouched("a taken draft id");

    // A group id that is taken.
    requireCode(env.confirm->confirmDrafts("k-1", ready.answer->planFingerprint, groupFor(ready, "manual-group", 'e'),
                    draftsFor(env, "manual-group", "k"), at(9000)),
        "run_draft_group.duplicate_id", "a group id that is taken");
    check(sameRequest(stored(env, "k-1"), ready), "...and the request is still the ready plan");

    // The refusals did not spoil anything: the same request now confirms.
    check(env.confirm->confirmDrafts("k-1", ready.answer->planFingerprint, groupFor(ready, groupId, 'e'), draftsFor(env, groupId, "k"), at(9000)).success(),
        "After every refusal the plan can still be confirmed");
}

inline void theDraftsOfASavedPlanCanBeRemovedButNeverSilentlyRecreated(const EnvFactory& make)
{
    const auto env = make();
    const auto ready = drive(env, "k-1", "run-1", S::PlanReady);
    const auto saved = mustConfirm(env, ready, 4000);
    for (const auto& draft : saved.drafts) check(env.drafts->remove(draft.id).success(), "setup: the user removes a draft");
    check(!env.drafts->findGroupById(saved.group.id).value().has_value(), "setup: the last draft took its group with it");

    requireCode(env.confirm->confirmDrafts("k-1", ready.answer->planFingerprint, groupFor(ready, "group-new", 'f'), draftsFor(env, "group-new", "new"), at(9000)),
        "follow_up.drafts_removed", "confirming after the drafts were removed");
    check(stored(env, "k-1").state == S::DraftsSaved && draftCount(env) == 0 && !env.drafts->findGroupById("group-new").value().has_value(),
        "Nothing was created again, and the request is still a record that its plan was saved");

    // Another group may later take the id of the removed one (the product never reuses ids, but nothing stops a writer): it is
    // not THIS request's group, and the Confirm must not hand it out as such.
    const auto draftEnv = draftEnvOf(env);
    auto squatter = rundraft_contract::makeGroup(draftEnv, saved.group.id, 'd');
    check(env.drafts->insertGroup(squatter, {rundraft_contract::groupDraft(draftEnv, saved.group.id, "squatter-draft", 0, "only")}).success(),
        "setup: a manual group takes the removed group's id");
    requireCode(env.confirm->confirmDrafts("k-1", ready.answer->planFingerprint, groupFor(ready, "group-new", 'f'), draftsFor(env, "group-new", "new"), at(9000)),
        "follow_up.drafts_removed", "a group with the same id that belongs to no request");
    check(env.drafts->findGroupById(saved.group.id).value()->origin == RunDraftGroupOrigin::ManualImport, "...and that group is untouched");
}

inline void concurrentConfirmsSaveOneGroup(const EnvFactory& make)
{
    const auto env = make();
    const auto ready = drive(env, "k-1", "run-1", S::PlanReady);
    std::atomic<int> created {0};
    std::atomic<int> returned {0};
    std::atomic<int> failed {0};
    std::mutex mutex;
    std::set<std::string> groups;
    std::vector<std::thread> threads;
    for (int index = 0; index < 6; ++index) {
        threads.emplace_back([&, index] {
            const std::string suffix = "-" + std::to_string(index);
            const std::string groupId = "group-race" + suffix;
            auto group = groupFor(ready, groupId, 'e');
            group.idempotencyKey = keyDigitFor("race" + suffix);
            const auto result = env.confirm->confirmDrafts("k-1", ready.answer->planFingerprint, group,
                draftsFor(env, groupId, "race" + suffix), at(9000));
            if (!result.success()) {
                ++failed;
                return;
            }
            (result.value().created ? created : returned)++;
            std::lock_guard lock(mutex);
            groups.insert(result.value().group.id);
        });
    }
    for (auto& thread : threads) thread.join();
    check(failed == 0 && created == 1 && returned == 5, "Of six racing confirms one saves, five get the same group back");
    check(groups.size() == 1 && draftCount(env) == 2, "...and exactly one group with two drafts exists");
    check(stored(env, "k-1").state == S::DraftsSaved, "...and the request is saved once");
}

inline void thePublicDraftPortCannotMakeAFollowUpGroup(const EnvFactory& make)
{
    const auto env = make();
    const auto ready = drive(env, "k-1", "run-1", S::PlanReady);
    requireCode(env.drafts->insertGroup(groupFor(ready, "group-k-1", 'e'), draftsFor(env, "group-k-1", "k")),
        "run_draft_group.follow_up_requires_confirm", "insertGroup of a follow-up group");
    check(draftCount(env) == 0 && !env.drafts->findGroupById("group-k-1").value().has_value() && stored(env, "k-1").state == S::PlanReady,
        "Nothing was written: only the Confirm of a request makes such a group");
    // The other origins keep working through the public port.
    auto manual = rundraft_contract::makeGroup(draftEnvOf(env), "manual-group", 'd');
    check(env.drafts->insertGroup(manual, {rundraft_contract::groupDraft(draftEnvOf(env), "manual-group", "manual-draft", 0, "only")}).success(),
        "A manual group is saved as before");
}

inline void runFollowUpPlanRequestRepositoryContract(const EnvFactory& make)
{
    insertAndReadBack(make);
    theLatestRequestOfARunIsTheNewestInAnyState(make);
    theGateRunsBeforeStorage(make);
    theRunAuthorityIsCheckedOnWrite(make);
    aDuplicateIdIsRefusedAndHarmless(make);
    oneOpenRequestPerSourceRun(make);
    transitionsFollowTheAgreedTableAndOnlyThat(make);
    whatEachStepStores(make);
    aStepIsACompareAndSet(make);
    theDeadlineIsOneAndTheClockNeverRunsBackwards(make);
    freeAndChargedFollowTheClaim(make);
    readsAreOrderedAndBounded(make);
    listByStateAfterWalksEveryRowOnce(make);
    aRunThatLeftItsProjectIsNeverRead(make);
    confirmSavesTheGroupTheDraftsAndTheStateTogether(make);
    aSecondConfirmReturnsTheSameGroupAndWritesNothing(make);
    confirmRefusesAnythingButTheReadyPlan(make);
    confirmRefusesAGroupThatIsNotTheRequestsOwn(make);
    confirmChecksEverySourceRunOfTheGroup(make);
    confirmRefusesDraftsThatAreNotTheStoredPlan(make);
    anAnswerThatIsNotTheAuthorisedPlanIsNeverStored(make);
    aConfirmThatCannotSaveTheGroupLeavesEverythingAsItWas(make);
    theDraftsOfASavedPlanCanBeRemovedButNeverSilentlyRecreated(make);
    concurrentConfirmsSaveOneGroup(make);
    thePublicDraftPortCannotMakeAFollowUpGroup(make);
}

} // namespace followup_contract
