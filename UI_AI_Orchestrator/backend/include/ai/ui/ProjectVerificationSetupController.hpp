#pragma once

#include <ai/application/verification/ManifestProposalSetVerifier.hpp>

#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <memory>
#include <stop_token>

namespace ai::application {
class ProjectReadinessService;
}

/**
 * AG-18.4B (reopened 2026-09-22): the Qt adapter for
 * ai::application::VerificationEnvironmentPresetSetupService, used only by
 * NewProjectDialog's "Verification setup" section. Never merged into
 * ProjectController - project identity/CRUD and verification-environment
 * setup are separate Backend contracts (VerificationEnvironmentPresetSetupService
 * deliberately never extends Project.description or any other Project
 * field), matching this codebase's own established one-controller-per-
 * dedicated-concern precedent (e.g. ManifestVerificationManualHandoffController
 * staying separate from ManifestController).
 *
 * composePreview()/resolveTargetPath()/createLocalPreset()/
 * discardGeneratedPreset()/releaseGeneratedPreset() are synchronous: each is a fast, local,
 * single-file operation - exactly the same synchronous-on-the-GUI-thread
 * discipline ProjectController::createProject() itself already uses for
 * its own filesystem checks. testExistingConfiguration() is the genuinely slow operation (a real,
 * local `cmake --list-presets` invocation, potentially a full configure)
 * and is therefore async/cancellable via QtConcurrent, one
 * std::stop_source, and one QFutureWatcher - mirroring
 * ManifestVerificationManualHandoffController's own create()/reverify()
 * pattern, simplified here since this controller carries no read-model
 * state and is never bound to a Run/Project selection broadcast.
 *
 * Every async probe is assigned a fresh, monotonically increasing probeId,
 * returned synchronously to the caller and echoed back in
 * testConfigurationFinished(). QML must compare that id against whatever
 * id it is still actually waiting on before applying a completion - fields
 * may have changed, the mode may have switched, or the dialog may have
 * been closed and reopened for a different project while the probe was
 * still running, and a stale completion must never be shown as if it
 * answered the CURRENT question. A probeId of 0 means the call was refused
 * synchronously (nothing started).
 *
 * Never writes anything silently: composePreview() and resolveTargetPath()
 * are read-only (the former validates the selected Qt installation; the
 * latter resolves the exact target path); createLocalPreset() - the only method
 * that writes - must be called only after the caller (NewProjectDialog) has
 * already shown that exact preview/path and received explicit user
 * confirmation. This controller performs no confirmation gating itself.
 * discardGeneratedPreset() is the caller's own compensating action for a
 * preset createLocalPreset() already wrote but whose immediately-following
 * Project-row creation failed - see ProjectsPage.qml.
 */
class ProjectVerificationSetupController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool testingConfiguration READ testingConfiguration NOTIFY testingConfigurationChanged)
    Q_PROPERTY(bool checkingProjectSetup READ checkingProjectSetup NOTIFY checkingProjectSetupChanged)

