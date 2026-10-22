#include <ai/ui/ProjectMemoryController.hpp>

#include <ai/application/RunContextAssembler.hpp>

#include <QDateTime>
#include <QMetaObject>
#include <QStringList>

namespace {

QString formatTime(std::chrono::system_clock::time_point value)
{
    const auto point = QDateTime::fromMSecsSinceEpoch(
        std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count());
    return point.toString(QStringLiteral("yyyy-MM-dd hh:mm"));
}

QVariantMap segmentMap(const ai::application::ProjectMemorySegment& value)
{
    return {
        {"segmentId", QString::fromStdString(value.id)},
        {"sourceRunId", QString::fromStdString(value.runId)},
        {"summary", QString::fromStdString(value.summary)},
        {"sourceTokenCount", QVariant::fromValue<qulonglong>(value.sourceTokenCount)},
        {"summaryTokenCount", QVariant::fromValue<qulonglong>(value.summaryTokenCount)},
        {"createdAt", formatTime(value.createdAt)}};
}

QVariantMap compactionRecordMap(const ai::application::ProjectMemoryCompactionRecord& value)
{
    QStringList sourceIds;
    for (const auto& id : value.sourceSegmentIds) sourceIds.push_back(QString::fromStdString(id));
    const auto statusName = ai::application::toString(value.status);
    return {
        {"recordId", QString::fromStdString(value.id)},
        {"providerId", QString::fromStdString(value.providerId)},
        {"modelId", QString::fromStdString(value.modelId)},
        {"sourceSegmentIds", sourceIds},
        {"sourceContentHash", QString::fromStdString(value.sourceContentHash)},
        {"resultSegmentId", value.resultSegmentId.has_value()
            ? QString::fromStdString(*value.resultSegmentId) : QString {}},
        {"inputTokens", QVariant::fromValue<qulonglong>(value.inputTokens)},
        {"outputTokens", QVariant::fromValue<qulonglong>(value.outputTokens)},
        {"estimatedCost", QStringLiteral("$%1").arg(value.costMicros / 1'000'000.0, 0, 'f', 4)},
        {"status", QString::fromLatin1(statusName)},
        {"errorCode", QString::fromStdString(value.errorCode)},
        {"errorMessage", QString::fromStdString(value.errorMessage)},
        {"createdAt", formatTime(value.createdAt)}};
}

} // namespace

ProjectMemoryController::ProjectMemoryController(
    ai::application::ProjectMemoryService& memoryService,
    ai::application::ProjectMemoryCompactionService& compactionService,
    ai::application::ProviderCatalogService& catalog,
    QObject* parent)
    : QObject(parent), memoryService_(memoryService), compactionService_(compactionService), catalog_(catalog)
{
}

ProjectMemoryController::~ProjectMemoryController()
{
    if (worker_) { worker_->request_stop(); worker_->join(); }
}

bool ProjectMemoryController::compacting() const noexcept { return worker_.has_value(); }
QString ProjectMemoryController::compactingProjectId() const { return compactingProjectId_; }

void ProjectMemoryController::report(const ai::common::Error& value)
{
    emit errorOccurred(QString::fromStdString(value.code), QString::fromStdString(value.message));
}

QVariantList ProjectMemoryController::segments(const QString& projectId)
{
    const auto result = memoryService_.listMemorySegments(projectId.toStdString());
    QVariantList rows;
    if (!result.success()) { report(*result.error()); return rows; }
    for (const auto& value : result.value()) rows.push_back(segmentMap(value));
    return rows;
}

QVariantList ProjectMemoryController::compactionHistory(const QString& projectId)
{
    const auto result = compactionService_.listCompactionRecords(projectId.toStdString());
    QVariantList rows;
    if (!result.success()) { report(*result.error()); return rows; }
    for (const auto& value : result.value()) rows.push_back(compactionRecordMap(value));
    return rows;
}

