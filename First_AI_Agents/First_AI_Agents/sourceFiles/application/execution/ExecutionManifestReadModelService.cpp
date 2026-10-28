#include "ai/application/execution/ExecutionManifestReadModelService.hpp"

#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/execution/ManifestRouting.hpp"
#include "ai/application/verification/QmlLintDiagnosticPolicy.hpp"
#include "ai/application/execution/TaskRouteResolver.hpp"

#include <algorithm>
#include <unordered_map>

namespace ai::application {

namespace {

// AG-18.4A helpers: see ManifestVerificationEnvironmentIssue's own doc
// comment for why this matches fixed Backend-authored diagnostic literals
// rather than a typed sub-reason field.

bool anyProfileBuildPresetsUnconfirmed(const VerificationEnvironmentSnapshot& snapshot)
{
    return std::any_of(snapshot.profiles.begin(), snapshot.profiles.end(),
        [](const VerificationProfile& profile) { return !profile.buildPresetsConfirmed; });
}

bool diagnosticIndicatesDrift(const std::string& diagnostic)
{
    return diagnostic
            == "The workspace could not be re-inspected to confirm it still matches the "
               "captured baseline"
        || diagnostic == "The workspace could not be compared against the captured baseline"
        || diagnostic == "The workspace changed since the baseline was captured";
}

bool diagnosticIndicatesToolUnavailable(const std::string& diagnostic)
{
    return diagnostic == "The workspace CMake preset file(s) could not be inspected"
        || diagnostic
            == "cmake could not be started, timed out, or failed while listing configure presets"
        || diagnostic
            == "cmake could not be started, timed out, or failed while listing build presets; "
               "configure preset(s) were still confirmed"
        || diagnostic == "One or more build presets have no confirmed configured-target catalog";
}

ManifestVerificationEnvironmentIssue classifyIssue(const VerificationEnvironmentSnapshot& snapshot)
{
    if (diagnosticIndicatesDrift(snapshot.diagnostic)) {
        return ManifestVerificationEnvironmentIssue::Drifted;
    }
    if (diagnosticIndicatesToolUnavailable(snapshot.diagnostic)
        || anyProfileBuildPresetsUnconfirmed(snapshot)) {
        return ManifestVerificationEnvironmentIssue::ToolUnavailable;
    }
    return ManifestVerificationEnvironmentIssue::None;
}

// AG-18.4A: reclassifies a stored SetupRequired snapshot to
// ProposedBootstrap at read time only, when a durable BootstrapPending
// decision exists for this Run - never mutates the persisted snapshot
// itself. See VerificationEnvironmentState's own doc comment: this is
// exactly the reclassification it reserves the third enum value for.
VerificationEnvironmentState reclassifyState(
    const VerificationEnvironmentSnapshot& snapshot,
    const std::optional<VerificationBootstrapDecision>& decision)
{
    if (snapshot.state == VerificationEnvironmentState::SetupRequired && decision.has_value()
        && decision->disposition == VerificationBootstrapDisposition::BootstrapPending) {
        return VerificationEnvironmentState::ProposedBootstrap;
    }
    return snapshot.state;
}

// AG-18.4A: "Retry after setup" is offered only when the durable lifecycle
// says this Run awaits external setup. The command itself additionally checks
// the live workspace for a preset file immediately before it reserves/creates
// the retry; this immutable read model deliberately does not watch files.
// canRetryAfterSetupReason is always set (bounded, non-blank) when false.
void populateRetryAfterSetupEligibility(
    ManifestVerificationEnvironmentView& view,
    const VerificationEnvironmentSnapshot& snapshot,
    const std::optional<VerificationBootstrapDecision>& decision)
{
    if (!decision.has_value()) {
        view.canRetryAfterSetup = false;
        view.canRetryAfterSetupReason =
            snapshot.state == VerificationEnvironmentState::ExistingProfile
            ? "A usable verification profile already exists; no retry is needed."
            : "No bootstrap disposition has been recorded for this Run yet.";
        return;
    }
    if (decision->disposition != VerificationBootstrapDisposition::ActionRequired) {
        view.canRetryAfterSetup = false;
        view.canRetryAfterSetupReason =
            "This Run's bootstrap lifecycle already has a disposition other than "
            "action-required; retrying is not applicable.";
        return;
    }
    view.canRetryAfterSetup = true;
    view.canRetryAfterSetupReason.clear();
}

// QML-lint advisory-severity policy (2026-09-28): the sole place this
// projection is built - reads ManifestWorkLedgerEntry::verification exactly
// as durably recorded, never re-derives pass/fail or re-classifies a tag.
// Fails closed (returns an error) rather than silently reporting
// "advisories absent" when the stored verification is structurally invalid
// or an advisory finding's fileKey cannot be resolved to a safe
// candidate-relative path.
common::Result<ManifestVerificationAdvisoryView> buildVerificationAdvisoryView(
    IManifestWorkLedgerRepository& workLedger, const RunId& runId)
{
    const auto entry = workLedger.findEntry(runId);
    if (!entry.success()) {
        return common::Result<ManifestVerificationAdvisoryView>::failure(*entry.error());
    }
    ManifestVerificationAdvisoryView view;
    if (!entry.value().has_value() || !entry.value()->verification.has_value()) {
        return common::Result<ManifestVerificationAdvisoryView>::ok(std::move(view));
    }
    const auto& verification = *entry.value()->verification;
    const auto structurallyValid = validateManifestProposalSetVerification(verification);
    if (!structurallyValid.success()) {
        return common::Result<ManifestVerificationAdvisoryView>::failure(
            {"execution_manifest_read_model.verification_corrupt",
             "This Run's stored combined verification record is structurally invalid"});
    }
    view.verificationAvailable = true;
    view.verificationPassed = requiredAcceptancePassed(verification);
    for (const auto& diagnostic : verification.diagnostics) {
        if (diagnostic.code != "verification.qml_warning_advisory") continue;
        for (const auto& finding : diagnostic.structuredFindings) {
            const auto path = displayQmlLintFindingPath(finding.fileKey);
            if (!path.has_value()) {
                return common::Result<ManifestVerificationAdvisoryView>::failure(
                    {"execution_manifest_read_model.verification_corrupt",
                     "An advisory finding does not contain a safe candidate-relative path"});
            }
            ManifestVerificationAdvisoryFindingView findingView;
            findingView.relativePath = *path;
            findingView.line = finding.line;
            findingView.column = finding.column;
            findingView.tag = finding.tag;
            findingView.text = finding.text;
            findingView.requirementId = diagnostic.requirementId;
            view.advisories.push_back(std::move(findingView));
        }
    }
    view.advisoryCount = view.advisories.size();
    view.hasAdvisories = view.advisoryCount > 0;
    return common::Result<ManifestVerificationAdvisoryView>::ok(std::move(view));
}

// AG-17.6: the routing projection. Everything here is derived from the frozen record
// (already integrity-checked at freeze) and the catalog for display names; nothing is
// re-decided. A record that fails its own integrity check is projected as "none" rather
// than shown half-trusted - the Run itself already failed closed on it.
void populateRouting(
    ExecutionManifestView& view,
    const ExecutionManifest& manifest,
    const IProviderRepository* providers,
    const IModelRepository* models)
{
    if (!manifest.routing.has_value() || ManifestRouting::recordProblem(*manifest.routing)) return;
    const auto& record = *manifest.routing;
    view.routingMode = toString(record.mode);

    std::unordered_map<std::string, std::size_t> taskCounts;
    for (std::size_t index = 0; index < manifest.tasks.size(); ++index) {
        const auto& task = manifest.tasks[index];
        const auto* member = ManifestRouting::assigneeOf(record, task);
        if (member == nullptr) continue;
        auto& taskView = view.tasks[index];
        taskView.assigneeMemberId = member->memberId;
        taskView.assigneeRole = member->role;
        ++taskCounts[member->memberId];
    }

    const auto labelFor = [&](const ManifestRoutingMember& member, std::string& providerName,
                              std::string& modelName) {
        std::optional<Provider> provider;
        std::optional<Model> model;
        if (providers != nullptr) {
            const auto found = providers->findById(member.providerId);
            if (found.success()) provider = found.value();
        }
        if (models != nullptr) {
            const auto found = models->findById(member.modelId);
            if (found.success()) model = found.value();
        }
        providerName = provider.has_value() && !provider->name.empty() ? provider->name : member.providerId;
        modelName = model.has_value()
            ? (model->displayName.empty() ? model->apiModelId : model->displayName)
            : member.modelId;
        if (provider.has_value() && model.has_value()) {
            return manifestRouteModelLabel(*provider, *model);
        }
        return providerName + " / " + modelName;
    };

    view.routingMembers.reserve(record.members.size());
    std::unordered_map<std::string, std::string> labelByMember;
    for (const auto& member : record.members) {
        ManifestRouteMemberView memberView;
        memberView.memberId = member.memberId;
        memberView.role = member.role;
        memberView.providerId = member.providerId;
        memberView.modelId = member.modelId;
        memberView.label = labelFor(member, memberView.providerName, memberView.modelName);
        memberView.lead = member.leadArchitect;
        memberView.workspaceAccess = member.workspaceAccess;
        const auto counted = taskCounts.find(member.memberId);
        memberView.assignedTaskCount = counted == taskCounts.end() ? 0 : counted->second;
        if (member.leadArchitect) {
            memberView.participation = "lead";
        } else if (!member.workspaceAccess) {
            memberView.participation = "not_assignable";
        } else {
            memberView.participation = memberView.assignedTaskCount > 0 ? "assigned" : "unused";
        }
        labelByMember[member.memberId] = memberView.label;
        view.routingMembers.push_back(std::move(memberView));
    }
    for (auto& taskView : view.tasks) {
        if (taskView.assigneeMemberId.empty()) continue;
        taskView.assigneeLabel = labelByMember[taskView.assigneeMemberId];
    }
}

// AG-17.7B: member usage/cost, reconstructed from the Run's ModelExecution rows and the
// frozen routing record (ManifestMemberUsage.hpp). Runs after populateRouting: a Manifest
// with no usable record has routingMode "none" and nothing is attributed. A read failure
// degrades only this section (the rest of the Manifest view is unaffected) and says why.
void populateUsage(
    ExecutionManifestView& view,
    const ExecutionManifest& manifest,
    const RunId& runId,
    const IModelExecutionRepository* modelExecutions)
{
    if (modelExecutions == nullptr || view.routingMode == "none") return;
    const auto rows = modelExecutions->listExecutions(ModelExecutionSource::Run, runId);
    if (!rows.success()) {
        view.usageDiagnostics.push_back("usage.unavailable: " + rows.error()->code);
        return;
    }
    auto projection = projectManifestMemberUsage(manifest, rows.value());
    if (!projection.available) return;
    view.usageAvailable = true;
    view.usagePartitionExact = projection.partitionExact;
    view.usageRunTotal = projection.runTotal;
    view.usageUnattributed = projection.unattributed;
    view.usageUnattributedExecutions = std::move(projection.unattributedExecutions);
    view.usageDiagnostics = std::move(projection.diagnostics);
    for (auto& memberView : view.routingMembers) {
        memberView.usage = projection.totalFor(memberView.memberId);
    }
}

} // namespace

const char* toString(ManifestVerificationEnvironmentIssue value) noexcept
{
    switch (value) {
    case ManifestVerificationEnvironmentIssue::None: return "none";
    case ManifestVerificationEnvironmentIssue::Drifted: return "drifted";
    case ManifestVerificationEnvironmentIssue::ToolUnavailable: return "tool_unavailable";
    }
    return "none";
}

ExecutionManifestReadModelService::ExecutionManifestReadModelService(
    ExecutionManifestService& manifests,
    IManifestTaskExecutionRepository& taskExecutions,
    IRunWorkspaceScopeRepository* workspaceScope,
    IChangeProposalRepository* changeProposals,
    IVerificationEnvironmentRepository* verificationEnvironment,
    IVerificationBootstrapRepository* verificationBootstrap,
    IManifestWorkLedgerRepository* workLedger,
    const IProviderRepository* providers,
    const IModelRepository* models,
    const IModelExecutionRepository* modelExecutions)
    : manifests_(manifests)
    , taskExecutions_(taskExecutions)
    , workspaceScope_(workspaceScope)
    , changeProposals_(changeProposals)
    , verificationEnvironment_(verificationEnvironment)
    , verificationBootstrap_(verificationBootstrap)
    , workLedger_(workLedger)
    , providers_(providers)
    , models_(models)
    , modelExecutions_(modelExecutions)
{
}

common::Result<std::optional<ExecutionManifestView>> ExecutionManifestReadModelService::viewForRun(
    const RunId& runId) const
{
    const auto manifest = manifests_.findForRun(runId);
    if (!manifest.success()) {
        return common::Result<std::optional<ExecutionManifestView>>::failure(*manifest.error());
    }
    if (!manifest.value().has_value()) {
        return common::Result<std::optional<ExecutionManifestView>>::ok(std::nullopt);
    }

    const auto executions = taskExecutions_.listForRun(runId);
    if (!executions.success()) {
        return common::Result<std::optional<ExecutionManifestView>>::failure(*executions.error());
    }
    std::unordered_map<std::string, ManifestTaskState> statesByTaskId;
    for (const auto& record : executions.value()) statesByTaskId[record.taskId] = record.state;

    const auto noChanges = taskExecutions_.listNoChangesForRun(runId);
    if (!noChanges.success()) {
        return common::Result<std::optional<ExecutionManifestView>>::failure(*noChanges.error());
    }
    std::unordered_map<std::string, ManifestWorkerNoChanges> noChangesByTaskId;
    for (const auto& record : noChanges.value()) noChangesByTaskId[record.taskId] = record.evidence;

    // AG-15.3: grouped by taskId once, up front, so the per-task loop below
    // stays a single deterministic pass with no re-querying per task.
    std::unordered_map<std::string, std::vector<ManifestTaskScopeEntryView>> scopeByTaskId;
    bool workspaceScopeAvailable = false;
    std::vector<ManifestScopeDependencyEdgeView> scopeDependencyEdges;
    if (workspaceScope_ != nullptr) {
        const auto scope = workspaceScope_->findForRun(runId);
        if (!scope.success()) {
            return common::Result<std::optional<ExecutionManifestView>>::failure(*scope.error());
        }
        if (scope.value().has_value()) {
            workspaceScopeAvailable = true;
            for (const auto& entry : scope.value()->entries) {
                ManifestTaskScopeEntryView entryView;
                entryView.relativePath = entry.relativePath;
                entryView.role = toString(entry.role);
                entryView.isExpansion = false;
                scopeByTaskId[entry.taskId].push_back(std::move(entryView));
            }
            for (const auto& edge : scope.value()->dependencyEdges) {
                scopeDependencyEdges.push_back(
                    {edge.dependentTaskId, edge.providerTaskId, edge.relativePath});
            }
            const auto expansions = workspaceScope_->listExpansionsForRun(runId);
            if (!expansions.success()) {
                return common::Result<std::optional<ExecutionManifestView>>::failure(
                    *expansions.error());
            }
            for (const auto& expansion : expansions.value()) {
                ManifestTaskScopeEntryView entryView;
                entryView.relativePath = expansion.relativePath;
                entryView.role = toString(expansion.role);
                entryView.isExpansion = true;
                scopeByTaskId[expansion.taskId].push_back(std::move(entryView));
            }
        }
    }

    ExecutionManifestView view;
    view.runId = runId;
    view.requestedMaxParallelism = manifest.value()->requestedMaxParallelism;
    view.workspaceScopeAvailable = workspaceScopeAvailable;
    view.scopeDependencyEdges = std::move(scopeDependencyEdges);
    view.tasks.reserve(manifest.value()->tasks.size());
    for (const auto& task : manifest.value()->tasks) {
        ManifestTaskView taskView;
        taskView.id = task.id;
        taskView.objective = task.objective;
        taskView.role = task.role;
        taskView.dependencies = task.dependencies;
        for (const auto& path : task.ownedPaths) taskView.ownedPaths.push_back(detail::genericUtf8(path));
        taskView.acceptanceCriteria = task.acceptanceCriteria;
        taskView.acceptanceContract = task.acceptanceContract;
        taskView.contextReferences = task.contextReferences;
        taskView.requestedCapability = task.requestedCapability;

        const auto scopeIt = scopeByTaskId.find(task.id);
        if (scopeIt != scopeByTaskId.end()) taskView.scopeEntries = scopeIt->second;

        const auto stateIt = statesByTaskId.find(task.id);
        if (stateIt != statesByTaskId.end()) {
            taskView.status = stateIt->second.status;
            taskView.errorCode = stateIt->second.errorCode;
            taskView.errorMessage = stateIt->second.errorMessage;
            taskView.changeSetId = stateIt->second.changeSetId;
            taskView.attempts = stateIt->second.attempts;
            taskView.outputSummary = stateIt->second.outputSummary;
            taskView.inspectedPaths = stateIt->second.inspectedRelativePaths;

            if (changeProposals_ != nullptr && stateIt->second.changeSetId.has_value()) {
                const auto changes = changeProposals_->listChanges(*stateIt->second.changeSetId);
                if (!changes.success()) {
                    return common::Result<std::optional<ExecutionManifestView>>::failure(
                        *changes.error());
                }
                for (const auto& change : changes.value()) {
                    if (change.operation == FileChangeOperation::Create) {
                        taskView.proposedCreatedPaths.push_back(detail::genericUtf8(change.relativePath));
                    } else {
                        taskView.proposedModifiedPaths.push_back(detail::genericUtf8(change.relativePath));
                    }
                }
            }
        }
        const auto noChangesIt = noChangesByTaskId.find(task.id);
        if (noChangesIt != noChangesByTaskId.end()) {
            taskView.workerNoChanges = noChangesIt->second;
        }
        view.tasks.push_back(std::move(taskView));
    }

    populateRouting(view, *manifest.value(), providers_, models_);
    populateUsage(view, *manifest.value(), runId, modelExecutions_);
    if (workLedger_ != nullptr) {
        auto advisory = buildVerificationAdvisoryView(*workLedger_, runId);
        if (!advisory.success()) {
            return common::Result<std::optional<ExecutionManifestView>>::failure(
                *advisory.error());
        }
        view.verificationAdvisory = std::move(advisory.value());
    }

    return common::Result<std::optional<ExecutionManifestView>>::ok(std::move(view));
}

common::Result<std::optional<ManifestVerificationEnvironmentView>>
ExecutionManifestReadModelService::verificationEnvironmentViewForRun(const RunId& runId) const
{
    if (verificationEnvironment_ == nullptr) {
        return common::Result<std::optional<ManifestVerificationEnvironmentView>>::ok(std::nullopt);
    }
    const auto snapshotResult = verificationEnvironment_->findForRun(runId);
    if (!snapshotResult.success()) {
        return common::Result<std::optional<ManifestVerificationEnvironmentView>>::failure(
            *snapshotResult.error());
    }
    if (!snapshotResult.value().has_value()) {
        return common::Result<std::optional<ManifestVerificationEnvironmentView>>::ok(std::nullopt);
    }
    const auto& snapshot = *snapshotResult.value();

    std::optional<VerificationBootstrapDecision> decision;
    std::optional<VerificationBootstrapTransition> resolvedTransition;
    if (verificationBootstrap_ != nullptr) {
        const auto decisionResult = verificationBootstrap_->findDecisionForRun(runId);
        if (!decisionResult.success()) {
            return common::Result<std::optional<ManifestVerificationEnvironmentView>>::failure(
                *decisionResult.error());
        }
        decision = decisionResult.value();
        if (decision.has_value()) {
            // AG-18.4A hardening: a durable bootstrap decision is only ever
            // trustworthy for THIS Run's THIS snapshot - never trusted in
            // isolation. Any identity mismatch means the two durable rows
            // disagree about which Run/project/baseline/discovery-schema
            // they belong to, which can only mean corruption (a mis-keyed
            // row, a bug, or direct tampering) - fail closed rather than
            // silently projecting a decision that may not actually describe
            // this snapshot.
            if (decision->runId != runId || decision->projectId != snapshot.projectId
                || decision->baselineFingerprint != snapshot.baselineFingerprint
                || decision->verificationSnapshotSchemaVersion != snapshot.schemaVersion) {
                return common::Result<std::optional<ManifestVerificationEnvironmentView>>::failure(
                    {"verification_environment.corrupt_bootstrap_record",
                     "The durable bootstrap decision's own identity does not match this Run's "
                     "verification-environment snapshot"});
            }
            const auto transitionsResult = verificationBootstrap_->listTransitionsForRun(runId);
            if (!transitionsResult.success()) {
                return common::Result<std::optional<ManifestVerificationEnvironmentView>>::failure(
                    *transitionsResult.error());
            }
            // AG-18.4A hardening: every transition must belong to this Run,
            // and at most one may be resolving (BootstrapVerified/
            // BootstrapFailed) - a second resolving transition for the same
            // Run is never legitimate (see VerificationBootstrapTransition's
            // own "append-only progress record" contract), so this never
            // silently picks the first one out of a corrupt set.
            std::size_t resolvingCount = 0;
            for (const auto& transition : transitionsResult.value()) {
                if (transition.runId != runId) {
                    return common::Result<std::optional<ManifestVerificationEnvironmentView>>::
                        failure({"verification_environment.corrupt_bootstrap_record",
                            "A bootstrap transition's own runId does not match this Run"});
                }
                if (transition.toDisposition == VerificationBootstrapDisposition::BootstrapVerified
                    || transition.toDisposition
                        == VerificationBootstrapDisposition::BootstrapFailed) {
                    ++resolvingCount;
                    resolvedTransition = transition;
                }
            }
            if (resolvingCount > 1) {
                return common::Result<std::optional<ManifestVerificationEnvironmentView>>::failure(
                    {"verification_environment.corrupt_bootstrap_record",
                     "More than one resolving bootstrap transition exists for this Run"});
            }
        }
    }

    ManifestVerificationEnvironmentView view;
    view.state = toString(reclassifyState(snapshot, decision));
    view.issue = toString(classifyIssue(snapshot));
    view.diagnostic = snapshot.diagnostic;
    view.profiles.reserve(snapshot.profiles.size());
    for (const auto& profile : snapshot.profiles) {
        ManifestVerificationProfileView profileView;
        profileView.profileId = profile.profileId;
        profileView.configurePresetName = profile.configurePresetName;
        profileView.configureSource = toString(profile.configureSource);
        profileView.resolvedBinaryDir = profile.resolvedBinaryDir;
        profileView.buildPresetsConfirmed = profile.buildPresetsConfirmed;
        profileView.buildPresets.reserve(profile.buildPresets.size());
        for (const auto& buildPreset : profile.buildPresets) {
            profileView.buildPresets.push_back(
                {buildPreset.name, buildPreset.allowedTargetsConfirmed, buildPreset.allowedTargets});
        }
        view.profiles.push_back(std::move(profileView));
    }

    if (decision.has_value()) {
        view.hasBootstrapDecision = true;
        view.bootstrapDisposition = toString(decision->disposition);
        view.bootstrapReasonCode = decision->reasonCode;
        view.bootstrapMessage = decision->message;
        view.bootstrapSetupPaths.reserve(decision->bootstrapSetupPaths.size());
        for (const auto& path : decision->bootstrapSetupPaths) {
            view.bootstrapSetupPaths.push_back(detail::genericUtf8(path));
        }
        if (resolvedTransition.has_value()) {
            view.hasBootstrapTransition = true;
            view.bootstrapTransitionDisposition = toString(resolvedTransition->toDisposition);
            if (resolvedTransition->candidateIdentity.has_value()) {
                view.bootstrapCandidateProfileId =
                    resolvedTransition->candidateIdentity->candidateProfileId;
                view.bootstrapCandidateBuildProfileId =
                    resolvedTransition->candidateIdentity->candidateBuildProfileId;
                view.bootstrapCandidateTargetId =
                    resolvedTransition->candidateIdentity->candidateTargetId;
            }
        }
    }

    populateRetryAfterSetupEligibility(view, snapshot, decision);

    // AG-18.4B now supports explicit preset setup while creating a Project,
    // but editing/configuring an already-existing Project from Run Details
    // remains a separate unimplemented surface. Keep the action visible and
    // typed-disabled rather than pretending the creation-time flow is
    // available here.
    view.canConfigureBackendProfile = false;
    view.canConfigureBackendProfileReasonCode =
        "verification_environment.profile_configuration_unavailable";
    view.canConfigureBackendProfileReason =
        "Verification profile configuration for an existing Project is not available "
        "from Run Details yet. Create or restore a project/user preset externally, "
        "then choose Retry after setup.";

    return common::Result<std::optional<ManifestVerificationEnvironmentView>>::ok(std::move(view));
}

} // namespace ai::application
