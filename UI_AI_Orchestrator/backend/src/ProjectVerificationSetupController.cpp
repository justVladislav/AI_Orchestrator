#include <ai/ui/ProjectVerificationSetupController.hpp>

#include <ai/application/verification/ProjectReadinessService.hpp>
#include <ai/application/verification/ProjectSetupReadiness.hpp>
#include <ai/application/verification/VerificationEnvironmentPresetSetupService.hpp>

#include <QtConcurrent/QtConcurrentRun>

#include <QDateTime>

#include <filesystem>
#include <optional>

namespace {

QVariantMap failureMap(const ai::common::Error& error)
{
    QVariantMap result;
    result["success"] = false;
    result["errorCode"] = QString::fromStdString(error.code);
    result["errorText"] = QString::fromStdString(error.message);
    return result;
}

ai::application::CreateLocalVerificationPresetRequest toRequest(
    const std::filesystem::path& projectRoot,
    const QString& generator,
    const QString& configuration,
    const QString& qtPrefixPath,
    const QString& configurePresetName,
    const QString& buildPresetName,
    const QString& binaryDirRelative)
{
    ai::application::CreateLocalVerificationPresetRequest request;
    request.projectRoot = projectRoot;
    request.generator = generator.toStdString();
    request.configuration = configuration.toStdString();
    request.qtPrefixPath = qtPrefixPath.toStdString();
    request.configurePresetName = configurePresetName.toStdString();
    request.buildPresetName = buildPresetName.toStdString();
    request.binaryDirRelative = binaryDirRelative.toStdString();
    return request;
}

} // namespace

ProjectVerificationSetupController::ProjectVerificationSetupController(QObject* parent)
    : QObject(parent)
{
    connect(&probeWatcher_, &QFutureWatcher<ProbeOutcome>::finished,
        this, &ProjectVerificationSetupController::finishProbe);
    connect(&setupCheckWatcher_, &QFutureWatcher<SetupCheckOutcome>::finished,
        this, &ProjectVerificationSetupController::finishSetupCheck);
}

ProjectVerificationSetupController::~ProjectVerificationSetupController()
{
    if (activeProbe_) activeProbe_->stopSource.request_stop();
    if (activeSetupCheck_) activeSetupCheck_->stopSource.request_stop();
    probeWatcher_.waitForFinished();
    setupCheckWatcher_.waitForFinished();
}

QVariantMap ProjectVerificationSetupController::composePreview(
    const QString& generator,
    const QString& configuration,
    const QString& qtPrefixPath,
    const QString& configurePresetName,
    const QString& buildPresetName,
    const QString& binaryDirRelative) const
{
    // projectRoot is irrelevant to composePresetDocument() - it never
    // touches the filesystem for its own sake - so an empty placeholder is
    // safe here.
    const auto request = toRequest(std::filesystem::path(), generator, configuration,
        qtPrefixPath, configurePresetName, buildPresetName, binaryDirRelative);
    const auto composed = ai::application::VerificationEnvironmentPresetSetupService::composePresetDocument(
        request);
    if (!composed.success()) return failureMap(*composed.error());

    QVariantMap result;
    result["success"] = true;
    result["json"] = QString::fromStdString(composed.value());
    return result;
}

QVariantMap ProjectVerificationSetupController::resolveTargetPath(
    const QString& projectRootPath) const
{
    const auto resolved = ai::application::VerificationEnvironmentPresetSetupService::resolveTargetPath(
        std::filesystem::path(projectRootPath.toStdWString()));
    if (!resolved.success()) return failureMap(*resolved.error());

    QVariantMap result;
    result["success"] = true;
    result["targetPath"] = QString::fromStdWString(resolved.value().wstring());
    return result;
}

QVariantMap ProjectVerificationSetupController::createLocalPreset(
    const QString& projectRootPath,
    const QString& generator,
    const QString& configuration,
    const QString& qtPrefixPath,
    const QString& configurePresetName,
    const QString& buildPresetName,
    const QString& binaryDirRelative)
{
    const auto request = toRequest(std::filesystem::path(projectRootPath.toStdWString()),
        generator, configuration, qtPrefixPath, configurePresetName, buildPresetName,
        binaryDirRelative);
    const auto created = ai::application::VerificationEnvironmentPresetSetupService::createLocalPreset(
        request);
    if (!created.success()) return failureMap(*created.error());

    QVariantMap result;
    result["success"] = true;
    result["json"] = QString::fromStdString(created.value().writtenJson);
    result["targetPath"] = QString::fromStdWString(created.value().writtenPath.wstring());
    const quint64 cleanupToken = ++nextCleanupToken_;
    pendingPresets_.insert(cleanupToken,
        PendingPreset {result["targetPath"].toString(), result["json"].toString()});
    result["cleanupToken"] = QVariant::fromValue(cleanupToken);
    return result;
}

