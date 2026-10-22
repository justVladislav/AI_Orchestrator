#pragma once

#include <ai/application/ApplicationTypes.hpp>

#include <QAbstractListModel>
#include <vector>

/** Read-only Qt projection of application projects for QML views. */
class ProjectListModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        ProjectIdRole = Qt::UserRole + 1,
        NameRole,
        DescriptionRole,
        RootPathRole,
        StatusRole,
        CreatedAtRole,
        UpdatedAtRole
    };
    Q_ENUM(Role)

    explicit ProjectListModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    // Controllers replace snapshots only on the Qt GUI thread.
    void replace(std::vector<ai::application::Project> projects);

private:
    std::vector<ai::application::Project> projects_;
};
