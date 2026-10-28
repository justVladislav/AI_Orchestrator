#include "ai/application/budget/CostCalculation.hpp"
#include "ai/providers/OpenAIResponseParser.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <cstdint>

namespace {

using namespace ai::application;

ai::models::TokenUsage usage(
    std::uint64_t input, std::uint64_t output,
    std::uint64_t cacheCreation = 0, std::uint64_t cacheRead = 0)
{
    ai::models::TokenUsage value;
    value.inputTokens = input + cacheCreation + cacheRead;
    value.outputTokens = output;
    value.cacheCreationInputTokens = cacheCreation;
    value.cacheReadInputTokens = cacheRead;
    return value;
}

Model priced(
    std::uint64_t inputPrice, std::uint64_t outputPrice,
    std::uint64_t cacheWritePrice = 0, std::uint64_t cacheReadPrice = 0)
{
    Model model;
    model.inputPriceMicrosPerMillionTokens = inputPrice;
    model.outputPriceMicrosPerMillionTokens = outputPrice;
    model.cacheWritePriceMicrosPerMillionTokens = cacheWritePrice;
    model.cacheReadPriceMicrosPerMillionTokens = cacheReadPrice;
    return model;
}

// A model/usage pair with no cache activity must reproduce exactly the
// original plain input*price + output*price formula every existing
// consumer already relied on before MW-P2 - no regression for OpenAI or an
// uncached Anthropic call.
void testNoCacheUsageMatchesPlainFormula()
{
    const auto model = priced(3'000'000, 15'000'000);
    const auto result = computeSingleResponseCostMicros(model, usage(1000, 200));
    assert(result.costMicros == 1000ULL * 3'000'000ULL / 1'000'000ULL
        + 200ULL * 15'000'000ULL / 1'000'000ULL);
    assert(result.priceKnown);
}

// The heart of MW-P2's correctness fix: once Anthropic actually reports
// cache_creation/cache_read tokens, the OLD formula (inputTokens * plain
// price) would overcharge, since inputTokens is now the conservative sum
// including the (differently priced) cache categories.
void testCacheUsageIsBilledAtItsOwnRateNotThePlainRate()
{
    // $3/M plain input, $15/M output, $3.75/M cache-write, $0.30/M cache-read
    const auto model = priced(3'000'000, 15'000'000, 3'750'000, 300'000);
    const auto result = computeSingleResponseCostMicros(model, usage(100, 50, 200, 4000));
    const std::uint64_t expected =
        100ULL * 3'000'000ULL / 1'000'000ULL        // plain input
        + 200ULL * 3'750'000ULL / 1'000'000ULL      // cache write
        + 4000ULL * 300'000ULL / 1'000'000ULL       // cache read
        + 50ULL * 15'000'000ULL / 1'000'000ULL;     // output
    assert(result.costMicros == expected);
    assert(result.priceKnown);

    // Naively billing the whole conservative sum at the plain input rate
    // would produce a materially larger (wrong) number - guard against a
    // future regression collapsing back to the pre-MW-P2 formula.
    const std::uint64_t naiveWrongCost =
        static_cast<std::uint64_t>(static_cast<long double>(usage(100, 50, 200, 4000).inputTokens)
            * model.inputPriceMicrosPerMillionTokens / 1'000'000.0L)
        + 50ULL * 15'000'000ULL / 1'000'000ULL;
    assert(result.costMicros < naiveWrongCost);
}

void testZeroCachePriceWithRealCacheUsageIsHonestlyUnknown()
{
    // Plain/output prices are set; cache prices were never entered.
    const auto model = priced(3'000'000, 15'000'000);
    const auto result = computeSingleResponseCostMicros(model, usage(100, 50, 200, 4000));
    assert(!result.priceKnown);
    // Cost is still computed (the known categories contribute normally),
    // just documented as an underestimate via priceKnown - never a hard
    // failure, matching "caching is an optimization, never the safety
    // boundary."
    assert(result.costMicros == 100ULL * 3'000'000ULL / 1'000'000ULL
        + 50ULL * 15'000'000ULL / 1'000'000ULL);
}

void testAllZeroPricesWithNoUsageIsKnownZeroCost()
{
    // A brand-new model with no prices entered yet and no usage recorded is
    // truthfully zero cost, not "unknown" - there is nothing to be unknown
    // about.
    const auto model = priced(0, 0);
    const auto result = computeSingleResponseCostMicros(model, usage(0, 0));
    assert(result.costMicros == 0);
    assert(result.priceKnown);
}

void testMissingOutputPriceIsUnknownEvenWhenInputPriceIsSet()
{
    Model model = priced(3'000'000, 0);
    const auto result = computeSingleResponseCostMicros(model, usage(100, 50));
    assert(!result.priceKnown);
}

// Package 4 (2026-09-28 planner-terminal-budget-hardening): the exact CSV
// fixture from the real Anthropic usage export
// claude_api_tokens_2026_09_28_to_2026_09_28.csv, reconciled against the
// summed usage of the two negative-evidence Runs ade2ca50-0c15-4762-a2b8-
// 3ffb0e6f0b3e and 7385ad56-1720-4835-b717-123bcff298f9 plus three other
// same-day rows: 852 no-cache input, 284,235 cache-write, 3,518,953
// cache-read, 19,782 output. The provider's own grand total is exactly
// 3,823,822 - computeUsageTokenBreakdown() must reproduce it without ever
// double-counting cache creation on top of an already-inclusive raw total.
void testGrandProviderUsageMatchesExactCsvFixture()
{
    const auto value = usage(852, 19'782, 284'235, 3'518'953);
    const auto breakdown = computeUsageTokenBreakdown(value);
    assert(breakdown.nonCacheInputTokens == 852);
    assert(breakdown.cacheWriteTokens == 284'235);
    assert(breakdown.cacheReadTokens == 3'518'953);
    assert(breakdown.outputTokens == 19'782);
    // rawInputTokens is the provider-conservative total (matches
    // TokenUsage::inputTokens verbatim): 852 + 284,235 + 3,518,953.
    assert(breakdown.rawInputTokens == 852ULL + 284'235ULL + 3'518'953ULL);
    assert(breakdown.grandProviderUsageTokens == 3'823'822ULL);
    // The identity the header documents: grand total == raw input + output,
    // since raw input already sums every input-side category.
    assert(breakdown.grandProviderUsageTokens
        == breakdown.rawInputTokens + breakdown.outputTokens);
}

// Raw input and the non-cache (genuinely fresh) projection must diverge
// whenever real cache activity exists - a consumer that conflates the two
// would overstate how much "fresh" work a Run actually did.
void testRawInputAndNonCacheInputProjectionDifferCorrectly()
{
    const auto breakdown = computeUsageTokenBreakdown(usage(100, 10, 200, 4000));
    assert(breakdown.rawInputTokens == 100 + 200 + 4000);
    assert(breakdown.nonCacheInputTokens == 100);
    assert(breakdown.rawInputTokens != breakdown.nonCacheInputTokens);
}

// Zero cache activity must reduce every projection to the plain totals -
// the breakdown must never invent cache usage that was not reported.
void testNoCacheUsageBreakdownReducesToPlainTotals()
{
    const auto breakdown = computeUsageTokenBreakdown(usage(1000, 200));
    assert(breakdown.rawInputTokens == 1000);
    assert(breakdown.nonCacheInputTokens == 1000);
    assert(breakdown.cacheWriteTokens == 0);
    assert(breakdown.cacheReadTokens == 0);
    assert(breakdown.grandProviderUsageTokens == 1200);
}

// A usage value exactly as a provider reported it: `input` is the FULL total
// (unlike usage() above, which builds the total from its parts).
ai::models::TokenUsage reported(
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

// AG-17 gpt-6-sol checkpoint M1: a real-shaped OpenAI response is priced in
// three SEPARATE input categories - plain, cache-read and cache-write - never
// at the flat input rate and never with the cached tokens counted twice.
void testOpenAIWireUsageIsPricedInThreeSeparateInputCategories()
{
    const auto response = nlohmann::json::parse(R"json(
        {"usage": {"input_tokens": 1000000, "output_tokens": 50000,
                   "input_tokens_details": {"cached_tokens": 600000,
                                            "cache_write_tokens": 100000}}}
    )json");
    const auto value = ai::providers::openai_detail::parseOpenAITokenUsage(response);

    const auto breakdown = computeUsageTokenBreakdown(value);
    assert(breakdown.nonCacheInputTokens == 300'000);   // 1,000,000 - 600,000 - 100,000
    assert(breakdown.cacheReadTokens == 600'000);
    assert(breakdown.cacheWriteTokens == 100'000);
    assert(breakdown.rawInputTokens == 1'000'000);
    assert(breakdown.grandProviderUsageTokens == 1'050'000);

    // $2 input / $2.50 cache write / $0.20 cached input / $10 output per million.
    const auto model = priced(2'000'000, 10'000'000, 2'500'000, 200'000);
    const auto result = computeSingleResponseCostMicros(model, value);
    assert(result.priceKnown);
    assert(result.costMicros
        == 300'000ULL * 2'000'000ULL / 1'000'000ULL        // plain input   = 600,000
         + 100'000ULL * 2'500'000ULL / 1'000'000ULL        // cache write   = 250,000
         + 600'000ULL * 200'000ULL / 1'000'000ULL          // cache read    = 120,000
         + 50'000ULL * 10'000'000ULL / 1'000'000ULL);      // output        = 500,000
    assert(result.costMicros == 1'470'000);

    // Pricing every input token at the plain rate would be materially larger.
    assert(result.costMicros < 1'000'000ULL * 2'000'000ULL / 1'000'000ULL
        + 50'000ULL * 10'000'000ULL / 1'000'000ULL);
}

// The consequence for the CURRENT catalog rows (no cache price entered): an
// OpenAI turn with a cache hit is honestly unknown, not silently priced.
void testOpenAIWireUsageWithoutCachePricesIsHonestlyUnknown()
{
    const auto response = nlohmann::json::parse(R"json(
        {"usage": {"input_tokens": 1000, "output_tokens": 10,
                   "input_tokens_details": {"cached_tokens": 600}}}
    )json");
    const auto value = ai::providers::openai_detail::parseOpenAITokenUsage(response);
    const auto result = computeSingleResponseCostMicros(priced(1'750'000, 14'000'000), value);
    assert(!result.priceKnown);
    // Only the categories that do have a price contribute.
    assert(result.costMicros == 400ULL * 1'750'000ULL / 1'000'000ULL
        + 10ULL * 14'000'000ULL / 1'000'000ULL);

    // No cache hit -> the same model prices exactly, as before.
    const auto noCache = nlohmann::json::parse(
        R"json({"usage": {"input_tokens": 1000, "output_tokens": 10}})json");
    const auto plain = computeSingleResponseCostMicros(
        priced(1'750'000, 14'000'000),
        ai::providers::openai_detail::parseOpenAITokenUsage(noCache));
    assert(plain.priceKnown);
}

// Cache subsets that do not fit into the reported total are a malformed report:
// nothing may wrap an unsigned value, and the result must never look exact.
void testInconsistentCacheSubsetsNeverWrapAndAreNeverExactlyPriced()
{
    const auto model = priced(2'000'000, 10'000'000, 2'500'000, 200'000);

    // Both subsets together exceed the total (80 + 80 > 100).
    auto breakdown = computeUsageTokenBreakdown(reported(100, 10, 80, 80));
    assert(breakdown.nonCacheInputTokens == 0);
    assert(breakdown.rawInputTokens == 100);
    assert(breakdown.cacheWriteTokens == 80 && breakdown.cacheReadTokens == 80);
    assert(!computeSingleResponseCostMicros(model, reported(100, 10, 80, 80)).priceKnown);

    // Either subset alone exceeds the total.
    assert(computeUsageTokenBreakdown(reported(100, 10, 0, 200)).nonCacheInputTokens == 0);
    assert(!computeSingleResponseCostMicros(model, reported(100, 10, 0, 200)).priceKnown);
    assert(computeUsageTokenBreakdown(reported(100, 10, 200, 0)).nonCacheInputTokens == 0);
    assert(!computeSingleResponseCostMicros(model, reported(100, 10, 200, 0)).priceKnown);

    // The boundary is consistent: the subsets exactly fill the total (no plain
    // input at all), and that is an exact price.
    breakdown = computeUsageTokenBreakdown(reported(100, 0, 30, 70));
    assert(breakdown.nonCacheInputTokens == 0);
    const auto exact = computeSingleResponseCostMicros(model, reported(100, 0, 30, 70));
    assert(exact.priceKnown);
    assert(exact.costMicros == 30ULL * 2'500'000ULL / 1'000'000ULL
        + 70ULL * 200'000ULL / 1'000'000ULL);

    // A turn that is ENTIRELY cache-write (or entirely cache-read) is consistent too.
    assert(computeUsageTokenBreakdown(reported(100, 0, 100, 0)).nonCacheInputTokens == 0);
    const auto allWritten = computeSingleResponseCostMicros(model, reported(100, 0, 100, 0));
    assert(allWritten.priceKnown);
    assert(allWritten.costMicros == 100ULL * 2'500'000ULL / 1'000'000ULL);
    assert(computeSingleResponseCostMicros(model, reported(100, 0, 0, 100)).priceKnown);

    // A CONSISTENT report whose cost still does not fit a uint64 saturates and
    // is flagged on its own (the subset check above cannot be what catches it).
    const std::uint64_t everything = ~std::uint64_t {0};
    const auto tooLarge = computeSingleResponseCostMicros(model, reported(everything, 0, everything, 0));
    assert(!tooLarge.priceKnown);
    assert(tooLarge.costMicros == everything);

    // Values near the unsigned range: the subset SUM would wrap to a tiny
    // number and fake a plausible plain share; the cost itself does not fit a
    // uint64. Neither may produce an exact-looking result.
    const std::uint64_t huge = ~std::uint64_t {0};
    breakdown = computeUsageTokenBreakdown(reported(10, 0, huge, 2));
    assert(breakdown.nonCacheInputTokens == 0);
    const auto overflowing = computeSingleResponseCostMicros(model, reported(10, 0, huge, 2));
    assert(!overflowing.priceKnown);
    assert(overflowing.costMicros == huge);   // saturated, never a wrapped small number
}

// AG-17 gpt-6-sol checkpoint M2b (flips the M2a characterization, when the tier was stored and
// validated but not applied): the long-context tier of a model now changes the cost of a Response
// ABOVE its threshold - and only of that Response. A response at or below the threshold is priced
// exactly like the same model without a tier.
void testLongContextPricingChangesTheCostOnlyAboveTheThreshold()
{
    const auto flat = priced(2'000'000, 10'000'000, 2'500'000, 200'000);
    Model tiered = flat;
    tiered.longContextPricing = LongContextPricing {272'000, 4'000'000, 15'000'000, 5'000'000, 400'000};

    // Above the threshold the whole Response goes long. Plain input 90,000 (300,000 - 10,000 - 200,000):
    // 90,000*4 + 10,000*5 + 200,000*0.4 + 1,000*15 = 505,000; the flat model charges
    // 90,000*2 + 10,000*2.5 + 200,000*0.2 + 1,000*10 = 255,000.
    const auto above = reported(300'000, 1'000, 10'000, 200'000);
    const auto aboveFlat = computeSingleResponseCostMicros(flat, above);
    const auto aboveTiered = computeSingleResponseCostMicros(tiered, above);
    assert(aboveFlat.costMicros == 255'000 && aboveFlat.priceKnown);
    assert(aboveTiered.costMicros == 505'000 && aboveTiered.priceKnown);

    // Exactly AT the threshold (272,000 is not above it) and far below it: the same as flat.
    //   at:  62,000*2 + 10,000*2.5 + 200,000*0.2 + 1,000*10 = 199,000
    //   low: 1,000*2 + 50*10 = 2,500
    const auto at = reported(272'000, 1'000, 10'000, 200'000);
    const auto far = reported(1'000, 50, 0, 0);
    for (const auto& value : {at, far}) {
        const auto expected = computeSingleResponseCostMicros(flat, value);
        const auto actual = computeSingleResponseCostMicros(tiered, value);
        assert(actual.costMicros == expected.costMicros && actual.priceKnown == expected.priceKnown);
    }
    assert(computeSingleResponseCostMicros(tiered, at).costMicros == 199'000);
    assert(computeSingleResponseCostMicros(tiered, far).costMicros == 2'500);
}

// The invariant itself, at its exact edges and near the unsigned range.
void testCacheBreakdownConsistencyBoundaries()
{
    const std::uint64_t maximum = ~std::uint64_t {0};
    assert(ai::models::TokenUsage {}.cacheBreakdownIsConsistent());
    assert(reported(100, 0, 30, 70).cacheBreakdownIsConsistent());     // fills the input exactly
    assert(reported(100, 0, 100, 0).cacheBreakdownIsConsistent());     // all cache-write
    assert(reported(100, 0, 0, 100).cacheBreakdownIsConsistent());     // all cache-read
    assert(!reported(100, 0, 31, 70).cacheBreakdownIsConsistent());    // one token past, together
    assert(!reported(100, 0, 30, 71).cacheBreakdownIsConsistent());
    assert(!reported(100, 0, 101, 0).cacheBreakdownIsConsistent());    // write alone past the input
    assert(!reported(100, 0, 0, 101).cacheBreakdownIsConsistent());    // read alone past the input
    assert(!reported(0, 0, 1, 0).cacheBreakdownIsConsistent());
    assert(!reported(0, 0, 0, 1).cacheBreakdownIsConsistent());
    assert(!reported(10, 0, maximum, 2).cacheBreakdownIsConsistent()); // the sum would wrap to 1
    assert(reported(maximum, 0, maximum, 0).cacheBreakdownIsConsistent());
    assert(reported(maximum, 0, 0, maximum).cacheBreakdownIsConsistent());
    assert(!reported(maximum, 0, maximum, 1).cacheBreakdownIsConsistent());
}

// grandProviderUsageTokens is a display/audit total: it saturates at the
// representable maximum and never wraps to a small number.
void testGrandProviderUsageSaturatesInsteadOfWrapping()
{
    const std::uint64_t maximum = ~std::uint64_t {0};

    // A VALID report: the raw input total plus the output exceed the range.
    auto breakdown = computeUsageTokenBreakdown(reported(maximum, 5, 0, 0));
    assert(breakdown.nonCacheInputTokens == maximum);
    assert(breakdown.grandProviderUsageTokens == maximum);   // not 4
    breakdown = computeUsageTokenBreakdown(
        reported(maximum, 5, maximum / 2, maximum - maximum / 2));
    assert(breakdown.nonCacheInputTokens == 0);
    assert(breakdown.grandProviderUsageTokens == maximum);

    // MALFORMED huge cache subsets.
    breakdown = computeUsageTokenBreakdown(reported(10, 0, maximum, maximum));
    assert(breakdown.nonCacheInputTokens == 0);
    assert(breakdown.grandProviderUsageTokens == maximum);
    breakdown = computeUsageTokenBreakdown(reported(10, 7, maximum, 2));
    assert(breakdown.grandProviderUsageTokens == maximum);
    breakdown = computeUsageTokenBreakdown(reported(10, maximum, 0, 0));
    assert(breakdown.grandProviderUsageTokens == maximum);

    // Ordinary values are unchanged: grand == raw input + output.
    breakdown = computeUsageTokenBreakdown(reported(1'000, 200, 300, 400));
    assert(breakdown.nonCacheInputTokens == 300);
    assert(breakdown.grandProviderUsageTokens == 1'200);
    // One below the representable maximum is still an exact sum.
    breakdown = computeUsageTokenBreakdown(reported(maximum - 6, 5, 0, 0));
    assert(breakdown.grandProviderUsageTokens == maximum - 1);
    breakdown = computeUsageTokenBreakdown(reported(maximum - 6, 5, maximum / 4, 0));
    assert(breakdown.grandProviderUsageTokens == maximum - 1);
}

// ---------------------------------------------------------------------------
// AG-17 gpt-6-sol checkpoint M2b-1: per-RESPONSE long-context pricing (pure functions only; no
// production call site uses them yet).
//   flat   : $2 input / $0.20 cache-read / $2.50 cache-write / $10 output   (per million tokens)
//   long   : $4 input / $0.40 cache-read / $5    cache-write / $15 output   above 272,000 input tokens
// ---------------------------------------------------------------------------

Model tieredModel()
{
    Model model = priced(2'000'000, 10'000'000, 2'500'000, 200'000);
    model.longContextPricing =
        LongContextPricing {272'000, 4'000'000, 15'000'000, 5'000'000, 400'000};
    return model;
}

// What each part of one response costs at the flat and at the long prices (hand-computed).
//   short(300k): plain 50k*2 + write 50k*2.5 + read 200k*0.2 + out 1k*10   = 100,000+125,000+40,000+10,000
//   long (300k): plain 50k*4 + write 50k*5   + read 200k*0.4 + out 1k*15   = 200,000+250,000+80,000+15,000
constexpr std::uint64_t kLongResponseCost = 545'000;

void testTheAccumulatorClassifiesEachResponseStrictlyByItsOwnInput()
{
    constexpr std::uint64_t threshold = 272'000;

    // Exactly at the threshold: SHORT. One token above: the WHOLE response, every category.
    ai::models::TokenUsage total;
    accumulateResponseUsage(total, reported(272'000, 1'000, 50'000, 200'000), threshold);
    assert(total.longContext.empty());
    assert(total.inputTokens == 272'000 && total.cacheReadInputTokens == 200'000);

    ai::models::TokenUsage above;
    accumulateResponseUsage(above, reported(272'001, 1'000, 50'000, 200'000), threshold);
    assert(above.longContext.inputTokens == 272'001);
    assert(above.longContext.outputTokens == 1'000);          // output goes long as well
    assert(above.longContext.cacheCreationInputTokens == 50'000);
    assert(above.longContext.cacheReadInputTokens == 200'000);
    assert(above.inputTokens == 272'001 && above.outputTokens == 1'000);   // totals unchanged in meaning

    // NEVER by the accumulated input: ten short responses add up far past the threshold.
    ai::models::TokenUsage many;
    for (int index = 0; index < 10; ++index) {
        accumulateResponseUsage(many, reported(100'000, 500, 0, 40'000), threshold);
    }
    assert(many.inputTokens == 1'000'000);
    assert(many.longContext.empty());

    // A threshold of 0 is flat pricing: nothing is ever classified.
    ai::models::TokenUsage flat;
    accumulateResponseUsage(flat, reported(5'000'000, 1'000, 0, 0), 0);
    assert(flat.inputTokens == 5'000'000 && flat.longContext.empty());

    // Saturating: totals and partition stop at the maximum instead of wrapping.
    const std::uint64_t maximum = ~std::uint64_t {0};
    ai::models::TokenUsage huge;
    accumulateResponseUsage(huge, reported(maximum, maximum, 0, 0), threshold);
    accumulateResponseUsage(huge, reported(maximum, maximum, 0, 0), threshold);
    assert(huge.inputTokens == maximum && huge.outputTokens == maximum);
    assert(huge.longContext.inputTokens == maximum && huge.longContext.outputTokens == maximum);
    assert(!huge.accountingTrusted);   // saturation lost the exact count (see the dedicated test below)
}

// A counter that saturates has LOST its exact value, so the aggregate stops being authoritative:
// no wrap, and no exact-looking price either - even when the (clamped) cost still fits a uint64.
void testSaturationLosesTheExactCountAndTheTrust()
{
    constexpr std::uint64_t threshold = 272'000;
    const std::uint64_t maximum = ~std::uint64_t {0};

    // Landing EXACTLY on the maximum still fits; adding zero to a maximal counter is not an overflow;
    // adding anything else is.
    ai::models::TokenUsage exact;
    accumulateResponseUsage(exact, reported(maximum - 5, 0, 0, 0), 0);
    accumulateResponseUsage(exact, reported(5, 0, 0, 0), 0);
    assert(exact.inputTokens == maximum && exact.accountingTrusted);
    accumulateResponseUsage(exact, reported(0, 0, 0, 0), 0);
    assert(exact.inputTokens == maximum && exact.accountingTrusted);
    accumulateResponseUsage(exact, reported(1, 0, 0, 0), 0);
    assert(exact.inputTokens == maximum && !exact.accountingTrusted);

    // The output counter alone.
    ai::models::TokenUsage output;
    accumulateResponseUsage(output, reported(0, maximum, 0, 0), 0);
    accumulateResponseUsage(output, reported(0, 0, 0, 0), 0);
    assert(output.outputTokens == maximum && output.accountingTrusted);
    accumulateResponseUsage(output, reported(0, 1, 0, 0), 0);
    assert(output.outputTokens == maximum && !output.accountingTrusted);

    // Every counter is still accumulated when another one overflows in the same call (no short-circuit).
    ai::models::TokenUsage both;
    accumulateResponseUsage(both, reported(maximum, 7, 0, 0), 0);
    accumulateResponseUsage(both, reported(1, 5, 0, 0), 0);
    assert(both.inputTokens == maximum && both.outputTokens == 12 && !both.accountingTrusted);

    // Untrusted stays untrusted whatever follows.
    accumulateResponseUsage(both, reported(10, 1, 0, 0), 0);
    assert(!both.accountingTrusted);

    // Two long responses at the maximum: the partition and the totals saturate together.
    ai::models::TokenUsage partitioned;
    accumulateResponseUsage(partitioned, reported(maximum, 0, 0, 0), threshold);
    assert(partitioned.accountingTrusted && partitioned.longContext.inputTokens == maximum);
    accumulateResponseUsage(partitioned, reported(maximum, 0, 0, 0), threshold);
    assert(partitioned.inputTokens == maximum && partitioned.longContext.inputTokens == maximum);
    assert(longContextPartitionIsValid(partitioned));   // structurally sound - only the count is lost
    assert(!partitioned.accountingTrusted);

    // The cost consequence. With prices this small the clamped cost STILL fits a uint64, so only the
    // lost trust can make it unknown. The control differs from the real aggregate in that flag alone.
    Model tiny = priced(1, 1, 1, 1);
    tiny.longContextPricing = LongContextPricing {threshold, 1, 1, 1, 1};

    const auto partitionedCost = computeAggregatedExecutionCostMicros(tiny, partitioned);
    assert(partitionedCost.costMicros > 0 && partitionedCost.costMicros < maximum);
    assert(!partitionedCost.priceKnown);
    ai::models::TokenUsage partitionedControl = partitioned;
    partitionedControl.accountingTrusted = true;
    const auto partitionedControlCost = computeAggregatedExecutionCostMicros(tiny, partitionedControl);
    assert(partitionedControlCost.priceKnown);
    assert(partitionedControlCost.costMicros == partitionedCost.costMicros);

    ai::models::TokenUsage unpartitioned;   // threshold 0: the no-partition path
    accumulateResponseUsage(unpartitioned, reported(maximum, 0, 0, 0), 0);
    accumulateResponseUsage(unpartitioned, reported(maximum, 0, 0, 0), 0);
    assert(unpartitioned.longContext.empty());
    const auto flatCost = computeAggregatedExecutionCostMicros(tiny, unpartitioned);
    assert(flatCost.costMicros > 0 && flatCost.costMicros < maximum);
    assert(!flatCost.priceKnown);
    ai::models::TokenUsage flatControl = unpartitioned;
    flatControl.accountingTrusted = true;
    const auto flatControlCost = computeAggregatedExecutionCostMicros(tiny, flatControl);
    assert(flatControlCost.priceKnown && flatControlCost.costMicros == flatCost.costMicros);
}

void testAWholeLongResponseIsPricedAtTheLongTariff()
{
    const Model model = tieredModel();

    const auto atThreshold = computeSingleResponseCostMicros(model, reported(272'000, 1'000, 50'000, 200'000));
    assert(atThreshold.priceKnown);
    assert(atThreshold.costMicros == 219'000);   // plain 22k*2 + write 125k + read 40k + out 10k, all FLAT

    const auto oneAbove = computeSingleResponseCostMicros(model, reported(272'001, 1'000, 50'000, 200'000));
    assert(oneAbove.priceKnown);
    assert(oneAbove.costMicros == 433'004);      // plain 22,001*4 + write 250k + read 80k + out 15k, all LONG

    const auto longResponse = computeSingleResponseCostMicros(model, reported(300'000, 1'000, 50'000, 200'000));
    assert(longResponse.priceKnown && longResponse.costMicros == kLongResponseCost);

    // The same response on the same model without a tier would be priced flat.
    Model flat = model;
    flat.longContextPricing = LongContextPricing {};
    const auto flatCost = computeSingleResponseCostMicros(flat, reported(300'000, 1'000, 50'000, 200'000));
    assert(flatCost.priceKnown && flatCost.costMicros == 275'000);
    assert(flatCost.costMicros < longResponse.costMicros);
}

void testAMixedExecutionIsPricedExactlyPerResponse()
{
    const Model model = tieredModel();
    const auto shortResponse = reported(100'000, 2'000, 10'000, 60'000);   // 117,000 at the flat prices
    const auto longResponse = reported(300'000, 1'000, 50'000, 200'000);   // 545,000 at the long prices
    constexpr std::uint64_t threshold = 272'000;

    ai::models::TokenUsage shortFirst;
    accumulateResponseUsage(shortFirst, shortResponse, threshold);
    accumulateResponseUsage(shortFirst, longResponse, threshold);
    ai::models::TokenUsage longFirst;
    accumulateResponseUsage(longFirst, longResponse, threshold);
    accumulateResponseUsage(longFirst, shortResponse, threshold);

    assert(longContextPartitionIsValid(shortFirst) && longContextPartitionIsValid(longFirst));
    assert(shortFirst.inputTokens == 400'000 && shortFirst.outputTokens == 3'000);
    assert(shortFirst.longContext.inputTokens == 300'000);

    const auto mixed = computeAggregatedExecutionCostMicros(model, shortFirst);
    assert(mixed.priceKnown);
    assert(mixed.costMicros == 117'000 + kLongResponseCost);                // exact, per response
    const auto reordered = computeAggregatedExecutionCostMicros(model, longFirst);
    assert(reordered.priceKnown && reordered.costMicros == mixed.costMicros);   // order never matters

    // The two wrong shortcuts are both different: the whole execution at flat prices, and the whole
    // execution as "long" because its SUMMED input crosses the threshold.
    ai::models::TokenUsage noPartition = shortFirst;
    noPartition.longContext = {};
    assert(computeAggregatedExecutionCostMicros(model, noPartition).costMicros == 392'000);
    assert(mixed.costMicros != 392'000);
    assert(mixed.costMicros != 769'000);

    // The single-response entry point is the same accumulator: it agrees with the aggregate of one.
    ai::models::TokenUsage one;
    accumulateResponseUsage(one, longResponse, threshold);
    const auto viaAggregate = computeAggregatedExecutionCostMicros(model, one);
    const auto viaSingle = computeSingleResponseCostMicros(model, longResponse);
    assert(viaSingle.costMicros == viaAggregate.costMicros && viaSingle.priceKnown == viaAggregate.priceKnown);
}

void testThePartitionIsASubsetOfTheTotalsByConstruction()
{
    const Model model = tieredModel();
    ai::models::TokenUsage total;
    std::uint64_t seed = 12345;
    const auto next = [&seed](std::uint64_t modulo) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return (seed >> 33) % modulo;
    };
    for (int index = 0; index < 200; ++index) {
        const std::uint64_t input = 1'000 + next(600'000);
        const std::uint64_t write = next(input / 3 + 1);
        const std::uint64_t read = next((input - write) / 2 + 1);
        accumulateResponseUsage(total, reported(input, next(5'000), write, read),
            model.longContextPricing.thresholdInputTokens);
        assert(longContextPartitionIsValid(total));
        assert(total.longContext.inputTokens <= total.inputTokens);
        assert(total.longContext.outputTokens <= total.outputTokens);
        assert(total.longContext.cacheCreationInputTokens <= total.cacheCreationInputTokens);
        assert(total.longContext.cacheReadInputTokens <= total.cacheReadInputTokens);
        assert(total.accountingTrusted);
    }
    assert(!total.longContext.empty());   // the sequence really crossed the threshold
}

void testAMalformedPartitionIsNeverKnownAndNeverWraps()
{
    const Model model = tieredModel();
    const std::uint64_t maximum = ~std::uint64_t {0};
    const auto withPartition = [](ai::models::TokenUsage total, ai::models::TokenUsage::LongContextShare share) {
        total.longContext = share;
        return total;
    };
    const auto rejected = [&](const ai::models::TokenUsage& usage) {
        assert(!longContextPartitionIsValid(usage));
        const auto cost = computeAggregatedExecutionCostMicros(model, usage);
        assert(!cost.priceKnown);       // never "known"
        assert(cost.costMicros == 0);   // and nothing provable is claimed - not a wrapped huge number either
    };

    const ai::models::TokenUsage total = reported(100'000, 5'000, 20'000, 30'000);
    // Each counter of the partition larger than its total.
    rejected(withPartition(total, {100'001, 0, 0, 0}));
    rejected(withPartition(total, {0, 5'001, 0, 0}));
    rejected(withPartition(total, {30'000, 0, 20'001, 0}));
    rejected(withPartition(total, {40'000, 0, 0, 30'001}));
    // The partition's own cache breakdown does not fit its own input.
    rejected(withPartition(reported(200'000, 0, 100'000, 100'000), {50'000, 0, 40'000, 40'000}));
    // The SHORT REMAINDER is inconsistent although the total and the partition each are not:
    // total 100 (all of it cached) minus a partition of 60 with 10 cached leaves 40 with 90 cached.
    rejected(withPartition(reported(100, 0, 0, 100), {60, 0, 0, 10}));
    // Near the unsigned range: the partition's cache sum would wrap to a tiny number.
    rejected(withPartition(reported(maximum, 0, maximum, 2), {maximum, 0, maximum, 2}));

    // Controls: sound partitions are valid, including the largest possible one.
    assert(longContextPartitionIsValid(withPartition(total, {40'000, 1'000, 5'000, 10'000})));
    assert(longContextPartitionIsValid(withPartition(total, {100'000, 5'000, 20'000, 30'000})));
    const ai::models::TokenUsage nearMax = reported(maximum, maximum, maximum / 2, maximum - maximum / 2);
    assert(longContextPartitionIsValid(withPartition(nearMax, {maximum, maximum - 1, maximum / 2, maximum - maximum / 2})));
    // ...and a valid but enormous one is not "known": its cost does not fit a uint64.
    const auto enormous = computeAggregatedExecutionCostMicros(
        model, withPartition(nearMax, {maximum, maximum - 1, maximum / 2, maximum - maximum / 2}));
    assert(!enormous.priceKnown && enormous.costMicros == maximum);
}

void testAZeroLongPriceOfAUsedCategoryIsAnUnknownLowerBound()
{
    const auto longResponse = reported(300'000, 1'000, 50'000, 200'000);   // uses every category

    Model noWritePrice = tieredModel();
    noWritePrice.longContextPricing.cacheWritePriceMicrosPerMillionTokens = 0;
    const auto missingWrite = computeSingleResponseCostMicros(noWritePrice, longResponse);
    assert(!missingWrite.priceKnown);
    assert(missingWrite.costMicros == kLongResponseCost - 250'000);   // the known part is kept

    Model noInputPrice = tieredModel();
    noInputPrice.longContextPricing.inputPriceMicrosPerMillionTokens = 0;
    const auto missingInput = computeSingleResponseCostMicros(noInputPrice, longResponse);
    assert(!missingInput.priceKnown && missingInput.costMicros == kLongResponseCost - 200'000);

    // A category that the long response does not use needs no price.
    const auto noWriteUsed = computeSingleResponseCostMicros(noWritePrice, reported(300'000, 1'000, 0, 200'000));
    assert(noWriteUsed.priceKnown);
    assert(noWriteUsed.costMicros == 100'000ULL * 4 + 200'000ULL * 400'000ULL / 1'000'000ULL + 15'000);

    // A fully unpriced tier prices nothing of a long response and is unknown; short responses stay exact.
    Model unpricedTier = tieredModel();
    unpricedTier.longContextPricing = LongContextPricing {272'000, 0, 0, 0, 0};
    const auto unpricedLong = computeSingleResponseCostMicros(unpricedTier, longResponse);
    assert(!unpricedLong.priceKnown && unpricedLong.costMicros == 0);
    assert(computeSingleResponseCostMicros(unpricedTier, reported(100'000, 2'000, 10'000, 60'000)).priceKnown);

    // The flat prices keep their own rule inside a mixed execution.
    Model noFlatRead = tieredModel();
    noFlatRead.cacheReadPriceMicrosPerMillionTokens = 0;
    ai::models::TokenUsage mixed;
    accumulateResponseUsage(mixed, reported(100'000, 2'000, 10'000, 60'000), 272'000);
    accumulateResponseUsage(mixed, longResponse, 272'000);
    assert(!computeAggregatedExecutionCostMicros(noFlatRead, mixed).priceKnown);
}

void testAFlatModelKeepsTheOriginalCalculationExactly()
{
    const Model flat = priced(2'000'000, 10'000'000, 2'500'000, 200'000);
    Model aboveButNoTier = flat;   // threshold 0: a flat model, whatever else it carries
    // Hand-computed flat prices ($2 / $10 / $2.5 cache-write / $0.2 cache-read per million):
    //   1,000 in, 200 out:                    1,000*2 + 200*10                              =     4,000
    //   300,000 in (50,000 w, 200,000 r), 1,000 out: 50,000*2 + 50,000*2.5 + 200,000*0.2 + 1,000*10 = 275,000
    //   5,000,000 in (1,000,000 w, 3,000,000 r), 9,000 out: 1,000,000*2 + 1,000,000*2.5
    //                                           + 3,000,000*0.2 + 9,000*10                 = 5,190,000
    struct Golden {
        ai::models::TokenUsage usage;
        std::uint64_t costMicros;
    };
    const Golden golden[] = {
        {reported(1'000, 200, 0, 0), 4'000},
        {reported(300'000, 1'000, 50'000, 200'000), 275'000},
        {reported(5'000'000, 9'000, 1'000'000, 3'000'000), 5'190'000},
        {reported(0, 0, 0, 0), 0},
    };
    for (const auto& item : golden) {
        const auto single = computeSingleResponseCostMicros(aboveButNoTier, item.usage);
        const auto aggregate = computeAggregatedExecutionCostMicros(flat, item.usage);
        assert(single.costMicros == item.costMicros && single.priceKnown);
        assert(aggregate.costMicros == item.costMicros && aggregate.priceKnown);
    }

    // Malformed reports stay malformed on a flat model: single-response classification and the
    // aggregate agree, and neither may look exact.
    const ai::models::TokenUsage malformed[] = {
        reported(100, 10, 80, 80),                                   // inconsistent cache subsets
        reported(~std::uint64_t {0}, 5, ~std::uint64_t {0}, 0),      // does not fit a uint64
    };
    for (const auto& usage : malformed) {
        const auto single = computeSingleResponseCostMicros(aboveButNoTier, usage);
        const auto aggregate = computeAggregatedExecutionCostMicros(flat, usage);
        assert(single.costMicros == aggregate.costMicros);
        assert(!single.priceKnown && !aggregate.priceKnown);
    }

    // A tiered model prices a response BELOW its threshold exactly as the flat model does:
    //   200,000 in (20,000 w, 100,000 r), 3,000 out: 80,000*2 + 20,000*2.5 + 100,000*0.2 + 3,000*10 = 260,000
    const auto below = reported(200'000, 3'000, 20'000, 100'000);
    const auto tieredBelow = computeSingleResponseCostMicros(tieredModel(), below);
    assert(tieredBelow.costMicros == 260'000 && tieredBelow.priceKnown);
    assert(computeSingleResponseCostMicros(flat, below).costMicros == 260'000);
}

void testAPartitionOnADisabledTierIsALowerBoundNeverFlat()
{
    const auto shortResponse = reported(100'000, 2'000, 10'000, 60'000);
    const auto longResponse = reported(300'000, 1'000, 50'000, 200'000);
    ai::models::TokenUsage mixed;
    accumulateResponseUsage(mixed, shortResponse, 272'000);
    accumulateResponseUsage(mixed, longResponse, 272'000);
    assert(!mixed.longContext.empty());

    // The model that prices it has NO tier (flat): only the short part, which can be proven, is priced;
    // the long part contributes 0 - it is not billed at the flat prices.
    const Model flat = priced(2'000'000, 10'000'000, 2'500'000, 200'000);
    const auto cost = computeAggregatedExecutionCostMicros(flat, mixed);
    assert(!cost.priceKnown);
    assert(cost.costMicros == 117'000);
    // ...and strictly less than what the whole usage would cost if the long part WERE priced flat:
    // the same totals with the partition cleared are one flat-priced aggregate.
    ai::models::TokenUsage everythingFlat = mixed;
    everythingFlat.longContext = {};
    assert(cost.costMicros < computeAggregatedExecutionCostMicros(flat, everythingFlat).costMicros);

    // ...and an invalid partition on a disabled tier proves nothing at all.
    ai::models::TokenUsage broken = mixed;
    broken.longContext.inputTokens = broken.inputTokens + 1;
    const auto brokenCost = computeAggregatedExecutionCostMicros(flat, broken);
    assert(!brokenCost.priceKnown && brokenCost.costMicros == 0);
}

void testTheAccumulatorNeverTrustsAPartitionOrAMalformedResponse()
{
    constexpr std::uint64_t threshold = 272'000;
    const Model model = tieredModel();

    // A response that arrives carrying a partition (a programming error): its numbers are NOT
    // copied, the response is classified by itself, and the aggregate is marked untrusted.
    ai::models::TokenUsage carried = reported(300'000, 1'000, 50'000, 200'000);
    carried.longContext.inputTokens = 7;
    ai::models::TokenUsage total;
    accumulateResponseUsage(total, carried, threshold);
    assert(!total.accountingTrusted);
    assert(total.longContext.inputTokens == 300'000);   // by classification, not 300,007
    const auto untrusted = computeAggregatedExecutionCostMicros(model, total);
    assert(!untrusted.priceKnown);                      // never exact-looking
    assert(untrusted.costMicros == kLongResponseCost);  // the arithmetic itself is still the best estimate

    // The same through the single-response entry point, below the threshold too.
    ai::models::TokenUsage shortCarried = reported(100'000, 2'000, 10'000, 60'000);
    shortCarried.longContext.outputTokens = 1;
    assert(!computeSingleResponseCostMicros(model, shortCarried).priceKnown);

    // Duration metadata is part of the Backend-derived partition too. It must
    // not hide behind LongContextShare::empty() merely because the older four
    // counters are zero.
    ai::models::TokenUsage durationCarried = reported(100'000, 2'000, 10'000, 60'000);
    durationCarried.longContext.cacheCreationDuration = {true, 0, 0};
    assert(!computeSingleResponseCostMicros(model, durationCarried).priceKnown);

    // A response whose own cache breakdown does not fit its input.
    ai::models::TokenUsage malformed;
    accumulateResponseUsage(malformed, reported(10, 1, 0, 50), threshold);
    assert(!malformed.accountingTrusted);
    assert(!computeAggregatedExecutionCostMicros(model, malformed).priceKnown);

    // Untrusted stays untrusted: later well-formed responses cannot repair it, and an untrusted
    // aggregate cannot be laundered by being added to another one.
    accumulateResponseUsage(malformed, reported(100'000, 2'000, 10'000, 60'000), threshold);
    assert(!malformed.accountingTrusted);
    ai::models::TokenUsage laundering;
    accumulateResponseUsage(laundering, malformed, threshold);
    assert(!laundering.accountingTrusted);

    // Clean responses leave it trusted.
    ai::models::TokenUsage clean;
    accumulateResponseUsage(clean, reported(100'000, 2'000, 10'000, 60'000), threshold);
    accumulateResponseUsage(clean, reported(300'000, 1'000, 50'000, 200'000), threshold);
    assert(clean.accountingTrusted);
}

// Pre-canary checkpoint of the gpt-6-sol migration (provider-free): the catalog entry the user is
// going to enter - exactly the official Standard rates and the long-context tier - must be a valid
// tier for its context window, and real-shaped OpenAI Responses `usage` objects, parsed by the REAL
// parser, must be priced exactly and as KNOWN: short, long, on both sides of the strict threshold,
// with and without cache-write, and per Response across a multi-turn execution.
//   $2 input / $0.20 cached input / $2.50 cache write / $10 output per million tokens, and
//   $4 / $0.40 / $5 / $15 for a Response above 272,000 input tokens (1,050,000 context window).
void testTheGpt6SolCatalogEntryPricesRealOpenAIResponsesExactlyAndAsKnown()
{
    Model entry;
    entry.id = "gpt-6-sol";
    entry.apiModelId = "gpt-6-sol";
    entry.contextWindowTokens = 1'050'000;
    entry.maxOutputTokens = 128'000;
    entry.inputPriceMicrosPerMillionTokens = 2'000'000;
    entry.outputPriceMicrosPerMillionTokens = 10'000'000;
    entry.cacheWritePriceMicrosPerMillionTokens = 2'500'000;
    entry.cacheReadPriceMicrosPerMillionTokens = 200'000;
    entry.longContextPricing = LongContextPricing {272'000, 4'000'000, 15'000'000, 5'000'000, 400'000};
    // The Backend would accept this tier for this window (the rule the service, the migration and
    // every read share), and it is a complete tier: no category is "not entered".
    assert(checkLongContextPricing(entry.contextWindowTokens, entry.longContextPricing)
        == LongContextPricingProblem::None);
    assert(entry.longContextPricing.thresholdInputTokens < entry.contextWindowTokens);

    const auto parse = [](const char* json) {
        return ai::providers::openai_detail::parseOpenAITokenUsage(nlohmann::json::parse(json));
    };
    const auto priceOf = [&](const char* json) { return computeSingleResponseCostMicros(entry, parse(json)); };

    // Short, with cache read and cache write: plain 30,000*2 + read 80,000*0.2 + write 10,000*2.5
    // + output 2,000*10 = 60,000 + 16,000 + 25,000 + 20,000.
    const auto shortResponse = priceOf(R"json({"usage": {"input_tokens": 120000, "output_tokens": 2000,
        "input_tokens_details": {"cached_tokens": 80000, "cache_write_tokens": 10000}}})json");
    assert(shortResponse.priceKnown && shortResponse.costMicros == 121'000);

    // The older wire shape without cache_write_tokens: plain 40,000*2 + read 60,000*0.2 + out 1,000*10.
    const auto noCacheWrite = priceOf(R"json({"usage": {"input_tokens": 100000, "output_tokens": 1000,
        "input_tokens_details": {"cached_tokens": 60000}}})json");
    assert(noCacheWrite.priceKnown && noCacheWrite.costMicros == 80'000 + 12'000 + 10'000);

    // Long: the whole Response at the long tariff - plain 50,000*4 + read 200,000*0.4 + write
    // 50,000*5 + out 1,000*15 = 200,000 + 80,000 + 250,000 + 15,000 (flat prices would give 275,000).
    const auto longResponse = priceOf(R"json({"usage": {"input_tokens": 300000, "output_tokens": 1000,
        "input_tokens_details": {"cached_tokens": 200000, "cache_write_tokens": 50000}}})json");
    assert(longResponse.priceKnown && longResponse.costMicros == 545'000);

    // The strict threshold: 272,000 is still short, 272,001 flips the whole Response.
    const auto atThreshold = priceOf(R"json({"usage": {"input_tokens": 272000, "output_tokens": 0}})json");
    const auto aboveThreshold = priceOf(R"json({"usage": {"input_tokens": 272001, "output_tokens": 0}})json");
    assert(atThreshold.priceKnown && atThreshold.costMicros == 544'000);
    assert(aboveThreshold.priceKnown && aboveThreshold.costMicros == 1'088'004);

    // A multi-turn execution is priced per Response, never by its summed input: one short and one
    // long Response = 121,000 + 545,000, not the whole 420,000 input at either tariff.
    ai::models::TokenUsage execution;
    accumulateResponseUsage(execution, parse(R"json({"usage": {"input_tokens": 120000,
        "output_tokens": 2000, "input_tokens_details": {"cached_tokens": 80000,
        "cache_write_tokens": 10000}}})json"), entry.longContextPricing.thresholdInputTokens);
    accumulateResponseUsage(execution, parse(R"json({"usage": {"input_tokens": 300000,
        "output_tokens": 1000, "input_tokens_details": {"cached_tokens": 200000,
        "cache_write_tokens": 50000}}})json"), entry.longContextPricing.thresholdInputTokens);
    const auto aggregate = computeAggregatedExecutionCostMicros(entry, execution);
    assert(aggregate.priceKnown && aggregate.costMicros == 121'000 + 545'000);
    assert(execution.inputTokens == 420'000 && execution.outputTokens == 3'000);
}

void testDurationAwareCacheWritesUseTheCorrectFlatAndLongContextPrices()
{
    Model model = priced(1'000'000, 1'000'000, 2'000'000, 500'000);
    model.cacheWriteOneHourPriceMicrosPerMillionTokens = 4'000'000;
    model.longContextPricing = LongContextPricing {
        200, 3'000'000, 6'000'000, 5'000'000, 700'000, 8'000'000};

    auto shortResponse = reported(100, 0, 30, 0);
    shortResponse.cacheCreationDuration = {true, 10, 20};
    auto longResponse = reported(300, 0, 50, 0);
    longResponse.cacheCreationDuration = {true, 15, 35};

    ai::models::TokenUsage total;
    accumulateResponseUsage(total, shortResponse, 200);
    accumulateResponseUsage(total, longResponse, 200);
    assert(total.cacheCreationDuration.known);
    assert(total.cacheCreationDuration.fiveMinuteTokens == 25);
    assert(total.cacheCreationDuration.oneHourTokens == 55);
    assert(total.longContext.cacheCreationDuration.known);
    assert(total.longContext.cacheCreationDuration.fiveMinuteTokens == 15);
    assert(total.longContext.cacheCreationDuration.oneHourTokens == 35);

    // Short: plain 70*1 + 5m 10*2 + 1h 20*4 = 170.
    // Long: plain 250*3 + 5m 15*5 + 1h 35*8 = 1,105.
    const auto cost = computeAggregatedExecutionCostMicros(model, total);
    assert(cost.priceKnown);
    assert(cost.costMicros == 1'275);
}

void testAggregatedUsageMergePreservesOnlyProvableDurationAccounting()
{
    auto shortResponse = reported(100, 1, 30, 20);
    shortResponse.cacheCreationDuration = {true, 10, 20};
    auto longResponse = reported(300, 2, 50, 40);
    longResponse.cacheCreationDuration = {true, 15, 35};

    ai::models::TokenUsage shortAggregate;
    ai::models::TokenUsage longAggregate;
    accumulateResponseUsage(shortAggregate, shortResponse, 200);
    accumulateResponseUsage(longAggregate, longResponse, 200);
    mergeAccumulatedUsage(shortAggregate, longAggregate);
    assert(shortAggregate.accountingTrusted);
    assert(shortAggregate.inputTokens == 400 && shortAggregate.outputTokens == 3);
    assert(shortAggregate.cacheCreationInputTokens == 80);
    assert(shortAggregate.cacheReadInputTokens == 60);
    assert(shortAggregate.cacheCreationDuration.known);
    assert(shortAggregate.cacheCreationDuration.fiveMinuteTokens == 25);
    assert(shortAggregate.cacheCreationDuration.oneHourTokens == 55);
    assert(shortAggregate.longContext.inputTokens == 300);
    assert(shortAggregate.longContext.cacheCreationDuration.known);
    assert(shortAggregate.longContext.cacheCreationDuration.fiveMinuteTokens == 15);
    assert(shortAggregate.longContext.cacheCreationDuration.oneHourTokens == 35);

    // An unpartitioned positive cache write makes the combined duration
    // unknown rather than pretending the known portion covers the whole.
    auto unknownPositive = reported(20, 0, 5, 0);
    mergeAccumulatedUsage(shortAggregate, unknownPositive);
    assert(shortAggregate.accountingTrusted);
    assert(!shortAggregate.cacheCreationDuration.known);

    // Unknown zero is neutral: a later independently accumulated total with
    // a complete split keeps that split.
    ai::models::TokenUsage known;
    accumulateResponseUsage(known, shortResponse, 200);
    mergeAccumulatedUsage(known, reported(10, 0, 0, 0));
    assert(known.cacheCreationDuration.known);
    assert(known.cacheCreationDuration.fiveMinuteTokens == 10);
    assert(known.cacheCreationDuration.oneHourTokens == 20);

    auto invalid = reported(20, 0, 5, 0);
    invalid.cacheCreationDuration = {true, 2, 2};
    mergeAccumulatedUsage(known, invalid);
    assert(!known.accountingTrusted);
    assert(!known.cacheCreationDuration.known);
}

} // namespace

int runCostCalculationTests()
{
    testNoCacheUsageMatchesPlainFormula();
    testCacheUsageIsBilledAtItsOwnRateNotThePlainRate();
    testZeroCachePriceWithRealCacheUsageIsHonestlyUnknown();
    testAllZeroPricesWithNoUsageIsKnownZeroCost();
    testMissingOutputPriceIsUnknownEvenWhenInputPriceIsSet();
    testGrandProviderUsageMatchesExactCsvFixture();
    testRawInputAndNonCacheInputProjectionDifferCorrectly();
    testNoCacheUsageBreakdownReducesToPlainTotals();
    testOpenAIWireUsageIsPricedInThreeSeparateInputCategories();
    testOpenAIWireUsageWithoutCachePricesIsHonestlyUnknown();
    testInconsistentCacheSubsetsNeverWrapAndAreNeverExactlyPriced();
    testLongContextPricingChangesTheCostOnlyAboveTheThreshold();
    testCacheBreakdownConsistencyBoundaries();
    testGrandProviderUsageSaturatesInsteadOfWrapping();
    testTheAccumulatorClassifiesEachResponseStrictlyByItsOwnInput();
    testSaturationLosesTheExactCountAndTheTrust();
    testAWholeLongResponseIsPricedAtTheLongTariff();
    testAMixedExecutionIsPricedExactlyPerResponse();
    testThePartitionIsASubsetOfTheTotalsByConstruction();
    testAMalformedPartitionIsNeverKnownAndNeverWraps();
    testAZeroLongPriceOfAUsedCategoryIsAnUnknownLowerBound();
    testAFlatModelKeepsTheOriginalCalculationExactly();
    testAPartitionOnADisabledTierIsALowerBoundNeverFlat();
    testTheAccumulatorNeverTrustsAPartitionOrAMalformedResponse();
    testTheGpt6SolCatalogEntryPricesRealOpenAIResponsesExactlyAndAsKnown();
    testDurationAwareCacheWritesUseTheCorrectFlatAndLongContextPrices();
    testAggregatedUsageMergePreservesOnlyProvableDurationAccounting();
    return 0;
}
