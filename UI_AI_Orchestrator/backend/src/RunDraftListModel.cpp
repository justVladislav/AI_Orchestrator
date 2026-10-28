#include <ai/ui/RunDraftListModel.hpp>

#include <ai/application/planning/RunDraftGroup.hpp>
#include <ai/ui/PlanDraftProjection.hpp>

#include <QDateTime>
#include <QVariantList>
#include <QVariantMap>

#include <utility>

namespace {

// Same shape as RunListModel.cpp's assignments() (the rich Run
// assignmentSnapshot projection), not ExecutionProfileListModel's narrow
// one - a draft's roster must round-trip everything NewRunDialog's custom
// roster editor produces. Kept as its own copy rather than a shared helper
// since RunListModel.cpp's version is file-local.
QVariantList rosterToVariant(const std::vector<ai::application::StageModelAssignment>& roster)
{
    QVariantList result;
    result.reserve(static_cast<qsizetype>(roster.size()));
    for (const auto& entry : roster) {
        QVariantList dependencies;
        dependencies.reserve(static_cast<qsizetype>(entry.dependencies.size()));
        for (const auto& dependency : entry.dependencies) {
            dependencies.push_back(QString::fromStdString(dependency));
        }
        result.push_back(QVariantMap {
            {QStringLiteral("stageType"), QString::fromStdString(entry.stageType)},
            {QStringLiteral("providerId"), QString::fromStdString(entry.providerId)},
            {QStringLiteral("modelId"), QString::fromStdString(entry.modelId)},
            {QStringLiteral("agentName"), QString::fromStdString(entry.agentName)},
            {QStringLiteral("role"), QString::fromStdString(entry.role)},
            {QStringLiteral("instructions"), QString::fromStdString(entry.instructions)},
            {QStringLiteral("position"), entry.position},
            {QStringLiteral("leadArchitect"), entry.leadArchitect},
            {QStringLiteral("timeoutMs"), QVariant::fromValue<qlonglong>(entry.timeout.count())},
            {QStringLiteral("dependencies"), dependencies},
            {QStringLiteral("workspaceAccess"), entry.workspaceAccess},
            {QStringLiteral("maxAttempts"), entry.maxAttempts},
            {QStringLiteral("fallbackProviderId"), entry.fallbackProviderId.has_value()
                ? QString::fromStdString(*entry.fallbackProviderId) : QString {}},
            {QStringLiteral("fallbackModelId"), entry.fallbackModelId.has_value()
                ? QString::fromStdString(*entry.fallbackModelId) : QString {}}
        });
    }
    return result;
}

QDateTime toQDateTime(std::chrono::system_clock::time_point value)
{
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch());
    return QDateTime::fromMSecsSinceEpoch(milliseconds.count(), Qt::UTC);
}

} // namespace

RunDraftListModel::RunDraftListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int RunDraftListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(drafts_.size());
}

QVariant RunDraftListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0
        || index.row() >= static_cast<int>(drafts_.size())) {
        return {};
    }

    const auto& draft = drafts_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case DraftIdRole: return QString::fromStdString(draft.id);
    case NameRole: return QString::fromStdString(draft.name);
    case ProjectIdRole: return QString::fromStdString(draft.projectId);
    case TitleRole: return QString::fromStdString(draft.title);
    case TaskRole: return QString::fromStdString(draft.task);
    case LaunchModeRole:
        return QString::fromLatin1(ai::application::toString(draft.launchMode));
    case ExecutionProfileIdRole:
        return draft.executionProfileId.has_value()
            ? QString::fromStdString(*draft.executionProfileId) : QString {};
    case RosterRole: return rosterToVariant(draft.roster);
    case MaxParallelismRole: return draft.maxParallelism;
    case WorkspacePathRole:
        return draft.workspacePath.has_value()
            ? QString::fromStdString(draft.workspacePath->generic_string()) : QString {};
    case UpdatedAtRole: return toQDateTime(draft.updatedAt);
    default: break;
    }

    const auto& view = planViews_[static_cast<std::size_t>(index.row())];
    const auto& provenance = draft.provenance;
    switch (role) {
    case GroupIdRole: return provenance ? QString::fromStdString(provenance->groupId) : QString {};
    case GroupTitleRole: return view.groupTitle;
    case GroupOriginRole: return view.groupOrigin;
    case GroupOrdinalRole: return provenance ? static_cast<int>(provenance->ordinal) : -1;
    case GroupDraftCountRole: return view.groupDraftCount;
    case PlanKeyRole: return provenance ? QString::fromStdString(provenance->planKey) : QString {};
    case DependsOnRole: return provenance ? ai::ui::toQStringList(provenance->dependsOn) : QStringList {};
    case DependsOnTitlesRole: return view.dependsOnTitles;
    case SourceRunCountRole: return view.sourceRunCount;
    case EditedRole: return view.edited;
    default: return {};
    }
}

QHash<int, QByteArray> RunDraftListModel::roleNames() const
{
    return {
        {DraftIdRole, "draftId"},
        {NameRole, "name"},
        {ProjectIdRole, "projectId"},
        {TitleRole, "title"},
        {TaskRole, "task"},
        {LaunchModeRole, "launchMode"},
        {ExecutionProfileIdRole, "executionProfileId"},
        {RosterRole, "roster"},
        {MaxParallelismRole, "maxParallelism"},
        {WorkspacePathRole, "workspacePath"},
        {UpdatedAtRole, "updatedAt"},
        {GroupIdRole, "groupId"},
        {GroupTitleRole, "groupTitle"},
        {GroupOriginRole, "groupOrigin"},
        {GroupOrdinalRole, "groupOrdinal"},
        {GroupDraftCountRole, "groupDraftCount"},
        {PlanKeyRole, "planKey"},
        {DependsOnRole, "dependsOn"},
        {DependsOnTitlesRole, "dependsOnTitles"},
        {SourceRunCountRole, "sourceRunCount"},
        {EditedRole, "edited"}
    };
}

void RunDraftListModel::replace(std::vector<ai::application::RunDraft> drafts,
    std::map<ai::application::RunDraftGroupId, ai::application::RunDraftGroup> groups)
{
    std::vector<PlanView> views;
    views.reserve(drafts.size());
    std::map<ai::application::RunDraftGroupId, int> perGroup;
    for (const auto& draft : drafts) {
        if (draft.provenance.has_value()) ++perGroup[draft.provenance->groupId];
    }
    for (const auto& draft : drafts) {
        PlanView view;
        if (draft.provenance.has_value()) {
            const auto group = groups.find(draft.provenance->groupId);
            if (group != groups.end()) {
                view.groupTitle = QString::fromStdString(group->second.planTitle);
                view.groupOrigin = QString::fromLatin1(ai::application::toString(group->second.origin));
                view.sourceRunCount = static_cast<int>(group->second.sourceRunIds.size());
            }
            view.groupDraftCount = perGroup[draft.provenance->groupId];
            view.dependsOnTitles = ai::ui::draftDependencyTitles(draft, drafts);
            view.edited = ai::ui::draftEditedSinceCreation(draft);
        }
        views.push_back(std::move(view));
    }
    beginResetModel();
    drafts_ = std::move(drafts);
    planViews_ = std::move(views);
    endResetModel();
}
