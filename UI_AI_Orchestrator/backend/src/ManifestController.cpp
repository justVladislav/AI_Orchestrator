#include <ai/ui/ManifestController.hpp>

#include <QLocale>
#include <QtConcurrent/QtConcurrentRun>

namespace {

// AG-17.7B: the amount QML shows next to a usage line. Empty when there is nothing to show
// or the price is entirely unknown - an unknown price is never rendered as "$0.000000".
// For a partial state the amount is a lower bound (the caller words it as such).
QString usageCostText(const ai::application::ManifestUsageTotals& usage)
{
    const auto state = usage.priceState();
    if (state != ai::application::ManifestUsagePriceState::Known
        && state != ai::application::ManifestUsagePriceState::Partial) {
        return {};
    }
    return QStringLiteral("$%1").arg(QLocale::c().toString(
        static_cast<double>(usage.costMicros) / 1'000'000.0, 'f', 6));
}

QVariantMap usageToVariant(const ai::application::ManifestUsageTotals& usage)
{
    const auto number = [](std::uint64_t value) { return QVariant::fromValue<qulonglong>(value); };
    return QVariantMap {
        {QStringLiteral("executions"), number(usage.executions)},
        {QStringLiteral("completed"), number(usage.completed)},
        {QStringLiteral("partial"), number(usage.partial)},
        {QStringLiteral("failed"), number(usage.failed)},
        {QStringLiteral("cancelled"), number(usage.cancelled)},
        {QStringLiteral("inFlight"), number(usage.inFlight)},
        {QStringLiteral("interrupted"), number(usage.interrupted)},
        {QStringLiteral("inputTokens"), number(usage.inputTokens)},
        {QStringLiteral("outputTokens"), number(usage.outputTokens)},
        {QStringLiteral("costMicros"), number(usage.costMicros)},
        {QStringLiteral("unknownPriceExecutions"), number(usage.unknownPriceExecutions)},
        {QStringLiteral("priceState"), QString::fromLatin1(toString(usage.priceState()))},
        {QStringLiteral("costText"), usageCostText(usage)},
        {QStringLiteral("saturated"), usage.saturated}};
}

} // namespace

ManifestController::ManifestController(
    ai::application::ExecutionManifestReadModelService& readModel,
    ai::application::VerificationBootstrapRetryService& bootstrapRetryService,
    QObject* parent)
    : QObject(parent)
    , readModel_(readModel)
    , bootstrapRetryService_(bootstrapRetryService)
{
    connect(&continuationWatcher_,
        &QFutureWatcher<ContinuationCommandOutcome>::finished,
        this, &ManifestController::finishVerificationContinuation);
}

ManifestController::ManifestController(
    ai::application::ExecutionManifestReadModelService& readModel,
    ai::application::VerificationBootstrapRetryService& bootstrapRetryService,
    ai::application::ManifestVerificationContinuationReadModelService& continuationReadModel,
    ai::application::ManifestVerificationContinuationPreparationService& continuationPreparation,
    ai::application::ManifestVerificationContinuationRepairService& continuationRepair,
    QObject* parent)
    : ManifestController(readModel, bootstrapRetryService, parent)
{
    continuationReadModel_ = &continuationReadModel;
    continuationPreparation_ = &continuationPreparation;
    continuationRepair_ = &continuationRepair;
}

ManifestController::~ManifestController()
{
    if (continuationStopSource_) continuationStopSource_->request_stop();
    continuationWatcher_.waitForFinished();
}

void ManifestController::clear()
{
    clearManifestOnly();
    clearVerificationEnvironment();
    clearVerificationContinuation();
}

void ManifestController::clearVerificationContinuation()
{
    hasVerificationContinuation_ = false;
    verificationContinuation_.clear();
}

