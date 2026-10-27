#include <ai/ui/RunController.hpp>

#include <ai/application/IWorkflowRunner.hpp>
#include <ai/application/verification/ManifestVerificationManualHandoffTypes.hpp>
#include <ai/ui/ProviderBudgetVariant.hpp>
#include <ai/ui/RunContractVariant.hpp>

#include <QDateTime>
#include <QDesktopServices>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <utility>

namespace {

constexpr std::chrono::milliseconds kDefaultRunTimeout {300000};

bool isTerminalEvent(ai::application::RunEventType type)
{
    using ai::application::RunEventType;
    return type == RunEventType::RunCompleted
        || type == RunEventType::RunFailed
        || type == RunEventType::RunCancelled
        || type == RunEventType::RunInterrupted;
}

bool eventMayChangeArtifacts(ai::application::RunEventType type)
{
    using ai::application::RunEventType;
    // A RunWarning event does not change an artifact, but the warnings are read together with them.
    return type == RunEventType::StageCompleted || type == RunEventType::RunWarning || isTerminalEvent(type);
}

bool isUserVisibleRun(const ai::application::Run& run)
{
    // Manual-handoff attempts are durable child Runs because restart
    // reconciliation, retry/replay, and immutable audit history need their
    // lifecycle. They are not top-level user orchestration Runs: their full
    // status and verification diagnostics belong to ManualHandoffPanel's
    // attempt history instead of appearing as an empty duplicate Runs row.
    return run.workflowId != ai::application::kManifestVerificationManualHandoffWorkflowId;
}

QVariantMap assignmentMap(const ai::application::StageModelAssignment& assignment)
{
    QVariantList dependencies;
    dependencies.reserve(static_cast<qsizetype>(assignment.dependencies.size()));
    for (const auto& dependency : assignment.dependencies)
        dependencies.push_back(QString::fromStdString(dependency));

    return {
        {QStringLiteral("stageType"), QString::fromStdString(assignment.stageType)},
        {QStringLiteral("providerId"), QString::fromStdString(assignment.providerId)},
        {QStringLiteral("modelId"), QString::fromStdString(assignment.modelId)},
        {QStringLiteral("agentName"), QString::fromStdString(assignment.agentName)},
        {QStringLiteral("role"), QString::fromStdString(assignment.role)},
        {QStringLiteral("instructions"), QString::fromStdString(assignment.instructions)},
        {QStringLiteral("position"), assignment.position},
        {QStringLiteral("leadArchitect"), assignment.leadArchitect},
        {QStringLiteral("timeoutMs"),
            QVariant::fromValue<qlonglong>(assignment.timeout.count())},
        {QStringLiteral("dependencies"), dependencies},
        {QStringLiteral("workspaceAccess"), assignment.workspaceAccess},
        {QStringLiteral("maxAttempts"), assignment.maxAttempts},
        {QStringLiteral("fallbackProviderId"), assignment.fallbackProviderId.has_value()
            ? QString::fromStdString(*assignment.fallbackProviderId) : QString {}},
        {QStringLiteral("fallbackModelId"), assignment.fallbackModelId.has_value()
            ? QString::fromStdString(*assignment.fallbackModelId) : QString {}}
    };
}

QVariant optionalDateTime(const std::optional<std::chrono::system_clock::time_point>& value)
{
    if (!value.has_value()) return {};
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        value->time_since_epoch());
    return QDateTime::fromMSecsSinceEpoch(milliseconds.count(), Qt::UTC);
}

// The history marker of a Run created by a conversion (see RunListModel::LegacyConversionRole).
std::string conversionMarker(const ai::application::LegacyConversionIndexEntry& entry)
{
    return entry.mode.has_value() ? std::string(ai::application::toString(*entry.mode))
                                  : std::string("unreadable");
}

// A row of the LOCKED target roster shown by the conversion dialog. Deliberately not
// assignmentMap(): a target row never carries instruction text (the lead's instructions are not
// used by the Manifest Planner, and the conversion summary shows lengths only).
QVariantMap conversionMemberMap(
    const ai::application::StageModelAssignment& member, const QString& modelLabel)
{
    return {
        {QStringLiteral("memberId"), QString::fromStdString(member.stageType)},
        {QStringLiteral("agentName"), QString::fromStdString(member.agentName)},
        {QStringLiteral("role"), QString::fromStdString(member.role)},
        {QStringLiteral("providerId"), QString::fromStdString(member.providerId)},
        {QStringLiteral("modelId"), QString::fromStdString(member.modelId)},
        {QStringLiteral("modelLabel"), modelLabel},
        {QStringLiteral("leadArchitect"), member.leadArchitect},
        {QStringLiteral("workspaceAccess"), member.workspaceAccess},
        {QStringLiteral("position"), member.position}
    };
}

} // namespace

