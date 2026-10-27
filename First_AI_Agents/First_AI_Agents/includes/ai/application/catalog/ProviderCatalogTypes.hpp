#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ai::application {

using ProviderId = std::string;
using ModelId = std::string;

// Provider "kind" is a stable, persisted string rather than a closed C++
// enum, matching the literal DTO shape this contract was requested with.
// ProviderCatalogService still enforces a closed set of known values
// ("openai", "anthropic", "openai_compatible", "local") through
// isKnownProviderKind() so persistence and future UI code cannot silently
// drift into an unrecognized spelling.
[[nodiscard]] bool isKnownProviderKind(std::string_view kind) noexcept;

/**
 * Provider connection metadata. credentialReference is a stable, opaque
 * pointer into ICredentialStore (e.g. an OS keychain entry name) - the
 * secret value itself is never a field on this type and is never persisted
 * here or in SQLite.
 */
struct Provider {
    ProviderId id;
    std::string name;
    std::string kind;
    std::string baseUrl;
    bool enabled {true};
    std::optional<std::string> credentialReference;
    std::chrono::system_clock::time_point createdAt;
    std::chrono::system_clock::time_point updatedAt;
};

struct CreateProviderRequest {
    std::string name;
    std::string kind;
    std::string baseUrl;
    bool enabled {true};
    std::optional<std::string> credentialReference;
};

struct UpdateProviderRequest {
    ProviderId id;
    std::string name;
    std::string kind;
    std::string baseUrl;
    std::optional<std::string> credentialReference;
};

/**
 * Optional long-context price tier of a Model (AG-17 gpt-6-sol checkpoint M2).
 * A provider may bill a Response that reports MORE than `thresholdInputTokens`
 * input tokens entirely - input, cache-read, cache-write AND output - at higher
 * rates. The rule is per Response (by that Response's own usage.input_tokens,
 * strictly greater, never `>=`), never per Run or per ModelExecution.
 *
 * thresholdInputTokens == 0 means FLAT pricing: the model has no tier and all
 * four long-context prices must be 0 (ProviderCatalogService refuses anything
 * else when the model is saved). A non-zero threshold enables the tier and must
 * be strictly below the model's context window. A long-context price of 0 is
 * "not entered" (unknown), exactly like the flat prices - never "free" and never
 * "the same as the flat price"; partially filled tiers are allowed.
 *
 * The values are user-entered catalog data. No provider- or model-specific
 * default exists anywhere in the Backend. They take part in no fingerprint and
 * no routing identity, so a price edit can never make a Retry incompatible.
 * Applying them to cost is a separate step; storing and validating them is not.
 */
struct LongContextPricing {
    std::uint64_t thresholdInputTokens {0};
    std::uint64_t inputPriceMicrosPerMillionTokens {0};
    std::uint64_t outputPriceMicrosPerMillionTokens {0};
    std::uint64_t cacheWritePriceMicrosPerMillionTokens {0};
    std::uint64_t cacheReadPriceMicrosPerMillionTokens {0};
    // ACD-2: separately entered one-hour cache-write rate. Appended so old
    // aggregate initializers retain their field mapping.
    std::uint64_t cacheWriteOneHourPriceMicrosPerMillionTokens {0};

    [[nodiscard]] constexpr bool enabled() const noexcept { return thresholdInputTokens > 0; }

    friend constexpr bool operator==(const LongContextPricing&, const LongContextPricing&) = default;
};

// The ways a tier can be logically invalid for a given context window.
enum class LongContextPricingProblem {
    None,
    // Threshold 0 (flat pricing) but a long-context price is set: the price would be silently
    // ignored - and read back as flat pricing, i.e. an understated cost.
    PricesWithoutThreshold,
    // Threshold > 0 but not STRICTLY below the context window (a window that was not entered cannot
    // hold a tier either): the tier could never be reached.
    ThresholdOutOfRange
};