void ManifestController::populateVerificationContinuation(
    const ai::application::ManifestVerificationContinuationView& view)
{
    QVariantList criteria;
    for (const auto& criterion : view.criteria) {
        criteria.push_back(QVariantMap {
            {QStringLiteral("requirementId"), QString::fromStdString(criterion.requirementId)},
            {QStringLiteral("required"), criterion.required},
            {QStringLiteral("sourceStatus"), QString::fromLatin1(criterion.sourceStatus.c_str())},
            {QStringLiteral("finalStatus"), QString::fromLatin1(criterion.finalStatus.c_str())}});
    }
    QVariantList findings;
    for (const auto& finding : view.findings) {
        findings.push_back(QVariantMap {
            {QStringLiteral("diagnosticCode"), QString::fromStdString(finding.diagnosticCode)},
            {QStringLiteral("requirementId"), QString::fromStdString(finding.requirementId)},
            {QStringLiteral("relativePath"), QString::fromStdString(finding.relativePath)},
            {QStringLiteral("attributionKind"), QString::fromLatin1(finding.attributionKind.c_str())},
            {QStringLiteral("ownerTaskId"), QString::fromStdString(finding.ownerTaskId)},
            {QStringLiteral("ownerProposalId"), QString::fromStdString(finding.ownerProposalId)}});
    }
    // QML-P0: the child's own final combined verification, classified by
    // ManifestContentOutcome - "needs_manual_action" only when it is
    // safety-clean, build-clean, and Advisory-lint-only. remainingFindings
    // is populated only in that case; it never authorizes Apply and never
    // overrides childStatus - see ManifestVerificationContinuationView's
    // own doc comment.
    QVariantList remainingFindings;
    for (const auto& finding : view.remainingFindings) {
        remainingFindings.push_back(QVariantMap {
            {QStringLiteral("diagnosticCode"), QString::fromStdString(finding.diagnosticCode)},
            {QStringLiteral("requirementId"), QString::fromStdString(finding.requirementId)},
            {QStringLiteral("relativePath"), QString::fromStdString(finding.relativePath)},
            {QStringLiteral("line"), finding.line},
            {QStringLiteral("column"), finding.column},
            {QStringLiteral("tag"), QString::fromStdString(finding.tag)},
            {QStringLiteral("message"), QString::fromStdString(finding.message)},
            {QStringLiteral("severity"), QString::fromLatin1(finding.severity.c_str())}});
    }
    hasVerificationContinuation_ = true;
    verificationContinuation_ = QVariantMap {
        {QStringLiteral("selectedRole"), QString::fromLatin1(view.selectedRole.c_str())},
        {QStringLiteral("sourceRunId"), QString::fromStdString(view.sourceRunId)},
        {QStringLiteral("childRunId"), QString::fromStdString(view.childRunId)},
        {QStringLiteral("sourceStatus"), QString::fromLatin1(view.sourceStatus.c_str())},
        {QStringLiteral("childStatus"), QString::fromLatin1(view.childStatus.c_str())},
        {QStringLiteral("hasDurableCheckpoint"), view.hasDurableCheckpoint},
        {QStringLiteral("checkpointStatus"), QString::fromLatin1(view.checkpointStatus.c_str())},
        {QStringLiteral("hasAttribution"), view.hasAttribution},
        {QStringLiteral("attributionStatus"), QString::fromLatin1(view.attributionStatus.c_str())},
        {QStringLiteral("plannerSkipped"), view.plannerSkipped},
        {QStringLiteral("canContinue"), view.canContinue},
        {QStringLiteral("canStartRepair"), view.canStartRepair},
        {QStringLiteral("blockCode"), QString::fromStdString(view.blockCode)},
        {QStringLiteral("blockMessage"), QString::fromStdString(view.blockMessage)},
        {QStringLiteral("criteria"), criteria},
        {QStringLiteral("findings"), findings},
        {QStringLiteral("contentOutcome"), QString::fromLatin1(view.contentOutcome.c_str())},
        {QStringLiteral("remainingFindings"), remainingFindings}};
}

void ManifestController::clearManifestOnly()
{
    hasManifest_ = false;
    tasks_.clear();
    requestedMaxParallelism_ = 1;
    hasWorkspaceScope_ = false;
    scopeDependencyEdges_.clear();
    routingMode_ = QStringLiteral("none");
    routingMembers_.clear();
    usageAvailable_ = false;
    usagePartitionExact_ = false;
    usageRunTotal_.clear();
    usageUnattributedCount_ = 0;
    usageUnattributed_.clear();
    usageDiagnostics_.clear();
    hasVerificationAdvisory_ = false;
    verificationAdvisoryPassed_ = false;
    hasAdvisories_ = false;
    advisoryCount_ = 0;
    advisories_.clear();
}