QVariantMap ProjectVerificationSetupController::discardGeneratedPreset(quint64 cleanupToken)
{
    const auto pending = pendingPresets_.constFind(cleanupToken);
    if (cleanupToken == 0 || pending == pendingPresets_.cend()) {
        return failureMap({"verification_setup.cleanup_token_unknown",
            "Generated-preset cleanup authority is missing or already consumed"});
    }
    ai::application::CreateLocalVerificationPresetResult created;
    created.writtenPath = std::filesystem::path(pending->path.toStdWString());
    created.writtenJson = pending->json.toStdString();
    const auto result = ai::application::VerificationEnvironmentPresetSetupService::discardGeneratedPreset(
        created);
    if (!result.success()) return failureMap(*result.error());
    pendingPresets_.remove(cleanupToken);
    QVariantMap response;
    response["success"] = true;
    return response;
}

bool ProjectVerificationSetupController::releaseGeneratedPreset(quint64 cleanupToken)
{
    return cleanupToken != 0 && pendingPresets_.remove(cleanupToken) == 1;
}

quint64 ProjectVerificationSetupController::testExistingConfiguration(
    const QString& projectRootPath,
    const QString& configurePresetName,
    const QString& buildPresetName)
{
    if (projectRootPath.isEmpty() || configurePresetName.isEmpty() || testingConfiguration_) {
        return 0;
    }

    const std::filesystem::path projectRoot(projectRootPath.toStdWString());
    const std::string wantedConfigurePreset = configurePresetName.toStdString();
    const std::string wantedBuildPreset = buildPresetName.toStdString();
    activeProbe_ = std::make_shared<ActiveProbe>();
    auto activeProbe = activeProbe_;
    const quint64 probeId = ++nextProbeId_;
    testingConfiguration_ = true;
    emit testingConfigurationChanged();

    probeWatcher_.setFuture(QtConcurrent::run(
        [projectRoot, wantedConfigurePreset, wantedBuildPreset, activeProbe, probeId] {
            ProbeOutcome outcome;
            outcome.probeId = probeId;
            if (wantedBuildPreset.empty()) {
                // The check configures AND builds the pair; a configure preset alone proves nothing about the build.
                outcome.success = false;
                outcome.errorCode = QString::fromLatin1(ai::application::kProjectSetupPresetNotFound);
                outcome.errorText = QStringLiteral("Enter the build preset name as well: the check configures the project with "
                    "the configure preset and then lists the targets of the build preset.");
                return outcome;
            }
            ai::application::SystemManifestVerificationCommandRunner runner;
            // The same honest check a Run uses: the preset must exist, the toolchain must be MSVC x64 and preparable, a real
            // configure must pass, and the build preset must report its targets. Found preset names alone prove none of that.
            const auto checked = ai::application::ProjectSetupReadiness::check(projectRoot,
                ai::application::ProjectSetupSelection {wantedConfigurePreset, wantedBuildPreset}, runner,
                activeProbe->stopSource.get_token());
            if (!checked.ready) {
                outcome.success = false;
                outcome.errorCode = QString::fromStdString(checked.failureCode);
                outcome.errorText = QString::fromStdString(checked.failureMessage);
                return outcome;
            }
            outcome.success = true;
            outcome.state = QStringLiteral("Ready");
            outcome.diagnostic = QStringLiteral("Configured; %1 targets confirmed").arg(checked.targets.size());
            outcome.configurePresetConfirmed = true;
            outcome.buildPresetConfirmed = true;
            return outcome;
        }));
    return probeId;
}

QVariantMap ProjectVerificationSetupController::projectSetupStatus(const QString& projectId) const
{
    QVariantMap result;
    result["exists"] = false;
    result["checking"] = checkingProjectSetup_ && checkingProjectId_ == projectId;
    result["candidates"] = candidatesByProject_.value(projectId);
    if (readiness_ == nullptr) {
        result["errorCode"] = QStringLiteral("project_setup.unavailable");
        return result;
    }
    const auto found = readiness_->current(projectId.toStdString());
    if (!found.success()) {
        result["errorCode"] = QString::fromStdString(found.error()->code);
        result["errorText"] = QString::fromStdString(found.error()->message);
        return result;
    }
    if (!found.value().has_value()) return result;
    const auto& record = *found.value();
    result["exists"] = true;
    result["state"] = QString::fromLatin1(ai::application::toString(record.state));
    result["configurePreset"] = QString::fromStdString(record.configurePreset);
    result["buildPreset"] = QString::fromStdString(record.buildPreset);
    result["failureCode"] = QString::fromStdString(record.failureCode);
    result["failureMessage"] = QString::fromStdString(record.failureMessage);
    result["targetCount"] = static_cast<int>(record.targets.size());
    result["checkedAt"] = record.checkedAt.time_since_epoch().count() == 0
        ? QString()
        : QDateTime::fromMSecsSinceEpoch(std::chrono::duration_cast<std::chrono::milliseconds>(
              record.checkedAt.time_since_epoch()).count()).toString(Qt::ISODate);
    return result;
}

