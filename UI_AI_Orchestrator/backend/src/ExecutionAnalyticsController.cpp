#include "ai/ui/ExecutionAnalyticsController.hpp"

#include <chrono>
#include <unordered_map>

ExecutionAnalyticsController::ExecutionAnalyticsController(
    ai::application::ExecutionAnalyticsService& analytics,
    ai::application::ProviderCatalogService& catalog,
    QObject* parent)
    : QObject(parent), analytics_(analytics), catalog_(catalog), providers_(this)
{
}

qulonglong ExecutionAnalyticsController::executionCount() const noexcept { return totals_.executionCount; }
qulonglong ExecutionAnalyticsController::completedCount() const noexcept { return totals_.completedCount; }
qulonglong ExecutionAnalyticsController::failedCount() const noexcept { return totals_.failedCount; }
qulonglong ExecutionAnalyticsController::inputTokens() const noexcept { return totals_.inputTokens; }
qulonglong ExecutionAnalyticsController::outputTokens() const noexcept { return totals_.outputTokens; }
qulonglong ExecutionAnalyticsController::costMicros() const noexcept { return totals_.costMicros; }
QString ExecutionAnalyticsController::priceState() const
{
    return QString::fromLatin1(toString(totals_.priceState()));
}
QString ExecutionAnalyticsController::costText() const { return executionCostText(totals_); }
qulonglong ExecutionAnalyticsController::unknownPriceExecutionCount() const noexcept
{
    return totals_.unknownPriceExecutionCount;
}
bool ExecutionAnalyticsController::priceIncomplete() const noexcept
{
    const auto state = totals_.priceState();
    return state == ai::application::ExecutionPriceState::Partial
        || state == ai::application::ExecutionPriceState::Unknown;
}
qulonglong ExecutionAnalyticsController::averageLatencyMs() const noexcept { return totals_.averageLatencyMs; }

bool ExecutionAnalyticsController::refreshProject(const QString& projectId)
{
    if (projectId.isEmpty()) { clear(); return true; }
    return apply(analytics_.projectSnapshot(projectId.toStdString()), projectId);
}

bool ExecutionAnalyticsController::refreshProjectRange(
    const QString& projectId, const QDateTime& fromInclusive, const QDateTime& toExclusive)
{
    if (projectId.isEmpty()) { clear(); return true; }
    if (!fromInclusive.isValid() || !toExclusive.isValid()
        || fromInclusive >= toExclusive) {
        emit errorOccurred(QStringLiteral("execution_analytics.invalid_date_range"),
                           tr("The analytics date range is invalid."));
        return false;
    }

    const auto from = std::chrono::system_clock::time_point {
        std::chrono::milliseconds {fromInclusive.toMSecsSinceEpoch()}};
    const auto to = std::chrono::system_clock::time_point {
        std::chrono::milliseconds {toExclusive.toMSecsSinceEpoch()}};
    return apply(analytics_.projectSnapshot(projectId.toStdString(), from, to), projectId);
}

bool ExecutionAnalyticsController::refreshSource(const QString& source, const QString& sourceId)
{
    const auto parsed = ai::application::modelExecutionSourceFromString(source.toStdString());
    if (!parsed.has_value()) {
        emit errorOccurred(QStringLiteral("execution_analytics.invalid_source"),
                           tr("Unknown model execution source."));
        return false;
    }
    if (sourceId.isEmpty()) { clear(); return true; }
    return apply(analytics_.sourceSnapshot(*parsed, sourceId.toStdString()), sourceId);
}

void ExecutionAnalyticsController::clear()
{
    totals_ = {};
    scopeId_.clear();
    providers_.replace({});
    emit summaryChanged();
}

bool ExecutionAnalyticsController::apply(
    ai::common::Result<ai::application::ExecutionAnalyticsSnapshot> result,
    const QString& scopeId)
{
    if (!result.success()) {
        emit errorOccurred(QString::fromStdString(result.error()->code),
                           QString::fromStdString(result.error()->message));
        return false;
    }

    std::unordered_map<std::string, QString> providerNames;
    std::unordered_map<std::string, QString> modelNames;
    const auto catalog = catalog_.listProviders();
    if (catalog.success()) {
        for (const auto& provider : catalog.value())
            providerNames.emplace(provider.id, QString::fromStdString(provider.name));
    }
    const auto models = catalog_.listModels();
    if (models.success()) {
        for (const auto& model : models.value())
            modelNames.emplace(model.id, QString::fromStdString(model.displayName));
    }

    std::vector<ProviderExecutionAnalyticsRow> rows;
    rows.reserve(result.value().providers.size());
    for (const auto& provider : result.value().providers) {
        const auto found = providerNames.find(provider.providerId);
        ProviderExecutionAnalyticsRow row;
        row.aggregate = provider;
        row.name = found == providerNames.end()
            ? QString::fromStdString(provider.providerId)
            : found->second;
        for (const auto& model : provider.models) {
            const auto modelFound = modelNames.find(model.modelId);
            if (modelFound != modelNames.end())
                row.modelNames.insert(QString::fromStdString(model.modelId), modelFound->second);
        }
        rows.push_back(std::move(row));
    }
    totals_ = result.value().totals;
    scopeId_ = scopeId;
    providers_.replace(std::move(rows));
    emit summaryChanged();
    return true;
}