void ManifestController::clearVerificationEnvironment()
{
    hasVerificationEnvironment_ = false;
    verificationEnvironmentState_.clear();
    verificationEnvironmentIssue_.clear();
    verificationEnvironmentDiagnostic_.clear();
    verificationProfiles_.clear();
    hasBootstrapDecision_ = false;
    bootstrapDisposition_.clear();
    bootstrapReasonCode_.clear();
    bootstrapMessage_.clear();
    bootstrapSetupPaths_.clear();
    hasBootstrapTransition_ = false;
    bootstrapTransitionDisposition_.clear();
    bootstrapCandidateProfileId_.clear();
    bootstrapCandidateBuildProfileId_.clear();
    bootstrapCandidateTargetId_.clear();
    canRetryAfterSetup_ = false;
    canRetryAfterSetupReason_.clear();
    canConfigureBackendProfile_ = false;
    canConfigureBackendProfileReasonCode_.clear();
    canConfigureBackendProfileReason_.clear();
}

void ManifestController::populateVerificationEnvironment(
    const ai::application::ManifestVerificationEnvironmentView& view)
{
    hasVerificationEnvironment_ = true;
    verificationEnvironmentState_ = QString::fromLatin1(view.state.c_str());
    verificationEnvironmentIssue_ = QString::fromLatin1(view.issue.c_str());
    verificationEnvironmentDiagnostic_ = QString::fromStdString(view.diagnostic);

    QVariantList profiles;
    profiles.reserve(static_cast<int>(view.profiles.size()));
    for (const auto& profile : view.profiles) {
        QVariantList buildPresets;
        buildPresets.reserve(static_cast<int>(profile.buildPresets.size()));
        for (const auto& buildPreset : profile.buildPresets) {
            QStringList allowedTargets;
            for (const auto& target : buildPreset.allowedTargets)
                allowedTargets << QString::fromStdString(target);
            buildPresets.push_back(QVariantMap {
                {QStringLiteral("name"), QString::fromStdString(buildPreset.name)},
                {QStringLiteral("allowedTargetsConfirmed"), buildPreset.allowedTargetsConfirmed},
                {QStringLiteral("allowedTargets"), allowedTargets}});
        }
        profiles.push_back(QVariantMap {
            {QStringLiteral("profileId"), QString::fromStdString(profile.profileId)},
            {QStringLiteral("configurePresetName"), QString::fromStdString(profile.configurePresetName)},
            {QStringLiteral("configureSource"), QString::fromLatin1(profile.configureSource.c_str())},
            {QStringLiteral("resolvedBinaryDir"), QString::fromStdString(profile.resolvedBinaryDir)},
            {QStringLiteral("buildPresetsConfirmed"), profile.buildPresetsConfirmed},
            {QStringLiteral("buildPresets"), buildPresets}});
    }
    verificationProfiles_ = std::move(profiles);

    hasBootstrapDecision_ = view.hasBootstrapDecision;
    bootstrapDisposition_ = QString::fromLatin1(view.bootstrapDisposition.c_str());
    bootstrapReasonCode_ = QString::fromStdString(view.bootstrapReasonCode);
    bootstrapMessage_ = QString::fromStdString(view.bootstrapMessage);
    QStringList setupPaths;
    for (const auto& path : view.bootstrapSetupPaths) setupPaths << QString::fromStdString(path);
    bootstrapSetupPaths_ = std::move(setupPaths);
    hasBootstrapTransition_ = view.hasBootstrapTransition;
    bootstrapTransitionDisposition_ = QString::fromLatin1(view.bootstrapTransitionDisposition.c_str());
    bootstrapCandidateProfileId_ = QString::fromStdString(view.bootstrapCandidateProfileId);
    bootstrapCandidateBuildProfileId_ = QString::fromStdString(view.bootstrapCandidateBuildProfileId);
    bootstrapCandidateTargetId_ = QString::fromStdString(view.bootstrapCandidateTargetId);

    canRetryAfterSetup_ = view.canRetryAfterSetup;
    canRetryAfterSetupReason_ = QString::fromStdString(view.canRetryAfterSetupReason);
    canConfigureBackendProfile_ = view.canConfigureBackendProfile;
    canConfigureBackendProfileReasonCode_ =
        QString::fromLatin1(view.canConfigureBackendProfileReasonCode.c_str());
    canConfigureBackendProfileReason_ = QString::fromStdString(view.canConfigureBackendProfileReason);
}

bool ManifestController::selectRun(const QString& runId)
{
    selectedRunId_ = runId;
    return refresh();
}