RunController::RunController(
    ai::application::OrchestrationService& service,
    ai::application::ArtifactService& artifactService,
    ai::application::ProviderCatalogService& providerCatalogService,
    ai::application::ManifestVerificationManualHandoffReadModelService& manualHandoffReadModel,
    QObject* parent)
    : QObject(parent)
    , service_(service)
    , artifactService_(artifactService)
    , providerCatalogService_(providerCatalogService)
    , manualHandoffReadModel_(manualHandoffReadModel)
    , model_(this)
    , stageModel_(this)
    , artifactModel_(this)
    , warningModel_(this)
{
    connect(
        &refreshWatcher_,
        &QFutureWatcher<RunRefreshOutcome>::finished,
        this,
        &RunController::finishRefresh);
    connect(
        &catalogWatcher_,
        &QFutureWatcher<CatalogLoadOutcome>::finished,
        this,
        &RunController::finishCatalogLoad);

    // Application events originate on RunManager's worker. Queue them onto the
    // controller thread before touching QAbstractListModel instances.
    QPointer<RunController> guard(this);
    service_.setEventHandler([guard](const ai::application::RunEvent& event) {
        if (!guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [guard, event] {
                if (guard) {
                    guard->handleEvent(event);
                }
            },
            Qt::QueuedConnection);
    });
    refresh();
    refreshAgentChoices();
}

QString RunController::startRunWithRoster(
    const QString& projectId,
    const QString& title,
    const QString& task,
    const QVariantList& roster,
    int maxParallelism,
    const QString& workspacePath,
    const QString& sourceRunId,
    const QString& projectTaskId,
    bool allowEvidenceBackedNoChanges,
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
    ai::application::StartRunRequest request;
    request.projectId = projectId.toStdString();
    request.title = title.toStdString();
    request.task = task.toStdString();
    request.providerBudget = ai::ui::parseProviderBudgetOverrides(providerBudget);
    request.interfaceContracts = std::move(contracts.value());
    // Only the Manifest workflow is ever selected explicitly (AG-17.6); any other
    // value keeps the historical roster runner, so a stale or unknown id cannot
    // route a roster somewhere unintended.
    request.workflowId = workflowId.toStdString() == ai::application::kExecutionManifestWorkflowId
        ? ai::application::kExecutionManifestWorkflowId
        : ai::application::kExecutionProfileWorkflowId;
    request.maxParallelism = static_cast<std::uint32_t>(std::max(0, maxParallelism));
    request.planningCompletionPolicy = allowEvidenceBackedNoChanges
        ? ai::application::PlanningCompletionPolicy::AllowEvidenceBackedNoChanges
        : ai::application::PlanningCompletionPolicy::RequireManifest;
    request.verificationRepairPolicy = automaticVerificationRepair
        ? ai::application::VerificationRepairPolicy::OneBoundedWave
        : ai::application::VerificationRepairPolicy::Disabled;
    if (!workspacePath.isEmpty()) {
        request.workspacePath = std::filesystem::path(workspacePath.toStdWString());
    }
    if (!projectTaskId.isEmpty()) {
        request.projectTaskId = projectTaskId.toStdString();
    }

    for (qsizetype index = 0; index < roster.size(); ++index) {
        const QVariantMap row = roster.at(index).toMap();
        ai::application::StageModelAssignment assignment;
        assignment.stageType = row.value(QStringLiteral("stageType"),
            QStringLiteral("agent-%1").arg(index + 1)).toString().toStdString();
        assignment.providerId = row.value(QStringLiteral("providerId")).toString().toStdString();
        assignment.modelId = row.value(QStringLiteral("modelId")).toString().toStdString();
        assignment.timeout = std::chrono::milliseconds(
            row.value(QStringLiteral("timeoutMs"), kDefaultRunTimeout.count()).toLongLong());
        assignment.agentName = row.value(QStringLiteral("agentName")).toString().toStdString();
        assignment.role = row.value(QStringLiteral("role")).toString().toStdString();
        assignment.instructions = row.value(QStringLiteral("instructions")).toString().toStdString();
        assignment.position = static_cast<std::uint32_t>(index);
        assignment.leadArchitect = row.value(QStringLiteral("leadArchitect")).toBool();
        assignment.workspaceAccess = row.value(QStringLiteral("workspaceAccess")).toBool();
        const QVariantList dependencies = row.value(QStringLiteral("dependencies")).toList();
        assignment.dependencies.reserve(static_cast<std::size_t>(dependencies.size()));
        for (const QVariant& dependency : dependencies) {
            assignment.dependencies.push_back(dependency.toString().toStdString());
        }
        assignment.maxAttempts = static_cast<std::uint32_t>(
            std::max(0, row.value(QStringLiteral("maxAttempts"), 1).toInt()));
        const QString fallbackProviderId =
            row.value(QStringLiteral("fallbackProviderId")).toString();
        const QString fallbackModelId =
            row.value(QStringLiteral("fallbackModelId")).toString();
        if (!fallbackProviderId.isEmpty()) {
            assignment.fallbackProviderId = fallbackProviderId.toStdString();
        }
        if (!fallbackModelId.isEmpty()) {
            assignment.fallbackModelId = fallbackModelId.toStdString();
        }
        request.assignmentOverrides.push_back(std::move(assignment));
    }

    const auto result = sourceRunId.isEmpty()
        ? service_.startRun(request)
        : service_.startEditedRerun(sourceRunId.toStdString(), std::move(request));
    if (!result.success()) {
        reportError(*result.error());
        return {};
    }
    clearError();
    const QString runId = QString::fromStdString(result.value());
    selectRun(runId);
    refresh(projectFilter_);
    emit runAccepted(runId);
    return runId;
}

