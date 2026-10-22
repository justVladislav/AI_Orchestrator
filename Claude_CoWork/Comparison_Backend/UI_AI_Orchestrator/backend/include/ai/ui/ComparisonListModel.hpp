#pragma once

#include <ai/application/ComparisonTypes.hpp>

#include <QAbstractListModel>
#include <vector>

/**
 * Read-only Qt projection of ComparisonSessions for the comparison-group
 * picker in ComparisonToolbar.
 *
 * Exposes `itemId` (= comparisonId) and `itemLabel` (= title) roles for direct
 * binding to ComparisonComboBox, plus additional roles consumed by ComparisonPage.
 */
class ComparisonListModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        ComparisonIdRole = Qt::UserRole + 1,
        ItemIdRole,       // = comparisonId, alias for ComparisonComboBox compatibility
        TitleRole,
        ItemLabelRole,    // = title, alias for ComparisonComboBox compatibility
        ProjectIdRole,
        UpdatedAtRole,    // ISO-8601 / relative string produced by the controller
        StatusRole
    };
    Q_ENUM(Role)

    explicit ComparisonListModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void replace(std::vector<ai::application::ComparisonSession> sessions);

private:
    std::vector<ai::application::ComparisonSession> sessions_;
};