QString ManifestController::retryAfterSetup()
{
    if (selectedRunId_.isEmpty()) {
        errorText_ = QStringLiteral("client.no_run_selected: No Run is selected");
        emit stateChanged();
        return {};
    }
    const auto created = bootstrapRetryService_.createLinkedRetry(selectedRunId_.toStdString());
    if (!created.success()) {
        // AG-18.4A hardening: preserve the stable code, not only the
        // message - a caller (and anyone reading errorText) must be able to
        // tell exactly which typed Backend rejection occurred.
        errorText_ = QString::fromStdString(created.error()->code) + QStringLiteral(": ")
            + QString::fromStdString(created.error()->message);
        emit stateChanged();
        return {};
    }
    errorText_.clear();
    emit stateChanged();
    return QString::fromStdString(created.value());
}

bool ManifestController::refresh()
{
    if (selectedRunId_.isEmpty()) {
        clear();
        errorText_.clear();
        emit stateChanged();
        return true;
    }

    const auto continuation = continuationReadModel_ == nullptr
        ? ai::common::Result<std::optional<ai::application::ManifestVerificationContinuationView>>::ok(
            std::nullopt)
        : continuationReadModel_->viewForRun(selectedRunId_.toStdString());
    if (!continuation.success()) {
        clear();
        errorText_ = QString::fromStdString(continuation.error()->code)
            + QStringLiteral(": ") + QString::fromStdString(continuation.error()->message);
        emit stateChanged();
        return false;
    }
    if (continuation.value().has_value())
        populateVerificationContinuation(*continuation.value());
    else
        clearVerificationContinuation();

    // AG-18.4A hardening: independent of whether a manifest was ever frozen
    // for this Run - populated first so an ActionRequired Run (which never
    // freezes a manifest) still projects its bootstrap-lifecycle state
    // below. A repository/corruption failure here is never silently
    // swallowed as "no verification environment" - it clears all stale
    // state, preserves the stable code alongside the message, and fails
    // this whole refresh() call closed rather than proceeding to the
    // manifest fetch below with a known-bad read.
    const auto veView = readModel_.verificationEnvironmentViewForRun(selectedRunId_.toStdString());
    if (!veView.success()) {
        clear();
        errorText_ = QString::fromStdString(veView.error()->code) + QStringLiteral(": ")
            + QString::fromStdString(veView.error()->message);
        emit stateChanged();
        return false;
    }
    if (veView.value().has_value()) {
        populateVerificationEnvironment(*veView.value());
    } else {
        clearVerificationEnvironment();
    }

    const auto view = readModel_.viewForRun(selectedRunId_.toStdString());
    if (!view.success()) {
        clearManifestOnly();
        errorText_ = QString::fromStdString(view.error()->code) + QStringLiteral(": ")
            + QString::fromStdString(view.error()->message);
        emit stateChanged();
        return false;
    }
    if (!view.value().has_value()) {
        // No manifest was ever frozen for this run - the ordinary case for
        // every Mock/execution-profile run, a manifest-workflow run still in
        // its planning stage, or a terminal ActionRequired disposition (see
        // the verification-environment projection above, already populated).
        clearManifestOnly();
        errorText_.clear();
        emit stateChanged();
        return true;
    }

    const auto& manifestView = *view.value();
    hasManifest_ = true;
    requestedMaxParallelism_ = manifestView.requestedMaxParallelism;
    hasWorkspaceScope_ = manifestView.workspaceScopeAvailable;

    QVariantList scopeDependencyEdges;
    for (const auto& edge : manifestView.scopeDependencyEdges) {
        scopeDependencyEdges.push_back(QVariantMap {
            {QStringLiteral("dependentTaskId"), QString::fromStdString(edge.dependentTaskId)},
            {QStringLiteral("providerTaskId"), QString::fromStdString(edge.providerTaskId)},
            {QStringLiteral("relativePath"), QString::fromStdString(edge.relativePath)}});
    }
    scopeDependencyEdges_ = std::move(scopeDependencyEdges);

    // AG-17.6: the routing projection, exactly as the read model classified it.
    routingMode_ = QString::fromStdString(manifestView.routingMode);
    QVariantList routingMembers;
    routingMembers.reserve(static_cast<int>(manifestView.routingMembers.size()));
    for (const auto& member : manifestView.routingMembers) {
        routingMembers.push_back(QVariantMap {
            {QStringLiteral("memberId"), QString::fromStdString(member.memberId)},
            {QStringLiteral("role"), QString::fromStdString(member.role)},
            {QStringLiteral("providerId"), QString::fromStdString(member.providerId)},
            {QStringLiteral("providerName"), QString::fromStdString(member.providerName)},
            {QStringLiteral("modelId"), QString::fromStdString(member.modelId)},
            {QStringLiteral("modelName"), QString::fromStdString(member.modelName)},
            {QStringLiteral("label"), QString::fromStdString(member.label)},
            {QStringLiteral("lead"), member.lead},
            {QStringLiteral("workspaceAccess"), member.workspaceAccess},
            {QStringLiteral("assignedTaskCount"), static_cast<quint32>(member.assignedTaskCount)},
            {QStringLiteral("participation"), QString::fromStdString(member.participation)},
            {QStringLiteral("usage"), usageToVariant(member.usage)}});
    }
    routingMembers_ = std::move(routingMembers);

    // AG-17.7B: Run-level usage reconstruction - exactly as the read model projected it.
    usageAvailable_ = manifestView.usageAvailable;
    usagePartitionExact_ = manifestView.usagePartitionExact;
    usageRunTotal_ = usageToVariant(manifestView.usageRunTotal);
    usageUnattributedCount_ = static_cast<int>(manifestView.usageUnattributed.executions);
    QVariantList unattributed;
    unattributed.reserve(static_cast<int>(manifestView.usageUnattributedExecutions.size()));
    for (const auto& execution : manifestView.usageUnattributedExecutions) {
        unattributed.push_back(QVariantMap {
            {QStringLiteral("id"), QString::fromStdString(execution.id)},
            {QStringLiteral("position"), static_cast<quint32>(execution.position)},
            {QStringLiteral("attempt"), static_cast<quint32>(execution.attempt)},
            {QStringLiteral("role"), QString::fromStdString(execution.role)},
            {QStringLiteral("reason"), QString::fromStdString(execution.reason)}});
    }
    usageUnattributed_ = std::move(unattributed);
    QStringList usageDiagnostics;
    for (const auto& diagnostic : manifestView.usageDiagnostics) {
        usageDiagnostics.push_back(QString::fromStdString(diagnostic));
    }
    usageDiagnostics_ = std::move(usageDiagnostics);

    // QML-lint advisory-severity policy (2026-09-28): Backend-computed
    // exactly as ExecutionManifestReadModelService projected it - this
    // controller never classifies a finding's tag, and never derives Apply
    // eligibility from hasAdvisories_.
    const auto& advisoryView = manifestView.verificationAdvisory;
    hasVerificationAdvisory_ = advisoryView.verificationAvailable;
    verificationAdvisoryPassed_ = advisoryView.verificationPassed;
    hasAdvisories_ = advisoryView.hasAdvisories;
    advisoryCount_ = static_cast<int>(advisoryView.advisoryCount);
    QVariantList advisories;
    advisories.reserve(static_cast<int>(advisoryView.advisories.size()));
    for (const auto& finding : advisoryView.advisories) {
        advisories.push_back(QVariantMap {
            {QStringLiteral("relativePath"), QString::fromStdString(finding.relativePath)},
            {QStringLiteral("line"), finding.line},
            {QStringLiteral("column"), finding.column},
            {QStringLiteral("tag"), QString::fromStdString(finding.tag)},
            {QStringLiteral("message"), QString::fromStdString(finding.text)},
            {QStringLiteral("requirementId"),
                finding.requirementId.has_value()
                    ? QString::fromStdString(*finding.requirementId)
                    : QString()}});
    }
    advisories_ = std::move(advisories);

    QVariantList tasks;
    tasks.reserve(static_cast<int>(manifestView.tasks.size()));
    for (const auto& task : manifestView.tasks) {
        QStringList dependencies;
        for (const auto& dependency : task.dependencies) dependencies << QString::fromStdString(dependency);
        QStringList ownedPaths;
        for (const auto& path : task.ownedPaths) ownedPaths << QString::fromStdString(path);
        QStringList acceptanceCriteria;
        for (const auto& criterion : task.acceptanceCriteria) acceptanceCriteria << QString::fromStdString(criterion);
        QStringList contextReferences;
        for (const auto& reference : task.contextReferences) contextReferences << QString::fromStdString(reference);

        // AG-04: safe, bounded worker no-op evidence - already validated by
        // the Backend (see ManifestWorkerNoChanges.hpp), never raw model
        // JSON. Absent for every task except the one that actually resolved
        // no-op.
        const bool hasNoChanges = task.workerNoChanges.has_value();
        QVariantList criterionEvidence;
        QStringList inspectedPaths;
        if (hasNoChanges) {
            for (const auto& evidence : task.workerNoChanges->criterionEvidence) {
                criterionEvidence.push_back(QVariantMap {
                    {QStringLiteral("requirementId"), QString::fromStdString(evidence.requirementId)},
                    {QStringLiteral("inspectedPath"), QString::fromStdString(evidence.inspectedPath)},
                    {QStringLiteral("observation"), QString::fromStdString(evidence.observation)}});
            }
            for (const auto& path : task.workerNoChanges->inspectedRelativePaths)
                inspectedPaths << QString::fromStdString(path);
        }

        // AG-05: the typed acceptance-requirement *definitions* this task
        // was frozen with (what each requirement actually demands), not
        // just their pass/fail results (already shown separately via AG-08's
        // verification report). Absent for a legacy execution-manifest.v1
        // manifest that never carried a typed contract.
        const bool hasAcceptanceContract = task.acceptanceContract.has_value();
        QVariantList acceptanceRequirements;
        if (hasAcceptanceContract) {
            for (const auto& requirement : task.acceptanceContract->requirements) {
                QStringList requiredOutputs;
                for (const auto& output : requirement.requiredOutputs)
                    requiredOutputs << QString::fromStdString(output);
                acceptanceRequirements.push_back(QVariantMap {
                    {QStringLiteral("id"), QString::fromStdString(requirement.id)},
                    {QStringLiteral("description"), QString::fromStdString(requirement.description)},
                    {QStringLiteral("requiredOutputs"), requiredOutputs},
                    {QStringLiteral("verificationKind"), QString::fromLatin1(
                        ai::application::toString(requirement.verificationKind))},
                    {QStringLiteral("required"), requirement.required}});
            }
        }

        // AG-15.3: workspace-scope projection - all Backend truth, this
        // controller never computes a role or category itself.
        QVariantList scopeEntries;
        for (const auto& entry : task.scopeEntries) {
            scopeEntries.push_back(QVariantMap {
                {QStringLiteral("relativePath"), QString::fromStdString(entry.relativePath)},
                {QStringLiteral("role"), QString::fromStdString(entry.role)},
                {QStringLiteral("isExpansion"), entry.isExpansion}});
        }
        QStringList scopeInspectedPaths;
        for (const auto& path : task.inspectedPaths) scopeInspectedPaths << QString::fromStdString(path);
        QStringList proposedCreatedPaths;
        for (const auto& path : task.proposedCreatedPaths) proposedCreatedPaths << QString::fromStdString(path);
        QStringList proposedModifiedPaths;
        for (const auto& path : task.proposedModifiedPaths) proposedModifiedPaths << QString::fromStdString(path);

        tasks.push_back(QVariantMap {
            {QStringLiteral("id"), QString::fromStdString(task.id)},
            {QStringLiteral("objective"), QString::fromStdString(task.objective)},
            {QStringLiteral("role"), QString::fromStdString(task.role)},
            {QStringLiteral("dependencies"), dependencies},
            {QStringLiteral("ownedPaths"), ownedPaths},
            {QStringLiteral("acceptanceCriteria"), acceptanceCriteria},
            {QStringLiteral("contextReferences"), contextReferences},
            {QStringLiteral("requestedCapability"),
                QString::fromLatin1(ai::application::toString(task.requestedCapability))},
            {QStringLiteral("status"), QString::fromLatin1(ai::application::toString(task.status))},
            {QStringLiteral("errorCode"), QString::fromStdString(task.errorCode)},
            {QStringLiteral("errorMessage"), QString::fromStdString(task.errorMessage)},
            {QStringLiteral("changeSetId"), task.changeSetId.has_value()
                ? QString::fromStdString(*task.changeSetId) : QString()},
            {QStringLiteral("attempts"), static_cast<quint32>(task.attempts)},
            {QStringLiteral("outputSummary"), QString::fromStdString(task.outputSummary)},
            {QStringLiteral("hasNoChangesEvidence"), hasNoChanges},
            {QStringLiteral("noChangesReason"), hasNoChanges
                ? QString::fromLatin1(ai::application::toString(task.workerNoChanges->reason))
                : QString()},
            {QStringLiteral("noChangesSummary"), hasNoChanges
                ? QString::fromStdString(task.workerNoChanges->summary) : QString()},
            {QStringLiteral("noChangesCriterionEvidence"), criterionEvidence},
            {QStringLiteral("noChangesInspectedPaths"), inspectedPaths},
            {QStringLiteral("hasAcceptanceContract"), hasAcceptanceContract},
            {QStringLiteral("acceptanceRequirements"), acceptanceRequirements},
            {QStringLiteral("scopeEntries"), scopeEntries},
            {QStringLiteral("scopeInspectedPaths"), scopeInspectedPaths},
            {QStringLiteral("proposedCreatedPaths"), proposedCreatedPaths},
            {QStringLiteral("proposedModifiedPaths"), proposedModifiedPaths},
            {QStringLiteral("assigneeMemberId"), QString::fromStdString(task.assigneeMemberId)},
            {QStringLiteral("assigneeRole"), QString::fromStdString(task.assigneeRole)},
            {QStringLiteral("assigneeLabel"), QString::fromStdString(task.assigneeLabel)}
        });
    }
    tasks_ = std::move(tasks);
    errorText_.clear();
    emit stateChanged();
    return true;
}

