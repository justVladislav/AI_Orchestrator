#include <ai/ui/ComparisonController.hpp>

#include <QMetaObject>

namespace {

QString fromStd(const std::string& s) { return QString::fromStdString(s); }

} // namespace

ComparisonController::ComparisonController(
    ai::application::ComparisonService& service,
    ai::application::ProjectService& projectService,
    ai::application::ProviderCatalogService& catalogService,
    QObject* parent)
    : QObject(parent)
    , service_(service)
    , projectService_(projectService)
    , catalogService_(catalogService)
    , projectsModel_(this)
    , comparisonListModel_(this)
    , resultListModel_(this)
{
    loadProjects();
}

ComparisonController::~ComparisonController() = default;

QAbstractItemModel* ComparisonController::projectsModel() noexcept
{
    return &projectsModel_;
}

QAbstractItemModel* ComparisonController::comparisonListModel() noexcept
{
    return &comparisonListModel_;
}

QAbstractItemModel* ComparisonController::resultListModel() noexcept
{
    return &resultListModel_;
}

QString ComparisonController::selectedProjectId() const { return selectedProjectId_; }
QString ComparisonController::selectedComparisonId() const { return selectedComparisonId_; }
bool ComparisonController::isLoading() const { return isLoading_; }
QString ComparisonController::lastError() const { return lastError_; }

QString ComparisonController::selectedComparisonUpdatedAt() const
{
    if (selectedComparisonId_.isEmpty()) return {};
    const int n = comparisonListModel_.rowCount();
    for (int i = 0; i < n; ++i) {
        const QModelIndex idx = comparisonListModel_.index(i);
        if (comparisonListModel_.data(idx, ComparisonListModel::ComparisonIdRole).toString()
            == selectedComparisonId_) {
            return comparisonListModel_.data(idx, ComparisonListModel::UpdatedAtRole).toString();
        }
    }
    return {};
}

// ── Selection ──────────────────────────────────────────────────────────────

void ComparisonController::selectProject(const QString& projectId)
{
    if (projectId == selectedProjectId_) return;
    selectedProjectId_ = projectId;
    emit selectedProjectIdChanged();

    selectedComparisonId_.clear();
    emit selectedComparisonIdChanged();

    resultListModel_.replace({});

    if (projectId.isEmpty()) {
        comparisonListModel_.replace({});
        return;
    }
    loadComparisons(projectId);
}

void ComparisonController::selectComparison(const QString& comparisonId)
{
    if (comparisonId == selectedComparisonId_) return;
    selectedComparisonId_ = comparisonId;
    emit selectedComparisonIdChanged();

    if (comparisonId.isEmpty()) {
        resultListModel_.replace({});
        return;
    }
    loadResults(comparisonId);
}

// ── Result mutations ───────────────────────────────────────────────────────

void ComparisonController::markAsBest(const QString& comparisonId, const QString& resultId)
{
    const auto res = service_.setResultBest(
        comparisonId.toStdString(),
        resultId.toStdString());
    if (!res.success()) {
        reportError(*res.error());
        return;
    }
    clearError();
    if (!selectedComparisonId_.isEmpty()) {
        loadResults(selectedComparisonId_);
    }
}

void ComparisonController::toggleSynthesisSelection(const QString& resultId)
{
    // Find current state
    const auto found = service_.findResult(resultId.toStdString());
    if (!found.success() || !found.value().has_value()) {
        return;
    }
    const bool currentlySelected = found.value()->selectedForSynthesis;
    const auto res = service_.setResultSynthesisSelected(
        resultId.toStdString(), !currentlySelected);
    if (!res.success()) {
        reportError(*res.error());
        return;
    }
    clearError();
    if (!selectedComparisonId_.isEmpty()) {
        loadResults(selectedComparisonId_);
    }
}

void ComparisonController::retryResult(const QString& resultId)
{
    const auto res = service_.resetResultPending(resultId.toStdString());
    if (!res.success()) {
        reportError(*res.error());
        return;
    }
    clearError();
    if (!selectedComparisonId_.isEmpty()) {
        loadResults(selectedComparisonId_);
    }
}

