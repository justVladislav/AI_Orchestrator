#pragma once

#include <ai/application/ExecutionProfileTypes.hpp>

#include <QAbstractListModel>

#include <vector>

/** Read-only Qt projection of persisted execution profiles. */
class ExecutionProfileListModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        ProfileIdRole = Qt::UserRole + 1,
        NameRole,
        WorkflowIdRole,
        EnabledRole,
        IsDefaultRole,
        AssignmentsRole,
        ProviderBudgetRole
    };

    explicit ExecutionProfileListModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void replace(std::vector<ai::application::ExecutionProfile> profiles);

private:
    std::vector<ai::application::ExecutionProfile> profiles_;
};