void RunController::refreshAgentChoices()
{
    if (catalogWatcher_.isRunning()) {
        catalogReloadPending_ = true;
        return;
    }
    startCatalogLoad();
}

void RunController::startCatalogLoad()
{
    agentChoicesLoading_ = true;
    emit agentChoicesLoadingChanged();
    auto* catalog = &providerCatalogService_;
    catalogWatcher_.setFuture(QtConcurrent::run([catalog] {
        CatalogLoadOutcome outcome;
        auto providers = catalog->listProviders();
        if (!providers.success()) {
            outcome.error = *providers.error();
            return outcome;
        }
        auto models = catalog->listModels();
        if (!models.success()) {
            outcome.error = *models.error();
            return outcome;
        }
        outcome.providers = std::move(providers.value());
        outcome.models = std::move(models.value());
        return outcome;
    }));
}

void RunController::finishCatalogLoad()
{
    CatalogLoadOutcome outcome = catalogWatcher_.future().takeResult();
    if (outcome.error.has_value()) {
        reportError(*outcome.error);
    } else {
        QVariantList choices;
        for (const auto& model : *outcome.models) {
            if (!model.enabled) continue;
            const auto provider = std::find_if(
                outcome.providers->begin(),
                outcome.providers->end(),
                [&](const auto& candidate) { return candidate.id == model.providerId; });
            if (provider == outcome.providers->end() || !provider->enabled) continue;
            QVariantMap choice;
            choice.insert(QStringLiteral("providerId"), QString::fromStdString(provider->id));
            choice.insert(QStringLiteral("providerName"), QString::fromStdString(provider->name));
            choice.insert(QStringLiteral("modelId"), QString::fromStdString(model.id));
            choice.insert(QStringLiteral("modelName"), QString::fromStdString(model.displayName));
            choice.insert(QStringLiteral("label"), QStringLiteral("%1 · %2")
                .arg(QString::fromStdString(provider->name),
                     QString::fromStdString(model.displayName)));
            choices.push_back(std::move(choice));
        }
        if (agentChoices_ != choices) {
            agentChoices_ = std::move(choices);
            emit agentChoicesChanged();
        }
        clearError();
    }

    agentChoicesLoading_ = false;
    emit agentChoicesLoadingChanged();
    if (catalogReloadPending_) {
        catalogReloadPending_ = false;
        startCatalogLoad();
    }
}

QVariantMap RunController::rerunDraft(const QString& runId)
{
    const auto result = service_.prepareRerunDraft(runId.toStdString());
    if (!result.success()) {
        reportError(*result.error());
        return {};
    }

    const auto& draft = result.value();
    QVariantList roster;
    roster.reserve(static_cast<qsizetype>(draft.request.assignmentOverrides.size()));
    for (const auto& assignment : draft.request.assignmentOverrides)
        roster.push_back(assignmentMap(assignment));

    QVariantMap mapped {
        {QStringLiteral("sourceRunId"), QString::fromStdString(draft.sourceRunId)},
        {QStringLiteral("projectId"), QString::fromStdString(draft.request.projectId)},
        {QStringLiteral("title"), QString::fromStdString(draft.request.title)},
        {QStringLiteral("task"), QString::fromStdString(draft.request.task)},
        {QStringLiteral("workflowId"), QString::fromStdString(draft.request.workflowId)},
        {QStringLiteral("launchMode"), QString::fromLatin1(
            ai::application::toString(draft.launchMode))},
        {QStringLiteral("executionProfileId"), draft.request.executionProfileId.has_value()
            ? QString::fromStdString(*draft.request.executionProfileId) : QString {}},
        {QStringLiteral("maxParallelism"), draft.request.maxParallelism},
        {QStringLiteral("workspacePath"), draft.request.workspacePath.has_value()
            ? QString::fromStdString(draft.request.workspacePath->generic_string()) : QString {}},
        {QStringLiteral("allowEvidenceBackedNoChanges"),
            draft.request.planningCompletionPolicy
                == ai::application::PlanningCompletionPolicy::AllowEvidenceBackedNoChanges},
        {QStringLiteral("automaticVerificationRepair"),
            draft.request.verificationRepairPolicy
                == ai::application::VerificationRepairPolicy::OneBoundedWave},
        {QStringLiteral("migratedFromLegacyWorkflow"), draft.migratedFromLegacyWorkflow},
        {QStringLiteral("roster"), roster},
        // Only the limits somebody set explicitly; empty = the Backend defaults, automatic turns.
        {QStringLiteral("providerBudget"),
            ai::ui::providerBudgetOverridesToVariant(draft.request.providerBudget)},
        // PC-2: the frozen contracts of the source Run, reopened exactly (the author may edit them).
        {QStringLiteral("interfaceContracts"),
            ai::ui::runContractsToVariant(draft.request.interfaceContracts)}
    };
    // AG-17.7C-2: a legacy roster Run that has a Manifest counterpart (or that the legacy runner
    // cannot execute at all) also carries the conversion preview, so the page can open the
    // conversion dialog instead of a legacy dialog it must not offer. A failed preview only means
    // "no conversion section": the draft itself was prepared successfully.
    if (const auto preview = service_.previewLegacyConversion(runId.toStdString());
        preview.success()
        && (preview.value().plan.converts() || !preview.value().legacyRerunAvailable)) {
        mapped.insert(QStringLiteral("conversion"), conversionMap(preview.value()));
    }
    clearError();
    return mapped;
}

