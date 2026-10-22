#pragma once

#include <ai/application/ComparisonTypes.hpp>

#include <QAbstractListModel>
#include <vector>

/**
 * Read-only Qt projection of ComparisonResults for ComparisonResultCard
 * delegates and ComparisonSummaryPanel.
 *
 * Role names match the existing demo data field names used in ComparisonPage.qml
 * and ComparisonResultCard.qml so no QML code change is needed for the roles.
 */
class ComparisonResultListModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        ResultIdRole = Qt::UserRole + 1,
        ProviderIdRole,
        ProviderNameRole,
        ModelNameRole,
        StatusRole,
        ContentRole,
        StrengthsRole,     // QStringList
        WarningsRole,      // QStringList
        InputTokensRole,   // int
        OutputTokensRole,  // int
        TotalCostRole,
        LatencyTextRole,   // formatted "2.1s" string
        QualityScoreRole,  // int
        IsBestRole,        // bool
        SelectedForSynthesisRole // bool
    };
    Q_ENUM(Role)

    explicit ComparisonResultListModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void replace(std::vector<ai::application::ComparisonResult> results);
    // Applies only the mutable display fields for a single result without
    // rebuilding the full list (avoids flicker during live updates).
    void updateResult(const ai::application::ComparisonResult& result);

private:
    std::vector<ai::application::ComparisonResult> results_;
};
