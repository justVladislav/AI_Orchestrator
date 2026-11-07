#pragma once

#include <chrono>
#include <optional>

namespace ai::application {

// The one half-open period [fromInclusive, toExclusive) over a creation time, shared by the money composition of the compaction
// records and by the compaction coverage so that both pick the same rows for the same period. A bound that is not given is open;
// nothing is invented for it.
[[nodiscard]] constexpr bool inCreatedAtWindow(std::chrono::system_clock::time_point createdAt,
    const std::optional<std::chrono::system_clock::time_point>& fromInclusive,
    const std::optional<std::chrono::system_clock::time_point>& toExclusive) noexcept
{
    return (!fromInclusive.has_value() || createdAt >= *fromInclusive) && (!toExclusive.has_value() || createdAt < *toExclusive);
}

} // namespace ai::application
