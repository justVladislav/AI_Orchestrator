#pragma once

#include <ai/application/ApplicationTypes.hpp>

#include <QAbstractListModel>
#include <vector>

/**
 * Lightweight projection of the project list with `itemId`/`itemLabel` roles
 * so ComparisonToolbar's project ComboBox (which uses ComparisonComboBox) can
 * bind to it directly without a proxy model or QML adaptation layer.
 *
 * Populated by ComparisonController from ProjectService on construction and on
 * explicit refresh; it does NOT subscribe to live project changes itself.
 */
class ComparisonProjectsModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        ProjectIdRole = Qt::UserRole + 1,
        ItemIdRole,    // = projectId
        NameRole,
        ItemLabelRole  // = name
    };
    Q_ENUM(Role)

    explicit ComparisonProjectsModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void replace(std::vector<ai::application::Project> projects);

private:
    std::vector<ai::application::Project> projects_;
};
