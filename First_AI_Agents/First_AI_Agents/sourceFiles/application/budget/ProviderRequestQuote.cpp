#include "ai/application/budget/ProviderRequestQuote.hpp"

#include "ai/application/budget/CacheDurationAccounting.hpp"
#include "ai/application/Sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace ai::application {

namespace {

common::Error error(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

constexpr std::uint64_t kMicrosScale = 1'000'000;
constexpr std::uint64_t kMaximum = std::numeric_limits<std::uint64_t>::max();

bool checkedAdd(std::uint64_t& value, std::uint64_t addend) noexcept
{
    if (addend > kMaximum - value) {
        value = kMaximum;
        return false;
    }
    value += addend;
    return true;
}

struct ConservativeQuoteCost {
    std::uint64_t wholeMicros {0};
    std::uint64_t remainderMillionthsOfMicro {0};
    bool priceKnown {true};
    bool arithmeticOverflow {false};
};

void addWorstCaseCategory(
    ConservativeQuoteCost& total,
    std::uint64_t tokens,
    std::uint64_t flatRate,
    const LongContextPricing& longPricing,
    std::uint64_t longRate) noexcept
{
    if (tokens == 0 || total.arithmeticOverflow) return;

    const bool longTierEnabled = longPricing.enabled();
    if (flatRate == 0 || (longTierEnabled && longRate == 0)) {
        total.priceKnown = false;
    }
    const std::uint64_t conservativeRate = longTierEnabled
        ? std::max(flatRate, longRate)
        : flatRate;

    // Reuse ACD-1's reviewed exact scaler. An unpartitioned cache-duration
    // component is mathematically the same tokens*rate/1M operation needed
    // for a quote category, including its exact sub-micro remainder and
    // overflow signal.
    const auto part = computeCacheCreationDurationCost(
        {tokens, false, 0, 0}, {conservativeRate, 0});
    if (part.arithmeticOverflow
        || !checkedAdd(total.wholeMicros, part.costMicros)
        || !checkedAdd(
            total.remainderMillionthsOfMicro,
            part.costRemainderMillionthsOfMicro)) {
        total.wholeMicros = kMaximum;
        total.remainderMillionthsOfMicro = 0;
        total.priceKnown = false;
        total.arithmeticOverflow = true;
        return;
    }

    const std::uint64_t carry = total.remainderMillionthsOfMicro / kMicrosScale;
    total.remainderMillionthsOfMicro %= kMicrosScale;
    if (!checkedAdd(total.wholeMicros, carry)) {
        total.remainderMillionthsOfMicro = 0;
        total.priceKnown = false;
        total.arithmeticOverflow = true;
    }
}

ConservativeQuoteCost worstCaseMissCost(
    const Model& model,
    models::PromptCachePolicy promptCache,
    std::uint64_t estimatedInputTokens,
    std::uint64_t cacheableInputTokens,
    std::uint64_t maximumOutputTokens) noexcept
{
    ConservativeQuoteCost total;
    const std::uint64_t plainInputTokens = estimatedInputTokens - cacheableInputTokens;
    addWorstCaseCategory(
        total,
        plainInputTokens,
        model.inputPriceMicrosPerMillionTokens,
        model.longContextPricing,
        model.longContextPricing.inputPriceMicrosPerMillionTokens);

    if (cacheableInputTokens > 0) {
        const bool longTierEnabled = model.longContextPricing.enabled();
        const auto addCacheableWorstCase = [&](
                                               std::uint64_t flatWriteRate,
                                               std::uint64_t longWriteRate) {
            // Anthropic may silently skip prompt caching when the stable prefix
            // is shorter than the model's minimum cacheable length. Therefore
            // cacheable input must be quoted against both possible billings:
            // cache creation and ordinary input. A missing rate on either path
            // makes the monetary ceiling unknown rather than optimistic.
            if (model.inputPriceMicrosPerMillionTokens == 0
                || flatWriteRate == 0
                || (longTierEnabled
                    && (model.longContextPricing.inputPriceMicrosPerMillionTokens == 0
                        || longWriteRate == 0))) {
                total.priceKnown = false;
            }
            addWorstCaseCategory(
                total,
                cacheableInputTokens,
                std::max(model.inputPriceMicrosPerMillionTokens, flatWriteRate),
                model.longContextPricing,
                std::max(
                    model.longContextPricing.inputPriceMicrosPerMillionTokens,
                    longWriteRate));
        };
        if (promptCache.mode == models::PromptCacheMode::EphemeralFiveMinutes) {
            addCacheableWorstCase(
                model.cacheWritePriceMicrosPerMillionTokens,
                model.longContextPricing.cacheWritePriceMicrosPerMillionTokens);
        } else {
            addCacheableWorstCase(
                model.cacheWriteOneHourPriceMicrosPerMillionTokens,
                model.longContextPricing.cacheWriteOneHourPriceMicrosPerMillionTokens);
        }
    }

    addWorstCaseCategory(
        total,
        maximumOutputTokens,
        model.outputPriceMicrosPerMillionTokens,
        model.longContextPricing,
        model.longContextPricing.outputPriceMicrosPerMillionTokens);

    // A monetary cap is an upper bound, unlike persisted actual-cost analytics.
    // Round the combined exact remainder UP once, after all categories.
    if (!total.arithmeticOverflow && total.remainderMillionthsOfMicro > 0) {
        if (!checkedAdd(total.wholeMicros, 1)) {
            total.remainderMillionthsOfMicro = 0;
            total.priceKnown = false;
            total.arithmeticOverflow = true;
        }
    }
    return total;
}

} // namespace

ProviderRequestPricingIdentity providerRequestPricingIdentity(
    const Model& model,
    models::PromptCachePolicy promptCache)
{
    return {
        model.providerId,
        model.id,
        model.apiModelId,
        promptCache,
        model.contextWindowTokens,
        model.maxOutputTokens,
        model.inputPriceMicrosPerMillionTokens,
        model.outputPriceMicrosPerMillionTokens,
        model.cacheWritePriceMicrosPerMillionTokens,
        model.cacheReadPriceMicrosPerMillionTokens,
        model.cacheWriteOneHourPriceMicrosPerMillionTokens,
        model.longContextPricing};
}

std::string providerRequestPricingIdentityFingerprint(
    const ProviderRequestPricingIdentity& identity)
{
    const nlohmann::ordered_json json = {
        {"schemaVersion", "provider_request_pricing_identity.v1"},
        {"providerId", identity.providerId},
        {"modelId", identity.modelId},
        {"apiModelId", identity.apiModelId},
        {"promptCacheMode", models::toString(identity.promptCache.mode)},
        {"contextWindowTokens", identity.contextWindowTokens},
        {"modelMaxOutputTokens", identity.modelMaxOutputTokens},
        {"inputPriceMicrosPerMillionTokens", identity.inputPriceMicrosPerMillionTokens},
        {"outputPriceMicrosPerMillionTokens", identity.outputPriceMicrosPerMillionTokens},
        {"cacheWritePriceMicrosPerMillionTokens", identity.cacheWritePriceMicrosPerMillionTokens},
        {"cacheReadPriceMicrosPerMillionTokens", identity.cacheReadPriceMicrosPerMillionTokens},
        {"cacheWriteOneHourPriceMicrosPerMillionTokens", identity.cacheWriteOneHourPriceMicrosPerMillionTokens},
        {"longContext", {
            {"thresholdInputTokens", identity.longContextPricing.thresholdInputTokens},
            {"inputPriceMicrosPerMillionTokens", identity.longContextPricing.inputPriceMicrosPerMillionTokens},
            {"outputPriceMicrosPerMillionTokens", identity.longContextPricing.outputPriceMicrosPerMillionTokens},
            {"cacheWritePriceMicrosPerMillionTokens", identity.longContextPricing.cacheWritePriceMicrosPerMillionTokens},
            {"cacheReadPriceMicrosPerMillionTokens", identity.longContextPricing.cacheReadPriceMicrosPerMillionTokens},
            {"cacheWriteOneHourPriceMicrosPerMillionTokens", identity.longContextPricing.cacheWriteOneHourPriceMicrosPerMillionTokens}
        }}
    };
    return sha256Hex(json.dump());
}

common::Result<ProviderRequestWorstCaseQuote> quoteProviderRequestWorstCaseMiss(
    const Model& model,
    models::PromptCachePolicy promptCache,
    std::uint64_t estimatedInputTokens,
    std::uint64_t cacheableInputTokens,
    std::uint64_t maximumOutputTokens)
{
    if (!models::isKnown(promptCache.mode)) {
        return common::Result<ProviderRequestWorstCaseQuote>::failure(
            error("provider_quote.invalid_cache_policy", "The prompt-cache policy is unknown"));
    }
    if (maximumOutputTokens == 0 || maximumOutputTokens > model.maxOutputTokens) {
        return common::Result<ProviderRequestWorstCaseQuote>::failure(
            error("provider_quote.invalid_output_limit", "The quote output limit is zero or exceeds the model limit"));
    }
    if (estimatedInputTokens > model.contextWindowTokens
        || maximumOutputTokens > model.contextWindowTokens - estimatedInputTokens) {
        return common::Result<ProviderRequestWorstCaseQuote>::failure(
            error("provider_quote.context_window_exceeded", "The quoted input and output do not fit the model context window"));
    }
    if (cacheableInputTokens > estimatedInputTokens) {
        return common::Result<ProviderRequestWorstCaseQuote>::failure(
            error("provider_quote.invalid_cacheable_tokens", "Cacheable input exceeds total input"));
    }
    if ((promptCache.enabled() && cacheableInputTokens == 0)
        || (!promptCache.enabled() && cacheableInputTokens != 0)) {
        return common::Result<ProviderRequestWorstCaseQuote>::failure(
            error("provider_quote.cache_policy_mismatch", "The cache policy and cacheable input do not match"));
    }

    const ConservativeQuoteCost cost = worstCaseMissCost(
        model,
        promptCache,
        estimatedInputTokens,
        cacheableInputTokens,
        maximumOutputTokens);
    ProviderRequestWorstCaseQuote quote;
    quote.pricingIdentity = providerRequestPricingIdentity(model, promptCache);
    quote.pricingIdentityFingerprint = providerRequestPricingIdentityFingerprint(quote.pricingIdentity);
    quote.estimatedInputTokens = estimatedInputTokens;
    quote.cacheableInputTokens = cacheableInputTokens;
    quote.maximumOutputTokens = maximumOutputTokens;
    quote.estimatedCostMicros = cost.wholeMicros;
    quote.priceKnown = cost.priceKnown;
    return common::Result<ProviderRequestWorstCaseQuote>::ok(std::move(quote));
}

} // namespace ai::application
