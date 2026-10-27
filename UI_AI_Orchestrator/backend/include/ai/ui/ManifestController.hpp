#pragma once

#include <ai/application/ExecutionManifestReadModelService.hpp>
#include <ai/application/verification/ManifestVerificationContinuationPreparationService.hpp>
#include <ai/application/verification/ManifestVerificationContinuationReadModelService.hpp>
#include <ai/application/verification/ManifestVerificationContinuationRepairService.hpp>
#include <ai/application/verification/VerificationBootstrapRetryService.hpp>

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <memory>
#include <optional>
#include <stop_token>

/**
 * Qt read-model adapter for ExecutionManifestReadModelService: the frozen
 * manifest's tasks joined with their live per-task execution state for the
 * selected Run, plus (AG-18.4A) the Run's centrally-classified verification-
 * environment/bootstrap-lifecycle projection.
 *
 * Safety boundary (matches ExecutionManifestReadModelService's guarantee):
 * nothing this controller exposes to QML is an absolute filesystem path or a
 * secret - owned paths are already workspace-relative in the frozen
 * manifest, and error messages are the same secret-free, provider-neutral
 * strings ChangeProposalValidator/AgentToolTurnLoopRunner already produce
 * for every other proposal-facing surface.
 *
 * Pull-based, like AgentSessionController: there is no dedicated manifest
 * execution event stream to Qt yet, so RunsPage re-selects the run whenever
 * its own runData changes (the same signal that already drives
 * AgentSessionController). Reads here are small (at most a few dozen tasks
 * per the frozen policy ceiling) and go straight through synchronously,
 * unlike AgentSessionController's QFutureWatcher-backed load - a full agent
 * tool session transcript is a meaningfully heavier read than this.
 *
 * AG-18.4A: verificationEnvironmentState/verificationEnvironmentIssue and
 * canRetryAfterSetup/canConfigureBackendProfile are entirely Backend-computed
 * (ExecutionManifestReadModelService::verificationEnvironmentViewForRun) -
 * this controller never reclassifies state, parses diagnostic text, or
 * decides action eligibility itself.
 *
 * retryAfterSetup() calls only VerificationBootstrapRetryService::
 * createLinkedRetry(). This is NOT a "create a scoped setup Run" action - it
 * is a retry after the user has configured or restored a profile/preset.
 * The Backend command checks current preset-file presence before it starts a fresh, baseline-
 * bound Run linked to the predecessor's ActionRequired disposition. An
 * in-product action that actually launches a scoped setup Run does not
 * exist yet (tracked as AG-18.4C in ARCHITECTURE_GAPS.md, not implemented).
 * retryAfterSetup() never touches the Apply-gate (AG-16) or the bootstrap
 * lifecycle (AG-18.3) directly.
 */
