#include <ai/ui/ArtifactListModel.hpp>

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

ArtifactListModel::ArtifactListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int ArtifactListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(artifacts_.size());
}

QVariant ArtifactListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }

    const auto& artifact = artifacts_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case ArtifactIdRole: return QString::fromStdString(artifact.id);
    case RunIdRole: return QString::fromStdString(artifact.runId);
    case StageIdRole:
        return artifact.stageId.has_value()
            ? QVariant {QString::fromStdString(*artifact.stageId)}
            : QVariant {};
    case KindRole: return QString::fromLatin1(ai::application::toString(artifact.kind));
    case NameRole: return QString::fromStdString(artifact.name);
    case MediaTypeRole: return QString::fromStdString(artifact.mediaType);
    case ByteSizeRole: return QVariant::fromValue<qulonglong>(artifact.byteSize);
    case Sha256Role: return QString::fromStdString(artifact.sha256);
    case CreatedAtRole: return toDateTime(artifact.createdAt);
    default: return {};
    }
}

QHash<int, QByteArray> ArtifactListModel::roleNames() const
{
    return {
        {ArtifactIdRole, "artifactId"}, {RunIdRole, "runId"},
        {StageIdRole, "stageId"}, {KindRole, "kind"},
        {NameRole, "name"}, {MediaTypeRole, "mediaType"},
        {ByteSizeRole, "byteSize"}, {Sha256Role, "sha256"},
        {CreatedAtRole, "createdAt"}
    };
}

void ArtifactListModel::replace(std::vector<ai::application::Artifact> artifacts)
{
    beginResetModel();
    artifacts_ = std::move(artifacts);
    endResetModel();
}
