#include <ai/ui/RunListModel.hpp>

#include <ai/application/budget/CostCalculation.hpp>
#include <ai/ui/RunContractVariant.hpp>

#include <QDateTime>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

namespace {

// The frozen turn limit when the Backend raised it automatically (see
// ProviderExecutionBudgetPolicy::autoExtendTurns), else 0. The raise itself is
// Run state in the provider-budget usage; the frozen policy never changes.
std::uint64_t turnLimitRaisedFrom(const ai::application::Run& run)
{
    return run.providerBudget.autoExtendTurns
            && run.providerBudgetUsage.grantedMaxTurns > run.providerBudget.maxTurns
        ? run.providerBudget.maxTurns : 0;
}

// The turn limit the Run is actually held to.
std::uint64_t effectiveMaxTurns(const ai::application::Run& run)
{
    return turnLimitRaisedFrom(run) > 0
        ? std::min(run.providerBudgetUsage.grantedMaxTurns,
              ai::application::kProviderBudgetHardMaxTurns)
        : run.providerBudget.maxTurns;
}

QVariant optionalDateTime(
    const std::optional<std::chrono::system_clock::time_point>& value)
{
    if (!value.has_value()) {
        return {};
    }
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        value->time_since_epoch());
    return QDateTime::fromMSecsSinceEpoch(milliseconds.count(), Qt::UTC);
}

QVariantList assignments(const std::vector<ai::application::StageModelAssignment>& values)
{
    QVariantList result;
    result.reserve(static_cast<qsizetype>(values.size()));
    for (const auto& value : values) {
        QVariantList dependencies;
        dependencies.reserve(static_cast<qsizetype>(value.dependencies.size()));
        for (const auto& dependency : value.dependencies)
            dependencies.push_back(QString::fromStdString(dependency));
        result.push_back(QVariantMap {
            {QStringLiteral("stageType"), QString::fromStdString(value.stageType)},
            {QStringLiteral("providerId"), QString::fromStdString(value.providerId)},
            {QStringLiteral("modelId"), QString::fromStdString(value.modelId)},
            {QStringLiteral("agentName"), QString::fromStdString(value.agentName)},
            {QStringLiteral("role"), QString::fromStdString(value.role)},
            {QStringLiteral("instructions"), QString::fromStdString(value.instructions)},
            {QStringLiteral("position"), value.position},
            {QStringLiteral("leadArchitect"), value.leadArchitect},
            {QStringLiteral("timeoutMs"), QVariant::fromValue<qlonglong>(value.timeout.count())},
            {QStringLiteral("dependencies"), dependencies},
            {QStringLiteral("workspaceAccess"), value.workspaceAccess},
            {QStringLiteral("maxAttempts"), value.maxAttempts},
            {QStringLiteral("fallbackProviderId"), value.fallbackProviderId.has_value()
                ? QString::fromStdString(*value.fallbackProviderId) : QString {}},
            {QStringLiteral("fallbackModelId"), value.fallbackModelId.has_value()
                ? QString::fromStdString(*value.fallbackModelId) : QString {}}
        });
    }
    return result;
}

