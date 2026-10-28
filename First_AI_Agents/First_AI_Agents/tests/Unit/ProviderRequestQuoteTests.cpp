#include "ai/application/budget/ProviderRequestQuote.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ai::application;

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

Model pricedModel()
{
    Model model;
    model.providerId = "anthropic";
    model.id = "claude";
    model.apiModelId = "claude-test";
    model.contextWindowTokens = 1'000;
    model.maxOutputTokens = 500;
    model.inputPriceMicrosPerMillionTokens = 2'000'000;
    model.outputPriceMicrosPerMillionTokens = 10'000'000;
    model.cacheWritePriceMicrosPerMillionTokens = 2'500'000;
    model.cacheReadPriceMicrosPerMillionTokens = 200'000;
    model.cacheWriteOneHourPriceMicrosPerMillionTokens = 4'000'000;
    model.longContextPricing = {
        200, 3'000'000, 15'000'000, 5'000'000, 300'000, 8'000'000};
    return model;
}

ai::models::PromptCachePolicy policy(ai::models::PromptCacheMode mode)
{
    return {mode};
}

void testDisabledAndBothDurationsQuoteTheWorstCaseMiss()
{
    const Model model = pricedModel();
    const auto disabled = quoteProviderRequestWorstCaseMiss(
        model, policy(ai::models::PromptCacheMode::Disabled), 100, 0, 50);
    const auto five = quoteProviderRequestWorstCaseMiss(
        model, policy(ai::models::PromptCacheMode::EphemeralFiveMinutes), 100, 60, 50);
    const auto hour = quoteProviderRequestWorstCaseMiss(
        model, policy(ai::models::PromptCacheMode::EphemeralOneHour), 100, 60, 50);
    require(disabled.success() && disabled.value().priceKnown
            && disabled.value().estimatedCostMicros == 1'050,
        "disabled: the higher applicable long-context rates were not quoted");
    require(five.success() && five.value().priceKnown
            && five.value().estimatedCostMicros == 1'170,
        "five-minute: cache-write miss was not quoted");
    require(hour.success() && hour.value().priceKnown
            && hour.value().estimatedCostMicros == 1'350,
        "one-hour: cache-write miss was not quoted at the one-hour rate");
    require(five.value().maximumOutputTokens == 50
            && five.value().cacheableInputTokens == 60,
        "quote: authorised token ceilings were not returned");
}

void testLongContextUsesTheWholeResponseTier()
{
    const auto quote = quoteProviderRequestWorstCaseMiss(
        pricedModel(), policy(ai::models::PromptCacheMode::EphemeralOneHour),
        201, 60, 50);
    require(quote.success() && quote.value().priceKnown
            && quote.value().estimatedCostMicros == 1'653,
        "long context: the whole response was not priced at long-context rates");

    const auto belowThresholdEstimate = quoteProviderRequestWorstCaseMiss(
        pricedModel(), policy(ai::models::PromptCacheMode::EphemeralOneHour),
        199, 60, 50);
    require(belowThresholdEstimate.success()
            && belowThresholdEstimate.value().priceKnown
            && belowThresholdEstimate.value().estimatedCostMicros == 1'647,
        "long context: an approximate estimate below the threshold was allowed to underquote");
}

void testQuoteRoundsTheCombinedCostUpOnce()
{
    Model model = pricedModel();
    model.longContextPricing = {};
    model.inputPriceMicrosPerMillionTokens = 100'000;
    model.outputPriceMicrosPerMillionTokens = 500'000;
    const auto quote = quoteProviderRequestWorstCaseMiss(
        model, policy(ai::models::PromptCacheMode::Disabled), 1, 0, 1);
    require(quote.success() && quote.value().priceKnown
            && quote.value().estimatedCostMicros == 1,
        "rounding: a fractional-micro upper bound was rounded down");
}

void testCacheableInputAlsoCoversTheProviderSkippingCaching()
{
    Model model = pricedModel();
    model.longContextPricing = {};
    model.inputPriceMicrosPerMillionTokens = 5'000'000;
    model.cacheWritePriceMicrosPerMillionTokens = 1'000'000;
    model.cacheWriteOneHourPriceMicrosPerMillionTokens = 2'000'000;
    const auto quote = quoteProviderRequestWorstCaseMiss(
        model, policy(ai::models::PromptCacheMode::EphemeralOneHour), 100, 100, 1);
    require(quote.success() && quote.value().priceKnown
            && quote.value().estimatedCostMicros == 510,
        "cache fallback: a short prefix silently billed as ordinary input was underquoted");

    model.inputPriceMicrosPerMillionTokens = 0;
    const auto missingFallbackRate = quoteProviderRequestWorstCaseMiss(
        model, policy(ai::models::PromptCacheMode::EphemeralOneHour), 100, 100, 1);
    require(missingFallbackRate.success() && !missingFallbackRate.value().priceKnown,
        "cache fallback: a missing ordinary-input rate was treated as a known ceiling");
}

void testPricingIdentityPinsPolicyAndEveryApplicableRate()
{
    Model model = pricedModel();
    const auto first = quoteProviderRequestWorstCaseMiss(
        model, policy(ai::models::PromptCacheMode::EphemeralOneHour), 100, 60, 50);
    const auto same = quoteProviderRequestWorstCaseMiss(
        model, policy(ai::models::PromptCacheMode::EphemeralOneHour), 100, 60, 50);
    require(first.success() && same.success()
            && first.value().pricingIdentity == same.value().pricingIdentity
            && first.value().pricingIdentityFingerprint
                == same.value().pricingIdentityFingerprint,
        "identity: identical pricing did not produce one stable identity");

    require(first.value().pricingIdentityFingerprint
            == "8f3ca03bc3b9685794bc6c2adee5d5be3260d70954c3569f1f0cd32b863914cd",
        "identity: the canonical pricing fingerprint changed across a restart");

    const auto base = first.value().pricingIdentity;
    std::vector<ProviderRequestPricingIdentity> changedIdentities;
    auto changed = base;
    changed.providerId += "-changed";
    changedIdentities.push_back(changed);
    changed = base;
    changed.modelId += "-changed";
    changedIdentities.push_back(changed);
    changed = base;
    changed.apiModelId += "-changed";
    changedIdentities.push_back(changed);
    changed = base;
    changed.contextWindowTokens += 1;
    changedIdentities.push_back(changed);
    changed = base;
    changed.modelMaxOutputTokens += 1;
    changedIdentities.push_back(changed);
    changed = base;
    changed.inputPriceMicrosPerMillionTokens += 1;
    changedIdentities.push_back(changed);
    changed = base;
    changed.outputPriceMicrosPerMillionTokens += 1;
    changedIdentities.push_back(changed);
    changed = base;
    changed.cacheWritePriceMicrosPerMillionTokens += 1;
    changedIdentities.push_back(changed);
    changed = base;
    changed.cacheReadPriceMicrosPerMillionTokens += 1;
    changedIdentities.push_back(changed);
    changed = base;
    changed.cacheWriteOneHourPriceMicrosPerMillionTokens += 1;
    changedIdentities.push_back(changed);
    changed = base;
    changed.longContextPricing.thresholdInputTokens += 1;
    changedIdentities.push_back(changed);
    changed = base;
    changed.longContextPricing.inputPriceMicrosPerMillionTokens += 1;
    changedIdentities.push_back(changed);
    changed = base;
    changed.longContextPricing.outputPriceMicrosPerMillionTokens += 1;
    changedIdentities.push_back(changed);
    changed = base;
    changed.longContextPricing.cacheWritePriceMicrosPerMillionTokens += 1;
    changedIdentities.push_back(changed);
    changed = base;
    changed.longContextPricing.cacheReadPriceMicrosPerMillionTokens += 1;
    changedIdentities.push_back(changed);
    changed = base;
    changed.longContextPricing.cacheWriteOneHourPriceMicrosPerMillionTokens += 1;
    changedIdentities.push_back(changed);

    for (const auto& identity : changedIdentities) {
        require(providerRequestPricingIdentityFingerprint(identity)
                != first.value().pricingIdentityFingerprint,
            "identity: a pinned model or price field did not change the fingerprint");
    }

    model.cacheReadPriceMicrosPerMillionTokens += 1;
    const auto changedRead = quoteProviderRequestWorstCaseMiss(
        model, policy(ai::models::PromptCacheMode::EphemeralOneHour), 100, 60, 50);
    require(changedRead.success()
            && changedRead.value().estimatedCostMicros
                == first.value().estimatedCostMicros
            && changedRead.value().pricingIdentityFingerprint
                != first.value().pricingIdentityFingerprint,
        "identity: the unused-on-miss cache-read rate was not pinned");

    const auto changedPolicy = quoteProviderRequestWorstCaseMiss(
        pricedModel(), policy(ai::models::PromptCacheMode::EphemeralFiveMinutes),
        100, 60, 50);
    require(changedPolicy.success()
            && changedPolicy.value().pricingIdentityFingerprint
                != first.value().pricingIdentityFingerprint,
        "identity: cache duration was not pinned");
}

void testUnknownOrIncoherentRequestsFailClosed()
{
    const Model model = pricedModel();
    const auto code = [](const auto& result) {
        return result.success() ? std::string {} : result.error()->code;
    };
    require(code(quoteProviderRequestWorstCaseMiss(
                model, policy(static_cast<ai::models::PromptCacheMode>(99)), 100, 0, 50))
            == "provider_quote.invalid_cache_policy",
        "validation: unknown policy was accepted");
    require(code(quoteProviderRequestWorstCaseMiss(
                model, policy(ai::models::PromptCacheMode::Disabled), 100, 1, 50))
            == "provider_quote.cache_policy_mismatch",
        "validation: disabled cache accepted cacheable input");
    require(code(quoteProviderRequestWorstCaseMiss(
                model, policy(ai::models::PromptCacheMode::EphemeralOneHour), 100, 0, 50))
            == "provider_quote.cache_policy_mismatch",
        "validation: enabled cache accepted an empty prefix");
    require(code(quoteProviderRequestWorstCaseMiss(
                model, policy(ai::models::PromptCacheMode::EphemeralOneHour), 100, 101, 50))
            == "provider_quote.invalid_cacheable_tokens",
        "validation: cacheable input exceeded total input");
    require(code(quoteProviderRequestWorstCaseMiss(
                model, policy(ai::models::PromptCacheMode::Disabled), 900, 0, 101))
            == "provider_quote.context_window_exceeded",
        "validation: quote exceeded the context window");
    require(code(quoteProviderRequestWorstCaseMiss(
                model, policy(ai::models::PromptCacheMode::Disabled), 100, 0, 501))
            == "provider_quote.invalid_output_limit",
        "validation: quote exceeded the model output limit");
}

void testMissingUsedPriceIsNeverCalledFree()
{
    Model model = pricedModel();
    model.cacheWriteOneHourPriceMicrosPerMillionTokens = 0;
    const auto quote = quoteProviderRequestWorstCaseMiss(
        model, policy(ai::models::PromptCacheMode::EphemeralOneHour), 100, 60, 50);
    require(quote.success() && !quote.value().priceKnown
            && quote.value().estimatedCostMicros == 1'350,
        "unknown price: known lower bound or unknown marker is wrong");

    model = pricedModel();
    model.longContextPricing.outputPriceMicrosPerMillionTokens = 0;
    const auto missingLongRate = quoteProviderRequestWorstCaseMiss(
        model, policy(ai::models::PromptCacheMode::Disabled), 100, 0, 50);
    require(missingLongRate.success() && !missingLongRate.value().priceKnown,
        "unknown price: a missing applicable long-context rate looked known below the threshold");
}

} // namespace

int main()
{
    try {
        testDisabledAndBothDurationsQuoteTheWorstCaseMiss();
        testLongContextUsesTheWholeResponseTier();
        testQuoteRoundsTheCombinedCostUpOnce();
        testCacheableInputAlsoCoversTheProviderSkippingCaching();
        testPricingIdentityPinsPolicyAndEveryApplicableRate();
        testUnknownOrIncoherentRequestsFailClosed();
        testMissingUsedPriceIsNeverCalledFree();
        std::cout << "ProviderRequestQuoteTests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ProviderRequestQuoteTests: " << error.what() << '\n';
        return 1;
    }
}
