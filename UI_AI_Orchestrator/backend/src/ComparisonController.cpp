#include "ai/ui/ComparisonController.hpp"

#include <QDateTime>
#include <QLocale>
#include <QMetaObject>
#include <QVariantList>
#include <algorithm>
#include <unordered_map>

namespace {

QDateTime toDateTime(std::chrono::system_clock::time_point value)
{
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch());
    return QDateTime::fromMSecsSinceEpoch(milliseconds.count(), Qt::UTC);
}

QString costText(std::uint64_t micros)
{
    return QStringLiteral("$%1").arg(QLocale::c().toString(
        static_cast<double>(micros) / 1'000'000.0, 'f', 6));
}

QString labelFor(const ai::application::ComparisonSessionReadModel& projection,
    const ai::application::ModelExecutionId& id)
{
    const auto found = std::find_if(projection.results.begin(), projection.results.end(),
        [&](const auto& row) { return row.execution.id == id; });
    if (found == projection.results.end()) return QString::fromStdString(id);
    return QStringLiteral("%1 · %2")
        .arg(QString::fromStdString(found->providerName),
             QString::fromStdString(found->modelName));
}

} // namespace

ComparisonController::ComparisonController(
    ai::application::ComparisonService& comparisons,
    ai::application::ComparisonReadModelService& readModel,
    ai::application::ProviderCatalogService& catalog,
    ai::application::ComparisonExecutionManager& executionManager,
    QObject* parent)
    : QObject(parent)
    , comparisons_(comparisons)
    , readModel_(readModel)
    , catalog_(catalog)
    , executionManager_(executionManager)
    , sessions_(this)
    , results_(this)
{
    executionManager_.setEventHandler(
        [this](const ai::application::ComparisonExecutionEvent& event) {
            const QString sessionId = QString::fromStdString(event.sessionId);
            QMetaObject::invokeMethod(this, [this, sessionId] {
                loadSessions(true);
                if (selectedSessionId_ == sessionId) loadCurrent();
                emit stateChanged();
            }, Qt::QueuedConnection);
        });
}

ComparisonController::~ComparisonController()
{
    executionManager_.setEventHandler({});
}

QString ComparisonController::currentSessionStatus() const
{
    if (!current_.has_value()) return {};
    return current_->session.status == ai::application::ComparisonSessionStatus::Pending
        ? QStringLiteral("queued")
        : QString::fromLatin1(ai::application::toString(current_->session.status));
}

QString ComparisonController::lastUpdatedText() const
{
    if (!current_.has_value()) return {};
    const auto& session = current_->session;
    const auto value = session.finishedAt.has_value() ? *session.finishedAt
        : session.startedAt.has_value() ? *session.startedAt : session.createdAt;
    return QLocale().toString(toDateTime(value).toLocalTime(), QLocale::ShortFormat);
}

QVariantMap ComparisonController::summaryData() const
{
    if (!current_.has_value()) return {};
    const auto& projection = *current_;
    const auto& summary = projection.summary;
    QVariantList selectedIds;
    QVariantList selectedLabels;
    for (const auto& id : projection.session.synthesisSelection) {
        selectedIds.push_back(QString::fromStdString(id));
        selectedLabels.push_back(labelFor(projection, id));
    }
    const QString recommendedId = summary.recommendedExecutionId.has_value()
        ? QString::fromStdString(*summary.recommendedExecutionId) : QString();
    return {
        {QStringLiteral("recommendedResultId"), recommendedId},
        {QStringLiteral("recommendedLabel"), summary.recommendedExecutionId.has_value()
            ? labelFor(projection, *summary.recommendedExecutionId) : QString()},
        {QStringLiteral("recommendationSource"),
            QString::fromStdString(summary.recommendationSource)},
        {QStringLiteral("consensusPoints"), QVariantList {}},
        {QStringLiteral("differencePoints"), QVariantList {}},
        {QStringLiteral("warnings"), QVariantList {}},
        {QStringLiteral("totalCostText"), costText(summary.costMicros)},
        {QStringLiteral("totalTokensText"),
            QLocale().toString(summary.inputTokens + summary.outputTokens)},
        {QStringLiteral("selectedResultIds"), selectedIds},
        {QStringLiteral("selectedLabels"), selectedLabels},
        {QStringLiteral("qualityComplete"), summary.qualityComplete},
        // Selection is persisted today, but the synthesis worker is the next
        // execution-layer milestone. Do not expose a clickable no-op command.
        {QStringLiteral("synthesisEnabled"), false},
        {QStringLiteral("selectionReady"), summary.synthesisEnabled},
        {QStringLiteral("synthesisStatus"), projection.session.synthesisExecutionId.has_value()
            ? QStringLiteral("queued") : QStringLiteral("idle")}
    };
}