QList<int> changedRoles(
    const ai::application::Run& previous,
    const ai::application::Run& current)
{
    QList<int> roles;
    const auto changed = [&roles](bool condition, int role) {
        if (condition) roles.push_back(role);
    };
    changed(previous.projectId != current.projectId, RunListModel::ProjectIdRole);
    changed(previous.parentRunId != current.parentRunId, RunListModel::ParentRunIdRole);
    changed(previous.title != current.title, RunListModel::TitleRole);
    changed(previous.task != current.task, RunListModel::TaskRole);
    changed(previous.workflowId != current.workflowId, RunListModel::WorkflowIdRole);
    changed(previous.status != current.status, RunListModel::StatusRole);
    changed(previous.progress != current.progress, RunListModel::ProgressRole);
    changed(previous.currentStageId != current.currentStageId, RunListModel::CurrentStageIdRole);
    changed(previous.errorMessage != current.errorMessage, RunListModel::ErrorMessageRole);
    changed(previous.usage.inputTokens != current.usage.inputTokens, RunListModel::InputTokensRole);
    changed(previous.usage.outputTokens != current.usage.outputTokens, RunListModel::OutputTokensRole);
    changed(previous.usage.cacheCreationInputTokens != current.usage.cacheCreationInputTokens,
        RunListModel::CacheCreationInputTokensRole);
    changed(previous.usage.cacheReadInputTokens != current.usage.cacheReadInputTokens,
        RunListModel::CacheReadInputTokensRole);
    changed(
        ai::application::computeUsageTokenBreakdown(previous.usage).nonCacheInputTokens
            != ai::application::computeUsageTokenBreakdown(current.usage).nonCacheInputTokens,
        RunListModel::NonCacheInputTokensRole);
    changed(previous.maxParallelism != current.maxParallelism, RunListModel::MaxParallelismRole);
    changed(previous.createdAt != current.createdAt, RunListModel::CreatedAtRole);
    changed(previous.startedAt != current.startedAt, RunListModel::StartedAtRole);
    changed(previous.finishedAt != current.finishedAt, RunListModel::FinishedAtRole);

    const QVariantList previousAssignments = assignments(previous.assignmentSnapshot);
    const QVariantList currentAssignments = assignments(current.assignmentSnapshot);
    if (previousAssignments != currentAssignments) {
        roles.push_back(RunListModel::PrimaryProviderIdRole);
        roles.push_back(RunListModel::PrimaryModelIdRole);
        roles.push_back(RunListModel::AssignmentsRole);
    }
    changed(previous.workspacePath != current.workspacePath, RunListModel::WorkspacePathRole);
    changed(previous.providerBudget.maxInputTokens != current.providerBudget.maxInputTokens,
        RunListModel::ProviderBudgetMaxInputTokensRole);
    changed(previous.providerBudget.maxOutputTokens != current.providerBudget.maxOutputTokens,
        RunListModel::ProviderBudgetMaxOutputTokensRole);
    changed(effectiveMaxTurns(previous) != effectiveMaxTurns(current),
        RunListModel::ProviderBudgetMaxTurnsRole);
    changed(turnLimitRaisedFrom(previous) != turnLimitRaisedFrom(current),
        RunListModel::ProviderBudgetTurnsRaisedFromRole);
    changed(previous.providerBudget.maxToolCalls != current.providerBudget.maxToolCalls,
        RunListModel::ProviderBudgetMaxToolCallsRole);
    changed(previous.providerBudget.maxToolResultBytes != current.providerBudget.maxToolResultBytes,
        RunListModel::ProviderBudgetMaxToolResultBytesRole);
    changed(previous.providerBudget.maxWallTimeMs != current.providerBudget.maxWallTimeMs,
        RunListModel::ProviderBudgetMaxWallTimeMsRole);
    changed(previous.providerBudget.maxRequestTimeMs != current.providerBudget.maxRequestTimeMs,
        RunListModel::ProviderBudgetMaxRequestTimeMsRole);
    changed(previous.providerBudgetUsage.turnsStarted != current.providerBudgetUsage.turnsStarted,
        RunListModel::ProviderTurnsStartedRole);
    changed(previous.providerBudgetUsage.turnsCompleted != current.providerBudgetUsage.turnsCompleted,
        RunListModel::ProviderTurnsCompletedRole);
    changed(previous.providerBudgetUsage.toolCalls != current.providerBudgetUsage.toolCalls,
        RunListModel::ProviderToolCallsRole);
    changed(previous.providerBudgetUsage.toolResultBytes != current.providerBudgetUsage.toolResultBytes,
        RunListModel::ProviderToolResultBytesRole);
    changed(previous.providerBudgetUsage.usageAuthoritative
            != current.providerBudgetUsage.usageAuthoritative,
        RunListModel::ProviderUsageAuthoritativeRole);
    changed(previous.providerBudgetUsage.stopCode != current.providerBudgetUsage.stopCode,
        RunListModel::ProviderBudgetStopCodeRole);
    changed(previous.providerBudgetUsage.inputTokens != current.providerBudgetUsage.inputTokens,
        RunListModel::ProviderBudgetInputTokensRole);
    changed(previous.providerBudgetUsage.outputTokens != current.providerBudgetUsage.outputTokens,
        RunListModel::ProviderBudgetOutputTokensRole);
    return roles;
}

} // namespace