class ManifestController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool hasManifest READ hasManifest NOTIFY stateChanged)
    Q_PROPERTY(QVariantList tasks READ tasks NOTIFY stateChanged)
    Q_PROPERTY(QString selectedRunId READ selectedRunId NOTIFY stateChanged)
    Q_PROPERTY(quint32 requestedMaxParallelism READ requestedMaxParallelism NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    // AG-15.3: Run-level workspace-scope projection. hasWorkspaceScope is
    // false whenever no RunWorkspaceScope was ever frozen for this Run (a
    // manifest frozen before AG-15.2 was wired, or the read model was built
    // without a workspace-scope repository) - distinct from "frozen with
    // zero dependency edges", exactly like ExecutionManifestView's own
    // workspaceScopeAvailable field.
    Q_PROPERTY(bool hasWorkspaceScope READ hasWorkspaceScope NOTIFY stateChanged)
    Q_PROPERTY(QVariantList scopeDependencyEdges READ scopeDependencyEdges NOTIFY stateChanged)

    // AG-17.6: the frozen routing identity, entirely Backend-projected
    // (ExecutionManifestReadModelService): routingMode is "none" (no record),
    // "single_assignment" or "roster"; routingMembers lists every member with its
    // role, provider/model label and Backend-classified participation
    // ("lead" | "assigned" | "unused" | "not_assignable"). QML renders both verbatim -
    // an unused worker is a normal state of an available-workers pool, never an error.
    Q_PROPERTY(QString routingMode READ routingMode NOTIFY stateChanged)
    Q_PROPERTY(QVariantList routingMembers READ routingMembers NOTIFY stateChanged)

    // AG-17.7B: member-level usage/cost, reconstructed Backend-side from the Run's
    // ModelExecution rows (ExecutionManifestReadModelService / ManifestMemberUsage). Every
    // routingMembers entry carries a nested `usage` map; the Run-level properties below
    // carry the exact-partition flag, the Run total and the unattributed rows (diagnostic
    // debt, never dropped). QML only renders: it never sums, prices or classifies. A usage
    // map's `priceState` ("none" | "known" | "partial" | "unknown") says whether `costText`
    // is a precise total, a lower bound or not available - an unknown price is never zero.
    Q_PROPERTY(bool usageAvailable READ usageAvailable NOTIFY stateChanged)
    Q_PROPERTY(bool usagePartitionExact READ usagePartitionExact NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap usageRunTotal READ usageRunTotal NOTIFY stateChanged)
    Q_PROPERTY(int usageUnattributedCount READ usageUnattributedCount NOTIFY stateChanged)
    Q_PROPERTY(QVariantList usageUnattributed READ usageUnattributed NOTIFY stateChanged)
    Q_PROPERTY(QStringList usageDiagnostics READ usageDiagnostics NOTIFY stateChanged)

    // QML-lint advisory-severity policy (2026-09-28): Run-level, entirely
    // Backend-computed (ExecutionManifestReadModelService::viewForRun's own
    // verificationAdvisory projection) - this controller never classifies a
    // finding's tag or computes Apply eligibility from advisory presence.
    // hasVerificationAdvisory is false whenever this Run's Work Ledger entry
    // has no verification identity yet (still executing, planning, or the
    // read model was built without a Work Ledger repository) - distinct from
    // "verification available but zero advisories" (hasAdvisories=false).
    // verificationPassed mirrors the same authoritative
    // requiredAcceptancePassed() result AG-16 itself would read; QML must
    // never infer Apply eligibility from hasAdvisories alone.
    Q_PROPERTY(bool hasVerificationAdvisory READ hasVerificationAdvisory NOTIFY stateChanged)
    Q_PROPERTY(bool verificationAdvisoryPassed READ verificationAdvisoryPassed NOTIFY stateChanged)
    Q_PROPERTY(bool hasAdvisories READ hasAdvisories NOTIFY stateChanged)
    Q_PROPERTY(int advisoryCount READ advisoryCount NOTIFY stateChanged)
    Q_PROPERTY(QVariantList advisories READ advisories NOTIFY stateChanged)

    // AG-18.4A: verification-environment/bootstrap-lifecycle projection.
    // hasVerificationEnvironment is false only when no
    // VerificationEnvironmentSnapshot was ever recorded for this Run at all
    // (a non-Manifest-workflow Run, or one that failed before discovery ran)
    // - independent of hasManifest, since an ActionRequired Run never
    // freezes a Manifest at all.
    Q_PROPERTY(bool hasVerificationEnvironment READ hasVerificationEnvironment NOTIFY stateChanged)
    Q_PROPERTY(QString verificationEnvironmentState READ verificationEnvironmentState NOTIFY stateChanged)
    Q_PROPERTY(QString verificationEnvironmentIssue READ verificationEnvironmentIssue NOTIFY stateChanged)
    Q_PROPERTY(QString verificationEnvironmentDiagnostic READ verificationEnvironmentDiagnostic NOTIFY stateChanged)
    Q_PROPERTY(QVariantList verificationProfiles READ verificationProfiles NOTIFY stateChanged)
    Q_PROPERTY(bool hasBootstrapDecision READ hasBootstrapDecision NOTIFY stateChanged)
    Q_PROPERTY(QString bootstrapDisposition READ bootstrapDisposition NOTIFY stateChanged)
    Q_PROPERTY(QString bootstrapReasonCode READ bootstrapReasonCode NOTIFY stateChanged)
    Q_PROPERTY(QString bootstrapMessage READ bootstrapMessage NOTIFY stateChanged)
    Q_PROPERTY(QStringList bootstrapSetupPaths READ bootstrapSetupPaths NOTIFY stateChanged)
    Q_PROPERTY(bool hasBootstrapTransition READ hasBootstrapTransition NOTIFY stateChanged)
    Q_PROPERTY(QString bootstrapTransitionDisposition READ bootstrapTransitionDisposition NOTIFY stateChanged)
    Q_PROPERTY(QString bootstrapCandidateProfileId READ bootstrapCandidateProfileId NOTIFY stateChanged)
    Q_PROPERTY(QString bootstrapCandidateBuildProfileId READ bootstrapCandidateBuildProfileId NOTIFY stateChanged)
    Q_PROPERTY(QString bootstrapCandidateTargetId READ bootstrapCandidateTargetId NOTIFY stateChanged)
    // "Retry after setup": eligibility and its reason are entirely
    // Backend-computed - QML must render canRetryAfterSetupReason as-is when
    // canRetryAfterSetup is false, never compute its own explanation. This
    // is a retry after the user has ALREADY configured/created a usable
    // profile or preset manually or externally. The command performs the
    // current preset-file preflight and surfaces its typed refusal - never a "create a scoped
    // setup Run" action (see AG-18.4C, not implemented).
    Q_PROPERTY(bool canRetryAfterSetup READ canRetryAfterSetup NOTIFY stateChanged)
    Q_PROPERTY(QString canRetryAfterSetupReason READ canRetryAfterSetupReason NOTIFY stateChanged)
    // "Configure verification profile": AG-18.4B's creation-time flow does
    // not yet extend to existing Projects in Run Details - this is always
    // false here with a stable Backend reason code/message (see
    // ExecutionManifestReadModelService's own doc comment). QML shows this
    // as a visible but disabled action, never omits it and never invents
    // its own reason.
    Q_PROPERTY(bool canConfigureBackendProfile READ canConfigureBackendProfile NOTIFY stateChanged)
    Q_PROPERTY(QString canConfigureBackendProfileReasonCode READ canConfigureBackendProfileReasonCode NOTIFY stateChanged)
    Q_PROPERTY(QString canConfigureBackendProfileReason READ canConfigureBackendProfileReason NOTIFY stateChanged)
    Q_PROPERTY(bool hasVerificationContinuation READ hasVerificationContinuation NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap verificationContinuation READ verificationContinuation NOTIFY stateChanged)
    Q_PROPERTY(bool verificationContinuationBusy READ verificationContinuationBusy NOTIFY stateChanged)

public:
    explicit ManifestController(
        ai::application::ExecutionManifestReadModelService& readModel,
        ai::application::VerificationBootstrapRetryService& bootstrapRetryService,
        QObject* parent = nullptr);
    ManifestController(
        ai::application::ExecutionManifestReadModelService& readModel,
        ai::application::VerificationBootstrapRetryService& bootstrapRetryService,
        ai::application::ManifestVerificationContinuationReadModelService& continuationReadModel,
        ai::application::ManifestVerificationContinuationPreparationService& continuationPreparation,
        ai::application::ManifestVerificationContinuationRepairService& continuationRepair,
        QObject* parent = nullptr);
    ~ManifestController() override;

    [[nodiscard]] bool hasManifest() const noexcept { return hasManifest_; }
    [[nodiscard]] QVariantList tasks() const { return tasks_; }
    [[nodiscard]] QString selectedRunId() const { return selectedRunId_; }
    [[nodiscard]] quint32 requestedMaxParallelism() const noexcept { return requestedMaxParallelism_; }
    [[nodiscard]] QString errorText() const { return errorText_; }
    [[nodiscard]] bool hasWorkspaceScope() const noexcept { return hasWorkspaceScope_; }
    [[nodiscard]] QVariantList scopeDependencyEdges() const { return scopeDependencyEdges_; }
    [[nodiscard]] QString routingMode() const { return routingMode_; }
    [[nodiscard]] QVariantList routingMembers() const { return routingMembers_; }
    [[nodiscard]] bool usageAvailable() const noexcept { return usageAvailable_; }
    [[nodiscard]] bool usagePartitionExact() const noexcept { return usagePartitionExact_; }
    [[nodiscard]] QVariantMap usageRunTotal() const { return usageRunTotal_; }
    [[nodiscard]] int usageUnattributedCount() const noexcept { return usageUnattributedCount_; }
    [[nodiscard]] QVariantList usageUnattributed() const { return usageUnattributed_; }
    [[nodiscard]] QStringList usageDiagnostics() const { return usageDiagnostics_; }

    [[nodiscard]] bool hasVerificationAdvisory() const noexcept { return hasVerificationAdvisory_; }
    [[nodiscard]] bool verificationAdvisoryPassed() const noexcept { return verificationAdvisoryPassed_; }
    [[nodiscard]] bool hasAdvisories() const noexcept { return hasAdvisories_; }
    [[nodiscard]] int advisoryCount() const noexcept { return advisoryCount_; }
    [[nodiscard]] QVariantList advisories() const { return advisories_; }

    [[nodiscard]] bool hasVerificationEnvironment() const noexcept { return hasVerificationEnvironment_; }
    [[nodiscard]] QString verificationEnvironmentState() const { return verificationEnvironmentState_; }
    [[nodiscard]] QString verificationEnvironmentIssue() const { return verificationEnvironmentIssue_; }
    [[nodiscard]] QString verificationEnvironmentDiagnostic() const { return verificationEnvironmentDiagnostic_; }
    [[nodiscard]] QVariantList verificationProfiles() const { return verificationProfiles_; }
    [[nodiscard]] bool hasBootstrapDecision() const noexcept { return hasBootstrapDecision_; }
    [[nodiscard]] QString bootstrapDisposition() const { return bootstrapDisposition_; }
    [[nodiscard]] QString bootstrapReasonCode() const { return bootstrapReasonCode_; }
    [[nodiscard]] QString bootstrapMessage() const { return bootstrapMessage_; }
    [[nodiscard]] QStringList bootstrapSetupPaths() const { return bootstrapSetupPaths_; }
    [[nodiscard]] bool hasBootstrapTransition() const noexcept { return hasBootstrapTransition_; }
    [[nodiscard]] QString bootstrapTransitionDisposition() const { return bootstrapTransitionDisposition_; }
    [[nodiscard]] QString bootstrapCandidateProfileId() const { return bootstrapCandidateProfileId_; }
    [[nodiscard]] QString bootstrapCandidateBuildProfileId() const { return bootstrapCandidateBuildProfileId_; }
    [[nodiscard]] QString bootstrapCandidateTargetId() const { return bootstrapCandidateTargetId_; }
    [[nodiscard]] bool canRetryAfterSetup() const noexcept { return canRetryAfterSetup_; }
    [[nodiscard]] QString canRetryAfterSetupReason() const { return canRetryAfterSetupReason_; }
    [[nodiscard]] bool canConfigureBackendProfile() const noexcept { return canConfigureBackendProfile_; }
    [[nodiscard]] QString canConfigureBackendProfileReasonCode() const { return canConfigureBackendProfileReasonCode_; }
    [[nodiscard]] QString canConfigureBackendProfileReason() const { return canConfigureBackendProfileReason_; }
    [[nodiscard]] bool hasVerificationContinuation() const noexcept { return hasVerificationContinuation_; }
    [[nodiscard]] QVariantMap verificationContinuation() const { return verificationContinuation_; }
    [[nodiscard]] bool verificationContinuationBusy() const noexcept { return verificationContinuationBusy_; }

    // Loads the manifest view for a run (if any was frozen). Clears the
    // current view (no error) when the run never froze a manifest - every
    // Mock/execution-profile Run and any manifest-workflow Run still in its
    // planning stage. Always also (re)loads the verification-environment
    // projection, independent of whether a manifest exists.
    //
    // AG-18.4A hardening: a repository/corruption failure from either read
    // (verificationEnvironmentViewForRun or viewForRun) is never silently
    // swallowed - it clears all stale state, sets errorText to
    // "code: message", and returns false. A caller must treat false here
    // exactly like any other Backend rejection, not as "nothing to show".
    Q_INVOKABLE bool selectRun(const QString& runId);
    Q_INVOKABLE bool refresh();

    // AG-18.4A: creates (or idempotently resumes) a fresh, baseline-bound
    // retry Run linked to the currently selected Run's durable
    // ActionRequired disposition - ONLY after the user has already
    // configured or created a usable verification profile/preset manually
    // or externally. This is NOT a "create a scoped setup Run" action (see
    // the class doc comment and AG-18.4C, not implemented). Calls only
    // VerificationBootstrapRetryService::createLinkedRetry() - never
    // touches the Apply-gate or the bootstrap lifecycle directly. Returns
    // the new Run's id on success, or an empty string on failure (errorText
    // is set to "code: message"). The caller is responsible for navigating
    // to the returned Run id; this controller does not select or refresh
    // it.
    Q_INVOKABLE QString retryAfterSetup();
    // MW-P5.6B: prepare (Backend admission + fresh qmllint/cmake
    // reconstruction + attribution - never a provider/LLM call) and the
    // paid repair dispatch (a real provider call) are two separate,
    // explicitly user-triggered actions - never chained automatically. A
    // successful prepare leaves a durable Prepared child Run; the caller
    // must invoke startVerificationContinuationRepair() as its own,
    // separately confirmed step to actually spend money and touch files.
    Q_INVOKABLE bool prepareVerificationContinuation();
    // Requires a Prepared child from a prior prepareVerificationContinuation()
    // call - re-reads it from the durable read-model view rather than
    // trusting any state cached across the free/paid boundary.
    // ManifestVerificationContinuationRepairService::repair() itself
    // re-validates the current baseline, source proposal states/hashes, and
    // the attribution checkpoint fresh before doing any paid work, so this
    // call never needs to re-derive that eligibility here.
    Q_INVOKABLE bool startVerificationContinuationRepair();
    Q_INVOKABLE bool cancelVerificationContinuation();

signals:
    void stateChanged();
    void verificationContinuationFinished(const QString& childRunId, const QString& status);

private:
    void clear();
    void clearManifestOnly();
    void clearVerificationEnvironment();
    void populateVerificationEnvironment(
        const ai::application::ManifestVerificationEnvironmentView& view);
    void clearVerificationContinuation();
    void populateVerificationContinuation(
        const ai::application::ManifestVerificationContinuationView& view);
    void finishVerificationContinuation();

    struct ContinuationCommandOutcome {
        QString childRunId;
        QString status;
        std::optional<ai::common::Error> error;
    };

    ai::application::ExecutionManifestReadModelService& readModel_;
    ai::application::VerificationBootstrapRetryService& bootstrapRetryService_;
    ai::application::ManifestVerificationContinuationReadModelService* continuationReadModel_ {nullptr};
    ai::application::ManifestVerificationContinuationPreparationService* continuationPreparation_ {nullptr};
    ai::application::ManifestVerificationContinuationRepairService* continuationRepair_ {nullptr};
    QString selectedRunId_;
    bool hasManifest_ {false};
    QVariantList tasks_;
    quint32 requestedMaxParallelism_ {1};
    QString errorText_;
    bool hasWorkspaceScope_ {false};
    QVariantList scopeDependencyEdges_;
    QString routingMode_ {QStringLiteral("none")};
    QVariantList routingMembers_;
    bool usageAvailable_ {false};
    bool usagePartitionExact_ {false};
    QVariantMap usageRunTotal_;
    int usageUnattributedCount_ {0};
    QVariantList usageUnattributed_;
    QStringList usageDiagnostics_;

    bool hasVerificationAdvisory_ {false};
    bool verificationAdvisoryPassed_ {false};
    bool hasAdvisories_ {false};
    int advisoryCount_ {0};
    QVariantList advisories_;

    bool hasVerificationEnvironment_ {false};
    QString verificationEnvironmentState_;
    QString verificationEnvironmentIssue_;
    QString verificationEnvironmentDiagnostic_;
    QVariantList verificationProfiles_;
    bool hasBootstrapDecision_ {false};
    QString bootstrapDisposition_;
    QString bootstrapReasonCode_;
    QString bootstrapMessage_;
    QStringList bootstrapSetupPaths_;
    bool hasBootstrapTransition_ {false};
    QString bootstrapTransitionDisposition_;
    QString bootstrapCandidateProfileId_;
    QString bootstrapCandidateBuildProfileId_;
    QString bootstrapCandidateTargetId_;
    bool canRetryAfterSetup_ {false};
    QString canRetryAfterSetupReason_;
    bool canConfigureBackendProfile_ {false};
    QString canConfigureBackendProfileReasonCode_;
    QString canConfigureBackendProfileReason_;
    bool hasVerificationContinuation_ {false};
    QVariantMap verificationContinuation_;
    bool verificationContinuationBusy_ {false};
    std::shared_ptr<std::stop_source> continuationStopSource_;
    QFutureWatcher<ContinuationCommandOutcome> continuationWatcher_;
};