quint64 ProjectVerificationSetupController::checkProjectSetup(
    const QString& projectId, const QString& projectRootPath)
{
    if (readiness_ == nullptr || projectId.isEmpty() || projectRootPath.isEmpty() || checkingProjectSetup_) return 0;
    auto* readiness = readiness_;
    const std::filesystem::path projectRoot(projectRootPath.toStdWString());
    const std::string id = projectId.toStdString();
    activeSetupCheck_ = std::make_shared<ActiveProbe>();
    auto active = activeSetupCheck_;
    const quint64 checkId = ++nextCheckId_;
    checkingProjectSetup_ = true;
    checkingProjectId_ = projectId;
    emit checkingProjectSetupChanged();

    setupCheckWatcher_.setFuture(QtConcurrent::run([readiness, projectRoot, id, projectId, active, checkId] {
        SetupCheckOutcome outcome;
        outcome.checkId = checkId;
        outcome.projectId = projectId;
        ai::application::SystemManifestVerificationCommandRunner runner;
        // forceRecheck: the explicit "check again" proves the setup again even when the record is fresh, and forgets a
        // remembered toolchain failure (so installing the missing component works without a restart).
        const auto checked = readiness->ensureReady(id, projectRoot, runner, active->stopSource.get_token(), true);
        outcome.ready = checked.ready;
        outcome.errorCode = QString::fromStdString(checked.failureCode);
        outcome.errorText = QString::fromStdString(checked.failureMessage);
        for (const auto& pair : checked.candidates) {
            outcome.candidates.push_back(QString::fromStdString(pair.configurePreset + " / " + pair.buildPreset));
        }
        return outcome;
    }));
    return checkId;
}

bool ProjectVerificationSetupController::cancelProjectSetupCheck()
{
    if (!checkingProjectSetup_ || !activeSetupCheck_) return false;
    return activeSetupCheck_->stopSource.request_stop();
}

QVariantMap ProjectVerificationSetupController::selectProjectSetupPair(
    const QString& projectId, const QString& projectRootPath, const QString& configurePreset, const QString& buildPreset)
{
    if (readiness_ == nullptr) {
        return failureMap({"project_setup.unavailable", "The project setup service is not available"});
    }
    const auto selected = readiness_->selectPair(projectId.toStdString(), std::filesystem::path(projectRootPath.toStdWString()),
        ai::application::ProjectSetupSelection {configurePreset.toStdString(), buildPreset.toStdString()});
    if (!selected.success()) return failureMap(*selected.error());
    candidatesByProject_.remove(projectId);
    emit projectSetupChanged(projectId);
    QVariantMap result;
    result["success"] = true;
    return result;
}

bool ProjectVerificationSetupController::cancelTestConfiguration()
{
    if (!testingConfiguration_ || !activeProbe_) return false;
    return activeProbe_->stopSource.request_stop();
}

void ProjectVerificationSetupController::finishProbe()
{
    const auto outcome = probeWatcher_.result();
    testingConfiguration_ = false;
    activeProbe_.reset();
    emit testingConfigurationChanged();
    emit testConfigurationFinished(outcome.probeId, outcome.success, outcome.state,
        outcome.diagnostic, outcome.configurePresetConfirmed, outcome.buildPresetConfirmed,
        outcome.errorCode, outcome.errorText);
}

void ProjectVerificationSetupController::finishSetupCheck()
{
    const auto outcome = setupCheckWatcher_.result();
    checkingProjectSetup_ = false;
    checkingProjectId_.clear();
    activeSetupCheck_.reset();
    if (outcome.candidates.isEmpty()) candidatesByProject_.remove(outcome.projectId);
    else candidatesByProject_.insert(outcome.projectId, outcome.candidates);
    emit checkingProjectSetupChanged();
    emit projectSetupChanged(outcome.projectId);
    emit projectSetupCheckFinished(outcome.checkId, outcome.projectId, outcome.ready, outcome.errorCode, outcome.errorText);
}