void ComparisonController::refresh()
{
    if (selectedProjectId_.isEmpty()) {
        loadProjects();
        return;
    }
    loadComparisons(selectedProjectId_);
    if (!selectedComparisonId_.isEmpty()) {
        loadResults(selectedComparisonId_);
    }
}

// ── Private helpers ────────────────────────────────────────────────────────

void ComparisonController::loadProjects()
{
    const auto res = projectService_.listProjects();
    if (!res.success()) {
        reportError(*res.error());
        return;
    }
    projectsModel_.replace(res.value());
}

void ComparisonController::loadComparisons(const QString& projectId)
{
    const auto res = service_.listSessionsByProject(projectId.toStdString());
    if (!res.success()) {
        reportError(*res.error());
        return;
    }
    const bool hasSessions = !res.value().empty();
    comparisonListModel_.replace(res.value());

    // Auto-select first comparison if none is currently selected
    if (selectedComparisonId_.isEmpty() && hasSessions) {
        const QVariant first = comparisonListModel_.data(
            comparisonListModel_.index(0), ComparisonListModel::ComparisonIdRole);
        if (first.isValid() && !first.toString().isEmpty()) {
            selectComparison(first.toString());
        }
    }
}

void ComparisonController::loadResults(const QString& comparisonId)
{
    isLoading_ = true;
    emit isLoadingChanged();

    const auto res = service_.listResultsBySession(comparisonId.toStdString());

    isLoading_ = false;
    emit isLoadingChanged();

    if (!res.success()) {
        reportError(*res.error());
        return;
    }
    resultListModel_.replace(res.value());
    clearError();
    emit resultsUpdated(comparisonId);
}

void ComparisonController::reportError(const ai::common::Error& error)
{
    lastError_ = QString::fromStdString(error.message);
    emit lastErrorChanged();
    emit operationFailed(
        QString::fromStdString(error.code),
        QString::fromStdString(error.message));
}

void ComparisonController::clearError()
{
    if (!lastError_.isEmpty()) {
        lastError_.clear();
        emit lastErrorChanged();
    }
}

QVariantList ComparisonController::resultsSnapshot() const
{
    QVariantList out;
    const int n = resultListModel_.rowCount();
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        const QModelIndex idx = resultListModel_.index(i);
        QVariantMap item;
        item[QStringLiteral("resultId")]            = resultListModel_.data(idx, ComparisonResultListModel::ResultIdRole);
        item[QStringLiteral("providerId")]          = resultListModel_.data(idx, ComparisonResultListModel::ProviderIdRole);
        item[QStringLiteral("providerName")]        = resultListModel_.data(idx, ComparisonResultListModel::ProviderNameRole);
        item[QStringLiteral("modelName")]           = resultListModel_.data(idx, ComparisonResultListModel::ModelNameRole);
        item[QStringLiteral("status")]              = resultListModel_.data(idx, ComparisonResultListModel::StatusRole);
        item[QStringLiteral("isBest")]              = resultListModel_.data(idx, ComparisonResultListModel::IsBestRole);
        item[QStringLiteral("selectedForSynthesis")]= resultListModel_.data(idx, ComparisonResultListModel::SelectedForSynthesisRole);
        item[QStringLiteral("qualityScore")]        = resultListModel_.data(idx, ComparisonResultListModel::QualityScoreRole);
        item[QStringLiteral("totalCost")]           = resultListModel_.data(idx, ComparisonResultListModel::TotalCostRole);
        item[QStringLiteral("inputTokens")]         = resultListModel_.data(idx, ComparisonResultListModel::InputTokensRole);
        item[QStringLiteral("outputTokens")]        = resultListModel_.data(idx, ComparisonResultListModel::OutputTokensRole);
        item[QStringLiteral("warnings")]            = resultListModel_.data(idx, ComparisonResultListModel::WarningsRole);
        out.append(item);
    }
    return out;
}