RunListModel::RunListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int RunListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(runs_.size());
}

QVariant RunListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }

    const auto& run = runs_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case RunIdRole: return QString::fromStdString(run.id);
    case ProjectIdRole: return QString::fromStdString(run.projectId);
    case ParentRunIdRole: return run.parentRunId.has_value()
        ? QString::fromStdString(*run.parentRunId) : QVariant {};
    case TitleRole: return QString::fromStdString(run.title);
    case TaskRole: return QString::fromStdString(run.task);
    case WorkflowIdRole: return QString::fromStdString(run.workflowId);
    case StatusRole: return QString::fromLatin1(ai::application::toString(run.status));
    case ProgressRole: return run.progress;
    case CurrentStageIdRole: return run.currentStageId.has_value()
        ? QString::fromStdString(*run.currentStageId) : QVariant {};
    case ErrorMessageRole: return QString::fromStdString(run.errorMessage);
    case InputTokensRole: return QVariant::fromValue<qulonglong>(run.usage.inputTokens);
    case OutputTokensRole: return QVariant::fromValue<qulonglong>(run.usage.outputTokens);
    case CacheCreationInputTokensRole:
        return QVariant::fromValue<qulonglong>(run.usage.cacheCreationInputTokens);
    case CacheReadInputTokensRole:
        return QVariant::fromValue<qulonglong>(run.usage.cacheReadInputTokens);
    case NonCacheInputTokensRole:
        return QVariant::fromValue<qulonglong>(
            ai::application::computeUsageTokenBreakdown(run.usage).nonCacheInputTokens);
    case MaxParallelismRole: return QVariant::fromValue<qulonglong>(run.maxParallelism);
    case CreatedAtRole: {
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            run.createdAt.time_since_epoch());
        return QDateTime::fromMSecsSinceEpoch(milliseconds.count(), Qt::UTC);
    }
    case StartedAtRole: return optionalDateTime(run.startedAt);
    case FinishedAtRole: return optionalDateTime(run.finishedAt);
    case PrimaryProviderIdRole: return run.assignmentSnapshot.empty()
        ? QString {} : QString::fromStdString(run.assignmentSnapshot.front().providerId);
    case PrimaryModelIdRole: return run.assignmentSnapshot.empty()
        ? QString {} : QString::fromStdString(run.assignmentSnapshot.front().modelId);
    case AssignmentsRole: return assignments(run.assignmentSnapshot);
    case WorkspacePathRole: return run.workspacePath.has_value()
        ? QString::fromStdString(run.workspacePath->generic_string()) : QString {};
    case ProviderBudgetMaxInputTokensRole:
        return QVariant::fromValue<qulonglong>(run.providerBudget.maxInputTokens);
    case ProviderBudgetMaxOutputTokensRole:
        return QVariant::fromValue<qulonglong>(run.providerBudget.maxOutputTokens);
    case ProviderBudgetMaxTurnsRole:
        return QVariant::fromValue<qulonglong>(effectiveMaxTurns(run));
    case ProviderBudgetTurnsRaisedFromRole:
        return QVariant::fromValue<qulonglong>(turnLimitRaisedFrom(run));
    case ProviderBudgetMaxToolCallsRole:
        return QVariant::fromValue<qulonglong>(run.providerBudget.maxToolCalls);
    case ProviderBudgetMaxToolResultBytesRole:
        return QVariant::fromValue<qulonglong>(run.providerBudget.maxToolResultBytes);
    case ProviderBudgetMaxWallTimeMsRole:
        return QVariant::fromValue<qulonglong>(run.providerBudget.maxWallTimeMs);
    case ProviderBudgetMaxRequestTimeMsRole:
        return QVariant::fromValue<qulonglong>(run.providerBudget.maxRequestTimeMs);
    case ProviderTurnsStartedRole:
        return QVariant::fromValue<qulonglong>(run.providerBudgetUsage.turnsStarted);
    case ProviderTurnsCompletedRole:
        return QVariant::fromValue<qulonglong>(run.providerBudgetUsage.turnsCompleted);
    case ProviderToolCallsRole:
        return QVariant::fromValue<qulonglong>(run.providerBudgetUsage.toolCalls);
    case ProviderToolResultBytesRole:
        return QVariant::fromValue<qulonglong>(run.providerBudgetUsage.toolResultBytes);
    case ProviderUsageAuthoritativeRole: return run.providerBudgetUsage.usageAuthoritative;
    case ProviderBudgetStopCodeRole:
        return QString::fromStdString(run.providerBudgetUsage.stopCode);
    case ProviderBudgetInputTokensRole:
        return QVariant::fromValue<qulonglong>(run.providerBudgetUsage.inputTokens);
    case ProviderBudgetOutputTokensRole:
        return QVariant::fromValue<qulonglong>(run.providerBudgetUsage.outputTokens);
    case InterfaceContractsRole: return ai::ui::runContractsToVariant(run.interfaceContracts);
    case InterfaceContractsFingerprintRole:
        return QString::fromStdString(
            ai::application::runInterfaceContractsFingerprint(run.interfaceContracts));
    case ManualHandoffResolutionRole:
        return index.row() < static_cast<int>(manualHandoffResolutions_.size())
            ? QString::fromLatin1(manualHandoffResolutions_[static_cast<std::size_t>(index.row())])
            : QStringLiteral("not_applicable");
    case LegacyConversionRole:
        return index.row() < static_cast<int>(legacyConversions_.size())
            ? QString::fromLatin1(legacyConversions_[static_cast<std::size_t>(index.row())])
            : QString {};
    default: return {};
    }
}

