#include "ai/application/ProviderExecutionBudget.hpp"
#include "ai/application/RunEvent.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <limits>
#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using namespace ai::application;

ai::models::TokenUsage usage(std::uint64_t input, std::uint64_t output)
{
    return {input, output};
}

ai::models::TokenUsage usageWithCache(
    std::uint64_t input, std::uint64_t output,
    std::uint64_t cacheCreation, std::uint64_t cacheRead)
{
    ai::models::TokenUsage value;
    value.inputTokens = input;
    value.outputTokens = output;
    value.cacheCreationInputTokens = cacheCreation;
    value.cacheReadInputTokens = cacheRead;
    return value;
}

ai::models::TokenUsage usageWithDuration(
    std::uint64_t input, std::uint64_t output,
    std::uint64_t cacheCreation, std::uint64_t cacheRead,
    std::uint64_t fiveMinute, std::uint64_t oneHour)
{
    auto value = usageWithCache(input, output, cacheCreation, cacheRead);
    value.cacheCreationDuration = {true, fiveMinute, oneHour};
    return value;
}

void testProfileOverridesCanOnlyLowerHardCeilings()
{
    ProviderExecutionBudgetOverrides overrides;
    overrides.maxInputTokens = 1;
    overrides.maxTurns = 12;
    const auto resolved = resolveProviderExecutionBudget(overrides);
    assert(resolved.success());
    assert(resolved.value().maxInputTokens == 1);
    assert(resolved.value().maxTurns == 12);

    overrides.maxTurns = kProviderBudgetHardMaxTurns + 1;
    const auto rejected = resolveProviderExecutionBudget(overrides);
    assert(!rejected.success());
    assert(rejected.error()->code == "provider_budget.invalid_policy");
}

void testAttemptedThirteenthTurnIsNeverAdmitted()
{
    ProviderExecutionBudgetPolicy policy;
    policy.maxTurns = 12;
    ProviderSessionBudgetGuard guard(policy);
    for (int i = 0; i < 12; ++i) {
        assert(guard.beginProviderTurn().success());
        assert(guard.completeProviderTurn(usage(1, 1)).success());
    }
    const auto thirteenth = guard.beginProviderTurn();
    assert(!thirteenth.success());
    assert(thirteenth.error()->code == "provider_budget.turn_limit_reached");
    assert(guard.usage().turnsStarted == 12);
}

void testInputTokensArePrimaryAcrossSessionBoundaries()
{
    ProviderExecutionBudgetPolicy policy;
    policy.maxInputTokens = 10;
    policy.maxTurns = 20;
    ProviderSessionBudgetGuard guard(policy);
    assert(guard.beginProviderTurn().success());
    assert(guard.completeProviderTurn(usage(6, 1)).success());
    // This represents a fresh Planner repair or worker session using the same
    // Run-scoped guard: no counter is reset at the session boundary.
    assert(guard.beginProviderTurn().success());
    const auto crossed = guard.completeProviderTurn(usage(5, 1));
    assert(!crossed.success());
    assert(crossed.error()->code == "provider_budget.input_token_limit_reached");
    assert(guard.usage().turnsStarted == 2);
    assert(guard.usage().inputTokens == 11);
    assert(!guard.beginProviderTurn().success());
}

void testMissingUsageFailsClosedBeforeAnotherRequest()
{
    ProviderSessionBudgetGuard guard(ProviderExecutionBudgetPolicy {});
    assert(guard.beginProviderTurn().success());
    const auto missing = guard.completeProviderTurn({});
    assert(!missing.success());
    assert(missing.error()->code == "provider_budget.usage_unavailable");
    assert(!guard.usage().usageAuthoritative);
    const auto next = guard.beginProviderTurn();
    assert(!next.success());
    assert(next.error()->code == "provider_budget.usage_unavailable");
}

void testToolAndOutputLimitsAreRunWide()
{
    ProviderExecutionBudgetPolicy policy;
    policy.maxOutputTokens = 3;
    policy.maxToolCalls = 2;
    policy.maxToolResultBytes = 5;
    ProviderSessionBudgetGuard outputGuard(policy);
    assert(outputGuard.beginProviderTurn().success());
    const auto output = outputGuard.completeProviderTurn(usage(1, 4));
    assert(!output.success());
    assert(output.error()->code == "provider_budget.output_token_limit_reached");

    ProviderSessionBudgetGuard toolGuard(policy);
    assert(toolGuard.recordToolActivity(1, 3).success());
    const auto bytes = toolGuard.recordToolActivity(1, 3);
    assert(!bytes.success());
    assert(bytes.error()->code == "provider_budget.tool_result_bytes_limit_reached");
}

void testWallAndRequestTimeAreBoundedDeterministically()
{
    auto now = std::chrono::steady_clock::time_point {};
    ProviderExecutionBudgetPolicy policy;
    policy.maxWallTimeMs = 100;
    policy.maxRequestTimeMs = 25;
    ProviderSessionBudgetGuard guard(policy, {}, {}, [&now] { return now; });
    assert(guard.boundedRequestTimeout(std::chrono::milliseconds(400))
        == std::chrono::milliseconds(25));
    now += std::chrono::milliseconds(100);
    const auto expired = guard.beginProviderTurn();
    assert(!expired.success());
    assert(expired.error()->code == "provider_budget.wall_time_limit_reached");
}

void testEveryMutationIsPersistedAndPersistenceFailureStopsExecution()
{
    std::vector<ProviderExecutionBudgetUsage> snapshots;
    ProviderSessionBudgetGuard guard(
        ProviderExecutionBudgetPolicy {}, {},
        [&snapshots](const ProviderExecutionBudgetUsage& value) {
            snapshots.push_back(value);
            return ai::common::Result<void>::ok();
        });
    assert(guard.beginProviderTurn().success());
    assert(guard.completeProviderTurn(usage(2, 1)).success());
    assert(guard.recordToolActivity(1, 4).success());
    assert(snapshots.size() == 3);
    assert(snapshots.back().inputTokens == 2);
    assert(snapshots.back().toolCalls == 1);

    ProviderSessionBudgetGuard failing(
        ProviderExecutionBudgetPolicy {}, {},
        [](const ProviderExecutionBudgetUsage&) {
            return ai::common::Result<void>::failure(
                {"test.write_failed", "injected persistence failure"});
        });
    const auto stopped = failing.beginProviderTurn();
    assert(!stopped.success());
    assert(stopped.error()->code == "provider_budget.persistence_failed");
    assert(failing.usage().turnsStarted == 1);
}

