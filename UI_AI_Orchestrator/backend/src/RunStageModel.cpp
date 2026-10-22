#include <ai/ui/RunStageModel.hpp>

#include <QDateTime>
#include <QString>

namespace {

QVariant optionalDateTime(
    const std::optional<std::chrono::system_clock::time_point>& value)
{
    if (!value.has_value()) {
        return {};
    }
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        value->time_since_epoch());
    return QDateTime::fromMSecsSinceEpoch(milliseconds.count(), Qt::UTC);
}

} // namespace

RunStageModel::RunStageModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int RunStageModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(stages_.size());
}

QVariant RunStageModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }

    const auto& stage = stages_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case StageIdRole: return QString::fromStdString(stage.id);
    case RunIdRole: return QString::fromStdString(stage.runId);
    case TypeRole: return QString::fromStdString(stage.type);
    case TitleRole: return QString::fromStdString(stage.title);
    case DescriptionRole: return QString::fromStdString(stage.description);
    case StatusRole: return QString::fromLatin1(ai::application::toString(stage.status));
    case ProgressRole: return stage.progress;
    case ProviderIdRole: return QString::fromStdString(stage.providerId);
    case ModelIdRole: return QString::fromStdString(stage.modelId);
    case InputTokensRole: return QVariant::fromValue<qulonglong>(stage.usage.inputTokens);
    case OutputTokensRole: return QVariant::fromValue<qulonglong>(stage.usage.outputTokens);
    case ErrorMessageRole: return QString::fromStdString(stage.errorMessage);
    case StartedAtRole: return optionalDateTime(stage.startedAt);
    case FinishedAtRole: return optionalDateTime(stage.finishedAt);
    default: return {};
    }
}

QHash<int, QByteArray> RunStageModel::roleNames() const
{
    return {
        {StageIdRole, "stageId"}, {RunIdRole, "runId"},
        {TypeRole, "type"}, {TitleRole, "title"},
        {DescriptionRole, "description"}, {StatusRole, "status"},
        {ProgressRole, "progress"}, {ProviderIdRole, "providerId"},
        {ModelIdRole, "modelId"}, {InputTokensRole, "inputTokens"},
        {OutputTokensRole, "outputTokens"}, {ErrorMessageRole, "errorMessage"},
        {StartedAtRole, "startedAt"}, {FinishedAtRole, "finishedAt"}
    };
}

void RunStageModel::replace(std::vector<ai::application::RunStage> stages)
{
    beginResetModel();
    stages_ = std::move(stages);
    endResetModel();
}