QHash<int, QByteArray> RunListModel::roleNames() const
{
    return {
        {RunIdRole, "runId"}, {ProjectIdRole, "projectId"},
        {ParentRunIdRole, "parentRunId"}, {TitleRole, "title"},
        {TaskRole, "task"}, {WorkflowIdRole, "workflowId"},
        {StatusRole, "status"}, {ProgressRole, "progress"},
        {CurrentStageIdRole, "currentStageId"}, {ErrorMessageRole, "errorMessage"},
        {InputTokensRole, "inputTokens"}, {OutputTokensRole, "outputTokens"},
        {CacheCreationInputTokensRole, "cacheCreationInputTokens"},
        {CacheReadInputTokensRole, "cacheReadInputTokens"},
        {NonCacheInputTokensRole, "nonCacheInputTokens"},
        {MaxParallelismRole, "maxParallelism"},
        {CreatedAtRole, "createdAt"}, {StartedAtRole, "startedAt"},
        {FinishedAtRole, "finishedAt"},
        {PrimaryProviderIdRole, "primaryProviderId"},
        {PrimaryModelIdRole, "primaryModelId"},
        {AssignmentsRole, "assignments"},
        {WorkspacePathRole, "workspacePath"},
        {ProviderBudgetMaxInputTokensRole, "providerBudgetMaxInputTokens"},
        {ProviderBudgetMaxOutputTokensRole, "providerBudgetMaxOutputTokens"},
        {ProviderBudgetMaxTurnsRole, "providerBudgetMaxTurns"},
        {ProviderBudgetMaxToolCallsRole, "providerBudgetMaxToolCalls"},
        {ProviderBudgetMaxToolResultBytesRole, "providerBudgetMaxToolResultBytes"},
        {ProviderBudgetMaxWallTimeMsRole, "providerBudgetMaxWallTimeMs"},
        {ProviderBudgetMaxRequestTimeMsRole, "providerBudgetMaxRequestTimeMs"},
        {ProviderTurnsStartedRole, "providerTurnsStarted"},
        {ProviderTurnsCompletedRole, "providerTurnsCompleted"},
        {ProviderToolCallsRole, "providerToolCalls"},
        {ProviderToolResultBytesRole, "providerToolResultBytes"},
        {ProviderUsageAuthoritativeRole, "providerUsageAuthoritative"},
        {ProviderBudgetStopCodeRole, "providerBudgetStopCode"},
        {ProviderBudgetInputTokensRole, "providerBudgetInputTokens"},
        {ProviderBudgetOutputTokensRole, "providerBudgetOutputTokens"},
        {ManualHandoffResolutionRole, "manualHandoffResolution"},
        {LegacyConversionRole, "legacyConversion"},
        {ProviderBudgetTurnsRaisedFromRole, "providerBudgetTurnsRaisedFrom"},
        {InterfaceContractsRole, "interfaceContracts"},
        {InterfaceContractsFingerprintRole, "interfaceContractsFingerprint"}
    };
}

