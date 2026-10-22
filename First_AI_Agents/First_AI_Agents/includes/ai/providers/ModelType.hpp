#pragma once

namespace ai::providers {

/**
 * @brief Explicit model choices understood by provider adapters.
 *
 * ModelTypeUnknown means the provider should use its runtime fallback path:
 * provider-specific environment variable first, then provider default.
 */
enum ModelType {
    ModelTypeUnknown,

    ModelTypeOpenAi56Sol,
    ModelTypeOpenAi56Terra,
    ModelTypeOpenAi56Luna,
    ModelTypeOpenAi54,
    ModelTypeOpenAi5,
    ModelTypeOpenAi5Mini,
    ModelTypeOpenAi5Nano,

    ModelTypeClaudeFable,
    ModelTypeClaudeOpus,
    ModelTypeClaudeHaiku,
    ModelTypeClaudeSonnet
};

} // namespace ai::providers