QString RunController::modelLabelFor(
    const std::string& providerId, const std::string& modelId) const
{
    const QString provider = QString::fromStdString(providerId);
    const QString model = QString::fromStdString(modelId);
    for (const QVariant& choice : agentChoices_) {
        const QVariantMap row = choice.toMap();
        if (row.value(QStringLiteral("providerId")).toString() == provider
            && row.value(QStringLiteral("modelId")).toString() == model) {
            return row.value(QStringLiteral("label")).toString();
        }
    }
    return QStringLiteral("%1 · %2").arg(provider, model);
}

QVariantMap RunController::conversionMap(
    const ai::application::LegacyConversionPreview& preview) const
{
    const auto& plan = preview.plan;

    QVariantList roster;
    roster.reserve(static_cast<qsizetype>(plan.roster.size()));
    for (const auto& member : plan.roster) {
        roster.push_back(conversionMemberMap(member, modelLabelFor(member.providerId, member.modelId)));
    }

    QVariantList dropped;
    dropped.reserve(static_cast<qsizetype>(plan.dropped.size()));
    bool instructionsDropped = false;
    for (const auto& entry : plan.dropped) {
        dropped.push_back(QVariantMap {
            {QStringLiteral("memberId"), QString::fromStdString(entry.memberId)},
            {QStringLiteral("setting"), QString::fromStdString(entry.setting)},
            {QStringLiteral("legacyValue"), QString::fromStdString(entry.legacyValue)},
            {QStringLiteral("reason"), QString::fromStdString(entry.reason)}
        });
        if (entry.setting == "instructions") instructionsDropped = true;
    }

    const auto& defaults = preview.defaults;
    const QVariantMap defaultsMap {
        {QStringLiteral("title"), QString::fromStdString(defaults.title)},
        {QStringLiteral("task"), QString::fromStdString(defaults.task)},
        {QStringLiteral("workspacePath"), defaults.workspacePath.has_value()
            ? QString::fromStdString(defaults.workspacePath->generic_string()) : QString {}},
        {QStringLiteral("maxParallelism"), defaults.maxParallelism},
        {QStringLiteral("allowEvidenceBackedNoChanges"),
            defaults.planningCompletionPolicy
                == ai::application::PlanningCompletionPolicy::AllowEvidenceBackedNoChanges},
        {QStringLiteral("automaticVerificationRepair"),
            defaults.verificationRepairPolicy
                == ai::application::VerificationRepairPolicy::OneBoundedWave}
    };

    return {
        {QStringLiteral("sourceRunId"), QString::fromStdString(preview.sourceRunId)},
        {QStringLiteral("sourceWorkflowId"), QString::fromStdString(plan.sourceWorkflowId)},
        {QStringLiteral("targetWorkflowId"), QString::fromStdString(plan.targetWorkflowId)},
        {QStringLiteral("policyId"), QString::fromStdString(plan.policyId)},
        {QStringLiteral("shape"), QString::fromLatin1(ai::application::toString(plan.shape))},
        {QStringLiteral("disposition"), QString::fromLatin1(ai::application::toString(plan.disposition))},
        {QStringLiteral("reasonCode"), QString::fromStdString(plan.reasonCode)},
        {QStringLiteral("reasonMessage"), QString::fromStdString(plan.reasonMessage)},
        {QStringLiteral("available"), plan.converts()},
        {QStringLiteral("required"), !preview.legacyRerunAvailable},
        {QStringLiteral("safeAutomaticRetry"), preview.safeAutomaticRetry},
        {QStringLiteral("legacyRerunAvailable"), preview.legacyRerunAvailable},
        {QStringLiteral("confirmationFingerprint"), QString::fromStdString(preview.confirmationFingerprint)},
        {QStringLiteral("hasDroppedInstructions"), instructionsDropped},
        {QStringLiteral("dropped"), dropped},
        {QStringLiteral("roster"), roster},
        {QStringLiteral("defaults"), defaultsMap}
    };
}

