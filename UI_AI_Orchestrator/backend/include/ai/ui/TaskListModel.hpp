#pragma once

#include <ai/application/ApplicationTypes.hpp>

#include <QAbstractListModel>
#include <vector>

class TaskListModel final : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Role {
        TaskIdRole = Qt::UserRole + 1, ProjectIdRole, TitleRole, DescriptionRole,
        StatusRole, PriorityRole, RequiresAttentionRole, DueAtRole, PlannedRunAtRole,
        CreatedAtRole, UpdatedAtRole
    };
    Q_ENUM(Role)
    explicit TaskListModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void replace(std::vector<ai::application::ProjectTask> tasks);
private:
    std::vector<ai::application::ProjectTask> tasks_;
};
