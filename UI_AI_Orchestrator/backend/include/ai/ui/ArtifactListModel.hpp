#pragma once

#include <ai/application/artifacts/ArtifactTypes.hpp>

#include <QAbstractListModel>

#include <vector>

/**
 * Read-only Qt projection of Backend-owned artifact metadata.
 *
 * The model deliberately exposes no filesystem path. QML can render artifact
 * identity and descriptive metadata, while path containment and file access
 * remain behind ArtifactService in the Backend.
 */
class ArtifactListModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        ArtifactIdRole = Qt::UserRole + 1,
        RunIdRole,
        StageIdRole,
        KindRole,
        NameRole,
        MediaTypeRole,
        ByteSizeRole,
        Sha256Role,
        CreatedAtRole
    };
    Q_ENUM(Role)

    explicit ArtifactListModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void replace(std::vector<ai::application::Artifact> artifacts);

private:
    std::vector<ai::application::Artifact> artifacts_;
};
