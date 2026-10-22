#pragma once

#include <ai/application/AgentToolTypes.hpp>

#include <QAbstractListModel>
#include <vector>

// Provider turns for the selected agent tool session. Every source field is
// already UI-safe (AgentProviderTurn never carries raw prompts or provider
// response text - see AgentToolTypes.hpp).
class AgentTurnListModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        TurnIdRole = Qt::UserRole + 1,
        TurnIndexRole,
        FinishReasonRole,
        InputTokensRole,
        OutputTokensRole,
        CallCountRole,
        CreatedAtTextRole
    };
    Q_ENUM(Role)

    explicit AgentTurnListModel(QObject* parent = nullptr);
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    void replace(std::vector<ai::application::AgentProviderTurn> rows);

private:
    std::vector<ai::application::AgentProviderTurn> rows_;
};
