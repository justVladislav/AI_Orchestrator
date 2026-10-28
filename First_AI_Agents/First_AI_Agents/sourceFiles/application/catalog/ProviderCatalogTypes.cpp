#include "ai/application/catalog/ProviderCatalogTypes.hpp"

namespace ai::application {

bool isKnownProviderKind(std::string_view kind) noexcept
{
    return kind == "openai"
        || kind == "anthropic"
        || kind == "openai_compatible"
        || kind == "local";
}

LongContextPricingProblem checkLongContextPricing(
    std::uint64_t contextWindowTokens,
    const LongContextPricing& pricing) noexcept
{
    if (!pricing.enabled()) {
        const bool anyPrice =
            pricing.inputPriceMicrosPerMillionTokens != 0
            || pricing.outputPriceMicrosPerMillionTokens != 0
            || pricing.cacheWritePriceMicrosPerMillionTokens != 0
            || pricing.cacheReadPriceMicrosPerMillionTokens != 0
            || pricing.cacheWriteOneHourPriceMicrosPerMillionTokens != 0;
        return anyPrice ? LongContextPricingProblem::PricesWithoutThreshold
                        : LongContextPricingProblem::None;
    }
    return pricing.thresholdInputTokens >= contextWindowTokens
        ? LongContextPricingProblem::ThresholdOutOfRange
        : LongContextPricingProblem::None;
}

const char* longContextPricingProblemCode(LongContextPricingProblem problem) noexcept
{
    switch (problem) {
    case LongContextPricingProblem::PricesWithoutThreshold:
        return "model.long_context_prices_without_threshold";
    case LongContextPricingProblem::ThresholdOutOfRange:
        return "model.long_context_threshold_out_of_range";
    case LongContextPricingProblem::None:
        break;
    }
    return "";
}

const char* longContextPricingProblemMessage(LongContextPricingProblem problem) noexcept
{
    switch (problem) {
    case LongContextPricingProblem::PricesWithoutThreshold:
        return "Long-context prices require a long-context threshold; "
               "set the threshold or clear the prices";
    case LongContextPricingProblem::ThresholdOutOfRange:
        return "The long-context threshold must be strictly below the model's context window";
    case LongContextPricingProblem::None:
        break;
    }
    return "";
}

} // namespace ai::application