void testParallelWorkersConsumeOneAtomicTurnAllowance()
{
    ProviderExecutionBudgetPolicy policy;
    policy.maxTurns = 7;
    ProviderSessionBudgetGuard guard(policy);
    std::atomic<int> admitted {0};
    std::vector<std::jthread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&] {
            for (int attempt = 0; attempt < 8; ++attempt) {
                if (guard.beginProviderTurn().success()) {
                    ++admitted;
                    assert(guard.completeProviderTurn(usage(1, 1)).success());
                }
            }
        });
    }
    workers.clear();
    assert(admitted.load() == 7);
    assert(guard.usage().turnsStarted == 7);
}

void testParallelTokenOvershootIsLimitedToOneAdmittedResponse()
{
    ProviderExecutionBudgetPolicy policy;
    policy.maxInputTokens = 10;
    policy.maxTurns = 20;
    ProviderSessionBudgetGuard guard(policy);
    std::atomic<int> admitted {0};
    std::vector<std::jthread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&] {
            if (guard.beginProviderTurn().success()) {
                ++admitted;
                static_cast<void>(guard.completeProviderTurn(usage(6, 1)));
            }
        });
    }
    workers.clear();
    assert(admitted.load() == 2);
    assert(guard.usage().turnsStarted == 2);
    assert(guard.usage().inputTokens == 12);
    assert(guard.usage().stopCode == "provider_budget.input_token_limit_reached");
}

void testCacheReadTokensDoNotCountTowardInputBudget()
{
    // Real-Run observation (498dd8c6-b76e-4271-b078-b4b321565222,
    // 2026-09-15): Anthropic has no server-side conversation continuation,
    // so every turn resends the whole growing history; without excluding
    // cache-read tokens here, a session's already-cached prefix (billed
    // once, then re-served at a steep discount) was still counted at full
    // weight against maxInputTokens every turn, exhausting a 500,000-token
    // budget while the Planner was still inspecting the workspace and had
    // not even submitted a manifest yet.
    ProviderExecutionBudgetPolicy policy;
    policy.maxInputTokens = 100;
    policy.maxTurns = 20;
    ProviderSessionBudgetGuard guard(policy);

    assert(guard.beginProviderTurn().success());
    assert(guard.completeProviderTurn(usageWithCache(90, 1, 10, 70)).success());
    assert(guard.usage().inputTokens == 20);
    assert(guard.usage().cacheReadInputTokens == 70);

    assert(guard.beginProviderTurn().success());
    assert(guard.completeProviderTurn(usageWithCache(90, 1, 0, 85)).success());
    assert(guard.usage().inputTokens == 25);
    assert(guard.usage().cacheReadInputTokens == 155);

    // Genuinely fresh/cache-write input still counts fully and can still
    // trip the cap - only re-served cache reads are excluded.
    assert(guard.beginProviderTurn().success());
    const auto tripped = guard.completeProviderTurn(usageWithCache(100, 1, 0, 0));
    assert(!tripped.success());
    assert(tripped.error()->code == "provider_budget.input_token_limit_reached");
    assert(guard.usage().inputTokens == 125);
}

// A response whose cache breakdown does not fit into its own input total is
// malformed. It must (a) stop the Run with the stable
// provider_budget.usage_invalid, (b) keep its FULL input and output for the
// audit, (c) earn no cache discount - so its whole input is spendable - and
// (d) never record the untrusted breakdown or look authoritative.
void requireInvalidCacheUsageStopsTheRunWithoutDiscount(
    const ai::models::TokenUsage& reported)
{
    ProviderExecutionBudgetPolicy policy;
    policy.maxInputTokens = 1'000;
    policy.maxTurns = 20;
    ProviderSessionBudgetGuard guard(policy);

    assert(guard.beginProviderTurn().success());
    const auto stopped = guard.completeProviderTurn(reported);
    assert(!stopped.success());
    assert(stopped.error()->code == "provider_budget.usage_invalid");

    const auto total = guard.usage();
    assert(total.inputTokens == reported.inputTokens);    // no discount: the whole input
    assert(total.outputTokens == reported.outputTokens);  // kept for the audit
    assert(total.cacheReadInputTokens == 0);              // an untrusted breakdown is never recorded
    assert(total.cacheCreationInputTokens == 0);
    assert(!total.usageAuthoritative);
    assert(total.turnsCompleted == 1);
    assert(total.stopCode == "provider_budget.usage_invalid");

    // The stop is sticky: nothing else is admitted.
    const auto next = guard.beginProviderTurn();
    assert(!next.success());
    assert(next.error()->code == "provider_budget.usage_invalid");
}

void testInconsistentCacheUsageStopsTheRunWithoutDiscount()
{
    const std::uint64_t huge = ~std::uint64_t {0};
    // cached_tokens > input_tokens
    requireInvalidCacheUsageStopsTheRunWithoutDiscount(usageWithCache(10, 1, 0, 50));
    // cache_write_tokens > input_tokens
    requireInvalidCacheUsageStopsTheRunWithoutDiscount(usageWithCache(10, 1, 50, 0));
    // each subset fits on its own, together they exceed the input
    requireInvalidCacheUsageStopsTheRunWithoutDiscount(usageWithCache(100, 1, 60, 60));
    // the one-token overshoot at the boundary
    requireInvalidCacheUsageStopsTheRunWithoutDiscount(usageWithCache(100, 1, 50, 51));
    // values whose SUM would wrap the unsigned range to a small, "valid-looking" number
    requireInvalidCacheUsageStopsTheRunWithoutDiscount(usageWithCache(10, 1, huge, 2));
    requireInvalidCacheUsageStopsTheRunWithoutDiscount(usageWithCache(10, 1, 0, huge));
}

// The hard ceiling must not be weakened by a malformed report: before this check
// "input 10, cached 50" counted as ZERO spent input and passed a ceiling of 5.
void testInconsistentCacheUsageNeverWeakensTheInputCeiling()
{
    ProviderExecutionBudgetPolicy policy;
    policy.maxInputTokens = 5;
    policy.maxTurns = 20;
    ProviderSessionBudgetGuard guard(policy);
    assert(guard.beginProviderTurn().success());
    const auto stopped = guard.completeProviderTurn(usageWithCache(10, 1, 0, 50));
    assert(!stopped.success());
    // The malformed report is named for what it is, not judged against the ceiling
    // as if its breakdown were real (the whole input is over the ceiling of 5 too).
    assert(stopped.error()->code == "provider_budget.usage_invalid");
    assert(guard.usage().inputTokens == 10);    // over the ceiling of 5, and recorded as such
    assert(guard.usage().inputTokens > policy.maxInputTokens);
}

