#pragma once

#include <ai/application/ArtifactService.hpp>
#include <ai/application/OrchestrationService.hpp>
#include <ai/application/ProviderCatalogService.hpp>
#include <ai/application/ManifestVerificationManualHandoffReadModelService.hpp>
#include <ai/ui/ArtifactListModel.hpp>
#include <ai/ui/RunWarningListModel.hpp>
#include <ai/ui/RunListModel.hpp>
#include <ai/ui/RunStageModel.hpp>

#include <QAbstractItemModel>
#include <QFutureWatcher>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <cstdint>
#include <optional>
#include <vector>

/**
 * Qt command/read-model adapter for OrchestrationService.
 * Worker events are queued onto this QObject's GUI thread before model updates.
 */
class RunController final : public QObject
{
    Q_OBJECT
    // Keep QML-facing selection state owned by this controller; views only
    // resolve the selected run through its stable ID.
    Q_PROPERTY(QAbstractItemModel* model READ model CONSTANT)
    Q_PROPERTY(QAbstractItemModel* stageModel READ stageModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* artifactModel READ artifactModel CONSTANT)
    // The durable warnings of the selected Run (see ArtifactService::createDiagnosticArtifact).
    Q_PROPERTY(QAbstractItemModel* warningModel READ warningModel CONSTANT)
    Q_PROPERTY(QString selectedRunId READ selectedRunId NOTIFY selectedRunIdChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QVariantList agentChoices READ agentChoices NOTIFY agentChoicesChanged)
    Q_PROPERTY(bool agentChoicesLoading READ agentChoicesLoading NOTIFY agentChoicesLoadingChanged)

public:
    explicit RunController(
        ai::application::OrchestrationService& service,
        ai::application::ArtifactService& artifactService,
        ai::application::ProviderCatalogService& providerCatalogService,
        ai::application::ManifestVerificationManualHandoffReadModelService& manualHandoffReadModel,
        QObject* parent = nullptr);
    ~RunController() override;

    [[nodiscard]] QAbstractItemModel* model() noexcept;
    [[nodiscard]] QAbstractItemModel* stageModel() noexcept;
    [[nodiscard]] QAbstractItemModel* artifactModel() noexcept;
    [[nodiscard]] QAbstractItemModel* warningModel() noexcept;
    [[nodiscard]] QString selectedRunId() const;
    [[nodiscard]] QString lastError() const;