bool ManifestController::prepareVerificationContinuation()
{
    if (selectedRunId_.isEmpty() || verificationContinuationBusy_) return false;
    if (!hasVerificationContinuation_
        || !verificationContinuation_.value(QStringLiteral("canContinue")).toBool()) {
        errorText_ = QStringLiteral("verification_continuation.action_not_allowed: ")
            + verificationContinuation_.value(QStringLiteral("blockMessage")).toString();
        emit stateChanged();
        return false;
    }

    if (continuationPreparation_ == nullptr) {
        errorText_ = QStringLiteral("verification_continuation.preparation_unavailable: ")
            + QStringLiteral("Verification continuation preparation is not available");
        emit stateChanged();
        return false;
    }

    const std::string sourceRunId = verificationContinuation_
        .value(QStringLiteral("sourceRunId")).toString().toStdString();
    continuationStopSource_ = std::make_shared<std::stop_source>();
    auto stopSource = continuationStopSource_;
    auto* preparation = continuationPreparation_;
    verificationContinuationBusy_ = true;
    errorText_.clear();
    emit stateChanged();
    // MW-P5.6B: this branch never calls repair() and therefore never makes a
    // provider/LLM request - only real, already-local qmllint/cmake
    // verification commands. The pre-existing-debt/no-owner checks below are
    // themselves free (already computed by prepare()'s own attribution step)
    // and exist so the UI can refuse to even OFFER the paid step rather than
    // silently discovering it is pointless after spending money.
    continuationWatcher_.setFuture(QtConcurrent::run(
        [sourceRunId, stopSource, preparation] {
            ContinuationCommandOutcome outcome;
            const auto prepared = preparation->prepare(sourceRunId, stopSource->get_token());
            if (!prepared.success()) {
                outcome.error = *prepared.error();
                return outcome;
            }
            outcome.childRunId = QString::fromStdString(prepared.value().childRun.id);

            bool hasOwner = false;
            for (const auto& finding : prepared.value().attribution.findings) {
                if (finding.kind
                        == ai::application::ManifestVerificationFindingAttributionKind::
                            PreExistingVerificationDebt) {
                    outcome.error = ai::common::Error {
                        "verification_continuation.pre_existing_debt_requires_prerequisite",
                        "Pre-existing verification debt requires an explicit prerequisite Run"};
                    return outcome;
                }
                hasOwner = hasOwner
                    || finding.kind == ai::application::ManifestVerificationFindingAttributionKind::
                        SourceProposalOwner
                    || finding.kind == ai::application::ManifestVerificationFindingAttributionKind::
                        IntegrationOwner;
            }
            if (!hasOwner) {
                outcome.error = ai::common::Error {
                    "verification_continuation.repair_scope_empty",
                    "No proposal owner is eligible for a repair worker"};
                return outcome;
            }
            outcome.status = QStringLiteral("prepared");
            return outcome;
        }));
    return true;
}

