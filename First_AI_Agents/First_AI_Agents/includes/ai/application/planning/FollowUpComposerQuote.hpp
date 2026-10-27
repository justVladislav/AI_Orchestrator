#pragma once

#include "ai/application/planning/FollowUpComposerContext.hpp"
#include "ai/application/catalog/ProviderCatalogTypes.hpp"
#include "ai/application/ProviderRequestQuote.hpp"
#include "ai/common/Result.hpp"
#include "ai/models/PromptCachePolicy.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ai::application {

// PC-5A-3: the pure quote of ONE follow-up composer call. No repository, filesystem, clock, provider or Qt, and it
// persists nothing: 5B stores the result, 5C re-runs it at the authorisation and at the claim.
//
// The system ceiling of one call is $0.36 (360 000 micros). The output the call may produce starts at
// min(16 384, the model's output maximum, what remains of the model's window after the estimated input) and is
// lowered, never silently later, to the greatest count whose WORST CASE still fits the cap. Every price in this
// module comes from quoteProviderRequestWorstCaseMiss (ACD-4); nothing here prices a token on its own.
inline constexpr std::uint64_t kFollowUpSystemCeilingMicros = 360'000;
inline constexpr std::uint64_t kFollowUpInitialMaxOutputTokens = 16'384;
inline constexpr std::uint64_t kFollowUpMinimumOutputTokens = 4'096;

// The prompt-cache identity of a follow-up request, decided by the PROVIDER KIND alone (never by elapsed time, never
// changed after the authorisation): "anthropic" -> ephemeral_1h, every other kind - including an unknown spelling -
// -> disabled. The policy only ever applies to the fixed system prompt (see quoteFollowUpComposerRequest).
[[nodiscard]] models::PromptCachePolicy followUpPromptCachePolicy(std::string_view providerKind) noexcept;

// WHO receives the authorised bytes. The pricing identity of the ACD-4 quote names the provider id, the model, the
// prices and the cache policy, but not what the provider id currently POINTS AT: ProviderCatalogService::updateProvider
// changes a provider's kind, baseUrl and credential reference in place under the same id. Authorising a quote for one
// endpoint and sending the file contents to another would therefore pass every pricing comparison. This identity is part
// of the quote, is stored with the request in 5B, and 5C re-derives it at the authorisation and at the claim
// (compareFollowUpQuotes).
//   - providerId and kind: as stored.
//   - baseUrl: the stored bytes, deliberately NOT normalised. The catalog does not validate or canonicalise it, and a
//     normaliser of my own would be a policy whose mistake hides a real endpoint change; a cosmetic edit costs a free
//     `stale` and a re-quote, which is the safe side.
//   - credentialReference: the catalog's pointer into the credential store, with "no reference" (nullopt) kept apart
//     from an empty reference. Replacing it can switch the call to another account or tenant, and so change who pays.
//     The guarantee is deliberately NARROW: the reference recorded in the catalog did not change between quote and
//     claim. It is NOT a promise that the secret bytes behind an unchanged reference stayed the same - a rotation under
//     the same name is invisible to the catalog.
struct FollowUpProviderExecutionIdentity {
    ProviderId providerId;
    std::string kind;
    std::string baseUrl;
    std::optional<std::string> credentialReference;

    friend bool operator==(const FollowUpProviderExecutionIdentity&, const FollowUpProviderExecutionIdentity&) = default;
};

[[nodiscard]] FollowUpProviderExecutionIdentity followUpProviderExecutionIdentity(const Provider& provider);
// Length-framed (no JSON): arbitrary stored bytes cannot make it throw or collide.
[[nodiscard]] std::string followUpProviderExecutionIdentityFingerprint(
    const FollowUpProviderExecutionIdentity& identity);