    // projectTaskId, when non-empty, links this Run to that ProjectTask
    // atomically with its creation (ignored - never needs to be passed - for
    // a retry/edited rerun, which already inherits its source Run's links
    // backend-side; see RunManager::enqueueInternal).
    Q_INVOKABLE QString startRun(
        const QString& projectId,
        const QString& title,
        const QString& task,
        const QString& executionProfileId = {},
        // Empty keeps "the current" workspace, i.e. the project's own
        // rootPath; a non-empty value must be an existing local directory.
        const QString& workspacePath = {},
        const QString& sourceRunId = {},
        const QString& projectTaskId = {},
        bool allowEvidenceBackedNoChanges = false,
        bool automaticVerificationRepair = true);
    Q_INVOKABLE QString startRunWithRoster(
        const QString& projectId,
        const QString& title,
        const QString& task,
        const QVariantList& roster,
        int maxParallelism = 4,
        const QString& workspacePath = {},
        const QString& sourceRunId = {},
        const QString& projectTaskId = {},
        bool allowEvidenceBackedNoChanges = false,
        bool automaticVerificationRepair = true,
        // AG-17.6: "execution-manifest-v1" runs the roster through the converged
        // Manifest workflow (the lead plans, available workers execute the frozen
        // tasks); anything else (the default) keeps the legacy fixed-stage runner.
        const QString& workflowId = {},
        // Explicit provider limits for this roster: an optional-key map (maxInputTokens,
        // maxOutputTokens, maxTurns, maxToolCalls, maxToolResultBytes, maxWallTimeMs,
        // maxRequestTimeMs). An empty map = the Backend defaults with an automatic turn limit; a
        // present maxTurns pins it. The Backend validates and ceilings every value.
        const QVariantMap& providerBudget = {},
        // PC-2: the Run author's interface contracts (a list of {id, kind, title, text,
        // appliesToPaths} maps, see RunContractVariant.hpp). The Backend validates, freezes and
        // delivers them verbatim to the workers they apply to; only the Manifest workflow accepts them.
        const QVariantList& interfaceContracts = {});
    [[nodiscard]] QVariantList agentChoices() const { return agentChoices_; }
    [[nodiscard]] bool agentChoicesLoading() const noexcept { return agentChoicesLoading_; }
    Q_INVOKABLE void refreshAgentChoices();
    // Backend-owned Edit & rerun snapshot. QML supplies only the source ID
    // and never reconstructs execution semantics from display-model fields.
    // For a legacy roster Run that has a Manifest counterpart the draft also carries a
    // `conversion` map (see legacyConversionPreview); `conversion.required` is true for a lone
    // workspace lead, which has NO legacy Edit & rerun variant.
    Q_INVOKABLE QVariantMap rerunDraft(const QString& runId);
    // AG-17.7C-2: the Backend-owned, read-only preview of converting a legacy roster Run into a
    // Manifest Run - what the conversion dialog shows and what a confirmation is bound to. Keys:
    // sourceRunId, sourceWorkflowId, targetWorkflowId, policyId, shape, disposition, reasonCode,
    // reasonMessage, available (the Backend can convert it), required (no legacy variant exists),
    // safeAutomaticRetry, legacyRerunAvailable, confirmationFingerprint (OPAQUE: QML hands it back
    // unchanged), hasDroppedInstructions, dropped[] ({memberId, setting, legacyValue, reason}; an
    // instructions entry is a LENGTH, never text), roster[] (the LOCKED target roster, without
    // instruction text) and defaults ({title, task, workspacePath, maxParallelism,
    // allowEvidenceBackedNoChanges, automaticVerificationRepair}). Empty map + lastError on failure.
    Q_INVOKABLE QVariantMap legacyConversionPreview(const QString& runId);
    // AG-17.7C-3: the stored record of a converted Run, for Run Details - read-only, immutable,
    // Backend-validated. `state` is "none" (an ordinary Run: show nothing), "present" or
    // "unreadable" (a corrupt or unreadable record: a typed fault shown on its own, never a half
    // record and never silently hidden). A "present" map carries sourceRunId, sourceRunTitle and
    // sourceRunAvailable (the source may have been deleted - the audit outlives it),
    // sourceWorkflowId, policyId, reasonCode, mode, confirmedAt (ONLY for explicit_user, the
    // Backend's own clock), createdAt, the three fingerprints, droppedCount,
    // hasDroppedInstructions and dropped[] ({memberId, setting, legacyValue, reason}; an
    // instructions entry is a length, never text).
    Q_INVOKABLE QVariantMap legacyConversionRecord(const QString& runId);
    // AG-17.7C-2: the explicit, user-confirmed conversion. The caller edits only title, task,
    // workspace, parallelism and the two policies; the roster, workflow and parent are rebuilt by
    // the Backend from the immutable source Run, and the confirmation is only the opaque
    // fingerprint the preview showed - there is no "confirmed" flag to forge. Returns
    // {accepted, runId, errorCode, errorMessage, confirmationStale, confirmationRequired}.
    Q_INVOKABLE QVariantMap startConvertedRerun(
        const QString& sourceRunId,
        const QString& title,
        const QString& task,
        const QString& workspacePath,
        int maxParallelism,
        bool allowEvidenceBackedNoChanges,
        bool automaticVerificationRepair,
        const QString& expectedConfirmationFingerprint);
    Q_INVOKABLE bool cancelRun(const QString& runId);
    Q_INVOKABLE QString retryRun(const QString& runId);
    Q_INVOKABLE void selectRun(const QString& runId);
    Q_INVOKABLE void refresh(const QString& projectId = {});
    // Re-read the current list shape plus its manual-resolution batch without
    // changing the active Backend project filter or reloading details.
    void refreshManualHandoffResolutions();
    Q_INVOKABLE bool openArtifact(const QString& artifactId);
    Q_INVOKABLE bool openArtifactsFolder(const QString& artifactId);
    // In-app, read-only alternative to openArtifact(): returns the artifact's
    // text content directly instead of shelling out via QDesktopServices, so
    // a QML dialog can display it (e.g. a run's context snapshot) without any
    // edit/save affordance. Empty string + lastError on failure, matching
    // openArtifact()'s error-reporting pattern.
    Q_INVOKABLE QString readArtifactText(const QString& artifactId);

signals:
    void runAccepted(const QString& runId);
    // A Retry was refused because converting that legacy Run to the Manifest workflow drops
    // settings the user must confirm (legacy_roster.conversion_confirmation_required). No Run was
    // created; the page answers by opening the conversion dialog for `sourceRunId`.
    void legacyConversionRequested(const QString& sourceRunId);
    void runUpdated(const QString& runId);
    void runFinished(const QString& runId, const QString& status);
    void selectedRunIdChanged();
    void operationFailed(const QString& code, const QString& message);
    void lastErrorChanged();
    void agentChoicesChanged();
    void agentChoicesLoadingChanged();

private:
    struct RunRefreshRequest {
        std::uint64_t requestId {0};
        QString projectFilter;
        QString selectedRunId;
        bool replaceRunList {false};
        bool refreshStages {false};
        bool refreshArtifacts {false};
        QSet<QString> changedRunIds;
        QSet<QString> terminalRunIds;
    };