QVariantMap RunController::legacyConversionRecord(const QString& runId)
{
    const auto found = service_.findLegacyConversionView(runId.toStdString());
    if (!found.success()) {
        // A corrupt (typed legacy_conversion.record_corrupt) or unreadable record - and equally a
        // source Run that could not be READ - degrades ONLY this section, and says so: the user is
        // never shown half of an audit, never shown nothing where an audit exists, and never told
        // that the source "no longer exists" when it was merely unreadable. It is not an
        // application-wide error, so no reportError.
        return {
            {QStringLiteral("state"), QStringLiteral("unreadable")},
            {QStringLiteral("errorCode"), QString::fromStdString(found.error()->code)},
            {QStringLiteral("errorMessage"), QString::fromStdString(found.error()->message)}
        };
    }
    if (!found.value().has_value()) {
        return {{QStringLiteral("state"), QStringLiteral("none")}};
    }
    const auto& record = found.value()->record;

    QVariantList dropped;
    dropped.reserve(static_cast<qsizetype>(record.dropped.size()));
    bool instructionsDropped = false;
    for (const auto& entry : record.dropped) {
        dropped.push_back(QVariantMap {
            {QStringLiteral("memberId"), QString::fromStdString(entry.memberId)},
            {QStringLiteral("setting"), QString::fromStdString(entry.setting)},
            {QStringLiteral("legacyValue"), QString::fromStdString(entry.legacyValue)},
            {QStringLiteral("reason"), QString::fromStdString(entry.reason)}
        });
        if (entry.setting == "instructions") instructionsDropped = true;
    }

    // The source Run may have been deleted since (the audit deliberately has no foreign key to
    // it), so its title is a convenience and its absence is a FACT the panel shows. Here the read
    // of the source has already SUCCEEDED (a failed read is the unreadable branch above), so an
    // empty source really means "no longer exists".
    const bool sourceAvailable = found.value()->source.has_value();
    const QString sourceTitle = sourceAvailable
        ? QString::fromStdString(found.value()->source->title) : QString {};

    return {
        {QStringLiteral("state"), QStringLiteral("present")},
        {QStringLiteral("runId"), QString::fromStdString(record.runId)},
        {QStringLiteral("sourceRunId"), QString::fromStdString(record.sourceRunId)},
        {QStringLiteral("sourceRunTitle"), sourceTitle},
        {QStringLiteral("sourceRunAvailable"), sourceAvailable},
        {QStringLiteral("sourceWorkflowId"), QString::fromStdString(record.sourceWorkflowId)},
        {QStringLiteral("policyId"), QString::fromStdString(record.policyId)},
        {QStringLiteral("reasonCode"), QString::fromStdString(record.reasonCode)},
        {QStringLiteral("mode"), QString::fromLatin1(ai::application::toString(record.confirmationMode))},
        // ONLY an explicit confirmation has a time; a safe automatic Retry never claims one.
        {QStringLiteral("confirmedAt"), optionalDateTime(record.confirmedAt)},
        {QStringLiteral("createdAt"), optionalDateTime(record.createdAt)},
        {QStringLiteral("sourceSnapshotFingerprint"), QString::fromStdString(record.sourceSnapshotFingerprint)},
        {QStringLiteral("targetRosterFingerprint"), QString::fromStdString(record.targetRosterFingerprint)},
        {QStringLiteral("confirmationFingerprint"), QString::fromStdString(record.confirmationFingerprint)},
        {QStringLiteral("droppedCount"), static_cast<int>(record.dropped.size())},
        {QStringLiteral("hasDroppedInstructions"), instructionsDropped},
        {QStringLiteral("dropped"), dropped}
    };
}

QVariantMap RunController::legacyConversionPreview(const QString& runId)
{
    const auto preview = service_.previewLegacyConversion(runId.toStdString());
    if (!preview.success()) {
        reportError(*preview.error());
        return {};
    }
    clearError();
    return conversionMap(preview.value());
}

QVariantMap RunController::startConvertedRerun(
    const QString& sourceRunId,
    const QString& title,
    const QString& task,
    const QString& workspacePath,
    int maxParallelism,
    bool allowEvidenceBackedNoChanges,
    bool automaticVerificationRepair,
    const QString& expectedConfirmationFingerprint)
{
    ai::application::ConvertedRerunOptions options;
    options.title = title.toStdString();
    options.task = task.toStdString();
    if (!workspacePath.isEmpty()) {
        options.workspacePath = std::filesystem::path(workspacePath.toStdWString());
    }
    options.maxParallelism = static_cast<std::uint32_t>(std::max(0, maxParallelism));
    options.planningCompletionPolicy = allowEvidenceBackedNoChanges
        ? ai::application::PlanningCompletionPolicy::AllowEvidenceBackedNoChanges
        : ai::application::PlanningCompletionPolicy::RequireManifest;
    options.verificationRepairPolicy = automaticVerificationRepair
        ? ai::application::VerificationRepairPolicy::OneBoundedWave
        : ai::application::VerificationRepairPolicy::Disabled;

    const auto result = service_.startConvertedRerun(
        sourceRunId.toStdString(), std::move(options), expectedConfirmationFingerprint.toStdString());
    if (!result.success()) {
        const auto& failure = *result.error();
        reportError(failure);
        return {
            {QStringLiteral("accepted"), false},
            {QStringLiteral("runId"), QString {}},
            {QStringLiteral("errorCode"), QString::fromStdString(failure.code)},
            {QStringLiteral("errorMessage"), QString::fromStdString(failure.message)},
            // The two outcomes the dialog reacts to differently: a stale confirmation is reloaded
            // and shown again, a missing one is a request to confirm. QML never compares codes.
            {QStringLiteral("confirmationStale"),
                failure.code == ai::application::kLegacyConversionConfirmationStaleCode},
            {QStringLiteral("confirmationRequired"),
                failure.code == ai::application::kLegacyConversionConfirmationRequiredCode}
        };
    }

    clearError();
    const QString runId = QString::fromStdString(result.value());
    selectRun(runId);
    refresh(projectFilter_);
    emit runAccepted(runId);
    return {
        {QStringLiteral("accepted"), true},
        {QStringLiteral("runId"), runId},
        {QStringLiteral("errorCode"), QString {}},
        {QStringLiteral("errorMessage"), QString {}},
        {QStringLiteral("confirmationStale"), false},
        {QStringLiteral("confirmationRequired"), false}
    };
}

