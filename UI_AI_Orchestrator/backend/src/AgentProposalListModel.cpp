#include "ai/ui/AgentProposalListModel.hpp"

#include <QDateTime>
#include <QLocale>

#include <algorithm>

namespace {

QString toLocalTimeText(std::chrono::system_clock::time_point value)
{
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch());
    return QLocale().toString(
        QDateTime::fromMSecsSinceEpoch(milliseconds.count(), Qt::UTC).toLocalTime(),
        QLocale::ShortFormat);
}

} // namespace

AgentProposalListModel::AgentProposalListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int AgentProposalListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant AgentProposalListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case ChangeSetIdRole: return QString::fromStdString(row.header.changeSetId);
    case StateRole:
        return QString::fromLatin1(ai::application::toString(row.header.state));
    case SummaryRole: return QString::fromStdString(row.header.summary);
    case AgentRoleRole: return QString::fromStdString(row.header.agentRole);
    case ChangeCountRole: return static_cast<int>(row.changes.size());
    case FindingCountRole: return static_cast<int>(row.findings.size());
    case CreatedAtTextRole: return toLocalTimeText(row.header.createdAt);
    case ReviewRejectedRole: {
        const auto found = rejections_.constFind(QString::fromStdString(row.header.changeSetId));
        return found != rejections_.constEnd();
    }
    case ReviewRejectedReasonRole: {
        const auto found = rejections_.constFind(QString::fromStdString(row.header.changeSetId));
        if (found == rejections_.constEnd() || !found->reason.has_value()) return QString();
        return QString::fromStdString(*found->reason);
    }
    case ReviewRejectedAtTextRole: {
        const auto found = rejections_.constFind(QString::fromStdString(row.header.changeSetId));
        if (found == rejections_.constEnd()) return QString();
        return toLocalTimeText(found->rejectedAt);
    }
    case SupersedesChangeSetIdRole:
        return row.header.supersedesChangeSetId.has_value()
            ? QString::fromStdString(*row.header.supersedesChangeSetId) : QString();
    case SupersededByChangeSetIdRole: {
        const auto successor = std::find_if(rows_.begin(), rows_.end(), [&](const auto& candidate) {
            return candidate.header.supersedesChangeSetId.has_value()
                && *candidate.header.supersedesChangeSetId == row.header.changeSetId;
        });
        return successor == rows_.end()
            ? QString() : QString::fromStdString(successor->header.changeSetId);
    }
    case SupersededRole:
        return std::any_of(rows_.begin(), rows_.end(), [&](const auto& candidate) {
            return candidate.header.supersedesChangeSetId.has_value()
                && *candidate.header.supersedesChangeSetId == row.header.changeSetId;
        });
    default: return {};
    }
}

QHash<int, QByteArray> AgentProposalListModel::roleNames() const
{
    return {
        {ChangeSetIdRole, "changeSetId"}, {StateRole, "state"},
        {SummaryRole, "summary"}, {AgentRoleRole, "agentRole"},
        {ChangeCountRole, "changeCount"}, {FindingCountRole, "findingCount"},
        {CreatedAtTextRole, "createdAtText"},
        {ReviewRejectedRole, "reviewRejected"},
        {ReviewRejectedReasonRole, "reviewRejectedReason"},
        {ReviewRejectedAtTextRole, "reviewRejectedAtText"},
        {SupersedesChangeSetIdRole, "supersedesChangeSetId"},
        {SupersededByChangeSetIdRole, "supersededByChangeSetId"},
        {SupersededRole, "superseded"}
    };
}

void AgentProposalListModel::replace(std::vector<ai::application::AgentProposalReadModel> rows)
{
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

void AgentProposalListModel::setReviewRejections(
    QHash<QString, ai::application::ProposalReviewRejection> rejections)
{
    rejections_ = std::move(rejections);
    if (rows_.empty()) return;
    emit dataChanged(
        index(0), index(static_cast<int>(rows_.size()) - 1),
        {ReviewRejectedRole, ReviewRejectedReasonRole, ReviewRejectedAtTextRole});
}
