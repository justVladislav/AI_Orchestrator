#pragma once

#include <ai/application/agents/AgentToolTypes.hpp>

#include <QAbstractListModel>
#include <vector>

// Audited tool calls for the selected agent tool session. Source data is
// AgentToolAuditRecord, which only ever stores sha256 hashes of arguments/
// results (never the raw JSON) - see AgentToolTypes.hpp. This model exposes
// status, truncation, and error/denial information, never a hash or any
// filesystem path.
class AgentToolCallListModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        AuditIdRole = Qt::UserRole + 1,
        ToolNameRole,
        StatusRole,
        DeniedRole,
        TruncatedRole,
        ReturnedBytesRole,
        ErrorCodeRole,
        CreatedAtTextRole,
        FinishedAtTextRole
    };
    Q_ENUM(Role)

    explicit AgentToolCallListModel(QObject* parent = nullptr);
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    void replace(std::vector<ai::application::AgentToolAuditRecord> rows);

private:
    std::vector<ai::application::AgentToolAuditRecord> rows_;
};
