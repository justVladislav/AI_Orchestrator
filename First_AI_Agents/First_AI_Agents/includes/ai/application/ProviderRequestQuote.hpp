#pragma once

#include "ai/application/CostCalculation.hpp"
#include "ai/application/catalog/ProviderCatalogTypes.hpp"
#include "ai/common/Result.hpp"
#include "ai/models/PromptCachePolicy.hpp"

#include <cstdint>
#include <string>

namespace ai::application {

// Immutable price/cache identity carried by a quote. It intentionally keeps
// cache-read rates even though a worst-case MISS uses none: a later persisted
// request can audit the complete pricing policy that was authorised.
struct ProviderRequestPricingIdentity {
    ProviderId providerId;
    ModelId modelId;
    std::string apiModelId;
    models::PromptCachePolicy promptCache;
    std::uint64_t contextWindowTokens {0};
    std::uint64_t modelMaxOutputTokens {0};
    std::uint64_t inputPriceMicrosPerMillionTokens {0};
    std::uint64_t outputPriceMicrosPerMillionTokens {0};
    std::uint64_t cacheWritePriceMicrosPerMillionTokens {0};
    std::uint64_t cacheReadPriceMicrosPerMillionTokens {0};
    std::uint64_t cacheWriteOneHourPriceMicrosPerMillionTokens {0};
    LongContextPricing longContextPricing;

    friend bool operator==(const ProviderRequestPricingIdentity&, const ProviderRequestPricingIdentity&) = default;
};

struct ProviderRequestWorstCaseQuote {
    ProviderRequestPricingIdentity pricingIdentity;
    std::string pricingIdentityFingerprint;
    std::uint64_t estimatedInputTokens {0};
    std::uint64_t cacheableInputTokens {0};
    std::uint64_t maximumOutputTokens {0};
    std::uint64_t estimatedCostMicros {0};
    bool priceKnown {false};
};

[[nodiscard]] ProviderRequestPricingIdentity providerRequestPricingIdentity(
    const Model& model,
    models::PromptCachePolicy promptCache);

[[nodiscard]] std::string providerRequestPricingIdentityFingerprint(
    const ProviderRequestPricingIdentity& identity);

// Prices the authorised upper bound of ONE response. With caching enabled the
// complete cacheable stable prefix is assumed to be a NEW write (never a hit),
// and maximumOutputTokens is assumed to be fully consumed. This is deliberately
// conservative; provider-reported usage remains authoritative after the call.
[[nodiscard]] common::Result<ProviderRequestWorstCaseQuote>
quoteProviderRequestWorstCaseMiss(
    const Model& model,
    models::PromptCachePolicy promptCache,
    std::uint64_t estimatedInputTokens,
    std::uint64_t cacheableInputTokens,
    std::uint64_t maximumOutputTokens);

} // namespace ai::application