public:
    explicit ProjectVerificationSetupController(QObject* parent = nullptr);
    ~ProjectVerificationSetupController() override;

    [[nodiscard]] bool testingConfiguration() const noexcept { return testingConfiguration_; }
    [[nodiscard]] bool checkingProjectSetup() const noexcept { return checkingProjectSetup_; }

    // The durable readiness owner (AG-18.4B-R1). Set once while the bridge is composed; without it the readiness methods below
    // answer with a typed "project_setup.unavailable" error.
    void setReadinessService(ai::application::ProjectReadinessService* service) noexcept { readiness_ = service; }

    // Validates every field, including a real, synchronous, filesystem-
    // backed check that qtPrefixPath is an absolute, existing directory
    // containing its own lib/cmake/Qt6/Qt6Config.cmake - never merely a
    // non-empty string. Never writes to disk. Returns a map with:
    //   success (bool), json (QString, present on success),
    //   errorCode/errorText (QString, present on failure).
    Q_INVOKABLE QVariantMap composePreview(
        const QString& generator,
        const QString& configuration,
        const QString& qtPrefixPath,
        const QString& configurePresetName,
        const QString& buildPresetName,
        const QString& binaryDirRelative) const;

    // Read-only - resolves and validates <projectRootPath>/CMakeUserPresets.json
    // (canonical containment, symlink/reparse-point rejection) without
    // checking whether that file already exists. Returns a map with:
    //   success (bool), targetPath (QString, present on success),
    //   errorCode/errorText (QString, present on failure).
    Q_INVOKABLE QVariantMap resolveTargetPath(const QString& projectRootPath) const;

    // The only method that writes to disk. Same request shape as
    // composePreview() plus projectRootPath. Returns a map with:
    //   success (bool), json/targetPath/cleanupToken (present on success),
    //   errorCode/errorText (QString, present on failure - in particular
    //   "verification_setup.preset_already_exists" when a preset (this
    //   service's own or a foreign one) already exists at the target path;
    //   never overwritten, merged, or read). On success, the caller MUST
    //   call discardGeneratedPreset(cleanupToken) if the immediately-following
    //   Project-row creation then fails, so a rejected Project never leaves
    //   a real orphaned preset file behind - see ProjectsPage.qml.
    Q_INVOKABLE QVariantMap createLocalPreset(
        const QString& projectRootPath,
        const QString& generator,
        const QString& configuration,
        const QString& qtPrefixPath,
        const QString& configurePresetName,
        const QString& buildPresetName,
        const QString& binaryDirRelative);

    // Identity-bound compensation. cleanupToken is opaque and resolves only
    // to a create result held privately by this controller; QML can never
    // nominate an arbitrary filesystem path. Returns success/errorCode/
    // errorText. A changed/replaced preset is retained and reported.
    Q_INVOKABLE QVariantMap discardGeneratedPreset(quint64 cleanupToken);

    // Forgets the opaque cleanup authority after Project creation succeeds.
    // It never touches the generated file.
    Q_INVOKABLE bool releaseGeneratedPreset(quint64 cleanupToken);

    // Tests a REAL, already-existing project's own on-disk preset (the
    // "Use existing presets" mode) - a real, local, provider-free
    // `cmake --list-presets=configure`/`=build` probe against
    // projectRootPath, reusing VerificationEnvironmentDiscoveryService::
    // discoverForCandidateRoot() unmodified (never a second, independent
    // parser), reporting whether configurePresetName/buildPresetName are
    // genuinely confirmed usable - never merely present in a JSON file.
    // Returns the assigned probeId (0 = refused synchronously: empty
    // projectRootPath/configurePresetName, or a probe already running).
    // Never invoked automatically - only this explicit "Test
    // configuration" action starts it, since a real configure step can
    // create a build tree as a side effect.
    Q_INVOKABLE quint64 testExistingConfiguration(
        const QString& projectRootPath,
        const QString& configurePresetName,
        const QString& buildPresetName);

    // ---- AG-18.4B-R1: the selected project's durable verification readiness ----

    // What the Backend has RECORDED for the project (a cheap database read; nothing is configured and no process starts):
    //   exists (bool), state ("unverified"/"ready"/"stale"/"failed"), configurePreset, buildPreset, failureCode, failureMessage,
    //   targetCount, checkedAt (ISO text or empty), candidates (list of "configure / build" the user can choose from, filled by
    //   the last check of this session), checking (bool). A Ready record that no longer matches the project is shown as the Run
    //   will see it: the Run re-proves it; only "Check again" does that on demand.
    Q_INVOKABLE QVariantMap projectSetupStatus(const QString& projectId) const;

    // "Check again": the explicit, cancellable setup check (a real configure, provider-free). It ignores a fresh record and a
    // remembered toolchain failure. Returns the assigned checkId (0 = refused: nothing selected or a check already running).
    Q_INVOKABLE quint64 checkProjectSetup(const QString& projectId, const QString& projectRootPath);
    Q_INVOKABLE bool cancelProjectSetupCheck();

    // The user's choice when several usable configure/build pairs exist. Returns success/errorCode/errorText.
    Q_INVOKABLE QVariantMap selectProjectSetupPair(
        const QString& projectId, const QString& projectRootPath, const QString& configurePreset, const QString& buildPreset);

    // Requests cancellation of whichever test*Configuration() probe is
    // currently running. False when nothing is busy or cancellation was
    // already requested.
    Q_INVOKABLE bool cancelTestConfiguration();

signals:
    void testingConfigurationChanged();
    void checkingProjectSetupChanged();
    // The recorded readiness of this project changed (a check finished or a pair was chosen): re-read projectSetupStatus().
    void projectSetupChanged(const QString& projectId);
    // Emitted once when a checkProjectSetup() finishes (also when cancelled). The caller ignores a checkId it is not waiting on.
    void projectSetupCheckFinished(quint64 checkId, const QString& projectId, bool ready, const QString& errorCode,
        const QString& errorText);
    // Emitted once when a test*Configuration() probe finishes (success,
    // failure, or cancellation). probeId is the exact id that call
    // returned - the caller must ignore this signal when probeId does not
    // match whatever probe it is still actually waiting on. state is the
    // discovered VerificationEnvironmentState's own toString()
    // ("ExistingProfile"/"SetupRequired") when success is true; diagnostic
    // is the snapshot's own bounded Backend diagnostic (may be empty).
    // errorCode/errorText are set only when the discovery call itself
    // failed (never merely "no usable profile found" - that is a
    // successful SetupRequired result, not a failure).
    void testConfigurationFinished(
        quint64 probeId,
        bool success,
        const QString& state,
        const QString& diagnostic,
        bool configurePresetConfirmed,
        bool buildPresetConfirmed,
        const QString& errorCode,
        const QString& errorText);

private:
    struct ProbeOutcome {
        quint64 probeId {0};
        bool success {false};
        QString state;
        QString diagnostic;
        bool configurePresetConfirmed {false};
        bool buildPresetConfirmed {false};
        QString errorCode;
        QString errorText;
    };
    struct ActiveProbe {
        std::stop_source stopSource;
    };
    struct SetupCheckOutcome {
        quint64 checkId {0};
        QString projectId;
        bool ready {false};
        QString errorCode;
        QString errorText;
        QStringList candidates;
    };
    struct PendingPreset {
        QString path;
        QString json;
    };

    void finishProbe();
    void finishSetupCheck();

    bool testingConfiguration_ {false};
    quint64 nextProbeId_ {0};
    quint64 nextCleanupToken_ {0};
    QHash<quint64, PendingPreset> pendingPresets_;
    std::shared_ptr<ActiveProbe> activeProbe_;
    QFutureWatcher<ProbeOutcome> probeWatcher_;

    ai::application::ProjectReadinessService* readiness_ {nullptr};
    bool checkingProjectSetup_ {false};
    quint64 nextCheckId_ {0};
    QString checkingProjectId_;
    std::shared_ptr<ActiveProbe> activeSetupCheck_;
    QHash<QString, QStringList> candidatesByProject_;
    QFutureWatcher<SetupCheckOutcome> setupCheckWatcher_;
};