RunController::~RunController()
{
    service_.setEventHandler({});
    // Background reads capture Application service references owned by
    // BackendRuntime. Join them before the controller/runtime composition is
    // dismantled so shutdown cannot race a late SQLite projection.
    if (refreshWatcher_.isRunning()) {
        refreshWatcher_.future().waitForFinished();
    }
    if (catalogWatcher_.isRunning()) {
        catalogWatcher_.future().waitForFinished();
    }
}

QAbstractItemModel* RunController::model() noexcept { return &model_; }
QAbstractItemModel* RunController::stageModel() noexcept { return &stageModel_; }
QAbstractItemModel* RunController::artifactModel() noexcept { return &artifactModel_; }
QAbstractItemModel* RunController::warningModel() noexcept { return &warningModel_; }
// Return by value so QML receives an independent, stable string value.
QString RunController::selectedRunId() const { return selectedRunId_; }
QString RunController::lastError() const { return lastError_; }

QString RunController::startRun(
    const QString& projectId,
    const QString& title,
    const QString& task,
    const QString& executionProfileId,
    const QString& workspacePath,
    const QString& sourceRunId,
    const QString& projectTaskId,
    bool allowEvidenceBackedNoChanges,
    bool automaticVerificationRepair)
{
    ai::application::StartRunRequest request;
    request.projectId = projectId.toStdString();
    request.title = title.toStdString();
    request.task = task.toStdString();
    request.planningCompletionPolicy = allowEvidenceBackedNoChanges
        ? ai::application::PlanningCompletionPolicy::AllowEvidenceBackedNoChanges
        : ai::application::PlanningCompletionPolicy::RequireManifest;
    request.verificationRepairPolicy = automaticVerificationRepair
        ? ai::application::VerificationRepairPolicy::OneBoundedWave
        : ai::application::VerificationRepairPolicy::Disabled;
    if (!executionProfileId.isEmpty()) {
        request.executionProfileId = executionProfileId.toStdString();
    }
    if (!workspacePath.isEmpty()) {
        request.workspacePath = std::filesystem::path(workspacePath.toStdWString());
    }
    if (!projectTaskId.isEmpty()) {
        request.projectTaskId = projectTaskId.toStdString();
    }

    const auto result = sourceRunId.isEmpty()
        ? service_.startRun(request)
        : service_.startEditedRerun(sourceRunId.toStdString(), std::move(request));
    if (!result.success()) {
        reportError(*result.error());
        return {};
    }

    clearError();
    const QString runId = QString::fromStdString(result.value());
    selectRun(runId);
    refresh(projectFilter_);
    emit runAccepted(runId);
    return runId;
}

bool RunController::cancelRun(const QString& runId)
{
    const auto result = service_.cancelRun(runId.toStdString());
    if (!result.success()) {
        reportError(*result.error());
        return false;
    }
    clearError();
    return true;
}

QString RunController::retryRun(const QString& runId)
{
    const auto result = service_.retryRun(runId.toStdString());
    if (!result.success()) {
        reportError(*result.error());
        // A legacy lone workspace lead whose conversion drops settings cannot be retried
        // silently: the user must see and confirm them, which is the conversion dialog's job.
        if (result.error()->code == ai::application::kLegacyConversionConfirmationRequiredCode) {
            emit legacyConversionRequested(runId);
        }
        return {};
    }
    clearError();
    const QString newRunId = QString::fromStdString(result.value());
    selectRun(newRunId);
    refresh(projectFilter_);
    emit runAccepted(newRunId);
    return newRunId;
}

void RunController::selectRun(const QString& runId)
{
    if (selectedRunId_ != runId) {
        selectedRunId_ = runId;
        emit selectedRunIdChanged();
        // Never display the outgoing run's details under the new selection
        // while its background projection is loading.
        stageModel_.replace({});
        artifactModel_.replace({});
        warningModel_.replace({});
    }
    pendingStages_ = true;
    pendingArtifacts_ = true;
    scheduleRefresh();
}

void RunController::refresh(const QString& projectId)
{
    projectFilter_ = projectId;
    pendingReplaceRunList_ = true;
    pendingStages_ = true;
    pendingArtifacts_ = true;
    scheduleRefresh();
}

void RunController::refreshManualHandoffResolutions()
{
    pendingReplaceRunList_ = true;
    scheduleRefresh();
}

bool RunController::openArtifact(const QString& artifactId)
{
    // Backend revalidates canonical containment on every resolve. QML only
    // passes the stable ID and never receives the local filesystem path.
    const auto resolved = artifactService_.resolveArtifactPath(artifactId.toStdString());
    if (!resolved.success()) {
        reportError(*resolved.error());
        return false;
    }

    const QUrl target = QUrl::fromLocalFile(
        QString::fromStdWString(resolved.value().wstring()));
    if (!QDesktopServices::openUrl(target)) {
        reportError({"artifact.open_failed", "No desktop application could open the artifact"});
        return false;
    }
    clearError();
    return true;
}

QString RunController::readArtifactText(const QString& artifactId)
{
    const auto content = artifactService_.getArtifactContent(artifactId.toStdString());
    if (!content.success()) {
        reportError(*content.error());
        return {};
    }
    clearError();
    return QString::fromStdString(content.value());
}