QVariantList ComparisonController::modelChoices() const
{
    const auto providers = catalog_.listProviders();
    const auto models = catalog_.listModels();
    if (!providers.success() || !models.success()) return {};
    std::unordered_map<ai::application::ProviderId, ai::application::Provider> enabledProviders;
    for (const auto& provider : providers.value()) {
        if (provider.enabled) enabledProviders.emplace(provider.id, provider);
    }
    QVariantList choices;
    for (const auto& model : models.value()) {
        const auto provider = enabledProviders.find(model.providerId);
        if (!model.enabled || provider == enabledProviders.end()) continue;
        choices.push_back(QVariantMap {
            {QStringLiteral("providerId"), QString::fromStdString(model.providerId)},
            {QStringLiteral("providerName"), QString::fromStdString(provider->second.name)},
            {QStringLiteral("modelId"), QString::fromStdString(model.id)},
            {QStringLiteral("modelName"), QString::fromStdString(model.displayName)},
            {QStringLiteral("isDefault"), model.isDefault}
        });
    }
    return choices;
}

bool ComparisonController::canCreateComparison() const noexcept
{
    return !selectedProjectId_.isEmpty() && !busy_ && modelChoices().size() >= 2;
}

bool ComparisonController::canRefresh() const noexcept
{
    return !selectedSessionId_.isEmpty() && !busy_;
}

bool ComparisonController::canCancel() const noexcept
{
    return !selectedSessionId_.isEmpty()
        && executionManager_.isActive(selectedSessionId_.toStdString());
}

bool ComparisonController::selectProject(const QString& projectId)
{
    if (selectedProjectId_ == projectId) return refresh();
    selectedProjectId_ = projectId;
    selectedSessionId_.clear();
    clearCurrent();
    const bool loaded = loadSessions(false);
    emit stateChanged();
    return loaded;
}

bool ComparisonController::selectSession(const QString& sessionId)
{
    if (selectedSessionId_ == sessionId && current_.has_value()) return refresh();
    selectedSessionId_ = sessionId;
    const bool loaded = loadCurrent();
    emit stateChanged();
    return loaded;
}

bool ComparisonController::refresh()
{
    busy_ = true;
    emit stateChanged();
    const bool sessionsLoaded = loadSessions(true);
    const bool currentLoaded = sessionsLoaded && loadCurrent();
    busy_ = false;
    emit stateChanged();
    return sessionsLoaded && currentLoaded;
}

QString ComparisonController::createComparison(const QVariantMap& request)
{
    ai::application::CreateComparisonSessionRequest command;
    command.projectId = request.value(QStringLiteral("projectId"), selectedProjectId_)
                            .toString().toStdString();
    command.title = request.value(QStringLiteral("title")).toString().toStdString();
    command.prompt = request.value(QStringLiteral("prompt")).toString().toStdString();
    command.contextHash = request.value(QStringLiteral("contextHash")).toString().toStdString();
    const auto slotValues = request.value(QStringLiteral("slots")).toList();
    command.modelSlots.reserve(static_cast<std::size_t>(slotValues.size()));
    for (const auto& value : slotValues) {
        const auto slotValue = value.toMap();
        command.modelSlots.push_back({
            slotValue.value(QStringLiteral("providerId")).toString().toStdString(),
            slotValue.value(QStringLiteral("modelId")).toString().toStdString(),
            slotValue.value(QStringLiteral("role"), QStringLiteral("candidate")).toString().toStdString()});
    }
    const auto created = comparisons_.createSession(command);
    if (!created.success()) {
        fail(*created.error());
        return {};
    }
    selectedProjectId_ = QString::fromStdString(created.value().projectId);
    selectedSessionId_ = QString::fromStdString(created.value().id);
    const auto queued = executionManager_.enqueue(created.value().id);
    if (!queued.success()) {
        static_cast<void>(comparisons_.finishSession(
            created.value().id,
            ai::application::ComparisonSessionStatus::Failed,
            queued.error()->code,
            queued.error()->message));
        fail(*queued.error());
        return {};
    }
    loadSessions(true);
    loadCurrent();
    emit stateChanged();
    return selectedSessionId_;
}

