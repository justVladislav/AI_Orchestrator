#include <ai/ui/SettingsController.hpp>

#include <ai/application/verification/VerificationScratchPolicy.hpp>

#include <QHash>
#include <QVariantMap>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <utility>

namespace {

QString qString(std::string_view value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

} // namespace

SettingsController::SettingsController(
    ai::application::ApplicationSettingsService& settings,
    ai::application::ExecutionProfileService& profiles,
    ai::application::ProviderCatalogService& catalog,
    ai::application::VerificationScratchService& verificationScratch,
    QObject* parent)
    : QObject(parent)
    , service_(settings)
    , profiles_(profiles)
    , catalog_(catalog)
    , verificationScratch_(verificationScratch)
{
    refresh();
}

QString SettingsController::themeMode() const
{
    return qString(ai::application::toString(value_.theme));
}

int SettingsController::interfaceScalePercent() const noexcept
{
    return static_cast<int>(value_.interfaceScalePercent);
}

bool SettingsController::compactMode() const noexcept { return value_.compactMode; }
bool SettingsController::reduceAnimations() const noexcept { return value_.reduceAnimations; }

QString SettingsController::runContextPolicy() const
{
    return qString(ai::application::toString(value_.runContextPolicy));
}

QString SettingsController::defaultExecutionProfileId() const
{
    return value_.defaultExecutionProfileId.has_value()
        ? QString::fromStdString(*value_.defaultExecutionProfileId) : QString {};
}

qulonglong SettingsController::defaultTimeoutMs() const noexcept
{
    return static_cast<qulonglong>(value_.defaultTimeoutMs);
}

int SettingsController::defaultMaxParallelAgents() const noexcept
{
    return static_cast<int>(value_.defaultMaxParallelAgents);
}

bool SettingsController::confirmManyModels() const noexcept
{
    return value_.confirmManyModels;
}

QString SettingsController::chatContextPolicy() const
{
    return qString(ai::application::toString(value_.chatContextPolicy));
}

QString SettingsController::defaultChatProviderId() const
{
    return value_.defaultChatProviderId.has_value()
        ? QString::fromStdString(*value_.defaultChatProviderId) : QString {};
}

QString SettingsController::defaultChatModelId() const
{
    return value_.defaultChatModelId.has_value()
        ? QString::fromStdString(*value_.defaultChatModelId) : QString {};
}

int SettingsController::chatHistoryLimit() const noexcept
{
    return static_cast<int>(value_.chatHistoryLimit);
}

QVariantList SettingsController::executionProfileChoices() const
{
    return executionProfileChoices_;
}

QVariantList SettingsController::chatModelChoices() const { return chatModelChoices_; }
QString SettingsController::lastError() const { return lastError_; }

void SettingsController::refresh()
{
    const auto result = service_.get();
    if (!result.success()) {
        report(*result.error());
        return;
    }
    value_ = result.value();
    clearError();
    emit settingsChanged();
    refreshChoices();
    refreshVerificationScratch();
}

QString SettingsController::verificationScratchRoot() const
{
    return scratchStatus_.configured.has_value()
        ? QString::fromStdWString(scratchStatus_.configured->wstring()) : QString {};
}

QString SettingsController::verificationScratchEffective() const
{
    return QString::fromStdWString(scratchStatus_.effective.wstring());
}

QString SettingsController::verificationScratchSource() const
{
    return scratchStatus_.source == ai::application::VerificationScratchSource::Setting
        ? QStringLiteral("setting") : QStringLiteral("default");
}

bool SettingsController::verificationScratchUsable() const noexcept
{
    return scratchStatus_.usable;
}

QString SettingsController::verificationScratchProblem() const
{
    return scratchStatus_.problem.has_value() ? QString::fromStdString(scratchStatus_.problem->message) : QString {};
}

int SettingsController::verificationScratchMaxLength() const noexcept
{
    return static_cast<int>(ai::application::kMaxVerificationScratchRootLength);
}

bool SettingsController::setVerificationScratchRoot(const QString& path)
{
    const QString trimmed = path.trimmed();
    const auto result = verificationScratch_.set(trimmed.isEmpty()
        ? std::optional<std::filesystem::path> {}
        : std::optional<std::filesystem::path> {std::filesystem::path(trimmed.toStdWString())});
    if (!result.success()) {
        report(*result.error());
        // The page still shows what is in force (the previous setting), not what was typed.
        refreshVerificationScratch();
        return false;
    }
    clearError();
    refreshVerificationScratch();
    return true;
}

void SettingsController::refreshVerificationScratch()
{
    scratchStatus_ = verificationScratch_.status();
    emit verificationScratchChanged();
}

bool SettingsController::setThemeMode(const QString& value)
{
    const auto parsed = ai::application::themePreferenceFromString(
        value.trimmed().toLower().toStdString());
    if (!parsed.has_value()) {
        report({"settings.invalid_theme", "Unsupported theme mode"});
        return false;
    }
    if (value_.theme == *parsed) return true;
    auto request = requestFromCache();
    request.theme = *parsed;
    return save(std::move(request));
}

bool SettingsController::setInterfaceScalePercent(int value)
{
    if (value < 0) {
        report({"settings.invalid_scale", "Interface scale cannot be negative"});
        return false;
    }
    if (interfaceScalePercent() == value) return true;
    auto request = requestFromCache();
    request.interfaceScalePercent = static_cast<std::uint32_t>(value);
    return save(std::move(request));
}

bool SettingsController::setCompactMode(bool value)
{
    if (value_.compactMode == value) return true;
    auto request = requestFromCache();
    request.compactMode = value;
    return save(std::move(request));
}

bool SettingsController::setReduceAnimations(bool value)
{
    if (value_.reduceAnimations == value) return true;
    auto request = requestFromCache();
    request.reduceAnimations = value;
    return save(std::move(request));
}

bool SettingsController::setRunContextPolicy(const QString& value)
{
    const auto parsed = ai::application::projectContextPolicyFromString(
        value.trimmed().toLower().toStdString());
    if (!parsed.has_value()) {
        report({"settings.invalid_context_policy", "Unsupported project context policy"});
        return false;
    }
    if (value_.runContextPolicy == *parsed) return true;
    auto request = requestFromCache();
    request.runContextPolicy = *parsed;
    return save(std::move(request));
}

bool SettingsController::setDefaultExecutionProfileId(const QString& value)
{
    const QString normalized = value.trimmed();
    if (defaultExecutionProfileId() == normalized) return true;
    auto request = requestFromCache();
    request.defaultExecutionProfileId = normalized.isEmpty()
        ? std::nullopt
        : std::optional<ai::application::ExecutionProfileId> {normalized.toStdString()};
    return save(std::move(request));
}

bool SettingsController::setDefaultTimeoutMs(qulonglong value)
{
    if (defaultTimeoutMs() == value) return true;
    auto request = requestFromCache();
    request.defaultTimeoutMs = static_cast<std::uint64_t>(value);
    return save(std::move(request));
}

bool SettingsController::setDefaultMaxParallelAgents(int value)
{
    if (value < 0) {
        report({"settings.invalid_parallelism", "Parallel agent count cannot be negative"});
        return false;
    }
    if (defaultMaxParallelAgents() == value) return true;
    auto request = requestFromCache();
    request.defaultMaxParallelAgents = static_cast<std::uint32_t>(value);
    return save(std::move(request));
}

bool SettingsController::setConfirmManyModels(bool value)
{
    if (value_.confirmManyModels == value) return true;
    auto request = requestFromCache();
    request.confirmManyModels = value;
    return save(std::move(request));
}

bool SettingsController::setChatContextPolicy(const QString& value)
{
    const auto parsed = ai::application::projectContextPolicyFromString(
        value.trimmed().toLower().toStdString());
    if (!parsed.has_value()) {
        report({"settings.invalid_context_policy", "Unsupported project context policy"});
        return false;
    }
    if (value_.chatContextPolicy == *parsed) return true;
    auto request = requestFromCache();
    request.chatContextPolicy = *parsed;
    return save(std::move(request));
}

bool SettingsController::setDefaultChatModel(
    const QString& providerId,
    const QString& modelId)
{
    const QString normalizedProvider = providerId.trimmed();
    const QString normalizedModel = modelId.trimmed();
    if (defaultChatProviderId() == normalizedProvider
        && defaultChatModelId() == normalizedModel) {
        return true;
    }
    auto request = requestFromCache();
    request.defaultChatProviderId = normalizedProvider.isEmpty()
        ? std::nullopt
        : std::optional<ai::application::ProviderId> {normalizedProvider.toStdString()};
    request.defaultChatModelId = normalizedModel.isEmpty()
        ? std::nullopt
        : std::optional<ai::application::ModelId> {normalizedModel.toStdString()};
    return save(std::move(request));
}

bool SettingsController::setChatHistoryLimit(int value)
{
    if (value < 0) {
        report({"settings.invalid_history_limit", "Chat history limit cannot be negative"});
        return false;
    }
    if (chatHistoryLimit() == value) return true;
    auto request = requestFromCache();
    request.chatHistoryLimit = static_cast<std::uint32_t>(value);
    return save(std::move(request));
}

ai::application::UpdateApplicationSettingsRequest
SettingsController::requestFromCache() const
{
    ai::application::UpdateApplicationSettingsRequest request;
    request.theme = value_.theme;
    request.interfaceScalePercent = value_.interfaceScalePercent;
    request.compactMode = value_.compactMode;
    request.reduceAnimations = value_.reduceAnimations;
    request.runContextPolicy = value_.runContextPolicy;
    request.defaultExecutionProfileId = value_.defaultExecutionProfileId;
    request.defaultTimeoutMs = value_.defaultTimeoutMs;
    request.defaultMaxParallelAgents = value_.defaultMaxParallelAgents;
    request.confirmManyModels = value_.confirmManyModels;
    request.chatContextPolicy = value_.chatContextPolicy;
    request.defaultChatProviderId = value_.defaultChatProviderId;
    request.defaultChatModelId = value_.defaultChatModelId;
    request.chatHistoryLimit = value_.chatHistoryLimit;
    return request;
}

bool SettingsController::save(ai::application::UpdateApplicationSettingsRequest request)
{
    const auto result = service_.update(request);
    if (!result.success()) {
        report(*result.error());
        return false;
    }
    value_ = result.value();
    clearError();
    emit settingsChanged();
    return true;
}

void SettingsController::refreshChoices()
{
    QVariantList profileRows;
    profileRows.push_back(QVariantMap {
        {QStringLiteral("value"), QString {}},
        {QStringLiteral("label"), tr("Use system default")}});
    const auto profiles = profiles_.listProfiles();
    if (!profiles.success()) {
        report(*profiles.error());
        return;
    }
    for (const auto& profile : profiles.value()) {
        if (!profile.enabled) continue;
        profileRows.push_back(QVariantMap {
            {QStringLiteral("value"), QString::fromStdString(profile.id)},
            {QStringLiteral("label"), QString::fromStdString(profile.name)}});
    }

    const auto providers = catalog_.listProviders();
    if (!providers.success()) {
        report(*providers.error());
        return;
    }
    const auto models = catalog_.listModels();
    if (!models.success()) {
        report(*models.error());
        return;
    }
    QHash<QString, QString> providerNames;
    QHash<QString, bool> providerEnabled;
    for (const auto& provider : providers.value()) {
        const QString id = QString::fromStdString(provider.id);
        providerNames.insert(id, QString::fromStdString(provider.name));
        providerEnabled.insert(id, provider.enabled);
    }
    QVariantList modelRows;
    modelRows.push_back(QVariantMap {
        {QStringLiteral("value"), QString {}},
        {QStringLiteral("providerId"), QString {}},
        {QStringLiteral("modelId"), QString {}},
        {QStringLiteral("label"), tr("Use first available model")}});
    for (const auto& model : models.value()) {
        const QString providerId = QString::fromStdString(model.providerId);
        if (!model.enabled || !providerEnabled.value(providerId, false)) continue;
        const QString modelId = QString::fromStdString(model.id);
        modelRows.push_back(QVariantMap {
            {QStringLiteral("value"), providerId + QLatin1Char('|') + modelId},
            {QStringLiteral("providerId"), providerId},
            {QStringLiteral("modelId"), modelId},
            {QStringLiteral("label"), tr("%1 / %2").arg(
                 providerNames.value(providerId), QString::fromStdString(model.displayName))}});
    }

    executionProfileChoices_ = std::move(profileRows);
    chatModelChoices_ = std::move(modelRows);
    emit choicesChanged();
}

void SettingsController::report(const ai::common::Error& error)
{
    const QString message = QString::fromStdString(error.message);
    if (lastError_ != message) {
        lastError_ = message;
        emit lastErrorChanged();
    }
    emit operationFailed(QString::fromStdString(error.code), message);
}

void SettingsController::clearError()
{
    if (lastError_.isEmpty()) return;
    lastError_.clear();
    emit lastErrorChanged();
}