struct FollowUpComposerQuote {
    std::string policyId;            // follow-up-composer.v1
    std::string contextFingerprint;  // the exact provider bytes this quote prices
    // The recipient of those bytes (see FollowUpProviderExecutionIdentity).
    FollowUpProviderExecutionIdentity providerIdentity;
    std::string providerIdentityFingerprint;
    // The ACD-4 worst-case-miss quote: the pricing identity (with the cache policy), its fingerprint, the estimated
    // input, the cacheable prefix, the output ceiling and the cost in micros, rounded UP.
    ProviderRequestWorstCaseQuote worstCase;
    // The cap the cost was fitted under: the user's cap, or the system ceiling when the user gave none.
    std::uint64_t capMicros {0};

    // What the provider call is allowed to produce. Shown in the quote dialog and stored immutably; the call uses
    // exactly this value.
    [[nodiscard]] std::uint64_t effectiveMaxOutputTokens() const noexcept { return worstCase.maximumOutputTokens; }
    [[nodiscard]] models::PromptCachePolicy promptCache() const noexcept { return worstCase.pricingIdentity.promptCache; }
};

// Prices the exact provider bytes (`systemPrompt`, `userContent`) of `context` for `model`. Only the fixed system
// prompt is ever the cacheable prefix (when the policy is enabled): the goal, the snapshot, the brief and the file
// excerpts are moving user content and never count as cacheable. `provider` must be the model's provider (its kind
// picks the cache policy, its identity names the recipient).
//
// WHAT THIS CHECKS about `context` is only that its bytes are internally consistent: the policy id, that the system
// prompt IS the fixed one, that the fingerprint seals exactly these two byte strings, that the token estimate is the
// one for these bytes, and the 48 000-token ceiling. WHAT IT DOES NOT CHECK: that the bytes came out of
// assembleFollowUpComposerContext - a self-consistent arbitrary user string is priced like any other - and it never
// reads the metadata fields (files, snapshotTokens, briefTokens, excerptTokens, wrapperTokens, briefTruncated). The
// guarantee therefore rests on the caller: 5C assembles the context itself from the persisted request at the quote,
// the authorisation and the claim, sends exactly these bytes, and compares the fresh quote with the stored one.
//
// Refusals are typed and FREE (nothing was sent): follow_up.quote_invalid_context (the bytes are not self-consistent,
// the system prompt is not the fixed one, or they exceed 48 000 tokens), follow_up.quote_provider_mismatch,
// follow_up.invalid_cap (a user cap of 0 or above the system ceiling - never clamped silently),
// follow_up.context_window_exceeded (the window cannot hold the input plus the minimum output),
// follow_up.model_output_limit_too_small (the model cannot produce the minimum output),
// follow_up.price_unknown (the ACD-4 quote cannot name the cost of the minimum output: a zero or missing applicable
// price) and follow_up.cap_too_low (even the minimum output exceeds the cap).
[[nodiscard]] common::Result<FollowUpComposerQuote> quoteFollowUpComposerRequest(
    const FollowUpComposerContext& context,
    const Provider& provider,
    const Model& model,
    std::optional<std::uint64_t> userCapMicros);

// What changed between the quote a request was authorised under (`stored`) and a quote freshly derived from the
// current catalog and a freshly assembled context (`fresh`). Anything but None is a FREE `stale`: the first
// difference in this order is reported so the dialog can say why. Both the identities and their fingerprints are
// compared, so a stored fingerprint cannot be edited on its own.
enum class FollowUpQuoteDrift {
    None,
    Context,   // policy id or the exact provider bytes
    Provider,  // provider id, kind or baseUrl: the recipient of the bytes
    Pricing,   // model, prices, window, cache policy
    Limits,    // token counts, the effective output, the cost or the cap
};

[[nodiscard]] const char* toString(FollowUpQuoteDrift value) noexcept;
[[nodiscard]] FollowUpQuoteDrift compareFollowUpQuotes(
    const FollowUpComposerQuote& stored,
    const FollowUpComposerQuote& fresh) noexcept;

} // namespace ai::application