// The boundary is valid: subsets that exactly fill the input are a real report
// and keep their discount (cache-read free of the cap, cache-write counted).
void testConsistentCacheUsageAtTheBoundaryKeepsItsSemantics()
{
    ProviderExecutionBudgetPolicy policy;
    policy.maxInputTokens = 1'000;
    policy.maxTurns = 20;
    ProviderSessionBudgetGuard guard(policy);

    assert(guard.beginProviderTurn().success());
    assert(guard.completeProviderTurn(usageWithCache(100, 1, 30, 70)).success());
    assert(guard.usage().inputTokens == 30);              // cache-write counts, cache-read does not
    assert(guard.usage().cacheReadInputTokens == 70);
    assert(guard.usage().cacheCreationInputTokens == 30);

    assert(guard.beginProviderTurn().success());
    assert(guard.completeProviderTurn(usageWithCache(100, 1, 0, 100)).success());   // all cached
    assert(guard.usage().inputTokens == 30);
    assert(guard.usage().cacheReadInputTokens == 170);

    assert(guard.beginProviderTurn().success());
    assert(guard.completeProviderTurn(usageWithCache(100, 1, 100, 0)).success());   // all written
    assert(guard.usage().inputTokens == 130);
    assert(guard.usage().cacheCreationInputTokens == 130);
    assert(guard.usage().usageAuthoritative);
    assert(guard.usage().stopCode.empty());
}

// AG-17 gpt-6-sol checkpoint M2b: the long-context partition and the accounting-trust flag of a
// TokenUsage exist for COST only. The provider budget reads neither: the same response with and
// without them leaves the guard in exactly the same state, and a nonsensical partition can neither
// spend nor save a ceiling.
void testTheBudgetGuardNeverReadsTheLongContextPartition()
{
    ProviderExecutionBudgetPolicy policy;
    policy.maxInputTokens = 1'000;
    policy.maxTurns = 20;
    ProviderSessionBudgetGuard plain(policy);
    ProviderSessionBudgetGuard decorated(policy);

    const ai::models::TokenUsage base = usageWithCache(90, 1, 10, 70);
    ai::models::TokenUsage withPartition = base;
    withPartition.longContext = {999'999, 888'888, 777'777, 666'666};   // larger than the totals: nonsense
    withPartition.accountingTrusted = false;

    assert(plain.beginProviderTurn().success() && decorated.beginProviderTurn().success());
    assert(plain.completeProviderTurn(base).success());
    assert(decorated.completeProviderTurn(withPartition).success());

    const auto expected = plain.usage();
    const auto actual = decorated.usage();
    assert(actual.inputTokens == expected.inputTokens && expected.inputTokens == 20);
    assert(actual.outputTokens == expected.outputTokens);
    assert(actual.cacheReadInputTokens == expected.cacheReadInputTokens);
    assert(actual.cacheCreationInputTokens == expected.cacheCreationInputTokens);
    assert(actual.usageAuthoritative == expected.usageAuthoritative && actual.usageAuthoritative);
    assert(actual.stopCode.empty() && expected.stopCode.empty());
}

// An invalid response that arrives AFTER the Run stopped is still billed: its full
// input and output reach the totals (no discount, no recorded breakdown), while
// the original stop reason stays and the accounting turns non-authoritative.
void testLateInconsistentResponseIsAccountedWithoutDiscount()
{
    ProviderExecutionBudgetPolicy policy;
    policy.maxInputTokens = 10;
    policy.maxTurns = 20;
    ProviderSessionBudgetGuard guard(policy, {}, {}, {}, 2);
    assert(guard.beginProviderTurn().success());
    assert(guard.beginProviderTurn().success());

    assert(!guard.completeProviderTurn(usage(11, 1)).success());   // trips the input cap
    const auto late = guard.completeProviderTurn(usageWithCache(9, 3, 2, 50));
    assert(!late.success());
    assert(late.error()->code == "provider_budget.input_token_limit_reached");   // original reason kept

    const auto total = guard.usage();
    assert(total.inputTokens == 11 + 9);          // the late response counts in full
    assert(total.outputTokens == 1 + 3);
    assert(total.cacheReadInputTokens == 0);      // the untrusted breakdown is not recorded
    assert(total.cacheCreationInputTokens == 0);
    assert(!total.usageAuthoritative);
    assert(total.stopCode == "provider_budget.input_token_limit_reached");
}

// A VALID response that would push an accumulator past the unsigned range still
// fails the Run closed with its own code - not the invalid-usage one.
void testAccumulatorOverflowOfValidUsageFailsClosed()
{
    ProviderExecutionBudgetPolicy policy;
    policy.maxInputTokens = 1'000;
    policy.maxTurns = 20;
    ProviderSessionBudgetGuard guard(policy);

    assert(guard.beginProviderTurn().success());
    assert(guard.completeProviderTurn(usage(999, 1)).success());     // just below the cap

    assert(guard.beginProviderTurn().success());
    const auto overflowed = guard.completeProviderTurn(usage(~std::uint64_t {0} - 10, 1));
    assert(!overflowed.success());
    assert(overflowed.error()->code == "provider_budget.usage_overflow");
    assert(guard.usage().inputTokens == 999);     // nothing was added
    assert(!guard.usage().usageAuthoritative);
}

// AG-17.3: with parallel provider turns, a response admitted BEFORE the Run
// stopped can arrive AFTER it. It is already billed, so it must reach the Run
// totals (tokens, cache breakdown, turn count, persisted usage) while the
// original terminal reason stays untouched.
void testLateInFlightResponseAfterStopIsStillAccounted()
{
    ProviderExecutionBudgetPolicy policy;
    policy.maxInputTokens = 10;
    policy.maxTurns = 20;
    std::vector<ProviderExecutionBudgetUsage> persisted;
    ProviderSessionBudgetGuard guard(
        policy, {},
        [&](const ProviderExecutionBudgetUsage& snapshot) {
            persisted.push_back(snapshot);
            return ai::common::Result<void>::ok();
        },
        {}, 2);
    assert(guard.beginProviderTurn().success());
    assert(guard.beginProviderTurn().success());           // two requests in flight

    const auto tripped = guard.completeProviderTurn(usage(11, 1));
    assert(!tripped.success());
    assert(tripped.error()->code == "provider_budget.input_token_limit_reached");

    // Fresh 9 - 4 cache-read = 5 countable input, 3 output, 2 cache-write, 4 cache-read.
    const auto late = guard.completeProviderTurn(usageWithCache(9, 3, 2, 4));
    assert(!late.success());
    assert(late.error()->code == "provider_budget.input_token_limit_reached");

    const auto total = guard.usage();
    assert(total.turnsStarted == 2);
    assert(total.turnsCompleted == 2);
    assert(total.inputTokens == 16);
    assert(total.outputTokens == 4);
    assert(total.cacheReadInputTokens == 4);
    assert(total.cacheCreationInputTokens == 2);
    assert(total.usageAuthoritative);
    assert(total.stopCode == "provider_budget.input_token_limit_reached");
    // The late usage reached the persisted Run record too (the only live source
    // of Run.usage), still carrying the original stop reason.
    assert(!persisted.empty());
    assert(persisted.back().inputTokens == 16);
    assert(persisted.back().outputTokens == 4);
    assert(persisted.back().cacheReadInputTokens == 4);
    assert(persisted.back().cacheCreationInputTokens == 2);
    assert(persisted.back().turnsCompleted == 2);
    assert(persisted.back().stopCode == "provider_budget.input_token_limit_reached");

    // Both slots were released: a new request is refused by the stop, never parked.
    assert(!guard.beginProviderTurn().success());
}