bool RunController::openArtifactsFolder(const QString& artifactId)
{
    // Same containment-validated resolve as openArtifact; only the parent
    // directory is handed to the shell, the path itself never reaches QML.
    const auto resolved = artifactService_.resolveArtifactPath(artifactId.toStdString());
    if (!resolved.success()) {
        reportError(*resolved.error());
        return false;
    }

    const QUrl target = QUrl::fromLocalFile(
        QString::fromStdWString(resolved.value().parent_path().wstring()));
    if (!QDesktopServices::openUrl(target)) {
        reportError({"artifact.open_folder_failed", "Could not open the artifacts folder"});
        return false;
    }
    clearError();
    return true;
}

void RunController::handleEvent(const ai::application::RunEvent& event)
{
    // This callback is on the GUI thread, but it performs no database work.
    // It only records dirty projections; events arriving in the same frame or
    // while a read is in flight collapse into one background refresh.
    const QString runId = QString::fromStdString(event.runId);
    pendingChangedRunIds_.insert(runId);
    if (selectedRunId_ == runId) {
        pendingStages_ = true;
        if (eventMayChangeArtifacts(event.type)) {
            pendingArtifacts_ = true;
        }
    }
    if (isTerminalEvent(event.type)) {
        pendingTerminalRunIds_.insert(runId);
    }
    scheduleRefresh(16);
}

void RunController::scheduleRefresh(int delayMs)
{
    if (refreshInFlight_ || refreshScheduled_) return;
    refreshScheduled_ = true;
    QTimer::singleShot(delayMs, this, [this] {
        refreshScheduled_ = false;
        startPendingRefresh();
    });
}

void RunController::startPendingRefresh()
{
    if (refreshInFlight_) return;
    if (!pendingReplaceRunList_ && !pendingStages_ && !pendingArtifacts_
        && pendingChangedRunIds_.isEmpty() && pendingTerminalRunIds_.isEmpty()) {
        return;
    }

    RunRefreshRequest request;
    request.requestId = ++nextRefreshId_;
    request.projectFilter = projectFilter_;
    request.selectedRunId = selectedRunId_;
    request.replaceRunList = pendingReplaceRunList_;
    request.refreshStages = pendingStages_;
    request.refreshArtifacts = pendingArtifacts_;
    request.changedRunIds = std::move(pendingChangedRunIds_);
    request.terminalRunIds = std::move(pendingTerminalRunIds_);

    pendingReplaceRunList_ = false;
    pendingStages_ = false;
    pendingArtifacts_ = false;
    pendingChangedRunIds_.clear();
    pendingTerminalRunIds_.clear();
    refreshInFlight_ = true;

    auto* service = &service_;
    auto* artifacts = &artifactService_;
    auto* manualHandoffReadModel = &manualHandoffReadModel_;
    refreshWatcher_.setFuture(QtConcurrent::run(
        [service, artifacts, manualHandoffReadModel, request = std::move(request)]() mutable {
            RunRefreshOutcome outcome;
            outcome.request = std::move(request);

            if (outcome.request.replaceRunList) {
                // Shape may have changed (new run, project-filter change,
                // initial load) - only a full listRuns() can tell us that.
                const std::optional<ai::application::ProjectId> filter =
                    outcome.request.projectFilter.isEmpty()
                    ? std::nullopt
                    : std::optional<ai::application::ProjectId> {
                        outcome.request.projectFilter.toStdString()};
                auto runs = service->listRuns(filter);
                if (runs.success()) {
                    std::erase_if(runs.value(), [](const ai::application::Run& run) {
                        return !isUserVisibleRun(run);
                    });
                    const auto resolutions = manualHandoffReadModel->resolutionsForRuns(
                        runs.value());
                    if (!resolutions.success()) {
                        outcome.runError = *resolutions.error();
                    } else {
                        std::vector<std::string> values;
                        values.reserve(resolutions.value().size());
                        for (const auto& row : resolutions.value()) {
                            values.emplace_back(toString(row.resolution));
                        }
                        outcome.manualHandoffResolutions = std::move(values);
                        // AG-17.7C-3: one cheap read marks every converted Run in the history.
                        // A failed read costs only the markers - the Run list is still applied
                        // and the fault is reported, never swallowed.
                        const auto index = service->listLegacyConversionIndex();
                        if (index.success()) {
                            std::unordered_map<std::string, std::string> markers;
                            for (const auto& entry : index.value()) {
                                markers.emplace(entry.runId, conversionMarker(entry));
                            }
                            std::vector<std::string> conversions;
                            conversions.reserve(runs.value().size());
                            for (const auto& run : runs.value()) {
                                const auto marker = markers.find(run.id);
                                conversions.push_back(
                                    marker == markers.end() ? std::string {} : marker->second);
                            }
                            outcome.legacyConversions = std::move(conversions);
                        } else {
                            outcome.legacyConversionIndexError = *index.error();
                        }
                        outcome.runs = std::move(runs.value());
                    }
                } else {
                    outcome.runError = *runs.error();
                }
            } else if (!outcome.request.changedRunIds.isEmpty()
                || !outcome.request.terminalRunIds.isEmpty()) {
                // The list shape is already known to be unchanged (a RunEvent
                // never adds/removes a row) - a single-row findRun() per
                // changed id replaces re-scanning every run in the project
                // just to pick out the handful that actually changed.
                std::vector<ai::application::Run> found;
                const QSet<QString> ids =
                    outcome.request.changedRunIds + outcome.request.terminalRunIds;
                for (const QString& id : ids) {
                    auto result = service->findRun(id.toStdString());
                    if (result.success() && result.value().has_value()
                        && isUserVisibleRun(*result.value())) {
                        found.push_back(std::move(*result.value()));
                    } else if (!result.success()) {
                        // Last error wins, matching the single-call listRuns()
                        // path this replaces. A run raced away between the
                        // event firing and this read (success + nullopt) is a
                        // silent skip, not an error - the same silent-skip
                        // model_.updateOne() already tolerates below.
                        outcome.runError = *result.error();
                    }
                }
                outcome.runs = std::move(found);
            }

            if (outcome.request.refreshStages
                && !outcome.request.selectedRunId.isEmpty()) {
                auto stages = service->listStages(
                    outcome.request.selectedRunId.toStdString());
                if (stages.success()) {
                    outcome.stages = std::move(stages.value());
                } else {
                    outcome.stageError = *stages.error();
                }
            }

            if (outcome.request.refreshArtifacts
                && !outcome.request.selectedRunId.isEmpty()) {
                auto listed = artifacts->listArtifacts(
                    outcome.request.selectedRunId.toStdString());
                if (listed.success()) {
                    outcome.artifacts = std::move(listed.value());
                } else {
                    outcome.artifactError = *listed.error();
                }
                // Read from the database every time, so the list is the same after a restart.
                auto warnings = artifacts->listWarnings(
                    outcome.request.selectedRunId.toStdString());
                if (warnings.success()) {
                    outcome.warnings = std::move(warnings.value());
                } else {
                    outcome.warningError = *warnings.error();
                }
            }
            return outcome;
        }));
}

