#pragma once

#include <ai/application/ProviderExecutionBudget.hpp>

#include <QString>
#include <QVariant>
#include <QVariantMap>

#include <cstdint>
#include <optional>

namespace ai::ui {

// The optional-key map QML and the controllers exchange for provider limits: a key is present only
// when that limit is set explicitly (the same shape an execution profile stores). An absent or null
// key means "the Backend default"; a present maxTurns pins the turn limit. QML never computes a
// limit, a ceiling or a default - the Backend resolves and validates the values.
[[nodiscard]] inline ai::application::ProviderExecutionBudgetOverrides
parseProviderBudgetOverrides(const QVariantMap& providerBudget)
{
    ai::application::ProviderExecutionBudgetOverrides result;
    const auto get = [&providerBudget](const char* name, std::optional<std::uint64_t>& target) {
        const QString key = QString::fromLatin1(name);
        if (!providerBudget.contains(key) || providerBudget.value(key).isNull()) return;
        bool ok = false;
        const qulonglong value = providerBudget.value(key).toULongLong(&ok);
        if (ok) target = static_cast<std::uint64_t>(value);
    };
    get("maxInputTokens", result.maxInputTokens);
    get("maxOutputTokens", result.maxOutputTokens);
    get("maxTurns", result.maxTurns);
    get("maxToolCalls", result.maxToolCalls);
    get("maxToolResultBytes", result.maxToolResultBytes);
    get("maxWallTimeMs", result.maxWallTimeMs);
    get("maxRequestTimeMs", result.maxRequestTimeMs);
    return result;
}

[[nodiscard]] inline QVariantMap providerBudgetOverridesToVariant(
    const ai::application::ProviderExecutionBudgetOverrides& budget)
{
    QVariantMap result;
    const auto put = [&result](const char* name, const std::optional<std::uint64_t>& value) {
        if (value) result.insert(QString::fromLatin1(name), QVariant::fromValue<qulonglong>(*value));
    };
    put("maxInputTokens", budget.maxInputTokens);
    put("maxOutputTokens", budget.maxOutputTokens);
    put("maxTurns", budget.maxTurns);
    put("maxToolCalls", budget.maxToolCalls);
    put("maxToolResultBytes", budget.maxToolResultBytes);
    put("maxWallTimeMs", budget.maxWallTimeMs);
    put("maxRequestTimeMs", budget.maxRequestTimeMs);
    return result;
}

} // namespace ai::ui
