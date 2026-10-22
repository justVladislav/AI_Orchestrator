#pragma once

#include <string_view>

namespace ai::models {

// Provider-neutral policy for a single non-agent request. Disabled is the
// compatibility default. An enabled policy applies only to the request's
// stable system prompt; callers must never infer reuse from elapsed time.
enum class PromptCacheMode {
    Disabled,
    EphemeralFiveMinutes,
    EphemeralOneHour
};

struct PromptCachePolicy {
    PromptCacheMode mode {PromptCacheMode::Disabled};

    [[nodiscard]] constexpr bool enabled() const noexcept
    {
        return mode != PromptCacheMode::Disabled;
    }

    friend constexpr bool operator==(const PromptCachePolicy&, const PromptCachePolicy&) = default;
};

[[nodiscard]] constexpr std::string_view toString(PromptCacheMode mode) noexcept
{
    switch (mode) {
    case PromptCacheMode::Disabled:
        return "disabled";
    case PromptCacheMode::EphemeralFiveMinutes:
        return "ephemeral_5m";
    case PromptCacheMode::EphemeralOneHour:
        return "ephemeral_1h";
    }
    return "unknown";
}

[[nodiscard]] constexpr bool isKnown(PromptCacheMode mode) noexcept
{
    return mode == PromptCacheMode::Disabled
        || mode == PromptCacheMode::EphemeralFiveMinutes
        || mode == PromptCacheMode::EphemeralOneHour;
}

} // namespace ai::models
