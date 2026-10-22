#pragma once

#include <ai/application/ApplicationTypes.hpp>

#include <QAbstractListModel>
#include <vector>

/** Stage timeline for the run selected through RunController. */
class RunStageModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        StageIdRole = Qt::UserRole + 1,
        RunIdRole,
        TypeRole,
        TitleRole,
        DescriptionRole,
        StatusRole,
        ProgressRole,
        ProviderIdRole,
        ModelIdRole,
        InputTokensRole,
        OutputTokensRole,
        ErrorMessageRole,
        StartedAtRole,
        FinishedAtRole
    };
    Q_ENUM(Role)

    explicit RunStageModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void replace(std::vector<ai::application::RunStage> stages);

private:
    std::vector<ai::application::RunStage> stages_;
};