void RunListModel::replace(
    std::vector<ai::application::Run> runs,
    std::vector<std::string> manualHandoffResolutions,
    std::vector<std::string> legacyConversions)
{
    if (manualHandoffResolutions.size() != runs.size()) {
        manualHandoffResolutions.assign(runs.size(), "not_applicable");
    }
    // A projection that does not line up with the Runs is never applied row by row: no marker is
    // better than a marker on the wrong Run.
    if (legacyConversions.size() != runs.size()) {
        legacyConversions.assign(runs.size(), std::string {});
    }
    // A full reset forces every bound QML Repeater/ListView delegate to be
    // rebuilt. Background projections therefore update stable rows in place
    // and publish only the roles that actually changed. Shape changes (new
    // run or changed filter membership) still require a reset.
    const bool sameShape = runs.size() == runs_.size()
        && std::equal(runs.begin(), runs.end(), runs_.begin(),
            [](const auto& a, const auto& b) { return a.id == b.id; });

    if (!sameShape) {
        beginResetModel();
        runs_ = std::move(runs);
        manualHandoffResolutions_ = std::move(manualHandoffResolutions);
        legacyConversions_ = std::move(legacyConversions);
        endResetModel();
        return;
    }

    for (std::size_t row = 0; row < runs_.size(); ++row) {
        QList<int> roles = changedRoles(runs_[row], runs[row]);
        if (manualHandoffResolutions_[row] != manualHandoffResolutions[row]) {
            roles.push_back(ManualHandoffResolutionRole);
        }
        if (legacyConversions_[row] != legacyConversions[row]) {
            roles.push_back(LegacyConversionRole);
        }
        runs_[row] = std::move(runs[row]);
        manualHandoffResolutions_[row] = std::move(manualHandoffResolutions[row]);
        legacyConversions_[row] = std::move(legacyConversions[row]);
        if (!roles.isEmpty()) {
            const QModelIndex changedIndex = index(static_cast<int>(row));
            emit dataChanged(changedIndex, changedIndex, roles);
        }
    }
}

void RunListModel::updateOne(const ai::application::Run& run)
{
    for (std::size_t i = 0; i < runs_.size(); ++i) {
        if (runs_[i].id == run.id) {
            const QList<int> roles = changedRoles(runs_[i], run);
            if (roles.isEmpty()) return;
            runs_[i] = run;
            const QModelIndex changed = index(static_cast<int>(i));
            emit dataChanged(changed, changed, roles);
            return;
        }
    }
}

QString RunListModel::statusFor(const QString& runId) const
{
    const std::string id = runId.toStdString();
    for (const auto& run : runs_) {
        if (run.id == id) return QString::fromLatin1(ai::application::toString(run.status));
    }
    return {};
}
