#pragma once

#include <ai/application/execution/IRunWarningRepository.hpp>

#include <QAbstractListModel>

#include <vector>

/**
 * Read-only Qt projection of the durable warnings of the selected Run (a diagnostic artifact that could not be stored
 * although the Run went on). The rows are read from the database, so they are the same after a restart.
 */
class RunWarningListModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        WarningIdRole = Qt::UserRole + 1,
        RunIdRole,
        StageIdRole,
        CodeRole,
        MessageRole,
        ArtifactNameRole,
        CreatedAtRole
    };
    Q_ENUM(Role)

    explicit RunWarningListModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void replace(std::vector<ai::application::RunWarning> warnings);

private:
    std::vector<ai::application::RunWarning> warnings_;
};