    struct RunRefreshOutcome {
        RunRefreshRequest request;
        std::optional<std::vector<ai::application::Run>> runs;
        std::optional<std::vector<std::string>> manualHandoffResolutions;
        // AG-17.7C-3: the history marker of every row, aligned with `runs`. A failed index read
        // only costs the markers (reported as an error); the Run list itself is still applied.
        std::optional<std::vector<std::string>> legacyConversions;
        std::optional<ai::common::Error> legacyConversionIndexError;
        std::optional<std::vector<ai::application::RunStage>> stages;
        std::optional<std::vector<ai::application::Artifact>> artifacts;
        std::optional<std::vector<ai::application::RunWarning>> warnings;
        std::optional<ai::common::Error> runError;
        std::optional<ai::common::Error> stageError;
        std::optional<ai::common::Error> artifactError;
        std::optional<ai::common::Error> warningError;
    };

    struct CatalogLoadOutcome {
        std::optional<std::vector<ai::application::Provider>> providers;
        std::optional<std::vector<ai::application::Model>> models;
        std::optional<ai::common::Error> error;
    };

    void handleEvent(const ai::application::RunEvent& event);
    void scheduleRefresh(int delayMs = 0);
    void startPendingRefresh();
    void finishRefresh();
    void startCatalogLoad();
    void finishCatalogLoad();
    void reportError(const ai::common::Error& error);
    void clearError();
    [[nodiscard]] QVariantMap conversionMap(
        const ai::application::LegacyConversionPreview& preview) const;
    // "Provider · Model" from the loaded catalog choices; the raw ids when the pair is not in
    // the catalog (a vanished or disabled model must stay visible, never disappear).
    [[nodiscard]] QString modelLabelFor(
        const std::string& providerId, const std::string& modelId) const;

    ai::application::OrchestrationService& service_;
    ai::application::ArtifactService& artifactService_;
    ai::application::ProviderCatalogService& providerCatalogService_;
    ai::application::ManifestVerificationManualHandoffReadModelService& manualHandoffReadModel_;
    RunListModel model_;
    RunStageModel stageModel_;
    ArtifactListModel artifactModel_;
    RunWarningListModel warningModel_;
    QString selectedRunId_;
    QString projectFilter_;
    QString lastError_;
    QVariantList agentChoices_;
    bool agentChoicesLoading_ {false};
    bool catalogReloadPending_ {false};
    QFutureWatcher<CatalogLoadOutcome> catalogWatcher_;

    std::uint64_t nextRefreshId_ {0};
    bool refreshScheduled_ {false};
    bool refreshInFlight_ {false};
    bool pendingReplaceRunList_ {false};
    bool pendingStages_ {false};
    bool pendingArtifacts_ {false};
    QSet<QString> pendingChangedRunIds_;
    QSet<QString> pendingTerminalRunIds_;
    QFutureWatcher<RunRefreshOutcome> refreshWatcher_;
};