// THE rule set of a long-context tier, shared by every layer that can let one in: the catalog
// service checks it on save, the SQLite layer on the v59 migration (for every existing row, before
// the schema version is raised) and on every Model read. Pure; never repairs anything.
[[nodiscard]] LongContextPricingProblem checkLongContextPricing(
    std::uint64_t contextWindowTokens,
    const LongContextPricing& pricing) noexcept;

// Stable typed error code and human message of a problem (empty for None).
[[nodiscard]] const char* longContextPricingProblemCode(LongContextPricingProblem problem) noexcept;
[[nodiscard]] const char* longContextPricingProblemMessage(LongContextPricingProblem problem) noexcept;

/**
 * One selectable model under a Provider. Prices are integer
 * micros-per-million-tokens (1 USD = 1,000,000 micros), never a formatted
 * UI string like "$1.25", so no locale/currency-formatting concern ever
 * enters the Application Layer or SQLite.
 */
struct Model {
    ModelId id;
    ProviderId providerId;
    std::string apiModelId;
    std::string displayName;
    std::uint64_t contextWindowTokens {0};
    std::uint64_t maxOutputTokens {0};
    std::uint64_t inputPriceMicrosPerMillionTokens {0};
    std::uint64_t outputPriceMicrosPerMillionTokens {0};
    // MW-P2: Anthropic prompt-cache rates. Zero means "not entered" (same
    // convention as the two price fields above), never "free" - a model
    // whose provider actually reports cache usage while these stay zero has
    // unknown, not zero, cache cost (see computeSingleResponseCostMicros()).
    // Meaningless for a provider/model that never reports cache usage.
    std::uint64_t cacheWritePriceMicrosPerMillionTokens {0};
    std::uint64_t cacheReadPriceMicrosPerMillionTokens {0};
    bool enabled {true};
    bool isDefault {false};
    std::chrono::system_clock::time_point createdAt;
    std::chrono::system_clock::time_point updatedAt;
    // Appended last so no positional initialisation of a Model ever shifts.
    LongContextPricing longContextPricing;
    std::uint64_t cacheWriteOneHourPriceMicrosPerMillionTokens {0};
};

// isDefault is deliberately absent from both request DTOs: a model always
// starts non-default and can only become the provider's default through the
// dedicated atomic ProviderCatalogService::setDefaultModel operation.
struct CreateModelRequest {
    ProviderId providerId;
    std::string apiModelId;
    std::string displayName;
    std::uint64_t contextWindowTokens {0};
    std::uint64_t maxOutputTokens {0};
    std::uint64_t inputPriceMicrosPerMillionTokens {0};
    std::uint64_t outputPriceMicrosPerMillionTokens {0};
    std::uint64_t cacheWritePriceMicrosPerMillionTokens {0};
    std::uint64_t cacheReadPriceMicrosPerMillionTokens {0};
    bool enabled {true};
    // Defaults to flat pricing (no tier).
    LongContextPricing longContextPricing;
    std::uint64_t cacheWriteOneHourPriceMicrosPerMillionTokens {0};
};

struct UpdateModelRequest {
    ModelId id;
    std::string apiModelId;
    std::string displayName;
    std::uint64_t contextWindowTokens {0};
    std::uint64_t maxOutputTokens {0};
    std::uint64_t inputPriceMicrosPerMillionTokens {0};
    std::uint64_t outputPriceMicrosPerMillionTokens {0};
    std::uint64_t cacheWritePriceMicrosPerMillionTokens {0};
    std::uint64_t cacheReadPriceMicrosPerMillionTokens {0};
    // std::nullopt (the default) leaves the stored tier exactly as it is, so a
    // caller that does not know about long-context pricing - every existing
    // price/metadata edit - can never silently wipe it. A value REPLACES the
    // stored tier; an all-zero value is how a tier is removed.
    std::optional<LongContextPricing> longContextPricing;
    std::optional<std::uint64_t> cacheWriteOneHourPriceMicrosPerMillionTokens;
};

} // namespace ai::application
