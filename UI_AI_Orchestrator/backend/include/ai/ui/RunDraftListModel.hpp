#pragma once

#include <ai/application/RunDraftTypes.hpp>

#include <QAbstractListModel>
#include <QString>
#include <QStringList>

#include <map>
#include <vector>

/**
 * Read-only Qt projection of saved Run drafts.
 *
 * PC-3D: a draft that came from a next-run plan also carries its group: `groupId` (empty for a manual draft),
 * `groupTitle`, `groupOrigin` ("chat_message" / "manual_import"), `groupOrdinal` (-1 for a manual draft),
 * `groupDraftCount` (drafts of that group still saved), `planKey`, `dependsOn` (plan keys) and `dependsOnTitles`
 * (the same dependencies as titles, advisory only), `sourceRunCount` and `edited` (title, task, contracts or
 * dependencies changed since the group was created). All of it is computed here, never in QML.
 */
class RunDraftListModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        DraftIdRole = Qt::UserRole + 1,
        NameRole,
        ProjectIdRole,
        TitleRole,
        TaskRole,
        LaunchModeRole,
        ExecutionProfileIdRole,
        RosterRole,
        MaxParallelismRole,
        WorkspacePathRole,
        UpdatedAtRole,
        GroupIdRole,
        GroupTitleRole,
        GroupOriginRole,
        GroupOrdinalRole,
        GroupDraftCountRole,
        PlanKeyRole,
        DependsOnRole,
        DependsOnTitlesRole,
        SourceRunCountRole,
        EditedRole
    };

    explicit RunDraftListModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    // `groups`: the groups of the drafts that have one (a draft whose group is missing shows no group title).
    void replace(std::vector<ai::application::RunDraft> drafts,
        std::map<ai::application::RunDraftGroupId, ai::application::RunDraftGroup> groups = {});

private:
    struct PlanView {
        QString groupTitle;
        QString groupOrigin;
        int groupDraftCount {0};
        QStringList dependsOnTitles;
        int sourceRunCount {0};
        bool edited {false};
    };

    std::vector<ai::application::RunDraft> drafts_;
    std::vector<PlanView> planViews_;   // parallel to drafts_
};
