#include <ai/ui/RunDraftController.hpp>

#include <ai/application/IWorkflowRunner.hpp>
#include <ai/ui/PlanDraftProjection.hpp>
#include <ai/ui/ProviderBudgetVariant.hpp>
#include <ai/ui/RunContractVariant.hpp>

#include <QVariantMap>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <utility>

namespace {

ai::application::RunLaunchMode parseLaunchMode(const QString& value)
{
    if (value == QStringLiteral("custom_roster")) {
        return ai::application::RunLaunchMode::CustomRoster;
    }
    if (value == QStringLiteral("saved_execution_profile")) {
        return ai::application::RunLaunchMode::SavedExecutionProfile;
    }
    return ai::application::RunLaunchMode::Mock;
}

QVariantList rosterMap(const std::vector<ai::application::StageModelAssignment>& roster)
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

} // namespace

RunDraftController::RunDraftController(
    ai::application::RunDraftService& service,
    QObject* parent)
    : QObject(parent)
    , service_(service)
    , drafts_(this)
{
    refresh();
}

QAbstractItemModel* RunDraftController::drafts() noexcept { return &drafts_; }
QString RunDraftController::lastError() const { return lastError_; }

QString RunDraftController::createDraft(
    const QString& name,
    const QString& projectId,
    const QString& title,
    const QString& task,
    const QString& launchMode,
    const QString& executionProfileId,
    const QVariantList& roster,
    int maxParallelism,
    const QString& workspacePath,
    bool automaticVerificationRepair,
    const QString& workflowId,
    const QVariantMap& providerBudget,
    const QVariantList& interfaceContracts)
{
    auto contracts = ai::ui::runContractsFromVariant(interfaceContracts);
    if (!contracts.success()) {
        reportError(*contracts.error());
        return {};
    }
    ai::application::CreateRunDraftRequest request;
    request.providerBudget = ai::ui::parseProviderBudgetOverrides(providerBudget);
    request.interfaceContracts = std::move(contracts.value());
    request.name = name.trimmed().toStdString();
    request.projectId = projectId.toStdString();
    request.title = title.toStdString();
    request.task = task.toStdString();
    request.launchMode = parseLaunchMode(launchMode);
    request.workflowId = workflowId.toStdString() == ai::application::kExecutionManifestWorkflowId
        ? std::string(ai::application::kExecutionManifestWorkflowId) : std::string {};
    if (!executionProfileId.isEmpty()) {
        request.executionProfileId = executionProfileId.toStdString();
    }
    request.roster = parseRoster(roster);
    request.maxParallelism = static_cast<std::uint32_t>(std::max(1, maxParallelism));
    request.verificationRepairPolicy = automaticVerificationRepair
        ? ai::application::VerificationRepairPolicy::OneBoundedWave
        : ai::application::VerificationRepairPolicy::Disabled;
    if (!workspacePath.isEmpty()) {
        request.workspacePath = std::filesystem::path(workspacePath.toStdString());
    }

    const auto result = service_.createDraft(std::move(request));
    if (!result.success()) {
        reportError(*result.error());
        return {};
    }
    clearError();
    const QString draftId = QString::fromStdString(result.value());
    refresh();
    return draftId;
}

bool RunDraftController::updateDraft(
    const QString& draftId,
    const QString& name,
    const QString& projectId,
    const QString& title,
    const QString& task,
    const QString& launchMode,
    const QString& executionProfileId,
    const QVariantList& roster,
    int maxParallelism,
    const QString& workspacePath,
    bool automaticVerificationRepair,
    const QString& workflowId,
    const QVariantMap& providerBudget,
    const QVariantList& interfaceContracts)
{
    auto contracts = ai::ui::runContractsFromVariant(interfaceContracts);
    if (!contracts.success()) {
        reportError(*contracts.error());
        return false;
    }
    ai::application::UpdateRunDraftRequest request;
    request.providerBudget = ai::ui::parseProviderBudgetOverrides(providerBudget);
    request.interfaceContracts = std::move(contracts.value());
    request.id = draftId.toStdString();
    request.name = name.trimmed().toStdString();
    request.projectId = projectId.toStdString();
    request.title = title.toStdString();
    request.task = task.toStdString();
    request.launchMode = parseLaunchMode(launchMode);
    request.workflowId = workflowId.toStdString() == ai::application::kExecutionManifestWorkflowId
        ? std::string(ai::application::kExecutionManifestWorkflowId) : std::string {};
    if (!executionProfileId.isEmpty()) {
        request.executionProfileId = executionProfileId.toStdString();
    }
    request.roster = parseRoster(roster);
    request.maxParallelism = static_cast<std::uint32_t>(std::max(1, maxParallelism));
    request.verificationRepairPolicy = automaticVerificationRepair
        ? ai::application::VerificationRepairPolicy::OneBoundedWave
        : ai::application::VerificationRepairPolicy::Disabled;
    if (!workspacePath.isEmpty()) {
        request.workspacePath = std::filesystem::path(workspacePath.toStdString());
    }

    const auto result = service_.updateDraft(std::move(request));
    if (!result.success()) {
        reportError(*result.error());
        return false;
    }
    clearError();
    refresh();
    return true;
}

bool RunDraftController::removeDraft(const QString& draftId)
{
    const auto result = service_.removeDraft(draftId.toStdString());
    if (!result.success()) {
        reportError(*result.error());
        return false;
    }
    clearError();
    refresh();
    return true;
}

