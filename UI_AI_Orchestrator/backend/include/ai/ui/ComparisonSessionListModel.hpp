#pragma once

#include <ai/application/ComparisonTypes.hpp>

#include <QAbstractListModel>
#include <vector>

struct ComparisonSessionRow {
    ai::application::ComparisonSession session;
    int resultCount {0};
};

class ComparisonSessionListModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        SessionIdRole = Qt::UserRole + 1,
        ProjectIdRole,
        TitleRole,
        StatusRole,
        UpdatedAtRole,
        UpdatedAtTextRole,
        ResultCountRole
    };
    Q_ENUM(Role)

    explicit ComparisonSessionListModel(QObject* parent = nullptr);
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    void replace(std::vector<ComparisonSessionRow> rows);

private:
    std::vector<ComparisonSessionRow> rows_;
};