void testCacheCreationDurationIsAccumulatedAndInvalidDetailFailsClosed()
{
    ProviderExecutionBudgetPolicy policy;
    policy.maxTurns = 20;
    ProviderSessionBudgetGuard guard(policy);

    assert(guard.beginProviderTurn().success());
    assert(guard.completeProviderTurn(
        usageWithDuration(100, 1, 30, 20, 10, 20)).success());
    assert(guard.beginProviderTurn().success());
    assert(guard.completeProviderTurn(
        usageWithDuration(200, 2, 50, 40, 15, 35)).success());
    assert(guard.usage().cacheCreationInputTokens == 80);
    assert(guard.usage().cacheCreationDuration.known);
    assert(guard.usage().cacheCreationDuration.fiveMinuteTokens == 25);
    assert(guard.usage().cacheCreationDuration.oneHourTokens == 55);

    // A legacy/provider-unknown positive write makes only the duration split
    // unknown. The conservative token totals remain authoritative.
    assert(guard.beginProviderTurn().success());
    assert(guard.completeProviderTurn(usageWithCache(20, 0, 5, 0)).success());
    assert(guard.usage().cacheCreationInputTokens == 85);
    assert(!guard.usage().cacheCreationDuration.known);
    assert(guard.usage().usageAuthoritative);

    ProviderSessionBudgetGuard malformed(policy);
    assert(malformed.beginProviderTurn().success());
    const auto refused = malformed.completeProviderTurn(
        usageWithDuration(20, 0, 5, 0, 2, 2));
    assert(!refused.success());
    assert(refused.error()->code == "provider_budget.usage_invalid");
    assert(malformed.usage().cacheCreationInputTokens == 5);
    assert(!malformed.usage().cacheCreationDuration.known);
    assert(!malformed.usage().usageAuthoritative);
}

void testTheFallbackEventKeepsTheCompleteCacheAudit()
{
    // The fallback is part of the durable audit path, not merely a UI event.
    struct RecordingSink final : IRunEventSink {
        RunEvent last;
        void publish(const RunEvent& event) override { last = event; }
        ai::common::Result<void> persistProviderBudgetUsage(
            const RunId& runId,
            const ProviderExecutionBudgetUsage& usage) override
        {
            return IRunEventSink::persistProviderBudgetUsage(runId, usage);
        }
    } sink;
    ProviderExecutionBudgetUsage persisted;
    persisted.inputTokens = 60;
    persisted.outputTokens = 3;
    persisted.cacheCreationInputTokens = 20;
    persisted.cacheReadInputTokens = 40;
    persisted.cacheCreationDuration = {true, 5, 15};
    const auto saved = sink.persistProviderBudgetUsage("run", persisted);
    if (!saved.success()
        || sink.last.usage.inputTokens != 100
        || sink.last.usage.outputTokens != 3
        || sink.last.usage.cacheCreationInputTokens != 20
        || sink.last.usage.cacheReadInputTokens != 40
        || !sink.last.usage.cacheCreationDuration.known
        || sink.last.usage.cacheCreationDuration.fiveMinuteTokens != 5
        || sink.last.usage.cacheCreationDuration.oneHourTokens != 15) {
        throw std::runtime_error("The fallback Run event lost provider cache accounting");
    }
}

void testLateResponseNeverReplacesTheOriginalStopReason()
{
    ProviderExecutionBudgetPolicy policy;
    policy.maxOutputTokens = 5;
    policy.maxTurns = 20;
    ProviderSessionBudgetGuard guard(policy, {}, {}, {}, 3);
    assert(guard.beginProviderTurn().success());
    assert(guard.beginProviderTurn().success());
    assert(guard.beginProviderTurn().success());
    assert(guard.completeProviderTurn(usage(1, 6)).error()->code
        == "provider_budget.output_token_limit_reached");

    // A late response without usage cannot be added, so the accounting is no
    // longer authoritative - but the Run's stop reason is still the first one.
    const auto unreported = guard.completeProviderTurn({});
    assert(!unreported.success());
    assert(unreported.error()->code == "provider_budget.output_token_limit_reached");
    assert(guard.usage().turnsCompleted == 2);
    assert(guard.usage().inputTokens == 1);
    assert(guard.usage().outputTokens == 6);
    assert(!guard.usage().usageAuthoritative);

    // A late response that would overflow is left out whole, not half-added.
    const auto overflowing = guard.completeProviderTurn(
        usage(1, std::numeric_limits<std::uint64_t>::max()));
    assert(!overflowing.success());
    assert(overflowing.error()->code == "provider_budget.output_token_limit_reached");
    assert(guard.usage().turnsCompleted == 3);
    assert(guard.usage().inputTokens == 1);
    assert(guard.usage().outputTokens == 6);
    assert(guard.usage().stopCode == "provider_budget.output_token_limit_reached");
}

void testLateResponsePersistenceFailureKeepsTheOriginalStopReason()
{
    ProviderExecutionBudgetPolicy policy;
    policy.maxInputTokens = 10;
    policy.maxTurns = 20;
    std::atomic<bool> failPersistence {false};
    ProviderSessionBudgetGuard guard(
        policy, {},
        [&](const ProviderExecutionBudgetUsage&) {
            return failPersistence.load()
                ? ai::common::Result<void>::failure({"test.write_failed", "injected"})
                : ai::common::Result<void>::ok();
        },
        {}, 2);
    assert(guard.beginProviderTurn().success());
    assert(guard.beginProviderTurn().success());
    assert(!guard.completeProviderTurn(usage(11, 1)).success());

    failPersistence = true;
    const auto late = guard.completeProviderTurn(usage(2, 2));
    assert(!late.success());
    // The real reason the Run stopped, not the later persistence hiccup.
    assert(late.error()->code == "provider_budget.input_token_limit_reached");
    assert(guard.usage().stopCode == "provider_budget.input_token_limit_reached");
    assert(guard.usage().inputTokens == 13);
    assert(guard.usage().outputTokens == 3);
}

