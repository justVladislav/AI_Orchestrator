#include <ai/ui/ExecutionProfileController.hpp>

#include <ai/ui/ProviderBudgetVariant.hpp>

#include <QVariantMap>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>

ExecutionProfileController::ExecutionProfileController(
    ai::application::ExecutionProfileService& service,
    QObject* parent)
    : QObject(parent)
    , service_(service)
    , profiles_(this)
    , runnableProfiles_(this)
{
    refresh();
}

QAbstractItemModel* ExecutionProfileController::profiles() noexcept { return &profiles_; }
QAbstractItemModel* ExecutionProfileController::runnableProfiles() noexcept
{
    return &runnableProfiles_;
}
QString ExecutionProfileController::defaultProfileId() const { return defaultProfileId_; }
QString ExecutionProfileController::lastError() const { return lastError_; }

QString ExecutionProfileController::createProfile(
    const QString& name,
    const QString& workflowId,
    const QVariantList& assignments,
    const QVariantMap& providerBudget)
{
    ai::application::CreateExecutionProfileRequest request;
    request.name = name.trimmed().toStdString();
    request.workflowId = workflowId.trimmed().toStdString();
    request.enabled = true;
    request.assignments = parseAssignments(assignments);
    request.providerBudget = parseProviderBudget(providerBudget);

    const auto result = service_.createProfile(std::move(request));
    if (!result.success()) {
        reportError(*result.error());
        return {};
    }
    clearError();
    const QString profileId = QString::fromStdString(result.value());
    refresh();
    return profileId;
}

bool ExecutionProfileController::updateProfile(
    const QString& profileId,
    const QString& name,
    const QString& workflowId,
    const QVariantList& assignments,
    const QVariantMap& providerBudget)
{
    ai::application::UpdateExecutionProfileRequest request;
    request.id = profileId.toStdString();
    request.name = name.trimmed().toStdString();
    request.workflowId = workflowId.trimmed().toStdString();
    request.assignments = parseAssignments(assignments);
    request.providerBudget = parseProviderBudget(providerBudget);

    const auto result = service_.updateProfile(std::move(request));
    if (!result.success()) {
        reportError(*result.error());
        return false;
    }
    clearError();
    refresh();
    return true;
}

bool ExecutionProfileController::setDefaultProfile(const QString& profileId)
{
    const auto result = service_.setDefaultProfile(profileId.toStdString());
    if (!result.success()) {
        reportError(*result.error());
        return false;
    }
    clearError();
    refresh();
    return true;
}

void ExecutionProfileController::refresh()
{
    const auto result = service_.listProfiles();
    if (!result.success()) {
        reportError(*result.error());
        return;
    }

    std::vector<ai::application::ExecutionProfile> all = result.value();
    std::vector<ai::application::ExecutionProfile> runnable;
    QString newDefault;
    runnable.reserve(all.size());
    for (const auto& profile : all) {
        if (profile.isDefault) {
            newDefault = QString::fromStdString(profile.id);
        }
        if (profile.enabled) {
            runnable.push_back(profile);
        }
    }

    profiles_.replace(std::move(all));
    runnableProfiles_.replace(std::move(runnable));
    if (defaultProfileId_ != newDefault) {
        defaultProfileId_ = newDefault;
        emit defaultProfileIdChanged();
    }
    clearError();
    emit profilesChanged();
}

std::vector<ai::application::StageModelAssignment>
ExecutionProfileController::parseAssignments(const QVariantList& assignments)
{
    std::vector<ai::application::StageModelAssignment> result;
    result.reserve(static_cast<std::size_t>(assignments.size()));
    for (const QVariant& entry : assignments) {
        const QVariantMap value = entry.toMap();
        ai::application::StageModelAssignment assignment;
        assignment.stageType = value.value(QStringLiteral("stageType")).toString().toStdString();
        assignment.providerId = value.value(QStringLiteral("providerId")).toString().toStdString();
        assignment.modelId = value.value(QStringLiteral("modelId")).toString().toStdString();
        assignment.timeout = std::chrono::milliseconds(
            value.value(QStringLiteral("timeoutMs")).toLongLong());
        const QVariantList dependencies = value.value(QStringLiteral("dependencies")).toList();
        assignment.dependencies.reserve(static_cast<std::size_t>(dependencies.size()));
        for (const QVariant& dependency : dependencies) {
            assignment.dependencies.push_back(dependency.toString().toStdString());
        }
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

ai::application::ProviderExecutionBudgetOverrides
ExecutionProfileController::parseProviderBudget(const QVariantMap& providerBudget)
{
    return ai::ui::parseProviderBudgetOverrides(providerBudget);
}

void ExecutionProfileController::reportError(const ai::common::Error& error)
{
    lastError_ = QString::fromStdString(error.message);
    emit lastErrorChanged();
    emit operationFailed(QString::fromStdString(error.code), lastError_);
}

void ExecutionProfileController::clearError()
{
    if (!lastError_.isEmpty()) {
        lastError_.clear();
        emit lastErrorChanged();
    }
}