bool ManifestController::startVerificationContinuationRepair()
{
    if (selectedRunId_.isEmpty() || verificationContinuationBusy_) return false;
    // MW-P5.6B: re-read the child identity from the durable read-model view
    // rather than trusting anything cached from a possibly much earlier
    // prepareVerificationContinuation() call - the caller may have selected
    // a different Run, or the workspace/source proposals may have moved on,
    // in the time between the free preparation and this explicitly
    // confirmed paid step. repair() itself re-validates the current
    // baseline, source proposal states/hashes, and the attribution
    // checkpoint fresh before doing any paid work; this call only needs to
    // name the exact child to repair.
    if (continuationReadModel_ == nullptr || continuationRepair_ == nullptr) {
        errorText_ = QStringLiteral("verification_continuation.repair_unavailable: ")
            + QStringLiteral("Verification continuation repair is not available");
        emit stateChanged();
        return false;
    }

    const auto durable = continuationReadModel_->viewForRun(selectedRunId_.toStdString());
    if (!durable.success()) {
        errorText_ = QString::fromStdString(durable.error()->code) + QStringLiteral(": ")
            + QString::fromStdString(durable.error()->message);
        emit stateChanged();
        return false;
    }
    if (!durable.value().has_value()) {
        clearVerificationContinuation();
        errorText_ = QStringLiteral("verification_continuation.repair_not_prepared: ")
            + QStringLiteral("No durable verification continuation exists for this Run");
        emit stateChanged();
        return false;
    }

    // Keep the visible projection synchronized with the same durable view
    // used for the paid boundary. This also removes a stale Prepared action
    // immediately if another process already started or finished the child.
    populateVerificationContinuation(*durable.value());
    const QString childRunId = QString::fromStdString(durable.value()->childRunId);
    const QString childStatus = QString::fromLatin1(durable.value()->childStatus.c_str());
    if (childRunId.isEmpty() || childStatus != QStringLiteral("prepared")) {
        errorText_ = QStringLiteral("verification_continuation.repair_not_prepared: ")
            + QStringLiteral("Prepare paid verification repair must complete successfully before "
                             "starting the paid repair");
        emit stateChanged();
        return false;
    }
    if (!durable.value()->canStartRepair) {
        const QString code = durable.value()->blockCode.empty()
            ? QStringLiteral("verification_continuation.repair_not_allowed")
            : QString::fromStdString(durable.value()->blockCode);
        const QString message = durable.value()->blockMessage.empty()
            ? QStringLiteral("The Backend did not authorize paid repair for this child")
            : QString::fromStdString(durable.value()->blockMessage);
        errorText_ = code + QStringLiteral(": ") + message;
        emit stateChanged();
        return false;
    }

    const std::string childRunIdStd = childRunId.toStdString();
    continuationStopSource_ = std::make_shared<std::stop_source>();
    auto stopSource = continuationStopSource_;
    auto* repair = continuationRepair_;
    verificationContinuationBusy_ = true;
    errorText_.clear();
    emit stateChanged();
    continuationWatcher_.setFuture(QtConcurrent::run(
        [childRunIdStd, stopSource, repair] {
            ContinuationCommandOutcome outcome;
            outcome.childRunId = QString::fromStdString(childRunIdStd);
            const auto repaired = repair->repair(childRunIdStd, stopSource->get_token());
            if (!repaired.success()) {
                outcome.error = *repaired.error();
                return outcome;
            }
            outcome.status = QString::fromLatin1(
                ai::application::toString(repaired.value().finalStatus));
            return outcome;
        }));
    return true;
}

bool ManifestController::cancelVerificationContinuation()
{
    if (!verificationContinuationBusy_ || !continuationStopSource_) return false;
    return continuationStopSource_->request_stop();
}

void ManifestController::finishVerificationContinuation()
{
    const auto outcome = continuationWatcher_.result();
    verificationContinuationBusy_ = false;
    continuationStopSource_.reset();
    if (outcome.error.has_value()) {
        errorText_ = QString::fromStdString(outcome.error->code) + QStringLiteral(": ")
            + QString::fromStdString(outcome.error->message);
    } else {
        errorText_.clear();
    }
    // Re-read every decision/lineage field from durable Backend storage. The
    // worker result is notification data, never the UI's source of truth.
    const QString durableError = errorText_;
    static_cast<void>(refresh());
    if (outcome.error.has_value()) errorText_ = durableError;
    emit stateChanged();
    emit verificationContinuationFinished(outcome.childRunId, outcome.status);
}