void testWaitingProviderRequestIsCooperativelyCancelled()
{
    ProviderSessionBudgetGuard guard(ProviderExecutionBudgetPolicy {});
    assert(guard.beginProviderTurn().success());

    std::string stoppedCode;
    std::jthread waiting([&](std::stop_token stopToken) {
        const auto result = guard.beginProviderTurn(stopToken);
        assert(!result.success());
        stoppedCode = result.error()->code;
    });
    waiting.request_stop();
    waiting.join();
    assert(stoppedCode == "provider_budget.cancelled");
    guard.abandonProviderTurn();
}

// ---------------------------------------------------------------------------
// AG-17.3: the Run-wide in-flight bound is adjustable while the guard is shared
// ---------------------------------------------------------------------------

bool waitUntil(const std::atomic<bool>& flag, std::chrono::milliseconds limit)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!flag.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return flag.load();
}

void testRaisingTheConcurrencyBoundAdmitsAWaitingRequest()
{
    ProviderSessionBudgetGuard guard(ProviderExecutionBudgetPolicy {});
    assert(guard.maxConcurrentRequests() == 1);
    assert(guard.beginProviderTurn().success());           // the one allowed request

    std::atomic<bool> admitted {false};
    std::thread waiting([&] {
        const auto result = guard.beginProviderTurn();
        assert(result.success());
        admitted = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(!admitted.load());                              // the bound holds it back

    assert(guard.setMaxConcurrentRequests(2) == 1);        // returns the previous bound
    assert(waitUntil(admitted, std::chrono::seconds(5)));  // raising wakes the waiter
    waiting.join();
    assert(guard.maxConcurrentRequests() == 2);
    assert(guard.usage().turnsStarted == 2);
    assert(guard.completeProviderTurn(usage(1, 1)).success());
    assert(guard.completeProviderTurn(usage(1, 1)).success());
}

void testLoweringTheConcurrencyBoundOnlyDelaysNewAdmissions()
{
    ProviderSessionBudgetGuard guard(ProviderExecutionBudgetPolicy {}, {}, {}, {}, 3);
    assert(guard.beginProviderTurn().success());
    assert(guard.beginProviderTurn().success());           // two in flight under a bound of 3

    assert(guard.setMaxConcurrentRequests(1) == 3);
    assert(guard.usage().turnsStarted == 2);               // nothing already admitted is revoked

    std::atomic<bool> admitted {false};
    std::thread waiting([&] {
        const auto result = guard.beginProviderTurn();
        assert(result.success());
        admitted = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(!admitted.load());
    assert(guard.completeProviderTurn(usage(1, 1)).success());   // one still in flight: 1 < 1 is false
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(!admitted.load());
    assert(guard.completeProviderTurn(usage(1, 1)).success());   // none in flight
    assert(waitUntil(admitted, std::chrono::seconds(5)));
    waiting.join();
    assert(guard.completeProviderTurn(usage(1, 1)).success());

    assert(guard.setMaxConcurrentRequests(0) == 1);        // clamped, never zero (no deadlock)
    assert(guard.maxConcurrentRequests() == 1);
}

// ---------------------------------------------------------------------------
// Reservation-aware layer (worker capacity fairness)
// ---------------------------------------------------------------------------

ProviderExecutionBudgetPolicy turnsPolicy(std::uint64_t maxTurns)
{
    ProviderExecutionBudgetPolicy policy;
    policy.maxTurns = maxTurns;
    return policy;
}

ProviderExecutionBudgetPolicy autoTurnsPolicy(std::uint64_t maxTurns)
{
    auto policy = turnsPolicy(maxTurns);
    policy.autoExtendTurns = true;
    return policy;
}

// Only a Run whose turn limit was not set explicitly is eligible for the
// automatic extension; a bare policy stays pinned (fail-closed).
void testOnlyAnUnsetTurnLimitIsAutomatic()
{
    assert(!ProviderExecutionBudgetPolicy {}.autoExtendTurns);

    const auto defaults = resolveProviderExecutionBudget({});
    assert(defaults.success());
    assert(defaults.value().autoExtendTurns);
    assert(defaults.value().maxTurns == 24);

    ProviderExecutionBudgetOverrides otherLimits;
    otherLimits.maxInputTokens = 100'000;
    otherLimits.maxToolCalls = 64;
    const auto lowered = resolveProviderExecutionBudget(otherLimits);
    assert(lowered.success());
    assert(lowered.value().autoExtendTurns);   // only maxTurns pins the turn limit

    ProviderExecutionBudgetOverrides explicitTurns;
    explicitTurns.maxTurns = 24;               // even the default value, when entered
    const auto pinned = resolveProviderExecutionBudget(explicitTurns);
    assert(pinned.success());
    assert(!pinned.value().autoExtendTurns);
}

// Edit & rerun puts back exactly the limits somebody set: a value that differs from the Backend
// default is explicit, the turn limit is explicit only when pinned, and resolving the result gives
// the same policy again.
void testOverridesFromPolicyReproduceThePolicyAndKeepDefaultsImplicit()
{
    assert(!hasExplicitProviderBudgetLimits({}));
    ProviderExecutionBudgetOverrides one;
    one.maxRequestTimeMs = 1;
    assert(hasExplicitProviderBudgetLimits(one));

    const auto defaults = resolveProviderExecutionBudget({});
    assert(defaults.success());
    assert(!hasExplicitProviderBudgetLimits(providerBudgetOverridesFromPolicy(defaults.value())));

    ProviderExecutionBudgetOverrides pinned;
    pinned.maxTurns = 24;                    // typed, even though it equals the default
    const auto pinnedPolicy = resolveProviderExecutionBudget(pinned);
    assert(pinnedPolicy.success() && !pinnedPolicy.value().autoExtendTurns);
    const auto fromPinned = providerBudgetOverridesFromPolicy(pinnedPolicy.value());
    assert(fromPinned.maxTurns == 24 && !fromPinned.maxInputTokens && !fromPinned.maxToolCalls);

    ProviderExecutionBudgetOverrides mixed;
    mixed.maxInputTokens = 100'000;
    mixed.maxToolResultBytes = 1024 * 1024;
    mixed.maxWallTimeMs = 60'000;
    const auto mixedPolicy = resolveProviderExecutionBudget(mixed);
    assert(mixedPolicy.success() && mixedPolicy.value().autoExtendTurns);
    const auto fromMixed = providerBudgetOverridesFromPolicy(mixedPolicy.value());
    assert(!fromMixed.maxTurns && fromMixed.maxInputTokens == 100'000
           && fromMixed.maxToolResultBytes == 1024 * 1024 && fromMixed.maxWallTimeMs == 60'000
           && !fromMixed.maxOutputTokens && !fromMixed.maxToolCalls && !fromMixed.maxRequestTimeMs);
    const auto again = resolveProviderExecutionBudget(fromMixed);
    assert(again.success() && again.value().maxInputTokens == 100'000
           && again.value().maxWallTimeMs == 60'000 && again.value().autoExtendTurns
           && again.value().maxTurns == mixedPolicy.value().maxTurns);
}

// raiseTurnLimit raises exactly to the requirement, records it in the usage
// that is persisted BEFORE it returns, never lowers, and is a no-op (without a
// write) when the limit already covers the requirement.
void testRaiseTurnLimitRaisesAndPersistsExactly()
{
    std::vector<ProviderExecutionBudgetUsage> snapshots;
    ProviderSessionBudgetGuard guard(
        autoTurnsPolicy(24), {},
        [&snapshots](const ProviderExecutionBudgetUsage& value) {
            snapshots.push_back(value);
            return ai::common::Result<void>::ok();
        });

    assert(guard.raiseTurnLimit(25).success());
    assert(guard.policy().maxTurns == 25);
    assert(guard.usage().grantedMaxTurns == 25);
    assert(snapshots.size() == 1 && snapshots.back().grantedMaxTurns == 25);

    assert(guard.raiseTurnLimit(25).success());   // already covered
    assert(guard.raiseTurnLimit(10).success());   // never lowers
    assert(guard.policy().maxTurns == 25);
    assert(snapshots.size() == 1);

    assert(guard.raiseTurnLimit(kProviderBudgetHardMaxTurns).success());   // the ceiling itself
    assert(guard.policy().maxTurns == kProviderBudgetHardMaxTurns);
    assert(snapshots.size() == 2);
}

void testRaiseTurnLimitRefusesPinnedAndAboveCeilingWithoutChangingAnything()
{
    std::vector<ProviderExecutionBudgetUsage> snapshots;
    const auto record = [&snapshots](const ProviderExecutionBudgetUsage& value) {
        snapshots.push_back(value);
        return ai::common::Result<void>::ok();
    };

    ProviderSessionBudgetGuard pinned(turnsPolicy(24), {}, record);
    const auto pinnedResult = pinned.raiseTurnLimit(30);
    assert(!pinnedResult.success());
    assert(pinnedResult.error()->code == "provider_budget.turn_limit_pinned");
    assert(pinned.policy().maxTurns == 24 && pinned.usage().grantedMaxTurns == 0);

    ProviderSessionBudgetGuard automatic(autoTurnsPolicy(24), {}, record);
    const auto tooMany = automatic.raiseTurnLimit(kProviderBudgetHardMaxTurns + 1);
    assert(!tooMany.success());
    assert(tooMany.error()->code == "provider_budget.execution_capacity_insufficient");
    assert(automatic.policy().maxTurns == 24 && automatic.usage().grantedMaxTurns == 0);
    assert(snapshots.empty());
    assert(automatic.beginProviderTurn().success());   // the Run is not stopped by a refusal
}

// A raise that could not be recorded must not survive: the Run stops with the
// persistence failure and the limit is back to what was durably recorded.
void testRaiseTurnLimitPersistenceFailureRollsBackAndStops()
{
    ProviderSessionBudgetGuard guard(
        autoTurnsPolicy(24), {},
        [](const ProviderExecutionBudgetUsage&) {
            return ai::common::Result<void>::failure({"test.write_failed", "injected"});
        });
    const auto raised = guard.raiseTurnLimit(30);
    assert(!raised.success());
    assert(raised.error()->code == "provider_budget.persistence_failed");
    assert(guard.policy().maxTurns == 24);
    assert(guard.usage().grantedMaxTurns == 0);
    assert(!guard.beginProviderTurn().success());
}

// A guard rebuilt from the persisted Run (restart, paid continuation repair)
// applies the recorded raise again; a pinned policy never does, and a corrupt
// value never exceeds the Backend ceiling.
void testRebuiltGuardKeepsTheGrantedTurnLimit()
{
    ProviderExecutionBudgetUsage persisted;
    persisted.turnsStarted = 25;
    persisted.turnsCompleted = 25;
    persisted.grantedMaxTurns = 30;

    ProviderSessionBudgetGuard rebuilt(autoTurnsPolicy(24), persisted);
    assert(rebuilt.policy().maxTurns == 30);
    assert(rebuilt.beginProviderTurn().success());   // 25 < 30: not an over-spent budget

    ProviderSessionBudgetGuard pinned(turnsPolicy(24), persisted);
    assert(pinned.policy().maxTurns == 24);
    assert(!pinned.beginProviderTurn().success());

    persisted.grantedMaxTurns = 1'000;
    ProviderSessionBudgetGuard clamped(autoTurnsPolicy(24), persisted);
    assert(clamped.policy().maxTurns == kProviderBudgetHardMaxTurns);

    persisted.grantedMaxTurns = 10;                  // below the frozen limit: ignored
    ProviderSessionBudgetGuard lower(autoTurnsPolicy(24), persisted);
    assert(lower.policy().maxTurns == 24);
}

// The raised limit is the one the guard enforces and reserves against.
void testRaisedTurnLimitIsWhatTheGuardEnforces()
{
    ProviderSessionBudgetGuard guard(autoTurnsPolicy(2));
    assert(guard.beginProviderTurn().success());
    assert(guard.completeProviderTurn(usage(1, 1)).success());
    assert(guard.beginProviderTurn().success());
    assert(guard.completeProviderTurn(usage(1, 1)).success());
    assert(!guard.beginProviderTurn().success());     // the third turn is over the limit

    ProviderSessionBudgetGuard raised(autoTurnsPolicy(2));
    assert(!raised.reserveTurns({{"a", 3}}).success());
    assert(raised.raiseTurnLimit(3).success());
    assert(raised.reserveTurns({{"a", 3}}).success());
    assert(raised.unreservedTurnsRemaining() == 0);
}

// Sum of reservations larger than the Run-wide unstarted turns registers
// NOTHING (all or nothing) and leaves every counter untouched; a sum that
// fits registers every owner.
void testReservationsAreAtomicAndBoundedByRunWideTurns()
{
    ProviderSessionBudgetGuard guard(turnsPolicy(10));
    const auto tooMany = guard.reserveTurns({{"a", 6}, {"b", 5}});
    assert(!tooMany.success());
    assert(tooMany.error()->code == "provider_budget.execution_capacity_insufficient");
    assert(guard.reservedUnusedTurns() == 0);
    assert(!guard.turnReservation("a").has_value());
    assert(!guard.turnReservation("b").has_value());
    assert(guard.usage().turnsStarted == 0);

    const auto fits = guard.reserveTurns({{"a", 6}, {"b", 4}});
    assert(fits.success());
    assert(guard.reservedUnusedTurns() == 10);
    assert(guard.unreservedTurnsRemaining() == 0);
    assert(guard.turnReservation("a")->reserved == 6);

    // A second registration for an already reserved owner is a conflict and
    // must not partially register its other members.
    const auto conflict = guard.reserveTurns({{"c", 1}, {"a", 1}});
    assert(!conflict.success());
    assert(conflict.error()->code == "provider_budget.turn_reservation_conflict");
    assert(!guard.turnReservation("c").has_value());

    // Started turns shrink what can still be reserved.
    ProviderSessionBudgetGuard partly(turnsPolicy(10));
    assert(partly.beginProviderTurn().success());
    assert(partly.completeProviderTurn(usage(1, 1)).success());
    assert(!partly.reserveTurns({{"a", 10}}).success());
    assert(partly.reserveTurns({{"a", 9}}).success());
}

// An early owner spends its own reservation and then only the unreserved
// surplus; it can never take a turn reserved for a pending owner, so a later
// owner always still gets its own terminal reserve.
void testEarlyOwnerCannotSpendAnotherOwnersReservation()
{
    ProviderSessionBudgetGuard guard(turnsPolicy(10));
    assert(guard.reserveTurns({{"early", 3}, {"late", 3}}).success());
    // 10 turns, 6 reserved -> 4 surplus.
    assert(guard.unreservedTurnsRemaining() == 4);

    std::uint64_t earlyTurns = 0;
    for (;;) {
        const auto admitted = guard.beginOwnedProviderTurn("early");
        if (!admitted.success()) {
            assert(admitted.error()->code == "provider_budget.task_turn_allocation_exhausted");
            break;
        }
        ++earlyTurns;
        assert(guard.completeProviderTurn(usage(1, 1)).success());
    }
    // Own 3 + all 4 surplus, never the 3 reserved for `late`.
    assert(earlyTurns == 7);
    assert(guard.usage().turnsStarted == 7);
    assert(guard.usage().stopCode.empty());          // not a sticky Run-wide stop
    assert(guard.reservedUnusedTurns() == 3);
    assert(guard.turnReservation("late")->used == 0);

    for (int i = 0; i < 3; ++i) {
        const auto admitted = guard.beginOwnedProviderTurn("late");
        assert(admitted.success());
        assert(admitted.value().consumedOwnReservation);
        assert(guard.completeProviderTurn(usage(1, 1)).success());
    }
    assert(guard.usage().turnsStarted == 10);
    assert(!guard.beginOwnedProviderTurn("late").success());
}

// The admission tells the owner - atomically with the admission itself - how
// many turns it may still start, so the loop can force the last one terminal.
void testAdmissionReportsOwnersRemainingTurns()
{
    ProviderSessionBudgetGuard guard(turnsPolicy(8));
    assert(guard.reserveTurns({{"a", 3}, {"b", 5}}).success());   // no surplus
    auto first = guard.beginOwnedProviderTurn("a");
    assert(first.success());
    assert(first.value().ownerTurnsRemainingAfterAdmission == 2);
    assert(guard.completeProviderTurn(usage(1, 1)).success());
    assert(guard.beginOwnedProviderTurn("a").value().ownerTurnsRemainingAfterAdmission == 1);
    assert(guard.completeProviderTurn(usage(1, 1)).success());
    const auto last = guard.beginOwnedProviderTurn("a");
    assert(last.success());
    assert(last.value().ownerTurnsRemainingAfterAdmission == 0);   // final turn
    assert(guard.completeProviderTurn(usage(1, 1)).success());
    assert(!guard.beginOwnedProviderTurn("a").success());
}

// Completed/Failed/Skipped work returns its UNUSED reservation to the shared
// surplus; a second release is a no-op.
void testReleasingReservationReturnsUnusedTurnsToSurplus()
{
    ProviderSessionBudgetGuard guard(turnsPolicy(10));
    assert(guard.reserveTurns({{"a", 4}, {"b", 4}}).success());
    assert(guard.unreservedTurnsRemaining() == 2);

    assert(guard.beginOwnedProviderTurn("a").success());
    assert(guard.completeProviderTurn(usage(1, 1)).success());
    guard.releaseTurnReservation("a");            // a finished after 1 turn
    assert(!guard.turnReservation("a").has_value());
    assert(guard.reservedUnusedTurns() == 4);      // only b's
    assert(guard.unreservedTurnsRemaining() == 5); // 9 unstarted - 4 reserved

    guard.releaseTurnReservation("a");            // idempotent
    guard.releaseTurnReservation("never-reserved");
    assert(guard.unreservedTurnsRemaining() == 5);

    // b can now spend its own 4 plus the 5 released/surplus turns.
    std::uint64_t bTurns = 0;
    while (guard.beginOwnedProviderTurn("b").success()) {
        ++bTurns;
        assert(guard.completeProviderTurn(usage(1, 1)).success());
    }
    assert(bTurns == 9);
    assert(guard.usage().turnsStarted == 10);
}

// A request that names no owner may only use the unreserved surplus while
// reservations are outstanding.
void testUnownedRequestsCannotConsumeReservedTurns()
{
    ProviderSessionBudgetGuard guard(turnsPolicy(5));
    assert(guard.reserveTurns({{"a", 4}}).success());   // 1 surplus
    assert(guard.beginProviderTurn().success());
    assert(guard.completeProviderTurn(usage(1, 1)).success());
    const auto blocked = guard.beginProviderTurn();
    assert(!blocked.success());
    assert(blocked.error()->code == "provider_budget.task_turn_allocation_exhausted");
    assert(guard.usage().stopCode.empty());
    guard.releaseAllTurnReservations();
    assert(guard.beginProviderTurn().success());
    assert(guard.completeProviderTurn(usage(1, 1)).success());
}

// Parallel workers can neither exceed the Run-wide ceiling nor steal each
// other's reservation: every owner still obtains at least its reserved turns.
void testParallelOwnersNeverStealReservationsOrExceedCeiling()
{
    constexpr int kOwners = 8;
    ProviderSessionBudgetGuard guard(turnsPolicy(30), {}, {}, {}, kOwners);
    std::unordered_map<std::string, std::uint64_t> reservations;
    for (int i = 0; i < kOwners; ++i) reservations["task-" + std::to_string(i)] = 2;
    assert(guard.reserveTurns(reservations).success());   // 16 reserved, 14 surplus

    std::atomic<std::uint64_t> admittedTotal {0};
    std::vector<std::atomic<std::uint64_t>> perOwner(kOwners);
    {
        std::vector<std::jthread> workers;
        for (int i = 0; i < kOwners; ++i) {
            workers.emplace_back([&, i] {
                const std::string owner = "task-" + std::to_string(i);
                for (int attempt = 0; attempt < 10; ++attempt) {
                    if (guard.beginOwnedProviderTurn(owner).success()) {
                        ++admittedTotal;
                        ++perOwner[static_cast<std::size_t>(i)];
                        assert(guard.completeProviderTurn(usage(1, 1)).success());
                    }
                }
            });
        }
    }
    // Never above the hard ceiling; every started turn is accounted exactly
    // once; and no owner lost its own reserved turns to another worker. (A
    // turn a worker leased ahead but never used is returned on release, so the
    // total may end just below the ceiling.)
    assert(admittedTotal.load() <= 30);
    assert(guard.usage().turnsStarted == admittedTotal.load());
    assert(guard.usage().turnsStarted + guard.reservedUnusedTurns() <= 30);
    for (int i = 0; i < kOwners; ++i) {
        assert(perOwner[static_cast<std::size_t>(i)].load() >= 2);
    }
    guard.releaseAllTurnReservations();
    assert(guard.reservedUnusedTurns() == 0);
}

// An unforced turn is always followed by an available one: the next turn is
// leased to the owner from the surplus atomically with the current admission,
// so a greedy sibling cannot take the last surplus turn and leave the owner
// refused without ever having been offered its forced terminal turn.
void testOwnerIsNeverRefusedAfterAnUnforcedTurn()
{
    ProviderSessionBudgetGuard guard(turnsPolicy(6));
    assert(guard.reserveTurns({{"a", 1}, {"b", 1}}).success());   // 4 surplus

    const auto aFirst = guard.beginOwnedProviderTurn("a");
    assert(aFirst.success());
    assert(aFirst.value().ownerTurnsRemainingAfterAdmission == 1);   // leased ahead
    assert(guard.completeProviderTurn(usage(1, 1)).success());

    // b drains everything b can legitimately take: its own turn plus the
    // surplus that is left after a's lease.
    std::uint64_t bTurns = 0;
    while (guard.beginOwnedProviderTurn("b").success()) {
        ++bTurns;
        assert(guard.completeProviderTurn(usage(1, 1)).success());
    }
    assert(bTurns == 4);

    const auto aLast = guard.beginOwnedProviderTurn("a");
    assert(aLast.success());
    assert(aLast.value().ownerTurnsRemainingAfterAdmission == 0);   // final, forced turn
    assert(guard.completeProviderTurn(usage(1, 1)).success());
    const auto aRefused = guard.beginOwnedProviderTurn("a");
    assert(!aRefused.success());
    // All six Run-wide turns are now started, so the hard ceiling itself answers.
    assert(aRefused.error()->code == "provider_budget.turn_limit_reached");
    assert(guard.usage().turnsStarted == 6);
}

} // namespace

int runProviderExecutionBudgetTests()
{
    testRaisingTheConcurrencyBoundAdmitsAWaitingRequest();
    testLoweringTheConcurrencyBoundOnlyDelaysNewAdmissions();
    testOnlyAnUnsetTurnLimitIsAutomatic();
    testOverridesFromPolicyReproduceThePolicyAndKeepDefaultsImplicit();
    testRaiseTurnLimitRaisesAndPersistsExactly();
    testRaiseTurnLimitRefusesPinnedAndAboveCeilingWithoutChangingAnything();
    testRaiseTurnLimitPersistenceFailureRollsBackAndStops();
    testRebuiltGuardKeepsTheGrantedTurnLimit();
    testRaisedTurnLimitIsWhatTheGuardEnforces();
    testReservationsAreAtomicAndBoundedByRunWideTurns();
    testEarlyOwnerCannotSpendAnotherOwnersReservation();
    testAdmissionReportsOwnersRemainingTurns();
    testReleasingReservationReturnsUnusedTurnsToSurplus();
    testUnownedRequestsCannotConsumeReservedTurns();
    testParallelOwnersNeverStealReservationsOrExceedCeiling();
    testOwnerIsNeverRefusedAfterAnUnforcedTurn();
    testProfileOverridesCanOnlyLowerHardCeilings();
    testAttemptedThirteenthTurnIsNeverAdmitted();
    testInputTokensArePrimaryAcrossSessionBoundaries();
    testMissingUsageFailsClosedBeforeAnotherRequest();
    testToolAndOutputLimitsAreRunWide();
    testWallAndRequestTimeAreBoundedDeterministically();
    testEveryMutationIsPersistedAndPersistenceFailureStopsExecution();
    testParallelWorkersConsumeOneAtomicTurnAllowance();
    testParallelTokenOvershootIsLimitedToOneAdmittedResponse();
    testCacheReadTokensDoNotCountTowardInputBudget();
    testTheBudgetGuardNeverReadsTheLongContextPartition();
    testInconsistentCacheUsageStopsTheRunWithoutDiscount();
    testInconsistentCacheUsageNeverWeakensTheInputCeiling();
    testConsistentCacheUsageAtTheBoundaryKeepsItsSemantics();
    testLateInconsistentResponseIsAccountedWithoutDiscount();
    testAccumulatorOverflowOfValidUsageFailsClosed();
    testLateInFlightResponseAfterStopIsStillAccounted();
    testCacheCreationDurationIsAccumulatedAndInvalidDetailFailsClosed();
    testTheFallbackEventKeepsTheCompleteCacheAudit();
    testLateResponseNeverReplacesTheOriginalStopReason();
    testLateResponsePersistenceFailureKeepsTheOriginalStopReason();
    testWaitingProviderRequestIsCooperativelyCancelled();
    return 0;
}
