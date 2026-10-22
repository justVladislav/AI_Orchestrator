#include <ai/ui/RunWarningListModel.hpp>

#include <QDateTime>
#include <QString>

#include <utility>

namespace {

QDateTime toDateTime(const std::chrono::system_clock::time_point& value)
{
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch());
    return QDateTime::fromMSecsSinceEpoch(milliseconds.count(), Qt::UTC);
}

} // namespace

RunWarningListModel::RunWarningListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int RunWarningListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(warnings_.size());
}

QVariant RunWarningListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }

    const auto& warning = warnings_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case WarningIdRole: return QString::fromStdString(warning.id);
    case RunIdRole: return QString::fromStdString(warning.runId);
    case StageIdRole:
        return warning.stageId.has_value()
            ? QVariant {QString::fromStdString(*warning.stageId)}
            : QVariant {};
    case CodeRole: return QString::fromStdString(warning.code);
    case MessageRole: return QString::fromStdString(warning.message);
    case ArtifactNameRole: return QString::fromStdString(warning.artifactName);
    case CreatedAtRole: return toDateTime(warning.createdAt);
    default: return {};
    }
}

QHash<int, QByteArray> RunWarningListModel::roleNames() const
{
    return {
        {WarningIdRole, "warningId"}, {RunIdRole, "runId"},
        {StageIdRole, "stageId"}, {CodeRole, "code"},
        {MessageRole, "message"}, {ArtifactNameRole, "artifactName"},
        {CreatedAtRole, "createdAt"}
    };
}

void RunWarningListModel::replace(std::vector<ai::application::RunWarning> warnings)
{
    beginResetModel();
    warnings_ = std::move(warnings);
    endResetModel();
}