void RunController::finishRefresh()
{
    RunRefreshOutcome outcome = refreshWatcher_.future().takeResult();
    refreshInFlight_ = false;
    const RunRefreshRequest& request = outcome.request;

    if (outcome.runError.has_value()) {
        reportError(*outcome.runError);
    } else if (outcome.runs.has_value()
        && request.projectFilter == projectFilter_
        // A command may durably create a run while an older full-list read is
        // still in flight. Applying that older snapshot would briefly remove
        // the new selected ID, causing QML's filter guard to select the
        // previous run. The already-pending replacement is authoritative.
        && !(request.replaceRunList && pendingReplaceRunList_)) {
        if (request.replaceRunList) {
            model_.replace(
                std::move(*outcome.runs),
                std::move(outcome.manualHandoffResolutions.value_or(
                    std::vector<std::string> {})),
                std::move(outcome.legacyConversions.value_or(std::vector<std::string> {})));
            if (outcome.legacyConversionIndexError.has_value()) {
                reportError(*outcome.legacyConversionIndexError);
            }
        } else {
            for (const auto& run : *outcome.runs) {
                if (request.changedRunIds.contains(QString::fromStdString(run.id))
                    || request.terminalRunIds.contains(QString::fromStdString(run.id))) {
                    model_.updateOne(run);
                }
            }
        }
    }

    if (request.selectedRunId == selectedRunId_) {
        if (outcome.stageError.has_value()) {
            reportError(*outcome.stageError);
        } else if (outcome.stages.has_value()) {
            stageModel_.replace(std::move(*outcome.stages));
        } else if (request.refreshStages && selectedRunId_.isEmpty()) {
            stageModel_.replace({});
        }

        if (outcome.artifactError.has_value()) {
            artifactModel_.replace({});
            reportError(*outcome.artifactError);
        } else if (outcome.artifacts.has_value()) {
            artifactModel_.replace(std::move(*outcome.artifacts));
        } else if (request.refreshArtifacts && selectedRunId_.isEmpty()) {
            artifactModel_.replace({});
        }

        if (outcome.warningError.has_value()) {
            warningModel_.replace({});
            reportError(*outcome.warningError);
        } else if (outcome.warnings.has_value()) {
            warningModel_.replace(std::move(*outcome.warnings));
        } else if (request.refreshArtifacts && selectedRunId_.isEmpty()) {
            warningModel_.replace({});
        }
    }

    // Publish after durable projections have been applied. AgentSessionPanel
    // uses this signal as a dirty notification and starts its own coalesced
    // background read, never a nested synchronous SQLite call.
    for (const QString& runId : request.changedRunIds) {
        emit runUpdated(runId);
    }
    for (const QString& runId : request.terminalRunIds) {
        if (!request.changedRunIds.contains(runId)) {
            emit runUpdated(runId);
        }
        emit runFinished(runId, model_.statusFor(runId));
    }

    if (pendingReplaceRunList_ || pendingStages_ || pendingArtifacts_
        || !pendingChangedRunIds_.isEmpty() || !pendingTerminalRunIds_.isEmpty()) {
        scheduleRefresh();
    }
}

void RunController::reportError(const ai::common::Error& error)
{
    lastError_ = QString::fromStdString(error.message);
    emit lastErrorChanged();
    emit operationFailed(QString::fromStdString(error.code), lastError_);
}

void RunController::clearError()
{
    if (!lastError_.isEmpty()) {
        lastError_.clear();
        emit lastErrorChanged();
    }
}
