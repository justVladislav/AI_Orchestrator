#include "ai/application/FollowUpComposerQuote.hpp"

#include "ai/application/conversations/ChatContextAssembler.hpp"
#include "ai/application/Sha256.hpp"

#include <algorithm>

namespace ai::application {
namespace {

common::Result<FollowUpComposerQuote> fail(std::string code, std::string message)
{
    return common::Result<FollowUpComposerQuote>::failure({std::move(code), std::move(message)});
}

void appendFramed(std::string& material, std::string_view value)
{
    material += std::to_string(value.size());
    material.push_back(':');
    material.append(value);
}

} // namespace

models::PromptCachePolicy followUpPromptCachePolicy(std::string_view providerKind) noexcept
{
    return {providerKind == "anthropic" ? models::PromptCacheMode::EphemeralOneHour : models::PromptCacheMode::Disabled};
}

FollowUpProviderExecutionIdentity followUpProviderExecutionIdentity(const Provider& provider)
{
    return {provider.id, provider.kind, provider.baseUrl, provider.credentialReference};
}

std::string followUpProviderExecutionIdentityFingerprint(const FollowUpProviderExecutionIdentity& identity)
{
    std::string material = "follow_up_provider_execution_identity.v1";
    appendFramed(material, identity.providerId);
    appendFramed(material, identity.kind);
    appendFramed(material, identity.baseUrl);
    // Presence first, so "no reference" and an empty reference are different identities.
    material += identity.credentialReference.has_value() ? "+" : "-";
    if (identity.credentialReference.has_value()) appendFramed(material, *identity.credentialReference);
    return sha256Hex(material);
}

const char* toString(FollowUpQuoteDrift value) noexcept
{
    switch (value) {
    case FollowUpQuoteDrift::None: return "none";
    case FollowUpQuoteDrift::Context: return "context";
    case FollowUpQuoteDrift::Provider: return "provider";
    case FollowUpQuoteDrift::Pricing: return "pricing";
    case FollowUpQuoteDrift::Limits: return "limits";
    }
    return "unknown";
}

FollowUpQuoteDrift compareFollowUpQuotes(
    const FollowUpComposerQuote& stored,
    const FollowUpComposerQuote& fresh) noexcept
{
    if (stored.policyId != fresh.policyId || stored.contextFingerprint != fresh.contextFingerprint) {
        return FollowUpQuoteDrift::Context;
    }
    if (!(stored.providerIdentity == fresh.providerIdentity)
        || stored.providerIdentityFingerprint != fresh.providerIdentityFingerprint) {
        return FollowUpQuoteDrift::Provider;
    }
    if (!(stored.worstCase.pricingIdentity == fresh.worstCase.pricingIdentity)
        || stored.worstCase.pricingIdentityFingerprint != fresh.worstCase.pricingIdentityFingerprint) {
        return FollowUpQuoteDrift::Pricing;
    }
    if (stored.worstCase.estimatedInputTokens != fresh.worstCase.estimatedInputTokens
        || stored.worstCase.cacheableInputTokens != fresh.worstCase.cacheableInputTokens
        || stored.worstCase.maximumOutputTokens != fresh.worstCase.maximumOutputTokens
        || stored.worstCase.estimatedCostMicros != fresh.worstCase.estimatedCostMicros
        || stored.worstCase.priceKnown != fresh.worstCase.priceKnown
        || stored.capMicros != fresh.capMicros) {
        return FollowUpQuoteDrift::Limits;
    }
    return FollowUpQuoteDrift::None;
}

common::Result<FollowUpComposerQuote> quoteFollowUpComposerRequest(
    const FollowUpComposerContext& context,
    const Provider& provider,
    const Model& model,
    std::optional<std::uint64_t> userCapMicros)
{
    // The quote prices the exact provider bytes, so it first proves that they are internally consistent: the fixed
    // system prompt (the only cacheable prefix), a fingerprint that seals exactly these two strings, and the estimate
    // for them. That is self-consistency, not origin: a caller that hands in other consistent bytes gets them priced.
    const std::uint64_t systemTokens = estimateChatTokens(context.systemPrompt);
    const std::uint64_t userTokens = estimateChatTokens(context.userContent);
    if (context.policyId != kFollowUpComposerPromptPolicyId) {
        return fail("follow_up.quote_invalid_context", "The context does not carry the follow-up composer policy id");
    }
    if (context.systemPrompt != followUpComposerSystemPrompt()) {
        return fail("follow_up.quote_invalid_context", "The context does not carry the fixed composer system prompt");
    }
    if (context.contextFingerprint != followUpComposerContextFingerprint(context.systemPrompt, context.userContent)) {
        return fail("follow_up.quote_invalid_context", "The context fingerprint does not seal its provider bytes");
    }
    if (context.estimatedInputTokens != systemTokens + userTokens) {
        return fail("follow_up.quote_invalid_context", "The context token estimate does not match its provider bytes");
    }
    if (context.estimatedInputTokens > kFollowUpComposerTotalTokens) {
        return fail("follow_up.quote_invalid_context", "The context exceeds the fixed composer input budget");
    }
    if (provider.id != model.providerId) {
        return fail("follow_up.quote_provider_mismatch", "The model does not belong to the supplied provider");
    }
    if (userCapMicros.has_value() && (*userCapMicros == 0 || *userCapMicros > kFollowUpSystemCeilingMicros)) {
        return fail("follow_up.invalid_cap", "The cap must be above zero and at most the system ceiling");
    }
    const std::uint64_t capMicros = userCapMicros.value_or(kFollowUpSystemCeilingMicros);

    const std::uint64_t inputTokens = context.estimatedInputTokens;
    if (inputTokens > model.contextWindowTokens
        || model.contextWindowTokens - inputTokens < kFollowUpMinimumOutputTokens) {
        return fail("follow_up.context_window_exceeded", "The model window cannot hold the input and the minimum output");
    }
    if (model.maxOutputTokens < kFollowUpMinimumOutputTokens) {
        return fail("follow_up.model_output_limit_too_small", "The model cannot produce the minimum output");
    }
    const std::uint64_t candidateOutputTokens = std::min(
        {kFollowUpInitialMaxOutputTokens, model.maxOutputTokens, model.contextWindowTokens - inputTokens});

    const models::PromptCachePolicy cachePolicy = followUpPromptCachePolicy(provider.kind);
    const std::uint64_t cacheableTokens = cachePolicy.enabled() ? systemTokens : 0;
    const auto quoteAt = [&](std::uint64_t outputTokens) {
        return quoteProviderRequestWorstCaseMiss(model, cachePolicy, inputTokens, cacheableTokens, outputTokens);
    };
    const auto fitsCap = [capMicros](const ProviderRequestWorstCaseQuote& quote) {
        return quote.priceKnown && quote.estimatedCostMicros <= capMicros;
    };

    ProviderRequestWorstCaseQuote chosen;
    const auto top = quoteAt(candidateOutputTokens);
    if (!top.success()) return common::Result<FollowUpComposerQuote>::failure(*top.error());
    if (fitsCap(top.value())) {
        chosen = top.value();
    } else {
        const auto bottom = quoteAt(kFollowUpMinimumOutputTokens);
        if (!bottom.success()) return common::Result<FollowUpComposerQuote>::failure(*bottom.error());
        if (!bottom.value().priceKnown) {
            return fail("follow_up.price_unknown", "A price the quote needs is missing or unusable");
        }
        if (bottom.value().estimatedCostMicros > capMicros) {
            return fail("follow_up.cap_too_low", "Even the minimum output does not fit the cap");
        }
        // The worst-case cost never decreases with the output count, so the greatest fitting count is found by
        // bisection. Invariant: `low` fits the cap, `high` does not.
        std::uint64_t low = kFollowUpMinimumOutputTokens;
        std::uint64_t high = candidateOutputTokens;
        chosen = bottom.value();
        while (high - low > 1) {
            const std::uint64_t middle = low + (high - low) / 2;
            const auto probe = quoteAt(middle);
            if (!probe.success()) return common::Result<FollowUpComposerQuote>::failure(*probe.error());
            if (fitsCap(probe.value())) {
                low = middle;
                chosen = probe.value();
            } else {
                high = middle;
            }
        }
    }

    FollowUpComposerQuote result;
    result.policyId = context.policyId;
    result.contextFingerprint = context.contextFingerprint;
    result.providerIdentity = followUpProviderExecutionIdentity(provider);
    result.providerIdentityFingerprint = followUpProviderExecutionIdentityFingerprint(result.providerIdentity);
    result.worstCase = std::move(chosen);
    result.capMicros = capMicros;
    return common::Result<FollowUpComposerQuote>::ok(std::move(result));
}

} // namespace ai::application
