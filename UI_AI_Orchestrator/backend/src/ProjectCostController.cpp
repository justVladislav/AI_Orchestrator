#include "ai/ui/ProjectCostController.hpp"

#include "ai/ui/CompactionCoverageProjection.hpp"
#include "ai/ui/ProjectCostProjection.hpp"

#include <chrono>
#include <unordered_map>

namespace project_cost = ai::ui::project_cost;

ProjectCostController::ProjectCostController(ai::application::ProjectCostService& costs,
                                             ai::application::ProviderCatalogService& catalog,
                                             QObject* parent)
    : QObject(parent), costs_(costs), catalog_(catalog), providers_(this)
{
}

QVariantMap ProjectCostController::coverage() const { return ai::ui::compaction_coverage::coverageMap(coverage_); }

QString ProjectCostController::priceState() const { return project_cost::priceStateName(totals_); }
QString ProjectCostController::costText() const { return project_cost::costText(totals_); }
bool ProjectCostController::priceIncomplete() const noexcept { return project_cost::priceIncomplete(totals_); }
qulonglong ProjectCostController::inputTokens() const noexcept { return totals_.inputTokens(); }
qulonglong ProjectCostController::outputTokens() const noexcept { return totals_.outputTokens(); }
qulonglong ProjectCostController::ledgerExecutionCount() const noexcept { return totals_.ledgerExecutionCount(); }
qulonglong ProjectCostController::ledgerAverageLatencyMs() const noexcept { return totals_.ledgerAverageLatencyMs(); }
qulonglong ProjectCostController::compactionRecordCount() const noexcept { return totals_.compactionRecordCount(); }
qulonglong ProjectCostController::compactionNoneCount() const noexcept { return totals_.compactionNoneCount(); }
qulonglong ProjectCostController::pricingEvidenceCount() const noexcept { return totals_.pricingEvidenceCount(); }
qulonglong ProjectCostController::incompletePriceCount() const noexcept { return totals_.incompletePriceCount(); }

bool ProjectCostController::refreshProject(const QString& projectId)
{
    if (projectId.isEmpty()) {
        clear();
        return true;
    }
    return apply(costs_.projectCost(projectId.toStdString()), projectId);
}

bool ProjectCostController::refreshProjectRange(
    const QString& projectId, const QDateTime& fromInclusive, const QDateTime& toExclusive)
{
    if (projectId.isEmpty()) {
        clear();
        return true;
    }
    if (!fromInclusive.isValid() || !toExclusive.isValid() || fromInclusive >= toExclusive) {
        return fail(projectId, QString::fromLatin1(ai::application::kProjectCostInvalidDateRangeCode),
                    tr("The project cost date range is invalid."));
    }
    const auto from = std::chrono::system_clock::time_point {std::chrono::milliseconds {fromInclusive.toMSecsSinceEpoch()}};
    const auto to = std::chrono::system_clock::time_point {std::chrono::milliseconds {toExclusive.toMSecsSinceEpoch()}};
    return apply(costs_.projectCost(projectId.toStdString(), from, to), projectId);
}

void ProjectCostController::clear()
{
    totals_ = {};
    coverage_.reset();
    loadState_ = QStringLiteral("idle");
    scopeId_.clear();
    errorCode_.clear();
    errorMessage_.clear();
    providers_.replace({});
    emit summaryChanged();
}

bool ProjectCostController::fail(const QString& scopeId, const QString& code, const QString& message)
{
    // Replace the whole state first: both observers (the properties and the signal) must see a consistent "unavailable".
    totals_ = {};
    coverage_.reset();   // never a stale coverage next to a failed money read
    loadState_ = QStringLiteral("unavailable");
    scopeId_ = scopeId;
    errorCode_ = code;
    errorMessage_ = message;
    providers_.replace({});
    emit summaryChanged();
    emit errorOccurred(code, message);
    return false;
}

bool ProjectCostController::apply(
    const ai::common::Result<ai::application::ProjectCostSnapshot>& result, const QString& scopeId)
{
    if (!result.success()) {
        return fail(scopeId, QString::fromStdString(result.error()->code), QString::fromStdString(result.error()->message));
    }

    // Names are metadata: a catalog that cannot be read falls back to the ids and never fails the estimate.
    std::unordered_map<std::string, QString> providerNames;
    std::unordered_map<std::string, QString> modelNames;
    const auto catalog = catalog_.listProviders();
    if (catalog.success()) {
        for (const auto& provider : catalog.value()) providerNames.emplace(provider.id, QString::fromStdString(provider.name));
    }
    const auto models = catalog_.listModels();
    if (models.success()) {
        for (const auto& model : models.value()) modelNames.emplace(model.id, QString::fromStdString(model.displayName));
    }

    std::vector<ProjectCostProviderRow> rows;
    rows.reserve(result.value().providers.size());
    for (const auto& provider : result.value().providers) {
        ProjectCostProviderRow row;
        row.cost = provider;
        const auto found = providerNames.find(provider.providerId);
        row.name = found == providerNames.end() ? QString::fromStdString(provider.providerId) : found->second;
        for (const auto& model : provider.models) {
            const auto modelFound = modelNames.find(model.modelId);
            if (modelFound != modelNames.end()) row.modelNames.insert(QString::fromStdString(model.modelId), modelFound->second);
        }
        rows.push_back(std::move(row));
    }
    totals_ = result.value().totals;
    coverage_ = result.value().compactionCoverage;   // from the same snapshot: the controller never reads the intents itself
    loadState_ = QStringLiteral("available");
    scopeId_ = scopeId;
    errorCode_.clear();
    errorMessage_.clear();
    providers_.replace(std::move(rows));
    emit summaryChanged();
    return true;
}