QVariantList ProjectMemoryController::providerChoices()
{
    const auto providers = catalog_.listProviders();
    const auto models = catalog_.listModels();
    QVariantList rows;
    if (!providers.success()) { report(*providers.error()); return rows; }
    if (!models.success()) { report(*models.error()); return rows; }
    for (const auto& provider : providers.value()) {
        for (const auto& model : models.value()) {
            if (model.providerId != provider.id) continue;
            const bool available = provider.enabled && model.enabled;
            const QString status = !available ? tr("Disabled")
                : provider.credentialReference.has_value() ? tr("Ready") : tr("Credential required");
            rows.push_back(QVariantMap {
                {"providerId", QString::fromStdString(provider.id)}, {"providerName", QString::fromStdString(provider.name)},
                {"modelId", QString::fromStdString(model.id)}, {"modelName", QString::fromStdString(model.displayName)},
                {"available", available}, {"statusText", status}});
        }
    }
    return rows;
}

bool ProjectMemoryController::compact(const QString& projectId, const QString& providerId, const QString& modelId)
{
    if (worker_) {
        emit errorOccurred(QStringLiteral("project_memory.busy"), tr("Wait for the current compaction or stop it first."));
        return false;
    }
    if (projectId.isEmpty() || providerId.isEmpty() || modelId.isEmpty()) return false;
    startCompaction(projectId, providerId, modelId);
    return true;
}

void ProjectMemoryController::startCompaction(QString projectId, QString providerId, QString modelId)
{
    compactingProjectId_ = projectId;
    emit compactingChanged();
    worker_.emplace([this, projectId, providerId, modelId](std::stop_token token) {
        auto result = compactionService_.compact(
            projectId.toStdString(), providerId.toStdString(), modelId.toStdString(), token);
        QMetaObject::invokeMethod(this, [this, projectId, result = std::move(result)]() mutable {
            if (!result.success()) report(*result.error());
            worker_.reset();
            compactingProjectId_.clear();
            emit segmentsChanged(projectId);
            emit compactionHistoryChanged(projectId);
            emit compactingChanged();
        }, Qt::QueuedConnection);
    });
}

void ProjectMemoryController::stopCompaction()
{
    if (worker_) worker_->request_stop();
}

bool ProjectMemoryController::clearMemory(const QString& projectId)
{
    if (projectId.isEmpty()) return false;
    auto result = memoryService_.removeMemorySegments(projectId.toStdString());
    if (!result.success()) { report(*result.error()); return false; }
    emit segmentsChanged(projectId);
    return true;
}

QVariantMap ProjectMemoryController::memoryBudgetEstimate(
    const QString& projectId, const QVariantList& modelIds)
{
    QVariantMap result {
        {"available", false},
        {"totalSegmentCount", 0},
        {"includedSegmentCount", 0},
        {"estimatedTokens", 0},
        {"budgetTokens", 0}};
    if (projectId.isEmpty()) return result;

    auto segmentsResult = memoryService_.listMemorySegments(projectId.toStdString());
    if (!segmentsResult.success()) { report(*segmentsResult.error()); return result; }
    auto segments = std::move(segmentsResult.value());
    result["totalSegmentCount"] = static_cast<int>(segments.size());
    if (segments.empty()) {
        result["available"] = true;
        return result;
    }

    std::vector<ai::application::StageModelAssignment> assignments;
    assignments.reserve(static_cast<std::size_t>(modelIds.size()));
    for (const QVariant& value : modelIds) {
        const QString modelId = value.toString();
        if (modelId.isEmpty()) continue;
        ai::application::StageModelAssignment assignment;
        assignment.modelId = modelId.toStdString();
        assignments.push_back(std::move(assignment));
    }
    const std::uint64_t budget = ai::application::projectMemoryBudget(
        assignments, [this](const ai::application::ModelId& id) { return catalog_.findModel(id); });

    ai::application::RunContextAssemblyRequest request;
    request.memorySegments = std::move(segments);
    request.memoryTokenBudget = budget;
    const auto assembled = ai::application::assembleRunContext(std::move(request));
    if (!assembled.success()) { report(*assembled.error()); return result; }

    result["available"] = true;
    result["includedSegmentCount"] = static_cast<int>(assembled.value().includedMemorySegmentIds.size());
    result["estimatedTokens"] = QVariant::fromValue<qulonglong>(assembled.value().estimatedMemoryTokens);
    result["budgetTokens"] = QVariant::fromValue<qulonglong>(budget);
    return result;
}