bool ComparisonController::cancelComparison()
{
    if (selectedSessionId_.isEmpty()) return false;
    const auto cancelled = executionManager_.requestCancellation(
        selectedSessionId_.toStdString());
    if (!cancelled.success()) return fail(*cancelled.error());
    return refresh();
}

bool ComparisonController::setPreferredResult(const QString& resultId)
{
    if (selectedSessionId_.isEmpty()) return false;
    const std::optional<ai::application::ModelExecutionId> value = resultId.isEmpty()
        ? std::nullopt : std::optional {resultId.toStdString()};
    const auto changed = comparisons_.setPreferredExecution(
        selectedSessionId_.toStdString(), value);
    if (!changed.success()) return fail(*changed.error());
    return loadCurrent();
}

bool ComparisonController::setResultSelected(const QString& resultId, bool selected)
{
    if (!current_.has_value()) return false;
    auto ids = current_->session.synthesisSelection;
    const auto value = resultId.toStdString();
    const auto found = std::find(ids.begin(), ids.end(), value);
    if (selected && found == ids.end()) ids.push_back(value);
    if (!selected && found != ids.end()) ids.erase(found);
    const auto changed = comparisons_.setSynthesisSelection(
        current_->session.id, std::move(ids));
    if (!changed.success()) return fail(*changed.error());
    return loadCurrent();
}

bool ComparisonController::loadSessions(bool preserveSelection)
{
    if (selectedProjectId_.isEmpty()) {
        sessions_.replace({});
        clearCurrent();
        clearError();
        return true;
    }
    const auto listed = comparisons_.listSessions(selectedProjectId_.toStdString());
    if (!listed.success()) return fail(*listed.error());
    std::vector<ComparisonSessionRow> rows;
    rows.reserve(listed.value().size());
    for (const auto& session : listed.value()) {
        const auto results = comparisons_.listResults(session.id);
        if (!results.success()) return fail(*results.error());
        rows.push_back({session, static_cast<int>(results.value().size())});
    }
    std::reverse(rows.begin(), rows.end());
    if (!preserveSelection || selectedSessionId_.isEmpty()
        || std::none_of(rows.begin(), rows.end(), [&](const auto& row) {
            return row.session.id == selectedSessionId_.toStdString();
        })) {
        selectedSessionId_ = rows.empty()
            ? QString() : QString::fromStdString(rows.front().session.id);
    }
    sessions_.replace(std::move(rows));
    clearError();
    return selectedSessionId_.isEmpty() ? (clearCurrent(), true) : loadCurrent();
}

bool ComparisonController::loadCurrent()
{
    if (selectedSessionId_.isEmpty()) {
        clearCurrent();
        return true;
    }
    auto projection = readModel_.session(selectedSessionId_.toStdString());
    if (!projection.success()) return fail(*projection.error());
    current_ = std::move(projection.value());
    results_.replace(current_->results);
    clearError();
    emit stateChanged();
    return true;
}

void ComparisonController::clearCurrent()
{
    current_.reset();
    results_.replace({});
}

bool ComparisonController::fail(const ai::common::Error& error)
{
    errorText_ = QString::fromStdString(error.message);
    emit errorOccurred(QString::fromStdString(error.code), errorText_);
    emit stateChanged();
    return false;
}

void ComparisonController::clearError()
{
    errorText_.clear();
}
