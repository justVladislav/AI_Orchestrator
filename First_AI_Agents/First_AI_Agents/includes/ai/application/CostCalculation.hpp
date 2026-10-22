#pragma once

#include "ai/application/ProviderCatalogTypes.hpp"
#include "ai/models/TokenUsage.hpp"

#include <cstdint>

namespace ai::application {

// Package 4 (2026-09-28 planner-terminal-budget-hardening): the single
// Backend-owned decomposition of a TokenUsage value into the exact
// categories a provider-issued usage/billing export (e.g. an Anthropic
// Console CSV) reports separately. Every consumer that needs to DISPLAY or
// cross-check usage - never just price it - reuses this instead of
// re-deriving the same arithmetic a second time (QML in particular must
// never compute any of these itself; see RunListModel's
// nonCacheInputTokens/rawProviderUsageTokens roles).
struct UsageTokenBreakdown {
    // TokenUsage::inputTokens verbatim: plain + cache-write + cache-read.
    // Never label this as "fresh"/fully billable input on its own - most of
    // it can be steeply-discounted cache-read tokens.
    std::uint64_t rawInputTokens {0};
    // rawInputTokens with BOTH cache subsets removed - the genuinely fresh,
    // never-cached portion of the input. Saturates to 0 rather than
    // underflowing if a malformed/mocked usage value ever violates the
    // invariant that rawInputTokens >= cacheWriteTokens + cacheReadTokens.
    std::uint64_t nonCacheInputTokens {0};
    // TokenUsage::cacheCreationInputTokens verbatim - a SUBSET already
    // included in rawInputTokens, never an additional amount to add on top
    // of it.
    std::uint64_t cacheWriteTokens {0};
    // TokenUsage::cacheReadInputTokens verbatim - also a subset of
    // rawInputTokens.
    std::uint64_t cacheReadTokens {0};
    // TokenUsage::outputTokens verbatim.
    std::uint64_t outputTokens {0};
    // The grand total a provider-issued CSV/usage export reports across a
    // set of turns: non-cache input + cache write + cache read + output.
    // Equal to rawInputTokens + outputTokens by construction, since
    // rawInputTokens already sums the three input categories - kept as its
    // own field so no consumer has to re-derive that identity itself.
    std::uint64_t grandProviderUsageTokens {0};
};

[[nodiscard]] UsageTokenBreakdown computeUsageTokenBreakdown(
    const models::TokenUsage& usage);

// MW-P2: TokenUsage::inputTokens is the conservative TOTAL of every
// input-side category a provider billed (plain + cache-creation +
// cache-read - see TokenUsage's own class comment). A model's four price
// fields bill each category at its own rate, so cost computation must
// split inputTokens back into its categories using the already-known
// cache breakdown, never multiply the total by the plain input rate.
struct CostBreakdown {
    std::uint64_t costMicros {0};
    // False when any usage category this Run actually consumed has a zero
    // (= "not entered") price on the model - the real cost for that
    // category is unknown, not zero, and costMicros undercounts it. A
    // caller that surfaces cost to the user should treat priceKnown=false
    // as "estimated, may be understated" rather than a precise total.
    // Also false when the usage itself is inconsistent - its cache-write +
    // cache-read subsets exceed its own total input (or the cost does not fit
    // the representable range): the plain-input share is then unknowable.
    bool priceKnown {true};
};

// Pure, provider-agnostic cost calculation shared by every cost-reporting
// consumer (chat, comparison, production runs, analytics) so the cache-aware
// formula is defined exactly once. For a provider/model that never reports
// cache usage, usage.cacheCreationInputTokens/cacheReadInputTokens are
// always zero and this reduces to the original plain-input-and-output
// formula. There is deliberately NO public "price this usage" function that
// does not say whether the usage is one Response or an aggregate: the two
// explicit entry points below are the only way in (M2b).

// ---------------------------------------------------------------------------
// AG-17 gpt-6-sol checkpoint M2b: per-RESPONSE long-context pricing.
//
// A provider may bill a Response whose OWN input_tokens exceed a threshold - strictly
// greater - entirely at higher rates (input, cache-read, cache-write AND output). The
// threshold and the prices are the SAME immutable Model copy resolved for the execution;
// the catalog is never re-read between accumulating usage and pricing it.
//
// Three explicitly named entry points, so no call site can price a response without
// classifying it:
//   accumulateResponseUsage()              the ONLY writer of the partition
//   computeSingleResponseCostMicros()      classifies ONE response itself
//   computeAggregatedExecutionCostMicros() prices usage the accumulator already built
// ---------------------------------------------------------------------------

// Adds ONE provider response to an accumulating execution total (saturating, never wrapping).
// With a non-zero threshold a response whose own inputTokens is strictly greater than it is
// ALSO added, in full, to total.longContext; classification is per response, never by the
// accumulated input. A response is never trusted to carry a partition: if it does - or its
// own cache breakdown does not fit its input, or it is itself untrusted - the aggregate
// becomes untrusted (total.accountingTrusted = false) instead of the problem being swallowed.
// So does any counter, total or partition, whose exact sum does not fit: it saturates at the
// maximum and its true value is lost (adding zero to an already-maximal counter is not an
// overflow; adding anything else is). Untrusted is sticky.
void accumulateResponseUsage(
    models::TokenUsage& total,
    const models::TokenUsage& response,
    std::uint64_t longContextThresholdInputTokens) noexcept;

// Merges one ALREADY-ACCUMULATED usage value into another. Unlike
// accumulateResponseUsage(), this preserves the addend's Backend-derived
// long-context partition instead of trying to classify the whole addend as a
// single provider response. All counters saturate and loss of trust is sticky.
// This is the only supported way to combine planner, worker or stage totals.
void mergeAccumulatedUsage(
    models::TokenUsage& total,
    const models::TokenUsage& addend) noexcept;

// True when usage.longContext is structurally sound: every partition counter is a subset of
// its total, the partition and the short remainder (total - partition) are each a consistent
// cache breakdown, and so is the total. Checked in this order, so nothing is ever subtracted
// before the subset relation is proven.
[[nodiscard]] bool longContextPartitionIsValid(const models::TokenUsage& usage) noexcept;

// Prices ONE response: classifies it by the model's own threshold, then prices the result.
[[nodiscard]] CostBreakdown computeSingleResponseCostMicros(
    const Model& model,
    const models::TokenUsage& responseUsage);

// Prices usage built by accumulateResponseUsage(). Without a partition every response was
// short (or the tier is off) and this is the flat, cache-aware calculation exactly. With one, the
// short remainder is priced at the flat prices and the long partition at the long-context
// prices, which are the only prices that may apply to it:
//   - a used category whose price was not entered contributes 0 and priceKnown=false (a lower bound);
//   - a partition while the model's tier is disabled, or untrusted accounting, is
//     priceKnown=false and the long part contributes 0 - NEVER a flat price for long tokens;
//   - a structurally invalid partition proves nothing: cost 0, priceKnown=false.
[[nodiscard]] CostBreakdown computeAggregatedExecutionCostMicros(
    const Model& model,
    const models::TokenUsage& accumulatedUsage);

} // namespace ai::application
