#include <ai/ui/ExecutionProfileListModel.hpp>

#include <ai/ui/ProviderBudgetVariant.hpp>

#include <QVariantList>
#include <QVariantMap>

#include <utility>

namespace {

QVariantList assignmentsToVariant(
    const std::vector<ai::application::StageModelAssignment>& assignments)
{
    QVariantList result;
    result.reserve(static_cast<qsizetype>(assignments.size()));
    for (const auto& assignment : assignments) {
        QVariantMap item;
        item.insert(QStringLiteral("stageType"), QString::fromStdString(assignment.stageType));
        item.insert(QStringLiteral("providerId"), QString::fromStdString(assignment.providerId));
        item.insert(QStringLiteral("modelId"), QString::fromStdString(assignment.modelId));
        item.insert(QStringLiteral("timeoutMs"),
            QVariant::fromValue<qlonglong>(assignment.timeout.count()));
        QVariantList dependencies;
        dependencies.reserve(static_cast<qsizetype>(assignment.dependencies.size()));
        for (const auto& dependency : assignment.dependencies) {
            dependencies.push_back(QString::fromStdString(dependency));
        }
        item.insert(QStringLiteral("dependencies"), dependencies);
        item.insert(QStringLiteral("maxAttempts"),
            QVariant::fromValue<qulonglong>(assignment.maxAttempts));
        item.insert(QStringLiteral("fallbackProviderId"),
            assignment.fallbackProviderId.has_value()
                ? QVariant {QString::fromStdString(*assignment.fallbackProviderId)} : QVariant {});
        item.insert(QStringLiteral("fallbackModelId"),
            assignment.fallbackModelId.has_value()
                ? QVariant {QString::fromStdString(*assignment.fallbackModelId)} : QVariant {});
        result.push_back(item);
    }
    return result;
}

QVariantMap providerBudgetToVariant(
    const ai::application::ProviderExecutionBudgetOverrides& budget)
{
    return ai::ui::providerBudgetOverridesToVariant(budget);
}

} // namespace

ExecutionProfileListModel::ExecutionProfileListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int ExecutionProfileListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(profiles_.size());
}

QVariant ExecutionProfileListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0
        || index.row() >= static_cast<int>(profiles_.size())) {
        return {};
    }

    const auto& profile = profiles_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case ProfileIdRole: return QString::fromStdString(profile.id);
    case NameRole: return QString::fromStdString(profile.name);
    case WorkflowIdRole: return QString::fromStdString(profile.workflowId);
    case EnabledRole: return profile.enabled;
    case IsDefaultRole: return profile.isDefault;
    case AssignmentsRole: return assignmentsToVariant(profile.assignments);
    case ProviderBudgetRole: return providerBudgetToVariant(profile.providerBudget);
    default: return {};
    }
}

QHash<int, QByteArray> ExecutionProfileListModel::roleNames() const
{
    return {
        {ProfileIdRole, "profileId"},
        {NameRole, "name"},
        {WorkflowIdRole, "workflowId"},
        {EnabledRole, "profileEnabled"},
        {IsDefaultRole, "isDefault"},
        {AssignmentsRole, "assignments"},
        {ProviderBudgetRole, "providerBudget"}
    };
}

void ExecutionProfileListModel::replace(
    std::vector<ai::application::ExecutionProfile> profiles)
{
    beginResetModel();
    profiles_ = std::move(profiles);
    endResetModel();
}
