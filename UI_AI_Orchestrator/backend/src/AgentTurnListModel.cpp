#include "ai/ui/AgentTurnListModel.hpp"

#include <QDateTime>
#include <QLocale>

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

AgentTurnListModel::AgentTurnListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int AgentTurnListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant AgentTurnListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case TurnIdRole: return QString::fromStdString(row.turnId);
    case TurnIndexRole: return QVariant::fromValue<qulonglong>(row.turnIndex);
    case FinishReasonRole: return QString::fromStdString(row.finishReason);
    case InputTokensRole: return row.inputTokens;
    case OutputTokensRole: return row.outputTokens;
    case CallCountRole: return QVariant::fromValue<qulonglong>(row.callCount);
    case CreatedAtTextRole: return toLocalTimeText(row.createdAt);
    default: return {};
    }
}

QHash<int, QByteArray> AgentTurnListModel::roleNames() const
{
    return {
        {TurnIdRole, "turnId"}, {TurnIndexRole, "turnIndex"},
        {FinishReasonRole, "finishReason"}, {InputTokensRole, "inputTokens"},
        {OutputTokensRole, "outputTokens"}, {CallCountRole, "callCount"},
        {CreatedAtTextRole, "createdAtText"}
    };
}

void AgentTurnListModel::replace(std::vector<ai::application::AgentProviderTurn> rows)
{
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}