QVariantMap RunDraftController::openDraft(const QString& draftId)
{
    const auto result = service_.findDraft(draftId.toStdString());
    if (!result.success()) {
        reportError(*result.error());
        return {};
    }
    if (!result.value().has_value()) {
        reportError({"run_draft.not_found", "Run draft not found"});
        return {};
    }

    const auto& draft = *result.value();
    QVariantMap mapped {
        {"draftId", QString::fromStdString(draft.id)},
        {"isDraft", true},
        {"projectId", QString::fromStdString(draft.projectId)},
        {"title", QString::fromStdString(draft.title)},
        {"task", QString::fromStdString(draft.task)},
        {"workflowId", QString::fromStdString(draft.workflowId)},
        {"launchMode", QString::fromLatin1(ai::application::toString(draft.launchMode))},
        {"executionProfileId", draft.executionProfileId
            ? QString::fromStdString(*draft.executionProfileId) : QString {}},
        {"maxParallelism", draft.maxParallelism},
        {"workspacePath", draft.workspacePath
            ? QString::fromStdString(draft.workspacePath->generic_string()) : QString {}},
        {"automaticVerificationRepair",
            draft.verificationRepairPolicy
                == ai::application::VerificationRepairPolicy::OneBoundedWave},
        {"migratedFromLegacyWorkflow", false},
        {"roster", rosterMap(draft.roster)},
        {"providerBudget", ai::ui::providerBudgetOverridesToVariant(draft.providerBudget)},
        {"interfaceContracts", ai::ui::runContractsToVariant(draft.interfaceContracts)}
    };
    // PC-3D: a draft that came from a next-run plan says so (group, order, advisory dependencies, whether it was
    // edited since creation); a manual draft has no `planGroup` key at all.
    if (draft.provenance.has_value()) {
        const auto group = service_.findGroup(draft.provenance->groupId);
        const auto siblings = service_.listGroupDrafts(draft.provenance->groupId);
        if (group.success() && siblings.success()) {
            mapped.insert(QStringLiteral("planGroup"),
                ai::ui::planGroupMap(draft, group.value(), siblings.value()));
        } else {
            reportError(group.success() ? *siblings.error() : *group.error());
            return {};
        }
    }
    clearError();
    return mapped;
}

void RunDraftController::refresh()
{
    const auto result = service_.listDrafts();
    if (!result.success()) {
        reportError(*result.error());
        return;
    }
    // PC-3D: the groups of the plan drafts, one read per distinct group (a handful at most).
    std::map<ai::application::RunDraftGroupId, ai::application::RunDraftGroup> groups;
    for (const auto& draft : result.value()) {
        if (!draft.provenance.has_value() || groups.count(draft.provenance->groupId) > 0) continue;
        const auto group = service_.findGroup(draft.provenance->groupId);
        if (!group.success()) {
            reportError(*group.error());
            return;
        }
        if (group.value().has_value()) groups.emplace(draft.provenance->groupId, *group.value());
    }
    drafts_.replace(result.value(), std::move(groups));
    clearError();
    emit draftsChanged();
}

std::vector<ai::application::StageModelAssignment>
RunDraftController::parseRoster(const QVariantList& roster)
{
    std::vector<ai::application::StageModelAssignment> result;
    result.reserve(static_cast<std::size_t>(roster.size()));
    for (const QVariant& entry : roster) {
        const QVariantMap value = entry.toMap();
        ai::application::StageModelAssignment assignment;
        assignment.stageType = value.value(QStringLiteral("stageType")).toString().toStdString();
        assignment.providerId = value.value(QStringLiteral("providerId")).toString().toStdString();
        assignment.modelId = value.value(QStringLiteral("modelId")).toString().toStdString();
        assignment.agentName = value.value(QStringLiteral("agentName")).toString().toStdString();
        assignment.role = value.value(QStringLiteral("role")).toString().toStdString();
        assignment.instructions = value.value(QStringLiteral("instructions")).toString().toStdString();
        assignment.position = static_cast<std::uint32_t>(
            std::max(0, value.value(QStringLiteral("position")).toInt()));
        assignment.leadArchitect = value.value(QStringLiteral("leadArchitect")).toBool();
        assignment.timeout = std::chrono::milliseconds(
            value.value(QStringLiteral("timeoutMs")).toLongLong());
        const QVariantList dependencies = value.value(QStringLiteral("dependencies")).toList();
        assignment.dependencies.reserve(static_cast<std::size_t>(dependencies.size()));
        for (const QVariant& dependency : dependencies) {
            assignment.dependencies.push_back(dependency.toString().toStdString());
        }
        assignment.workspaceAccess = value.value(QStringLiteral("workspaceAccess")).toBool();
        assignment.maxAttempts = static_cast<std::uint32_t>(
            std::max(0, value.value(QStringLiteral("maxAttempts"), 1).toInt()));
        const QString fallbackProviderId =
            value.value(QStringLiteral("fallbackProviderId")).toString();
        const QString fallbackModelId =
            value.value(QStringLiteral("fallbackModelId")).toString();
        if (!fallbackProviderId.isEmpty()) {
            assignment.fallbackProviderId = fallbackProviderId.toStdString();
        }
        if (!fallbackModelId.isEmpty()) {
            assignment.fallbackModelId = fallbackModelId.toStdString();
        }
        result.push_back(std::move(assignment));
    }
    return result;
}

void RunDraftController::reportError(const ai::common::Error& error)
{
    lastError_ = QString::fromStdString(error.message);
    emit lastErrorChanged();
    emit operationFailed(QString::fromStdString(error.code), lastError_);
}

void RunDraftController::clearError()
{
    if (!lastError_.isEmpty()) {
        lastError_.clear();
        emit lastErrorChanged();
    }
}
