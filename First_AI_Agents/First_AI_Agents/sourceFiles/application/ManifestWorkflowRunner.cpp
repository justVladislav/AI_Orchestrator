#include "ai/application/ManifestWorkflowRunner.hpp"

#include "ai/application/artifacts/ArtifactService.hpp"
#include "ai/application/CostCalculation.hpp"
#include "ai/application/ExecutionManifestDraftTypes.hpp"
#include "ai/application/ExecutionManifestExecutionCapacity.hpp"
#include "ai/application/ExecutionManifestPlannerTurnBudget.hpp"
#include "ai/application/IdGenerator.hpp"
#include "ai/application/agents/ModelExecutionService.hpp"
#include "ai/application/ManifestMandatoryVerificationPolicy.hpp"
#include "ai/application/ManifestRouting.hpp"
#include "ai/application/ManifestVerificationProfileBinder.hpp"
#include "ai/application/TaskRouteResolver.hpp"
#include "ai/application/QmlLintDiagnosticPolicy.hpp"
#include "ai/application/ProjectReadinessService.hpp"
#include "ai/application/RunWorkspaceScopeService.hpp"
#include "ai/application/Sha256.hpp"
#include "ai/application/VerificationEnvironmentDiscoveryService.hpp"
#include "ai/application/WorkspaceBaselineProvenance.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <limits>
#include <sstream>
#include <utility>

namespace ai::application {

namespace {

// Real-Run incident (Run 683efe84-e9cc-4e68-a460-39da61f5125c, 2026-09-11):
// this cap used to be 512 bytes, far below ManifestProposalSetVerifier's own
// carefully bounded (2048-byte) leak-safe diagnostic excerpt - so a Run's
// terminal error_message / Work Ledger terminal_reason was silently cut off
// mid-word, hiding whether the underlying warnings were actually resolved,
// with no untruncated copy anywhere else in the database. Sized to
// comfortably contain the full "Manifest verification failed: [<code>] "
// prefix plus a complete up-to-kMaximumExcerptBytes-sized diagnostic excerpt
// (see ManifestProposalSetVerifier.cpp) without re-truncating it.
constexpr std::size_t kMaximumTerminalFailureBytes = 2304;
// Mirrors ManifestProposalSetVerifier.cpp's kExcerptTruncationMarker: bounded,
// leak-safe (no raw content), appended only when this cap actually cut
// something, so its presence alone reliably tells a reader that further
// content was omitted rather than the message simply ending there.
constexpr std::string_view kTerminalMessageTruncationMarker =
    " [truncated: see verification record for the complete diagnostic]";
// Stage 8B Run 969e35fa...: how many blocking findings (file:line:column text) the terminal message
// and, per diagnostic, the verification report name; the complete set stays in the identity JSON.
constexpr std::size_t kTerminalFindingExcerptCount = 3;
constexpr std::size_t kReportFindingsPerDiagnostic = 8;

std::string normalizePlanningText(std::string_view text)
{
    std::string normalized;
    normalized.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') continue;
            normalized.push_back('\n');
        } else {
            normalized.push_back(text[i]);
        }
    }
    const auto nonSpace = [](unsigned char ch) { return std::isspace(ch) == 0; };
    const auto first = std::find_if(normalized.begin(), normalized.end(), nonSpace);
    const auto last = std::find_if(normalized.rbegin(), normalized.rend(), nonSpace).base();
    return first < last ? std::string(first, last) : std::string {};
}

void appendFingerprintField(
    std::ostringstream& material, std::string_view name, std::string_view value)
{
    material << name.size() << ':' << name << '=' << value.size() << ':' << value << '\n';
}

std::string planningInputFingerprintImpl(
    const WorkflowRunContext& context,
    const StageModelAssignment& assignment,
    const Provider& provider,
    const Model& model,
    const WorkspaceBaselineSnapshot& baseline,
    const std::optional<VerificationEnvironmentSnapshot>& verificationEnvironment,
    std::string_view capacityPolicyIdentity)
{
    std::ostringstream material;
    appendFingerprintField(material, "contract", "manifest-planning-input.v1");
    appendFingerprintField(material, "draft-schema", kExecutionManifestDraftSchemaVersion);
    appendFingerprintField(material, "ledger-context-schema", kManifestWorkLedgerSchemaVersion);
    appendFingerprintField(material, "baseline", baseline.fingerprint);
    appendFingerprintField(material, "project-description",
        normalizePlanningText(context.project.description));
    appendFingerprintField(material, "run-task", normalizePlanningText(context.run.task));
    appendFingerprintField(material, "workflow", context.run.workflowId);
    appendFingerprintField(material, "profile", context.run.executionProfileId.value_or(""));
    appendFingerprintField(material, "completion-policy",
        toString(context.run.planningCompletionPolicy));
    appendFingerprintField(material, "max-parallelism",
        std::to_string(context.run.maxParallelism));
    appendFingerprintField(material, "provider-id", provider.id);
    appendFingerprintField(material, "provider-kind", provider.kind);
    appendFingerprintField(material, "provider-base-url", provider.baseUrl);
    appendFingerprintField(material, "model-id", model.id);
    appendFingerprintField(material, "api-model-id", model.apiModelId);
    appendFingerprintField(material, "assignment-stage", assignment.stageType);
    appendFingerprintField(material, "assignment-agent", assignment.agentName);
    appendFingerprintField(material, "assignment-role", assignment.role);
    appendFingerprintField(material, "assignment-instructions",
        normalizePlanningText(assignment.instructions));
    appendFingerprintField(material, "assignment-timeout-ms",
        std::to_string(assignment.timeout.count()));
    appendFingerprintField(material, "assignment-position", std::to_string(assignment.position));
    appendFingerprintField(material, "assignment-lead", assignment.leadArchitect ? "1" : "0");
    appendFingerprintField(material, "assignment-workspace", assignment.workspaceAccess ? "1" : "0");
    appendFingerprintField(material, "assignment-attempts", std::to_string(assignment.maxAttempts));
    appendFingerprintField(material, "fallback-provider",
        assignment.fallbackProviderId.value_or(""));
    appendFingerprintField(material, "fallback-model", assignment.fallbackModelId.value_or(""));
    for (const auto& dependency : assignment.dependencies) {
        appendFingerprintField(material, "assignment-dependency", dependency);
    }
    const auto& budget = context.run.providerBudget;
    appendFingerprintField(material, "budget-schema", budget.schemaVersion);
    appendFingerprintField(material, "budget-input", std::to_string(budget.maxInputTokens));
    appendFingerprintField(material, "budget-output", std::to_string(budget.maxOutputTokens));
    appendFingerprintField(material, "budget-turns", std::to_string(budget.maxTurns));
    // A pinned turn limit and an automatically extendable one admit different
    // Manifests under the same number, so the mode is part of planning identity.
    appendFingerprintField(material, "budget-turns-auto", budget.autoExtendTurns ? "1" : "0");
    // Package 1 (2026-09-28 planner-terminal-budget-hardening): the
    // partition formula is a pure function of budget.maxTurns alone, but its
    // POLICY ID must still participate in planning identity/replay so that a
    // future change to the formula OR its forced-terminal wire semantics is
    // never silently treated as an unchanged-input retry of a prior failure
    // under the old policy. This is not a new mutable side channel - the id is a
    // Backend-owned compile-time constant, identical for every Run on this
    // build.
    appendFingerprintField(material, "planner-turn-budget-partition-policy",
        kPlannerTurnBudgetPartitionPolicyId);
    // Worker capacity fairness: the theoretical admission id AND the
    // conservative reservation policy id (executionCapacityPolicyIdentity())
    // decide whether an otherwise identical Run is admitted, so both must be
    // part of planning identity - a Retry after the reservation policy was
    // introduced or changed is a different input, never an unchanged replay
    // of a failure recorded under the older execution-capacity semantics.
    appendFingerprintField(material, "execution-capacity-policy", capacityPolicyIdentity);
    appendFingerprintField(material, "budget-tool-calls", std::to_string(budget.maxToolCalls));
    appendFingerprintField(material, "budget-tool-bytes", std::to_string(budget.maxToolResultBytes));
    appendFingerprintField(material, "budget-wall-ms", std::to_string(budget.maxWallTimeMs));
    appendFingerprintField(material, "budget-request-ms", std::to_string(budget.maxRequestTimeMs));
    if (verificationEnvironment.has_value()) {
        appendFingerprintField(material, "verification-schema", verificationEnvironment->schemaVersion);
        appendFingerprintField(material, "verification-project", verificationEnvironment->projectId);
        appendFingerprintField(material, "verification-baseline",
            verificationEnvironment->baselineFingerprint);
        appendFingerprintField(material, "verification-catalog-sha256", sha256Hex(
            ManifestVerificationProfileBinder::renderTrustedCatalog(*verificationEnvironment)));
    } else {
        appendFingerprintField(material, "verification-schema", "unavailable");
    }
    // AG-17.0: the mandatory-verification policy id plus the snapshot facts
    // that decide its outcome (per-preset target-catalog confirmation and
    // completeness, which the rendered catalog above does not carry). A
    // changed policy or catalog is a different input, never an unchanged
    // replay of a failure recorded under the older policy.
    appendFingerprintField(material, "mandatory-verification-policy",
        ManifestMandatoryVerificationPolicy::fingerprintMaterial(
            verificationEnvironment.has_value() ? &*verificationEnvironment : nullptr));
    // AG-17.4: a roster Run's planning input is the whole frozen roster, not just
    // the lead: who the workers are (provider, model, role, workspace capability,
    // exact instructions, position, timeout) decides what the Planner is told and
    // may assign. The routing record's fingerprint covers all of that, and the
    // roster contract id versions the prompt section and schema variant that
    // present it. Appended ONLY for a roster, so a single-assignment Run's
    // fingerprint - part of every persisted Retry/replay decision - is byte-for-byte
    // what it was before AG-17.4.
    if (context.run.assignmentSnapshot.size() > 1) {
        const auto routing = ManifestRouting::fromAssignments(context.run.assignmentSnapshot);
        appendFingerprintField(material, "planner-roster-contract", kPlannerRosterContractId);
        appendFingerprintField(material, "routing-mode", toString(routing.mode));
        appendFingerprintField(material, "routing-fingerprint", routing.fingerprint);
    }
    // PC-2: the Run author's interface contracts are Planner AND worker input, so a changed contract is a
    // different planning input, never an unchanged replay of an earlier failure. Appended ONLY when there
    // are contracts (an empty list has an empty fingerprint), so every Run without them keeps exactly the
    // planning identity it had before PC-2.
    if (const auto contractsFingerprint = runInterfaceContractsFingerprint(context.run.interfaceContracts);
        !contractsFingerprint.empty()) {
        appendFingerprintField(material, "interface-contracts", contractsFingerprint);
    }
    return sha256Hex(material.str());
}

std::string planningFailureSignature(const ExecutionManifestPlannerOutcome& outcome)
{
    std::vector<std::string> findingIdentities;
    const auto collect = [&](std::string_view phase,
                             const std::vector<ExecutionManifestFinding>& findings) {
        for (const auto& finding : findings) {
            findingIdentities.push_back(std::string(phase) + "\0" + finding.code + "\0"
                + finding.taskId.value_or(""));
        }
    };
    collect("primary", outcome.primaryFindings);
    collect("final", outcome.lastFindings);
    std::sort(findingIdentities.begin(), findingIdentities.end());
    std::ostringstream material;
    appendFingerprintField(material, "contract", "manifest-planning-failure.v1");
    appendFingerprintField(material, "outcome", outcome.errorCode);
    appendFingerprintField(material, "primary", outcome.primaryRejectionCode);
    appendFingerprintField(material, "final", outcome.lastRejectionCode);
    for (const auto& identity : findingIdentities) {
        appendFingerprintField(material, "finding", identity);
    }
    return sha256Hex(material.str());
}

RunEvent stageEvent(
    RunEventType type, const RunId& runId, const StageId& stageId,
    double runProgress, std::string message)
{
    RunEvent event;
    event.type = type;
    event.runId = runId;
    event.stageId = stageId;
    event.runProgress = runProgress;
    event.message = std::move(message);
    return event;
}

std::string renderDeterministicSummary(
    const ExecutionManifest& manifest,
    const ExecutionManifestExecutionOutcome& outcome)
{
    std::ostringstream out;
    out << "# Manifest execution summary\n\n";
    out << "Tasks: " << manifest.tasks.size() << "\n";
    out << "Tool turns: " << outcome.totalTurns
        << ", tool calls: " << outcome.totalToolCalls
        << ", successful inspections: " << outcome.successfulInspectionCalls
        << ", bytes returned: " << outcome.totalReturnedBytes << "\n\n";
    out << "## Worker task outcomes\n\n";
    out << "A task status reports worker termination only. It is not build, lint, "
           "test, or acceptance evidence; the authoritative Backend verification "
           "section below decides those claims.\n\n";
    for (const auto& task : manifest.tasks) {
        const auto it = outcome.taskStates.find(task.id);
        out << "- " << task.id << " (" << task.objective << "): ";
        if (it == outcome.taskStates.end()) {
            out << "not scheduled\n";
            continue;
        }
        out << toString(it->second.status);
        if (it->second.status == ManifestTaskStatus::Failed
                || it->second.status == ManifestTaskStatus::Skipped) {
            out << " - " << it->second.errorCode;
            if (!it->second.errorMessage.empty()) out << ": " << it->second.errorMessage;
        }
        if (it->second.changeSetId.has_value()) {
            out << " [proposal " << *it->second.changeSetId << "]";
        }
        out << "\n";
    }
    return out.str();
}

std::string boundedTerminalMessage(std::string value)
{
    if (value.size() > kMaximumTerminalFailureBytes) {
        const std::size_t markerSize = kTerminalMessageTruncationMarker.size();
        const std::size_t keep =
            kMaximumTerminalFailureBytes > markerSize
                ? kMaximumTerminalFailureBytes - markerSize : 0;
        value.resize(keep);
        value += std::string(kTerminalMessageTruncationMarker);
    }
    return value;
}

std::string executionFailureMessage(
    const ExecutionManifestExecutionOutcome& outcome)
{
    if (!outcome.executionErrorCode.empty()) {
        return boundedTerminalMessage("Manifest execution failed: ["
            + outcome.executionErrorCode + "] " + outcome.executionErrorMessage);
    }
    if (outcome.proposalSetVerification.has_value()) {
        const auto& verification = *outcome.proposalSetVerification;
        if (const auto* diagnostic = primaryFailureDiagnostic(verification)) {
            // A baseline-aware lint diagnostic carries a fixed message and keeps its file/line/text in
            // the structured findings: the reason names the blocking ones (bounded).
            return boundedTerminalMessage("Manifest verification failed: "
                + renderVerificationFailureReason(*diagnostic, kTerminalFindingExcerptCount));
        }
    }
    if (!outcome.verificationErrorCode.empty()) {
        std::string message = "Manifest verification failed: ["
            + outcome.verificationErrorCode + "]";
        if (!outcome.verificationErrorMessage.empty()) {
            message += " " + outcome.verificationErrorMessage;
        }
        return boundedTerminalMessage(std::move(message));
    }
    std::optional<std::pair<std::string, std::string>> firstFailedTask;
    for (const auto& [taskId, state] : outcome.taskStates) {
        if (state.status == ManifestTaskStatus::Failed
            && !state.errorCode.empty()) {
            const std::pair<std::string, std::string> candidate {
                taskId, state.errorCode};
            if (!firstFailedTask.has_value() || candidate < *firstFailedTask) {
                firstFailedTask = candidate;
            }
        }
    }
    if (firstFailedTask.has_value()) {
        return boundedTerminalMessage(
            "Manifest task failed: [" + firstFailedTask->second + "]");
    }
    return "Manifest execution finished with failures";
}

std::string renderIntegrationAdvice(std::string_view content)
{
    std::ostringstream out;
    out << "# Architect integration advice\n\n";
    out << "Disposition: non_authoritative_recommendation\n\n";
    out << "This model-authored advice cannot override Backend verification, "
           "task state, or the Run's terminal disposition.\n\n";
    out << "## Advice\n\n" << content << "\n";
    return out.str();
}

std::string renderVerificationSection(
    const ManifestProposalSetVerification& verification)
{
    std::ostringstream out;
    out << "Schema: " << verification.schemaVersion << "\n";
    out << "Workspace identity: " << verification.workspaceIdentity << "\n";
    out << "Baseline fingerprint: " << verification.baselineFingerprint << "\n";
    out << "Overlay fingerprint: " << verification.overlayFingerprint << "\n\n";

    out << "## Ordered proposal set\n\n";
    if (verification.orderedProposalIds.empty()) {
        out << "- No change proposals\n";
    } else {
        for (std::size_t index = 0; index < verification.orderedProposalIds.size(); ++index) {
            out << "- " << verification.orderedProposalIds[index];
            if (index < verification.orderedProposalContentHashes.size()) {
                out << " — " << verification.orderedProposalContentHashes[index];
            }
            out << "\n";
        }
    }

    out << "\n## Proposal safety and schema validation\n\nStatus: "
        << toString(verification.proposalSafetyStatus) << "\n";
    out << "\n## Combined build, lint, and test verification\n\nStatus: "
        << toString(verification.buildLintTestStatus) << "\n";
    out << "\n## Acceptance-criteria verification\n\nStatus: "
        << toString(verification.acceptanceStatus) << "\n";
    for (const auto& criterion : verification.criteria) {
        out << "- " << criterion.requirementId << " ["
            << (criterion.required ? "required" : "optional") << "]: "
            << toString(criterion.status) << "\n";
    }

    out << "\n## Backend verification checks\n\n";
    for (const auto& check : verification.checks) {
        out << "- " << check.profileId << " / " << toString(check.kind)
            << ": " << toString(check.status)
            << " (" << check.commandIdentity << ")\n";
    }
    if (!verification.diagnostics.empty()) {
        out << "\n## Diagnostics\n\n"
            << renderVerificationDiagnosticLines(
                   verification.diagnostics, kReportFindingsPerDiagnostic);
    }
    return out.str();
}

std::string renderVerificationReport(
    const ExecutionManifestExecutionOutcome& outcome)
{
    std::ostringstream out;
    out << "# Manifest proposal-set verification\n\n";
    if (!outcome.proposalSetVerification.has_value()) {
        out << "Disposition: not_run\n\n";
        out << "## Proposal safety and schema validation\n\nStatus: not_run\n\n";
        out << "## Combined build, lint, and test verification\n\nStatus: not_run\n\n";
        out << "## Acceptance-criteria verification\n\nStatus: not_run\n";
        if (!outcome.executionErrorCode.empty()) {
            out << "\nExecution admission error: "
                << outcome.executionErrorCode << "\n";
        } else if (!outcome.verificationErrorCode.empty()) {
            out << "\nVerification error: " << outcome.verificationErrorCode << "\n";
        }
        return out.str();
    }

    // Manifest verification reliability hardening: when a bounded
    // verification-repair wave ran, the report explicitly distinguishes
    // worker terminal completion (rendered separately, see
    // renderDeterministicSummary), primary combined verification, the
    // bounded repair itself, and final combined verification - never
    // silently presenting only the final result as if no repair happened,
    // and never overwriting or dropping the primary findings.
    if (outcome.verificationRepairAttempted
        && outcome.primaryProposalSetVerification.has_value()) {
        out << "Disposition: one_bounded_verification_repair_wave\n\n";
        out << "## Bounded verification repair\n\n";
        out << "Exactly one repair wave ran after the primary combined verification "
               "failed, covering exactly these tasks:\n";
        for (const auto& taskId : outcome.verificationRepairedTaskIds) {
            out << "- " << taskId << "\n";
        }
        out << "\n### Primary combined verification (before repair)\n\n";
        out << renderVerificationSection(*outcome.primaryProposalSetVerification);
        out << "\n### Final combined verification (after repair)\n\n";
        out << renderVerificationSection(*outcome.proposalSetVerification);
        return out.str();
    }

    out << renderVerificationSection(*outcome.proposalSetVerification);
    return out.str();
}

std::string renderWorkspaceProvenanceReport(
    const WorkspaceBaselineSnapshot& baseline,
    const std::optional<WorkspaceBaselineDifference>& difference,
    const WorkspaceCMakePreflightResult& preflight,
    const std::vector<ChangeSetId>& orderedProposalIds,
    std::string_view errorCode = {})
{
    std::ostringstream out;
    out << "# Workspace provenance\n\n";
    out << "Schema: workspace-provenance.v1\n";
    out << "Baseline fingerprint: " << baseline.fingerprint << "\n";
    out << "Repository revision available: "
        << (baseline.repositoryRevisionAvailable ? "yes" : "no") << "\n";
    if (baseline.repositoryRevision.has_value())
        out << "Repository revision: " << *baseline.repositoryRevision << "\n";
    out << "CMake preflight configured: "
        << (preflight.configured ? "yes" : "no") << "\n";
    for (const auto& id : preflight.passedCheckIds)
        out << "- Preflight passed: " << id << "\n";
    out << "\n## Current workspace comparison\n\n";
    if (!difference.has_value()) {
        out << "Classification: not_verifiable\n";
        if (!errorCode.empty()) out << "Error: " << errorCode << "\n";
    } else {
        out << "Classification: " << difference->classification << "\n";
        out << "Current fingerprint: " << difference->currentFingerprint << "\n";
        auto paths = [&](std::string_view label, const std::vector<std::string>& values) {
            for (const auto& path : values)
                out << "- " << label << ": " << path << "\n";
        };
        paths("added", difference->addedPaths);
        paths("modified", difference->modifiedPaths);
        paths("deleted", difference->deletedPaths);
    }
    out << "\n## Ordered proposal set\n\n";
    if (orderedProposalIds.empty()) out << "- No change proposals\n";
    else for (const auto& id : orderedProposalIds) out << "- " << id << "\n";
    out << "\nApplied state: not_applied_by_run\n";
    out << "Proposal approval/application remains a separate human action.\n";
    return out.str();
}

// AG-18.2 Section 2: a small, durable record of which verification-
// environment catalog a given Planner attempt actually saw - schema
// version, identity, and the *advertised* profile/build-profile/target ids
// (what was on offer), never the provider's raw submission or the frozen
// Manifest's own selections (those are already durable on the frozen
// requirements themselves via verificationProfileId/etc.). Written once,
// best-effort, immediately after a manifest is confirmed frozen.
std::string renderVerificationEnvironmentProvenance(
    const VerificationEnvironmentSnapshot& snapshot)
{
    std::ostringstream out;
    out << "# Verification environment provenance\n\n";
    out << "Schema: " << snapshot.schemaVersion << "\n";
    out << "Run: " << snapshot.runId << "\n";
    out << "Project: " << snapshot.projectId << "\n";
    out << "Baseline fingerprint: " << snapshot.baselineFingerprint << "\n";
    out << "State: " << toString(snapshot.state) << "\n\n";
    out << "## Advertised profiles\n\n";
    if (snapshot.profiles.empty()) out << "- No selectable profile was advertised\n";
    for (const auto& profile : snapshot.profiles) {
        out << "- profileId: " << profile.profileId
            << ", buildPresetsConfirmed: " << (profile.buildPresetsConfirmed ? "true" : "false")
            << "\n";
        for (const auto& buildPreset : profile.buildPresets) {
            out << "  - buildProfileId: " << buildPreset.name
                << ", allowedTargetsConfirmed: "
                << (buildPreset.allowedTargetsConfirmed ? "true" : "false") << "\n";
        }
    }
    return out.str();
}

std::string renderNoChangesSummary(
    const PlannerNoChangesNeeded& noChanges,
    const ExecutionManifestPlannerOutcome& outcome)
{
    std::ostringstream out;
    out << "# Manifest planning summary\n\n";
    out << "Disposition: no_changes_needed\n";
    out << "Summary: " << noChanges.summary << "\n";
    out << "Inspection calls: " << outcome.successfulInspectionCalls
        << ", tool turns: " << outcome.totalTurns
        << ", tool calls: " << outcome.totalToolCalls
        << ", bytes returned: " << outcome.totalReturnedBytes << "\n";
    if (!outcome.primaryRejectionCode.empty()) {
        out << "Primary rejection repaired: " << outcome.primaryRejectionCode << "\n";
    }
    if (!outcome.lastRejectionCode.empty()
            && outcome.lastRejectionCode != outcome.primaryRejectionCode) {
        out << "Last rejection repaired: " << outcome.lastRejectionCode << "\n";
    }
    const auto appendFindings = [&out](
        std::string_view heading,
        const std::vector<ExecutionManifestFinding>& findings) {
        if (findings.empty()) return;
        out << "\n## " << heading << "\n\n";
        for (const auto& finding : findings) {
            out << "- [" << finding.code << "] " << finding.message;
            if (finding.taskId.has_value()) out << " (task " << *finding.taskId << ")";
            out << "\n";
        }
    };
    appendFindings("Primary rejection findings", outcome.primaryFindings);
    if (!outcome.lastFindings.empty()) {
        appendFindings("Final rejection findings", outcome.lastFindings);
    }
    out << "\n## Completion evidence\n\n";
    for (const auto& item : noChanges.evidence) {
        out << "- " << item.criterion << "\n"
            << "  - Path: " << item.path << "\n"
            << "  - Observation: " << item.observation << "\n";
    }
    if (noChanges.recommendedNextStep.has_value()) {
        out << "\n## Recommended next step\n\n"
            << *noChanges.recommendedNextStep
            << "\n\nThis is a recommendation only; no additional stage was started.\n";
    }
    return out.str();
}

std::string plannerFailureMessage(const ExecutionManifestPlannerOutcome& outcome)
{
    std::ostringstream out;
    out << (outcome.errorCode.empty() ? "planner.failed" : outcome.errorCode);
    if (!outcome.primaryRejectionCode.empty()) {
        out << " (primary: " << outcome.primaryRejectionCode;
        if (!outcome.lastRejectionCode.empty()) {
            out << ", final: " << outcome.lastRejectionCode;
        }
        out << ")";
    }
    if (!outcome.errorMessage.empty()) out << ": " << outcome.errorMessage;
    return out.str();
}

std::string renderPlannerDiagnostics(const ExecutionManifestPlannerOutcome& outcome)
{
    // Real-Run observation (Run 1daf0451-c632-4465-8f43-0f8b8e6b3828,
    // 2026-09-14, real Stage 2B on OpenAI, first genuinely green acceptance
    // Run after MW-P3A closed): this renderer is called on BOTH the failure
    // path and the success-after-repair path (ManifestWorkflowRunner::run()
    // persists this report whenever primaryRejectionCode is non-empty, even
    // for an accepted Manifest, as an audit trail of what the repair fixed).
    // outcome.lastRejectionCode/lastFindings are only ever overwritten when a
    // later evaluated submission is ITSELF rejected (see
    // ExecutionManifestPlanner.cpp's two `if (!candidate.success())` sites) -
    // a successful repair never clears them, so they silently retain the
    // primary attempt's stale rejection. Unconditionally labeling that
    // "Final rejection" implied the accepted Manifest was still rejected.
    // Gate the label on outcome.success instead of trusting lastFindings'
    // emptiness alone.
    std::ostringstream out;
    out << "# Manifest planner diagnostics\n\n"
        << "Result: "
        << (!outcome.success ? "failed"
            : outcome.primaryRejectionCode.empty() ? "manifest_ready"
                                                   : "manifest_ready_after_repair") << "\n"
        << "Error code: " << outcome.errorCode << "\n"
        << "Error message: " << outcome.errorMessage << "\n"
        << "Primary rejection code: "
        << (outcome.primaryRejectionCode.empty() ? "none" : outcome.primaryRejectionCode)
        << "\n";
    if (outcome.success && outcome.primaryRejectionCode.empty()) {
        out << "Final rejection code: none - the Manifest was accepted on the first "
               "submission\n";
    } else if (outcome.success) {
        out << "Final rejection code: none - the repair resolved every primary finding "
               "and the Manifest was accepted\n";
    } else {
        out << "Final rejection code: "
            << (outcome.lastRejectionCode.empty() ? "none" : outcome.lastRejectionCode)
            << "\n";
    }
    out << "Repair attempts: " << outcome.repairAttempts
        << "\nInspection calls: " << outcome.successfulInspectionCalls
        << "\nTool turns: " << outcome.totalTurns
        << "\nTool calls: " << outcome.totalToolCalls
        << "\nBytes returned: " << outcome.totalReturnedBytes << "\n";

    const auto appendFindings = [&out](
        std::string_view heading,
        const std::vector<ExecutionManifestFinding>& findings) {
        out << "\n## " << heading << "\n\n";
        if (findings.empty()) {
            out << "No Backend validation findings were produced for this attempt.\n";
            return;
        }
        for (const auto& finding : findings) {
            out << "- [" << finding.code << "] " << finding.message;
            if (finding.taskId.has_value()) out << " (task " << *finding.taskId << ")";
            out << "\n";
        }
    };
    appendFindings("Primary rejection findings", outcome.primaryFindings);
    if (outcome.success) {
        if (!outcome.primaryRejectionCode.empty()) {
            out << "\n## Repair outcome\n\n"
                << "The accepted Manifest carries no remaining rejection; the primary "
                   "findings above were resolved by the one bounded repair and are kept "
                   "only as an audit trail.\n";
        }
    } else {
        appendFindings("Final rejection findings", outcome.lastFindings);
    }
    // Backend normalizations of the latest evaluated submission (see
    // ExecutionManifestPlannerOutcome::normalizations): after a success they describe the
    // accepted Manifest, so an operator can see what the Backend changed.
    if (!outcome.normalizations.empty()) {
        out << "\n## Backend normalizations\n\n";
        for (const auto& line : outcome.normalizations) out << "- " << line << "\n";
    }
    out << "\nRaw planner submissions are intentionally not retained in this report.\n";
    return out.str();
}

// A clean manifest run needs no billable reconciliation call: every task
// already reached a terminal, individually-explained state. Anything else
// (a real conflict, an incomplete/failed task) is exactly the "conflicts or
// incomplete acceptance criteria" case the target architecture reserves
// integration for.
bool needsIntegration(ManifestExecutionStatus status)
{
    return status == ManifestExecutionStatus::Failed;
}

} // namespace

std::string manifestPlanningInputFingerprint(
    const WorkflowRunContext& context,
    const StageModelAssignment& assignment,
    const Provider& provider,
    const Model& model,
    const WorkspaceBaselineSnapshot& baseline,
    const std::optional<VerificationEnvironmentSnapshot>& verificationEnvironment,
    std::string_view capacityPolicyIdentity)
{
    return planningInputFingerprintImpl(context, assignment, provider, model,
        baseline, verificationEnvironment, capacityPolicyIdentity);
}

ManifestWorkflowRunner::ManifestWorkflowRunner(
    providers::IProviderAgentSessionFactory& agentSessionFactory,
    AgentToolExecutionService& agentToolExecution,
    ChangeProposalService& changeProposals,
    ExecutionManifestService& manifests,
    IProviderClientFactory& clientFactory,
    IProviderRepository& providers,
    IModelRepository& models,
    ArtifactService* artifactService,
    ModelExecutionService* modelExecutionService,
    IManifestTaskExecutionRepository* taskExecutions,
    IManifestVerificationCommandRunner* verificationCommandRunner,
    ManifestWorkLedgerService* manifestWorkLedgerService,
    RunWorkspaceScopeService* runWorkspaceScopeService,
    VerificationEnvironmentDiscoveryService* verificationEnvironmentDiscoveryService,
    IVerificationBootstrapRepository* verificationBootstrapRepository)
    : planner_(agentSessionFactory, agentToolExecution, manifests)
    , executor_(agentSessionFactory, agentToolExecution, changeProposals, modelExecutionService)
    , verificationCommandRunner_(verificationCommandRunner != nullptr
          ? verificationCommandRunner : &systemVerificationCommandRunner_)
    , clientFactory_(clientFactory)
    , providers_(providers)
    , models_(models)
    , artifactService_(artifactService)
    , modelExecutionService_(modelExecutionService)
    , taskExecutions_(taskExecutions)
    , manifestWorkLedgerService_(manifestWorkLedgerService)
    , runWorkspaceScopeService_(runWorkspaceScopeService)
    , verificationEnvironmentDiscoveryService_(verificationEnvironmentDiscoveryService)
    , verificationBootstrapRepository_(verificationBootstrapRepository)
{
}

std::vector<RunStage> ManifestWorkflowRunner::createStages(
    const RunId& runId, const ResolvedExecutionPlan& /*plan*/) const
{
    // The task DAG is only known once planning completes, so (unlike the
    // legacy roster) it cannot be reflected as one stage per manifest task
    // here - see the class doc comment.
    const auto makeStage = [&runId](std::string suffix, std::string type, std::string title) {
        RunStage stage;
        stage.id = runId + ":" + suffix;
        stage.runId = runId;
        stage.type = std::move(type);
        stage.title = std::move(title);
        stage.description = "Manifest workflow stage";
        return stage;
    };
    return {
        makeStage("planning", "planning", "Manifest planning"),
        makeStage("execution", "execution", "Manifest execution")
    };
}

WorkflowResult ManifestWorkflowRunner::run(
    const WorkflowRunContext& context,
    std::stop_token stopToken,
    IRunEventSink& eventSink)
{
    models::TokenUsage totalUsage;
    if (context.run.assignmentSnapshot.empty()) {
        return {false, RunStatus::Failed,
            "manifest.route_resolution_failed: The Manifest workflow has no frozen assignment",
            totalUsage};
    }
    // AG-17.5: resolve the entire frozen assignment snapshot once. The lead
    // owns Planner/integration calls; every work task owns its own immutable
    // route through the same context. This happens before StageStarted and
    // before any provider/accounting activity.
    const auto routing = ManifestRouting::fromAssignments(context.run.assignmentSnapshot);
    auto routingContextResult = buildManifestRunRoutingContext(routing,
        context.run.assignmentSnapshot, providers_, models_,
        context.run.providerBudget.maxOutputTokens);
    if (!routingContextResult.success()) {
        return {false, RunStatus::Failed,
            routingContextResult.error()->code + ": "
                + routingContextResult.error()->message,
            totalUsage};
    }
    auto routingContext = std::move(routingContextResult.value());
    const StageModelAssignment& assignment = routingContext.leadAssignment;
    const Provider& provider = routingContext.leadProvider;
    const Model& model = routingContext.leadModel;
    auto routeResolver = std::make_shared<const TaskRouteResolver>(
        std::move(routingContext.taskRoutes));
    const auto& planningStage = context.stages.at(0);
    const auto& executionStage = context.stages.at(1);

    const auto budgetPolicyValid = validateProviderExecutionBudgetPolicy(
        context.run.providerBudget);
    if (!budgetPolicyValid.success()) {
        return {false, RunStatus::Failed,
            budgetPolicyValid.error()->code + ": " + budgetPolicyValid.error()->message,
            totalUsage};
    }
    auto providerBudgetGuard = std::make_shared<ProviderSessionBudgetGuard>(
        context.run.providerBudget,
        context.run.providerBudgetUsage,
        [&eventSink, runId = context.run.id](const ProviderExecutionBudgetUsage& usage) {
            return eventSink.persistProviderBudgetUsage(runId, usage);
        });

    const std::filesystem::path workspaceRoot =
        context.run.workspacePath.value_or(context.project.rootPath);
    const WorkspaceReadPolicy inspectPolicy = AgentToolExecutionService::buildPolicy(workspaceRoot);

    eventSink.publish(stageEvent(RunEventType::StageStarted, context.run.id, planningStage.id,
        0.0, planningStage.title + " started"));

    // Persist the policy-visible baseline before creating a provider session.
    // It contains relative paths, sizes and hashes only.
    const auto baseline = WorkspaceBaselineProvenance::capture(inspectPolicy);
    auto failBeforeProvider = [&](const std::string& message, std::string_view reason) {
        eventSink.publish(stageEvent(RunEventType::StageFailed, context.run.id,
            planningStage.id, 0.0, message));
        eventSink.publish(stageEvent(RunEventType::StageSkipped, context.run.id,
            executionStage.id, 0.5,
            "Manifest execution skipped: " + std::string(reason)));
        return WorkflowResult {false, RunStatus::Failed, message, totalUsage};
    };
    if (!baseline.success()) {
        return failBeforeProvider(baseline.error()->code + ": "
                + baseline.error()->message,
            "workspace baseline capture failed");
    }
    if (artifactService_ == nullptr) {
        return failBeforeProvider(
            "workspace_baseline.persistence_unavailable: Artifact persistence is required",
            "workspace baseline persistence unavailable");
    }
    const auto baselineJson = WorkspaceBaselineProvenance::renderJson(baseline.value());
    if (!baselineJson.success()) {
        return failBeforeProvider(baselineJson.error()->code + ": "
                + baselineJson.error()->message,
            "workspace baseline serialization failed");
    }
    const auto baselineArtifact = artifactService_->createArtifact(
        context.run.id, planningStage.id, ArtifactKind::Report,
        "workspace-baseline.v1.json", "application/json", baselineJson.value());
    if (!baselineArtifact.success()) {
        return failBeforeProvider("workspace_baseline.persistence_failed: "
                + baselineArtifact.error()->code,
            "workspace baseline persistence failed");
    }
    // A Run that must produce a Manifest always ends in a verification that needs the temporary copy: when the folder for it is not
    // usable, say so now, before the Planner is paid, instead of after the workers. (A Run that may end in "no changes needed" is
    // checked right before execution instead - see below - because it may never need the folder.)
    if (scratchProvider_ != nullptr
        && context.run.planningCompletionPolicy == PlanningCompletionPolicy::RequireManifest) {
        const auto scratch = scratchProvider_->resolve(workspaceRoot);
        if (!scratch.success()) {
            return failBeforeProvider(scratch.error()->code + ": " + scratch.error()->message,
                "the verification folder cannot be used");
        }
    }
    const auto preflight =
        WorkspaceBaselineProvenance::runConfiguredCMakePreflight(inspectPolicy);
    if (!preflight.success()) {
        return failBeforeProvider(preflight.error()->code + ": "
                + preflight.error()->message,
            "workspace preflight failed");
    }

    // AG-18.4B-R1: a project that has CMake presets must be configured and buildable on this computer before verification can say
    // anything. The proof is durable and reused: an unchanged project is not configured again, and nothing that goes wrong later
    // (a model error, an exhausted budget, a cancelled or rejected Run) resets it.
    //   * A Run that must produce a Manifest proves the setup HERE, before the Planner is paid, and fails with the typed reason.
    //   * A Run that may end in "no changes needed" (pure reading) starts nothing here - it only looks at the record (no process);
    //     if the record is not fresh the setup is proved later, before the first worker, and only when a Manifest was produced.
    // The live discovery below needs the configured build tree this step creates, so it comes first. The pair the user selected
    // (or that was proved) is the only pair the discovery offers to the Planner.
    std::optional<VerificationProfileSelection> selectedPair;
    bool setupProofDeferred = false;
    if (projectReadiness_ != nullptr) {
        const auto presetFile = VerificationEnvironmentDiscoveryService::hasPresetFileAtWorkspaceRoot(workspaceRoot);
        if (presetFile.success() && presetFile.value()) {
            if (context.run.planningCompletionPolicy == PlanningCompletionPolicy::RequireManifest) {
                const auto setup = projectReadiness_->ensureReady(
                    context.project.id, workspaceRoot, *verificationCommandRunner_, stopToken);
                if (!setup.ready) {
                    if (setup.failureCode == kProjectSetupCancelled || stopToken.stop_requested()) {
                        eventSink.publish(stageEvent(RunEventType::StageCancelled, context.run.id,
                            planningStage.id, 0.0, "Manifest planning cancelled"));
                        eventSink.publish(stageEvent(RunEventType::StageSkipped, context.run.id,
                            executionStage.id, 0.5, "Manifest execution skipped: the Run was cancelled"));
                        return {false, RunStatus::Cancelled, "Manifest planning cancelled", totalUsage};
                    }
                    return failBeforeProvider(setup.failureCode + ": " + setup.failureMessage,
                        "the project's verification setup is not ready");
                }
                selectedPair = VerificationProfileSelection {setup.selection.configurePreset, setup.selection.buildPreset};
            } else {
                const auto assessed = projectReadiness_->assess(context.project.id, workspaceRoot);
                if (!assessed.success() || !assessed.value().fresh) setupProofDeferred = true;
                if (assessed.success() && assessed.value().record.has_value()
                    && !assessed.value().record->configurePreset.empty() && !assessed.value().record->buildPreset.empty()) {
                    selectedPair = VerificationProfileSelection {
                        assessed.value().record->configurePreset, assessed.value().record->buildPreset};
                }
            }
        }
    }

    // AG-18.1: Backend-owned, provider-free discovery of the workspace's
    // CMake verification environment, before the Planner ever runs and
    // bound to the same baseline just captured above. Purely additive in
    // this package: the discovered snapshot is durably recorded but never
    // read by the Planner's prompt or used to gate this Run - see
    // VerificationEnvironmentDiscoveryService's own doc comment. Only a
    // genuine persistence failure fails the Run here, the same
    // "durable Backend-authored context is required infrastructure"
    // precedent as the baseline artifact write immediately above.
    // AG-18.2: the same snapshot discovery just recorded is immediately
    // re-fetched and fail-closed identity-validated before the Planner
    // session is ever created - never a second discovery, never treating a
    // repository read failure or a genuinely-missing row as an empty
    // catalog. std::nullopt (service not wired at all) is the only case
    // that legitimately skips this block; every environment-dependent
    // verification kind then simply fails binding later, exactly like an
    // empty catalog would.
    std::optional<VerificationEnvironmentSnapshot> verificationEnvironment;
    if (verificationEnvironmentDiscoveryService_ != nullptr) {
        const auto discovered = verificationEnvironmentDiscoveryService_->discoverAndRecord(
            context.run.id, context.project.id, workspaceRoot,
            baseline.value(), inspectPolicy, *verificationCommandRunner_, stopToken, selectedPair);
        if (!discovered.success()) {
            return failBeforeProvider(discovered.error()->code + ": "
                    + discovered.error()->message,
                "verification environment discovery failed");
        }

        const auto fetched = verificationEnvironmentDiscoveryService_->findForRun(context.run.id);
        if (!fetched.success()) {
            return failBeforeProvider(fetched.error()->code + ": " + fetched.error()->message,
                "verification environment snapshot could not be read");
        }
        if (!fetched.value().has_value()) {
            return failBeforeProvider(
                "verification_environment.snapshot_missing: "
                "No verification environment snapshot was recorded for this Run",
                "verification environment snapshot missing");
        }
        const auto identity = ManifestVerificationProfileBinder::validateSnapshotIdentity(
            *fetched.value(), context.run.id, context.project.id, baseline.value().fingerprint);
        if (!identity.success()) {
            return failBeforeProvider(identity.error()->code + ": " + identity.error()->message,
                "verification environment snapshot identity validation failed");
        }
        verificationEnvironment = std::move(fetched.value());
    }

    auto persistCurrentProvenance = [&](const StageId& stageId,
                                        const std::vector<ChangeSetId>& proposalIds) {
        struct ProvenanceResult {
            bool success {false};
            std::string errorCode;
        } result;
        const auto current = WorkspaceBaselineProvenance::capture(inspectPolicy);
        std::optional<WorkspaceBaselineDifference> difference;
        if (current.success()) {
            const auto compared = WorkspaceBaselineProvenance::compare(
                baseline.value(), current.value());
            if (compared.success()) difference = compared.value();
            else result.errorCode = compared.error()->code;
        } else {
            result.errorCode = current.error()->code;
        }
        const auto report = renderWorkspaceProvenanceReport(
            baseline.value(), difference, preflight.value(), proposalIds,
            result.errorCode);
        const auto artifact = artifactService_->createArtifact(
            context.run.id, stageId, ArtifactKind::Report,
            "manifest-workspace-provenance.md", "text/markdown", report);
        if (!artifact.success()) {
            result.errorCode = "workspace_provenance.persistence_failed";
            return result;
        }
        if (!difference.has_value()) return result;
        if (difference->classification != "unchanged") {
            result.errorCode = "workspace_provenance.external_drift";
            return result;
        }
        result.success = true;
        return result;
    };

    // AG-14: a bounded slice of prior Manifest Runs for this project - a
    // Failed Run's diagnosis, a prior Completed Run's own task/proposal
    // dispositions, and any human review/apply decisions since, none of
    // which this Planner could otherwise ever see (see
    // ManifestWorkLedgerService's own doc comment). Only the ledger-history
    // section is empty for a project's first Manifest Run or when the
    // service is not wired - AG-14.2: the current workspace baseline (and,
    // when recoverable, a real bounded added/modified/deleted diff against
    // the most recent Run's own baseline) is now always included via the
    // currentBaseline argument, never conditioned on ledger history existing.
    std::string workLedgerContext;
    if (manifestWorkLedgerService_ != nullptr) {
        auto ledgerSlice =
            manifestWorkLedgerService_->buildPlannerContext(context.project.id, baseline.value());
        if (!ledgerSlice.success()) {
            return failBeforeProvider(
                "manifest_work_ledger.context_unavailable: ["
                    + ledgerSlice.error()->code + "] " + ledgerSlice.error()->message,
                "work ledger context unavailable");
        }
        workLedgerContext = ledgerSlice.value().renderedText;
        if (!ledgerSlice.value().includedRunIds.empty()) {
            if (artifactService_ == nullptr) {
                return failBeforeProvider(
                    "manifest_work_ledger.context_persistence_unavailable: "
                    "Artifact persistence is required for ledger provenance",
                    "work ledger context persistence unavailable");
            }
            std::ostringstream ledgerArtifact;
            ledgerArtifact << "# Manifest Work Ledger context\n\n"
                << "The following prior Manifest Run(s) were included in this "
                   "Planner's context:\n\n";
            for (const auto& includedRunId : ledgerSlice.value().includedRunIds) {
                ledgerArtifact << "- " << includedRunId << "\n";
            }
            const auto persistedContext = artifactService_->createArtifact(
                context.run.id, planningStage.id, ArtifactKind::Report,
                "manifest-work-ledger-context.md", "text/markdown", ledgerArtifact.str());
            if (!persistedContext.success()) {
                return failBeforeProvider(
                    "manifest_work_ledger.context_persistence_failed: ["
                        + persistedContext.error()->code + "] "
                        + persistedContext.error()->message,
                    "work ledger context persistence failed");
            }
        }
    }

    // AG-15: a compact, bounded, deterministic catalog rendered from the
    // already-captured baseline (no new filesystem walk) - path, extension,
    // byte size, and the real Backend-computed sha256 for every workspace
    // file the deny-list already permits. Paired with a fixed instruction so
    // the Planner declares plannedCreatePaths/requiredReadPaths/
    // referencePaths/excludedPaths/deferredPaths per task, not just
    // ownedPaths - the frozen RunWorkspaceScope this enables is what lets
    // AG-15.2's same-attempt read gate and write-scope enforcement work at
    // all, so this instruction is unconditional (not gated on
    // runWorkspaceScopeService_ being wired) while remaining backward
    // compatible: the fields it asks for are already required by the
    // strict schema regardless of this text.
    const std::string discoveryCatalog =
        WorkspaceBaselineProvenance::renderDiscoveryCatalog(baseline.value());
    const std::string workspaceScopeInstruction =
        "\n\n# Workspace discovery catalog\n"
        "The bounded table below lists workspace files currently visible to "
        "you (relative path, extension, byte size, sha256). If its final line "
        "says it was truncated, use the read-only inspection tools to discover "
        "specific omitted paths before classifying them. For every task "
        "you declare, you MUST classify the specific files it needs using "
        "plannedCreatePaths (new files this task will create), "
        "requiredReadPaths (existing files this task must read before "
        "acting), referencePaths (existing files worth citing but not "
        "required reads), excludedPaths and deferredPaths (with a reason "
        "each) for anything relevant you are deliberately not including now. "
        "A file must never appear in more than one of these lists (plus "
        "ownedPaths) for the same task.\n\n"
        + discoveryCatalog;

    // AG-18.2: the same trusted catalog my binder resolves selections
    // against, rendered as read-only prompt text - the Planner may only
    // ever SELECT from this, never invent an id, and it is reused
    // unchanged across the repair attempt (see the repair prompt's own
    // "sessionRequest.userContent" prefix below).
    const std::string verificationEnvironmentInstruction = verificationEnvironment.has_value()
        ? "\n\n" + ManifestVerificationProfileBinder::renderTrustedCatalog(*verificationEnvironment)
        : std::string {};

    ExecutionManifestPlannerRequest plannerRequest;
    plannerRequest.sessionRequest.providerId = assignment.providerId;
    plannerRequest.sessionRequest.modelId = assignment.modelId;
    plannerRequest.sessionRequest.apiModelId = model.apiModelId;
    plannerRequest.sessionRequest.userContent =
        context.project.description + "\n\n# Run task\n" + context.run.task
        // PC-2: the same contracts the Backend will hand to the workers verbatim; the Planner sees them (and
        // their scopes) so it plans producers and consumers they reach, and is told not to carry the text.
        + (context.run.interfaceContracts.empty()
               ? std::string {}
               : "\n\n" + renderRunContractsForPlanner(context.run.interfaceContracts))
        + (workLedgerContext.empty() ? std::string {} : "\n\n" + workLedgerContext)
        + workspaceScopeInstruction + verificationEnvironmentInstruction;
    plannerRequest.verificationEnvironment = verificationEnvironment;
    // AG-17 M2b: every provider response of the planning session is classified against the
    // long-context threshold of `model` - the SAME Model object (routingContext.leadModel,
    // resolved once before the Run) whose prices later price planned.usage below.
    plannerRequest.longContextThresholdInputTokens = model.longContextPricing.thresholdInputTokens;
    // AG-17.1/17.5: the Backend-owned routing record and the repository-backed
    // roster catalog come from the same pre-resolved frozen identity. Both are
    // trusted inputs; neither is provider-authored.
    plannerRequest.trustedRouting = routingContext.routing;
    plannerRequest.rosterCatalog = routingContext.rosterCatalog;
    plannerRequest.sessionRequest.maxOutputTokens = model.maxOutputTokens > 0
        ? static_cast<std::uint32_t>(std::min<std::uint64_t>(
              {model.maxOutputTokens,
               context.run.providerBudget.maxOutputTokens,
               static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())}))
        : kAgentToolTurnLoopMaxOutputTokens;
    plannerRequest.sessionRequest.turnTimeout = assignment.timeout.count() > 0
        ? assignment.timeout : kAgentToolTurnLoopDefaultTurnTimeout;
    plannerRequest.sessionRequest.maxTurns = static_cast<std::size_t>(std::min<std::uint64_t>(
        kAgentToolTurnLoopMaxTurns, context.run.providerBudget.maxTurns));
    plannerRequest.sessionRequest.maxTotalToolCalls = static_cast<std::size_t>(
        std::min<std::uint64_t>(kAgentToolTurnLoopMaxTotalToolCalls,
            context.run.providerBudget.maxToolCalls));
    plannerRequest.sessionRequest.maxToolCallsPerTurn = kAgentToolTurnLoopMaxToolCallsPerTurn;
    plannerRequest.runId = context.run.id;
    plannerRequest.providerKind = provider.kind;
    plannerRequest.inspectPolicy = inspectPolicy;
    plannerRequest.baselineSnapshot = baseline.value();
    plannerRequest.trustedManifestPolicy.maxParallelism = context.run.maxParallelism;
    plannerRequest.allowEvidenceBackedNoChanges =
        context.run.planningCompletionPolicy
        == PlanningCompletionPolicy::AllowEvidenceBackedNoChanges;
    plannerRequest.providerBudgetGuard = providerBudgetGuard;

    const std::string inputFingerprint = manifestPlanningInputFingerprint(
        context, assignment, provider, model, baseline.value(),
        verificationEnvironment, executionCapacityPolicyIdentity());
    if (manifestWorkLedgerService_ != nullptr) {
        const auto authorized = manifestWorkLedgerService_->authorizePlanningAttempt(
            context.project.id, inputFingerprint, context.run.parentRunId.has_value());
        if (!authorized.success()) {
            return failBeforeProvider(
                authorized.error()->code + ": " + authorized.error()->message,
                "unchanged paid planning retry blocked");
        }
        if (artifactService_ == nullptr) {
            return failBeforeProvider(
                "planner.retry_guard_persistence_unavailable: Artifact persistence is required",
                "planning retry identity persistence unavailable");
        }
        const auto persistedFingerprint = artifactService_->createArtifact(
            context.run.id, planningStage.id, ArtifactKind::Report,
            "manifest-planning-input-fingerprint.txt", "text/plain", inputFingerprint);
        if (!persistedFingerprint.success()) {
            return failBeforeProvider(
                "planner.retry_guard_persistence_failed: ["
                    + persistedFingerprint.error()->code + "] "
                    + persistedFingerprint.error()->message,
                "planning retry identity persistence failed");
        }
    }

    // AG-18.5: planning is a real paid provider execution too. The audited
    // agent-tool session/turn ledger proves its tools and token usage, while
    // model_executions is the canonical cost/analytics ledger. Previously
    // only Manifest workers reached the latter, leaving the larger Planner
    // spend invisible to cost reporting.
    const auto plannerStartedAt = std::chrono::steady_clock::now();
    std::optional<ModelExecutionId> plannerExecutionId;
    if (modelExecutionService_ != nullptr) {
        CreateModelExecutionRequest create;
        create.source = ModelExecutionSource::Run;
        create.sourceId = context.run.id;
        create.projectId = context.project.id;
        create.providerId = assignment.providerId;
        create.modelId = assignment.modelId;
        create.role = "planner";
        create.position = assignment.position;
        create.attempt = 1;
        // inputFingerprint already binds every immutable Backend-owned
        // planning input, provider/model identity, baseline, and Run budget.
        create.requestHash = inputFingerprint;
        create.contextHash = sha256Hex(plannerRequest.sessionRequest.userContent);
        const auto created = modelExecutionService_->createExecution(create);
        if (!created.success()) {
            return failBeforeProvider(
                "model_execution.persistence_failed: Planner accounting could not be created: ["
                    + created.error()->code + "] " + created.error()->message,
                "planner accounting persistence failed");
        }
        const auto started = modelExecutionService_->startExecution(created.value().id);
        if (!started.success()) {
            FinishModelExecutionRequest failedStart;
            failedStart.status = ModelExecutionStatus::Failed;
            failedStart.errorCode = started.error()->code;
            failedStart.errorMessage = started.error()->message;
            static_cast<void>(modelExecutionService_->finishExecution(
                created.value().id, failedStart));
            return failBeforeProvider(
                "model_execution.persistence_failed: Planner accounting could not be started: ["
                    + started.error()->code + "] " + started.error()->message,
                "planner accounting start failed");
        }
        plannerExecutionId = created.value().id;
    }

    const auto planned = planner_.plan(std::move(plannerRequest), stopToken);
    mergeAccumulatedUsage(totalUsage, planned.usage);

    if (plannerExecutionId.has_value() && modelExecutionService_ != nullptr) {
        FinishModelExecutionRequest finish;
        finish.status = planned.cancelled
            ? ModelExecutionStatus::Cancelled
            : (planned.success ? ModelExecutionStatus::Completed
                               : ModelExecutionStatus::Failed);
        finish.outputContent = renderPlannerDiagnostics(planned);
        finish.outputPreview = finish.outputContent.substr(0, 1000);
        finish.inputTokens = planned.usage.inputTokens;
        finish.outputTokens = planned.usage.outputTokens;
        finish.cacheCreationInputTokens = planned.usage.cacheCreationInputTokens;
        finish.cacheReadInputTokens = planned.usage.cacheReadInputTokens;
        finish.cacheCreationDuration = planned.usage.cacheCreationDuration;
        // planned.usage was accumulated by the planning loop with `model`'s own threshold.
        const auto cost = computeAggregatedExecutionCostMicros(model, planned.usage);
        finish.costMicros = cost.costMicros;
        finish.costPriceKnown = cost.priceKnown;
        finish.latencyMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - plannerStartedAt).count());
        if (!planned.success) {
            finish.errorCode = planned.cancelled
                ? "planner.cancelled" : planned.errorCode;
            finish.errorMessage = planned.errorMessage;
        }
        const auto finished = modelExecutionService_->finishExecution(
            *plannerExecutionId, finish);
        if (!finished.success()) {
            const std::string message =
                "model_execution.persistence_failed: Planner accounting could not be finalized: ["
                + finished.error()->code + "] " + finished.error()->message;
            eventSink.publish(stageEvent(RunEventType::StageFailed, context.run.id,
                planningStage.id, 0.0, message));
            eventSink.publish(stageEvent(RunEventType::StageSkipped, context.run.id,
                executionStage.id, 0.5,
                "Manifest execution skipped: planner accounting was not finalized"));
            return {false, RunStatus::Failed, message, totalUsage};
        }
    }

    if (planned.cancelled) {
        eventSink.publish(stageEvent(RunEventType::StageCancelled, context.run.id, planningStage.id,
            0.0, "Manifest planning cancelled"));
        eventSink.publish(stageEvent(RunEventType::StageSkipped, context.run.id, executionStage.id,
            0.5, "Manifest execution skipped: planning was cancelled"));
        return {false, RunStatus::Cancelled, "Manifest planning cancelled", totalUsage};
    }
    if (!planned.success) {
        // AG-18.3 hardening (blocker 2): an ordinary Planner/provider
        // failure (malformed submission, provider.request_failed,
        // cancellation-adjacent errors, repair exhaustion after an invalid
        // bootstrap attempt, ...) is exactly that - it must never masquerade
        // as a durable ActionRequired decision. ActionRequired is reachable
        // ONLY via the explicit, successful VerificationActionRequired
        // disposition below.
        std::string failureMessage = plannerFailureMessage(planned);
        if (setupProofDeferred && failureMessage.find("verification_policy.target_catalog_incomplete") != std::string::npos) {
            // This Run may have ended without changes, so the project was not configured before planning; its target catalog is
            // therefore only what a previous check left behind.
            failureMessage += "\nThe project's verification setup was not checked before planning (this Run may end without "
                              "changes, so nothing was configured). Press Check again under Verification setup in Projects, then "
                              "start the Run again.";
        }
        if (artifactService_ != nullptr) {
            const auto persistedSignature = artifactService_->createArtifact(
                context.run.id, planningStage.id, ArtifactKind::Report,
                "manifest-planning-failure-signature.txt", "text/plain",
                planningFailureSignature(planned));
            if (!persistedSignature.success() && manifestWorkLedgerService_ != nullptr) {
                const std::string message = "planner.failure_identity_persistence_failed: ["
                    + persistedSignature.error()->code + "] "
                    + persistedSignature.error()->message;
                eventSink.publish(stageEvent(RunEventType::StageFailed, context.run.id,
                    planningStage.id, 0.0, message));
                eventSink.publish(stageEvent(RunEventType::StageSkipped, context.run.id,
                    executionStage.id, 0.5,
                    "Manifest execution skipped: planning failure identity was not persisted"));
                return {false, RunStatus::Failed, message, totalUsage};
            }
            const std::string diagnostics = renderPlannerDiagnostics(planned);
            static_cast<void>(artifactService_->createDiagnosticArtifact(
                context.run.id, planningStage.id, ArtifactKind::Report,
                "manifest-planning-diagnostics.md", "text/markdown", diagnostics));
        }
        eventSink.publish(stageEvent(RunEventType::StageFailed, context.run.id, planningStage.id,
            0.0, failureMessage));
        eventSink.publish(stageEvent(RunEventType::StageSkipped, context.run.id, executionStage.id,
            0.5, "Manifest execution skipped: no manifest was frozen"));
        return {false, RunStatus::Failed, failureMessage, totalUsage};
    }

    if (planned.disposition == PlanningDisposition::VerificationActionRequired) {
        // AG-18.3 hardening (blockers 1+2): the Planner explicitly asserted
        // - and ExecutionManifestPlanner::plan() already confirmed against
        // the immutable snapshot - that no usable verification profile is
        // available and no safe same-Run bootstrap could be formed. No
        // manifest was ever frozen, no worker was ever considered. The
        // decision must be durably persisted BEFORE this Run is reported as
        // terminal; a persistence failure is itself the Run's failure
        // reason, never silently swallowed.
        const std::string summary = planned.verificationActionRequired.has_value()
            ? planned.verificationActionRequired->summary
            : std::string {"Verification setup is required"};
        if (verificationBootstrapRepository_ != nullptr) {
            VerificationBootstrapDecision decision;
            decision.runId = context.run.id;
            decision.projectId = context.project.id;
            decision.baselineFingerprint = baseline.value().fingerprint;
            decision.verificationSnapshotSchemaVersion = verificationEnvironment.has_value()
                ? verificationEnvironment->schemaVersion : std::string {};
            decision.disposition = VerificationBootstrapDisposition::ActionRequired;
            decision.reasonCode = "verification_environment.action_required";
            decision.message = boundedTerminalMessage(summary);
            decision.decidedAt = std::chrono::system_clock::now();
            const auto recorded = verificationBootstrapRepository_->recordDecision(decision);
            if (!recorded.success()) {
                const std::string message = recorded.error()->code
                    + ": Could not durably record the ActionRequired decision";
                eventSink.publish(stageEvent(RunEventType::StageFailed, context.run.id,
                    planningStage.id, 0.0, message));
                eventSink.publish(stageEvent(RunEventType::StageSkipped, context.run.id,
                    executionStage.id, 0.5,
                    "Manifest execution skipped: ActionRequired could not be durably recorded"));
                return {false, RunStatus::Failed, message, totalUsage};
            }
        }
        if (artifactService_ != nullptr && planned.verificationActionRequired.has_value()) {
            static_cast<void>(artifactService_->createDiagnosticArtifact(
                context.run.id, planningStage.id, ArtifactKind::Report,
                "manifest-verification-action-required-identity.v1.json", "application/json",
                renderPlannerVerificationActionRequiredJson(*planned.verificationActionRequired)));
        }
        eventSink.publish(stageEvent(RunEventType::StageFailed, context.run.id,
            planningStage.id, 0.0, "Verification setup is required: " + summary));
        eventSink.publish(stageEvent(RunEventType::StageSkipped, context.run.id,
            executionStage.id, 0.5,
            "Manifest execution skipped: verification setup is required"));
        // Non-green: RunStatus::Failed, never Completed/NoChangesNeeded -
        // this Run genuinely did not accomplish its task, it safely
        // recognized that it could not.
        return {false, RunStatus::Failed,
            "verification_environment.action_required: " + summary, totalUsage};
    }

    if (planned.disposition == PlanningDisposition::NoChangesNeeded) {
        if (context.run.planningCompletionPolicy
            != PlanningCompletionPolicy::AllowEvidenceBackedNoChanges) {
            const std::string message =
                "planner.no_changes_disallowed: This Run requires an Execution Manifest";
            eventSink.publish(stageEvent(RunEventType::StageFailed, context.run.id,
                planningStage.id, 0.0, message));
            eventSink.publish(stageEvent(RunEventType::StageSkipped, context.run.id,
                executionStage.id, 0.5,
                "Manifest execution skipped: planning policy rejected no changes"));
            return {false, RunStatus::Failed, message, totalUsage};
        }
        if (!planned.noChangesNeeded.has_value() || planned.manifest.has_value()) {
            const std::string message =
                "Planner returned an inconsistent no-changes-needed outcome";
            eventSink.publish(stageEvent(RunEventType::StageFailed, context.run.id,
                planningStage.id, 0.0, message));
            eventSink.publish(stageEvent(RunEventType::StageSkipped, context.run.id,
                executionStage.id, 0.5,
                "Manifest execution skipped: planning outcome was inconsistent"));
            return {false, RunStatus::Failed, message, totalUsage};
        }

        const auto provenance = persistCurrentProvenance(planningStage.id, {});
        if (!provenance.success) {
            const std::string message = provenance.errorCode
                + ": Workspace provenance did not remain verifiable and unchanged";
            eventSink.publish(stageEvent(RunEventType::StageFailed, context.run.id,
                planningStage.id, 0.0, message));
            eventSink.publish(stageEvent(RunEventType::StageSkipped, context.run.id,
                executionStage.id, 1.0,
                "Manifest execution skipped: workspace provenance failed"));
            return {false, RunStatus::Failed, message, totalUsage};
        }

        // AG-14.2: this typed identity artifact is required, not best-effort
        // - ManifestWorkLedgerService::recordTerminalRun's "verified /
        // no_changes / not_run" invariant (called later, from
        // RunManager::finishRun *after* this Run's terminal status is
        // actually committed - never from inside this function, which runs
        // strictly before that commit) depends on its presence whenever
        // this Run is genuinely Completed via the no-op path. A write
        // failure here must fail the Run rather than silently produce a
        // Completed terminal status the ledger can never fully explain.
        const auto identityPersisted = artifactService_ != nullptr
            ? artifactService_->createArtifact(
                  context.run.id, planningStage.id, ArtifactKind::Report,
                  "manifest-planning-no-changes-identity.v1.json", "application/json",
                  renderPlannerNoChangesNeededJson(*planned.noChangesNeeded))
            : common::Result<Artifact>::failure(
                  {"manifest_planning_no_changes.identity_persistence_unavailable",
                   "Artifact persistence is required"});
        if (!identityPersisted.success()) {
            const std::string message = identityPersisted.error()->code
                + ": Could not persist the no-changes-needed identity record";
            eventSink.publish(stageEvent(RunEventType::StageFailed, context.run.id,
                planningStage.id, 0.5, message));
            eventSink.publish(stageEvent(RunEventType::StageSkipped, context.run.id,
                executionStage.id, 1.0,
                "Manifest execution skipped: no-changes identity persistence failed"));
            return {false, RunStatus::Failed, message, totalUsage};
        }

        std::string summary = renderNoChangesSummary(*planned.noChangesNeeded, planned);
        eventSink.publish(stageEvent(RunEventType::StageCompleted, context.run.id,
            planningStage.id, 0.5,
            planningStage.title + " completed: current task already satisfied"));
        eventSink.publish(stageEvent(RunEventType::StageSkipped, context.run.id,
            executionStage.id, 1.0,
            "Manifest execution skipped: current task already satisfied"));

        if (artifactService_ != nullptr) {
            static_cast<void>(artifactService_->createDiagnosticArtifact(
                context.run.id, planningStage.id, ArtifactKind::Report,
                "manifest-planning-no-changes.md", "text/markdown", summary));
        }

        return {true, RunStatus::Completed,
            "Current task already satisfied; no changes needed",
            totalUsage, std::move(summary)};
    }

    if (!planned.manifest.has_value() || planned.noChangesNeeded.has_value()) {
        const std::string message = "Planner returned an inconsistent manifest-ready outcome";
        eventSink.publish(stageEvent(RunEventType::StageFailed, context.run.id,
            planningStage.id, 0.0, message));
        eventSink.publish(stageEvent(RunEventType::StageSkipped, context.run.id,
            executionStage.id, 0.5,
            "Manifest execution skipped: planning outcome was inconsistent"));
        return {false, RunStatus::Failed, message, totalUsage};
    }

    // AG-18.2 Section 2: best-effort provenance of the verification
    // environment catalog this now-frozen Manifest was planned against.
    // Never fails the Run - this is a supplementary audit record, not
    // load-bearing infrastructure any downstream code reads back (unlike
    // the AG-14.2 identity artifact above, which the Work Ledger's
    // terminal-status invariant depends on).
    if (verificationEnvironment.has_value() && artifactService_ != nullptr) {
        static_cast<void>(artifactService_->createDiagnosticArtifact(
            context.run.id, planningStage.id, ArtifactKind::Report,
            "manifest-verification-environment-provenance.md", "text/markdown",
            renderVerificationEnvironmentProvenance(*verificationEnvironment)));
    }

    // AG-18.3 hardening (blocker 1): the frozen Manifest itself is the only
    // source of truth for whether this Run declared a same-Run bootstrap -
    // freeze() already enforced the full admission predicate
    // (ManifestBootstrapValidator) as part of the Planner's own
    // bind-then-freeze flow, so reaching this point with bootstrapDeclaration
    // set means it was genuinely admitted. This decision MUST be durably
    // persisted before any worker Agent session, ModelExecution row, task
    // dispatch, or ChangeProposal creation - unlike the best-effort
    // provenance artifact above, a persistence failure here fails the Run
    // before execution begins, exactly like the RunWorkspaceScope freeze
    // gate immediately below it.
    if (verificationBootstrapRepository_ != nullptr
        && planned.manifest->bootstrapDeclaration.has_value()) {
        VerificationBootstrapDecision decision;
        decision.runId = context.run.id;
        decision.projectId = context.project.id;
        decision.baselineFingerprint = baseline.value().fingerprint;
        decision.verificationSnapshotSchemaVersion = verificationEnvironment.has_value()
            ? verificationEnvironment->schemaVersion : std::string {};
        decision.disposition = VerificationBootstrapDisposition::BootstrapPending;
        decision.reasonCode = "verification_environment.bootstrap_pending";
        decision.message = "A same-Run verification-environment bootstrap was declared and "
            "admitted";
        decision.bootstrapSetupPaths = planned.manifest->bootstrapDeclaration->setupPaths;
        decision.owningTaskIds = {planned.manifest->bootstrapDeclaration->bootstrapTaskId};
        decision.decidedAt = std::chrono::system_clock::now();
        const auto recorded = verificationBootstrapRepository_->recordDecision(decision);
        if (!recorded.success()) {
            const std::string message = recorded.error()->code
                + ": Could not durably record the BootstrapPending decision";
            eventSink.publish(stageEvent(RunEventType::StageFailed, context.run.id,
                planningStage.id, 0.0, message));
            eventSink.publish(stageEvent(RunEventType::StageSkipped, context.run.id,
                executionStage.id, 0.5,
                "Manifest execution skipped: BootstrapPending could not be durably recorded"));
            return {false, RunStatus::Failed, message, totalUsage};
        }
    }

    // AG-15: the frozen RunWorkspaceScope is derived from the manifest that
    // was just frozen above (immutable, inside planner_.plan()) and the same
    // baseline snapshot already captured before the Planner ever ran. A
    // freeze failure here fails the Run before any worker is dispatched -
    // mirrors the AG-14.2 required-identity-persistence gate pattern, since
    // AG-15.2's write-scope allowlist and same-attempt read gate both depend
    // on this scope existing.
    if (runWorkspaceScopeService_ != nullptr) {
        const auto scopeFrozen = runWorkspaceScopeService_->freezeForManifest(
            context.run.id, *planned.manifest, baseline.value());
        if (!scopeFrozen.success()) {
            const std::string message = "[" + scopeFrozen.error()->code + "] "
                + scopeFrozen.error()->message;
            eventSink.publish(stageEvent(RunEventType::StageFailed, context.run.id,
                planningStage.id, 0.0, message));
            eventSink.publish(stageEvent(RunEventType::StageSkipped, context.run.id,
                executionStage.id, 0.5,
                "Manifest execution skipped: workspace scope could not be frozen"));
            return {false, RunStatus::Failed, message, totalUsage};
        }
    }

    eventSink.publish(stageEvent(RunEventType::StageCompleted, context.run.id, planningStage.id,
        0.5, planningStage.title + " completed: " + std::to_string(planned.manifest->tasks.size())
            + " task(s) frozen"));
    if (artifactService_ != nullptr
        && (!planned.primaryRejectionCode.empty() || !planned.normalizations.empty())) {
        const std::string diagnostics = renderPlannerDiagnostics(planned);
        static_cast<void>(artifactService_->createDiagnosticArtifact(
            context.run.id, planningStage.id, ArtifactKind::Report,
            "manifest-planning-diagnostics.md", "text/markdown", diagnostics));
    }

    ManifestVerificationEnvironmentPreflightRequest preflightRequest;
    preflightRequest.workspaceRoot = workspaceRoot;
    preflightRequest.tasks = planned.manifest->tasks;
    const auto environmentPreflight = ManifestVerificationEnvironmentPreflight::validate(
        preflightRequest, *verificationCommandRunner_, stopToken);
    if (!environmentPreflight.success()) {
        const bool cancelled =
            environmentPreflight.error()->code == "verification.environment_cancelled";
        const std::string message = "[" + environmentPreflight.error()->code + "] "
            + environmentPreflight.error()->message;
        eventSink.publish(stageEvent(RunEventType::StageSkipped, context.run.id,
            executionStage.id, 1.0,
            "Manifest execution skipped before worker dispatch: " + message));
        if (artifactService_ != nullptr) {
            std::ostringstream report;
            report << "# Manifest verification environment preflight\n\n"
                   << "Disposition: " << (cancelled ? "cancelled" : "blocked") << "\n\n"
                   << "Error code: " << environmentPreflight.error()->code << "\n\n"
                   << "Message: " << environmentPreflight.error()->message << "\n\n"
                   << "No Manifest worker session or change proposal was created.\n";
            static_cast<void>(artifactService_->createDiagnosticArtifact(
                context.run.id, planningStage.id, ArtifactKind::Report,
                "manifest-verification-preflight.md", "text/markdown", report.str()));
        }
        return {false, cancelled ? RunStatus::Cancelled : RunStatus::Failed,
            message, totalUsage};
    }

    // AG-17.2/17.5: planning must have frozen the same pre-resolved routing
    // identity before capacity admission and before execution starts. A
    // mismatch is an authority failure, never masked by capacity and never
    // repaired or silently routed through the lead/default fields.
    if (!planned.manifest->routing.has_value()
        || planned.manifest->routing->fingerprint != routingContext.routing.fingerprint) {
        const std::string message = "[manifest.route_identity_mismatch] The frozen Manifest "
            "does not carry the Run's pre-resolved routing identity";
        eventSink.publish(stageEvent(RunEventType::StageSkipped, context.run.id,
            executionStage.id, 1.0,
            "Manifest execution skipped before worker dispatch: " + message));
        return {false, RunStatus::Failed, message, totalUsage};
    }

    // MW-P6 v3 + worker capacity reservation: the Manifest is now frozen and
    // verification-environment preflight has succeeded, but no worker
    // task/session/ModelExecution or proposal exists yet. Admit the exact
    // pending provider-backed task set against the SAME shared Run-wide guard
    // before publishing execution as started, in this order: (1) the
    // theoretical protocol minimum derived per task shape by
    // minimumProviderTurnsForTask() (one turn for Inspect and for a Propose
    // task with no required reads, two for a Propose task whose
    // requiredReadPaths must be inspected before its separate terminal
    // turn); (2) each task's conservative serial-safe reserved floor versus
    // one worker session; (3) the sum of those floors versus the Run-wide
    // remainder - each floor counted twice, as the task's first-attempt
    // reservation and as its separate protected retry reservation (policy
    // worker_capacity_reservation_v3). The executor repeats the same admission
    // and then registers those reservations on the shared guard.
    //
    // Before that admission, a Run whose turn limit was not set explicitly has
    // it raised to exactly what the frozen Manifest needs (policy
    // manifest_turn_limit_auto_v1): the requirement is a deterministic fact of
    // the frozen task set, so a shortfall is fixed instead of failing the Run.
    // A pinned limit, a requirement above the Backend ceiling and a task floor
    // above one worker session raise nothing and are refused by the admission.
    const auto turnLimitExtension = extendManifestTurnLimitIfNeeded(
        *providerBudgetGuard, *planned.manifest, {});
    const auto executionCapacity = turnLimitExtension.success()
        ? admitManifestWorkerReservations(
              *planned.manifest, {}, providerBudgetGuard->policy(),
              providerBudgetGuard->usage())
        : common::Result<ManifestWorkerReservationPlan>::failure(*turnLimitExtension.error());
    if (!executionCapacity.success()) {
        const std::string message = "[" + executionCapacity.error()->code + "] "
            + executionCapacity.error()->message;
        eventSink.publish(stageEvent(RunEventType::StageSkipped, context.run.id,
            executionStage.id, 1.0,
            "Manifest execution skipped before worker dispatch: " + message));
        if (artifactService_ != nullptr) {
            std::ostringstream report;
            report << "# Manifest execution-capacity admission\n\n"
                   << "Disposition: blocked\n\n"
                   << "Error code: " << executionCapacity.error()->code << "\n\n"
                   << "Message: " << executionCapacity.error()->message << "\n\n"
                   << "No Manifest worker task attempt, provider session, model execution, "
                      "or change proposal was created.\n";
            static_cast<void>(artifactService_->createDiagnosticArtifact(
                context.run.id, planningStage.id, ArtifactKind::Report,
                "manifest-execution-capacity.md", "text/markdown", report.str()));
        }
        return {false, RunStatus::Failed, message, totalUsage};
    }
    eventSink.publish(stageEvent(RunEventType::StageStarted, context.run.id, executionStage.id,
        0.5, executionStage.title + " started"));
    if (turnLimitExtension.value().has_value()) {
        const auto& extension = *turnLimitExtension.value();
        std::ostringstream summary;
        summary << "Provider turn limit raised automatically from " << extension.previousMaxTurns
                << " to " << extension.newMaxTurns << " turn(s): "
                << extension.turnsStarted << " already used + "
                << extension.reservedTurns << " reserved for "
                << extension.pendingProviderTaskCount << " pending provider-backed task(s)";
        if (extension.retryReservedTurns > 0) {
            summary << " + " << extension.retryReservedTurns
                    << " protected for one retry session each";
        }
        eventSink.publish(stageEvent(RunEventType::StageProgress, context.run.id,
            executionStage.id, 0.5, summary.str()));
        if (artifactService_ != nullptr) {
            std::ostringstream report;
            report << "# Provider turn-limit extension\n\n"
                   << "Policy: " << kManifestTurnLimitPolicyId << "\n\n"
                   << "Previous limit: " << extension.previousMaxTurns << " turn(s)\n\n"
                   << "New limit: " << extension.newMaxTurns << " turn(s)\n\n"
                   << "Turns already used: " << extension.turnsStarted << "\n\n"
                   << "Turns reserved for " << extension.pendingProviderTaskCount
                   << " pending provider-backed task(s) under "
                   << kWorkerCapacityReservationPolicyId << ": "
                   << extension.reservedTurns << "\n\n";
            if (extension.retryReservedTurns > 0) {
                report << "Turns protected for one retry/repair session of each of those tasks "
                          "(a separate reservation only that task's later session can spend; "
                          "returned to the Run when the task needs no retry): "
                       << extension.retryReservedTurns << "\n\n";
            }
            report << "The limit was not set explicitly for this Run, so the Backend raised it "
                      "to exactly what the frozen Manifest requires (Backend ceiling: "
                   << kProviderBudgetHardMaxTurns << " turn(s)). All other budget limits are "
                      "unchanged.\n";
            static_cast<void>(artifactService_->createDiagnosticArtifact(
                context.run.id, planningStage.id, ArtifactKind::Report,
                "manifest-turn-limit.md", "text/markdown", report.str()));
        }
    }

    ExecutionManifestExecutionRequest executionRequest;
    executionRequest.routeResolver = routeResolver;
    executionRequest.interfaceContracts = context.run.interfaceContracts;
    executionRequest.manifest = *planned.manifest;
    executionRequest.runId = context.run.id;
    executionRequest.projectId = context.project.id;
    executionRequest.providerId = assignment.providerId;
    executionRequest.modelId = assignment.modelId;
    executionRequest.apiModelId = model.apiModelId;
    executionRequest.providerKind = provider.kind;
    executionRequest.pricingModel = model;
    // StageModelAssignment::position is 32-bit: the first worker slot is formed in 64
    // bits and range-checked BEFORE it is stored, or a Planner position of
    // UINT32_MAX would wrap to 0 here and slip past the allocator's own range check.
    const auto workerPositionBase = static_cast<std::uint64_t>(assignment.position) + 1;
    const bool workerPositionBaseFits =
        workerPositionBase <= std::numeric_limits<std::uint32_t>::max();
    if (workerPositionBaseFits) {
        executionRequest.workerModelExecutionPositionBase =
            static_cast<std::uint32_t>(workerPositionBase);
    }
    const auto modelExecutionPositions = workerPositionBaseFits
        ? allocateWorkerModelExecutionPositions(routeResolver.get(), *planned.manifest,
              executionRequest.workerModelExecutionPositionBase)
        : common::Result<TaskModelExecutionPositionPlan>::failure({
              "model_execution.position_invalid",
              "The Planner position leaves no room for worker model executions"});
    // One more durable slot is reserved for the optional non-authoritative
    // Architect integration call. It must never collide with the Planner or a
    // worker (including a second task assigned to the same roster member).
    if (!modelExecutionPositions.success()
        || modelExecutionPositions.value().lastPosition
            >= std::numeric_limits<std::uint32_t>::max()) {
        const std::string message =
            "model_execution.position_invalid: The Manifest task set does not fit after the Planner position";
        eventSink.publish(stageEvent(RunEventType::StageFailed, context.run.id,
            executionStage.id, 1.0, message));
        return {false, RunStatus::Failed, message, totalUsage};
    }
    executionRequest.maxOutputTokens = std::min<std::uint64_t>(
        std::max<std::uint64_t>(
            model.maxOutputTokens > 0
                ? model.maxOutputTokens
                : kAgentToolTurnLoopMaxOutputTokens,
            kWorkerProposeMinOutputTokens),
        context.run.providerBudget.maxOutputTokens);
    executionRequest.providerBudgetGuard = providerBudgetGuard;
    executionRequest.workspaceRoot = workspaceRoot;
    executionRequest.inspectPolicy = inspectPolicy;
    executionRequest.baselineSnapshot = baseline.value();
    executionRequest.runWorkspaceScopeService = runWorkspaceScopeService_;
    executionRequest.verificationCommandRunner = verificationCommandRunner_;
    executionRequest.verificationRepairPolicy = context.run.verificationRepairPolicy;
    // Planner-created Inspect/Propose tasks are required to carry typed
    // acceptance contracts. Product execution always verifies the combined
    // candidate; model-authored summaries can never opt this out.
    executionRequest.requireCombinedVerification = true;
    // A Manifest always ends in a verification of its combined candidate. A Run that was allowed to plan without proving the project's
    // setup (it might have needed no change) proves it now, before the first worker is paid. The Planner's cost is already spent; the
    // message says what to fix, and a recheck then a new Run goes on from there.
    if (setupProofDeferred && projectReadiness_ != nullptr) {
        const auto setup = projectReadiness_->ensureReady(
            context.project.id, workspaceRoot, *verificationCommandRunner_, stopToken);
        if (!setup.ready) {
            if (setup.failureCode == kProjectSetupCancelled || stopToken.stop_requested()) {
                eventSink.publish(stageEvent(RunEventType::StageCancelled, context.run.id,
                    executionStage.id, 1.0, "Manifest execution cancelled"));
                return {false, RunStatus::Cancelled, "Manifest execution cancelled", totalUsage};
            }
            const std::string message = setup.failureCode + ": " + setup.failureMessage;
            eventSink.publish(stageEvent(RunEventType::StageFailed, context.run.id,
                executionStage.id, 1.0, message));
            return {false, RunStatus::Failed, message, totalUsage};
        }
    }
    // Every temporary copy of this Run (dependency overlays and the combined verification) is built under the one root the Backend
    // validated, and a root that is not usable stops the Run here, before any worker is paid.
    if (scratchProvider_ != nullptr) {
        const auto scratch = scratchProvider_->resolve(workspaceRoot);
        if (!scratch.success()) {
            const std::string message = scratch.error()->code + ": " + scratch.error()->message;
            eventSink.publish(stageEvent(RunEventType::StageFailed, context.run.id,
                executionStage.id, 1.0, message));
            return {false, RunStatus::Failed, message, totalUsage};
        }
        executionRequest.verificationScratchRoot = scratch.value().path;
        executionRequest.dependencyOverlayScratchRoot = scratch.value().path;
    }
    const std::size_t totalTaskCount = planned.manifest->tasks.size();
    auto finishedTaskCount = std::make_shared<std::size_t>(0);
    if (taskExecutions_ != nullptr) {
        // Live per-task progress: persisted as each dispatched task reaches a
        // terminal outcome, still well before the whole manifest finishes.
        // Runs on execute()'s own calling thread (see the callback's own doc
        // comment), never from a task's std::async worker, so publishing a
        // RunEvent here is exactly as safe as every other stage event below.
        executionRequest.onTaskFinished =
            [this, runId = context.run.id, &eventSink, &executionStage, totalTaskCount, finishedTaskCount](
                const std::string& taskId, const ManifestTaskState& state) {
                static_cast<void>(taskExecutions_->upsert(runId, taskId, state));
                ++*finishedTaskCount;
                const double fraction = totalTaskCount > 0
                    ? static_cast<double>(*finishedTaskCount) / static_cast<double>(totalTaskCount)
                    : 0.0;
                eventSink.publish(stageEvent(RunEventType::StageProgress, runId, executionStage.id,
                    0.5 + 0.5 * fraction, taskId + " " + toString(state.status)));
            };
    }

    const auto executed = executor_.execute(std::move(executionRequest), stopToken);
    mergeAccumulatedUsage(totalUsage, executed.usage);

    // AG-18.3 hardening (blocker 1): an append-only progress record for a
    // Run that already has a BootstrapPending decision - BootstrapVerified
    // only when the executor itself confirms an authoritative Passed
    // combined result against the real candidate profile it rediscovered;
    // BootstrapFailed for every other outcome (binding failure, candidate
    // configure/build/lint failure, or any other non-Completed status).
    // Never green by default - absence of a candidate identity is always
    // treated as failed, never silently skipped. Its persistence result is
    // captured (bootstrapTransitionPersisted below) and folded into
    // `completed` further down: a Run whose candidate genuinely verified
    // but whose durable BootstrapVerified transition could not be written
    // must never be reported Completed - already-created proposals are
    // preserved for diagnosis, but the Run itself fails.
    bool bootstrapTransitionPersisted = true;
    std::string bootstrapTransitionPersistenceError;
    if (verificationBootstrapRepository_ != nullptr
        && planned.manifest->bootstrapDeclaration.has_value()) {
        VerificationBootstrapTransition transition;
        transition.id = context.run.id + ":bootstrap_"
            + (executed.bootstrapCandidateIdentity.has_value() ? "verified" : "failed");
        transition.runId = context.run.id;
        transition.transitionedAt = std::chrono::system_clock::now();
        if (executed.bootstrapCandidateIdentity.has_value()) {
            transition.toDisposition = VerificationBootstrapDisposition::BootstrapVerified;
            transition.reasonCode = "verification_environment.bootstrap_verified";
            transition.message = "The candidate overlay's rediscovered profile passed every "
                "deferred verification requirement";
            transition.candidateIdentity = executed.bootstrapCandidateIdentity;
        } else {
            transition.toDisposition = VerificationBootstrapDisposition::BootstrapFailed;
            transition.reasonCode = !executed.executionErrorCode.empty()
                ? executed.executionErrorCode
                : (!executed.verificationErrorCode.empty()
                    ? executed.verificationErrorCode
                    : "verification_environment.bootstrap_verification_failed");
            transition.message = boundedTerminalMessage(
                !executed.executionErrorMessage.empty() ? executed.executionErrorMessage
                    : (!executed.verificationErrorMessage.empty()
                        ? executed.verificationErrorMessage
                        : "The candidate overlay's bootstrap could not be verified"));
        }
        const auto appended = verificationBootstrapRepository_->appendTransition(transition);
        bootstrapTransitionPersisted = appended.success();
        if (!appended.success()) bootstrapTransitionPersistenceError = appended.error()->code;
    }

    if (taskExecutions_ != nullptr) {
        // Final full sync: the callback above never fires for a task
        // cascaded to Skipped (it is never dispatched at all), so this is
        // the only place that state - and the last-attempt snapshot for
        // every other task - is guaranteed durably reflected.
        for (const auto& [taskId, state] : executed.taskStates) {
            static_cast<void>(taskExecutions_->upsert(context.run.id, taskId, state));
        }
        // AG-04: the executor's fully validated ManifestWorkerNoChanges
        // payload only lives in this in-memory outcome once execute()
        // returns - persist it now, alongside the state sync above, or it
        // is lost the moment this function returns. onTaskFinished's live
        // callback intentionally does not carry this (it only sees
        // ManifestTaskState), so this final-sync point is the only place
        // it is ever durably recorded.
        for (const auto& [taskId, evidence] : executed.workerNoChanges) {
            static_cast<void>(taskExecutions_->upsertNoChanges(context.run.id, taskId, evidence));
        }
    }

    std::vector<ChangeSetId> finalProposalIds;
    if (executed.proposalSetVerification.has_value()) {
        finalProposalIds = executed.proposalSetVerification->orderedProposalIds;
    } else {
        for (const auto& task : planned.manifest->tasks) {
            const auto state = executed.taskStates.find(task.id);
            if (state != executed.taskStates.end()
                && state->second.changeSetId.has_value()) {
                finalProposalIds.push_back(*state->second.changeSetId);
            }
        }
    }
    const auto provenance = persistCurrentProvenance(
        executionStage.id, finalProposalIds);

    if (executed.status == ManifestExecutionStatus::Cancelled) {
        eventSink.publish(stageEvent(RunEventType::StageCancelled, context.run.id, executionStage.id,
            0.5, "Manifest execution cancelled"));
        return {false, RunStatus::Cancelled, "Manifest execution cancelled", totalUsage};
    }

    std::string summary = renderDeterministicSummary(*planned.manifest, executed);
    const std::string verificationReport = renderVerificationReport(executed);
    summary += "\n" + verificationReport;
    bool verificationPersisted = false;
    std::string verificationPersistenceError;
    if (artifactService_ == nullptr) {
        verificationPersistenceError = "verification.persistence_unavailable";
    } else {
        const auto recorded = artifactService_->createArtifact(
            context.run.id, executionStage.id, ArtifactKind::Report,
            "manifest-verification.md", "text/markdown", verificationReport);
        if (recorded.success()) {
            verificationPersisted = true;
        } else {
            verificationPersistenceError = recorded.error()->code;
        }
    }
    // AG-14.2: the typed manifest-verification-identity.v1.json artifact is
    // required (not best-effort) whenever combined verification actually
    // ran, mirroring verificationPersisted above - ManifestWorkLedgerService::
    // recordTerminalRun's "verified / no_changes / not_run" invariant
    // depends on this artifact's presence whenever this Run is genuinely
    // Completed. Vacuously true when verification never ran (nothing
    // required to persist); that case is already excluded from `completed`
    // by verificationPassed below.
    bool verificationIdentityPersisted = !executed.proposalSetVerification.has_value();
    std::string verificationIdentityPersistenceError;
    if (executed.proposalSetVerification.has_value()) {
        if (artifactService_ == nullptr) {
            verificationIdentityPersistenceError = "verification.identity_persistence_unavailable";
        } else {
            const auto recordedIdentity = artifactService_->createArtifact(
                context.run.id, executionStage.id, ArtifactKind::Report,
                "manifest-verification-identity.v1.json", "application/json",
                renderManifestProposalSetVerificationJson(*executed.proposalSetVerification));
            if (recordedIdentity.success()) {
                verificationIdentityPersisted = true;
            } else {
                verificationIdentityPersistenceError = recordedIdentity.error()->code;
            }
        }
    }
    // Manifest verification reliability hardening: when a bounded repair
    // wave ran, also persist the untouched PRIMARY verification identity as
    // a separate, distinctly-named artifact - best-effort only (never gates
    // `completed` below), since the final identity above already carries
    // the one authoritative result the ledger/Apply-gate/read model act on;
    // this artifact exists purely so "primary vs final" stays inspectable
    // after the fact, never overwritten by the final one.
    if (executed.verificationRepairAttempted
        && executed.primaryProposalSetVerification.has_value()
        && artifactService_ != nullptr) {
        static_cast<void>(artifactService_->createDiagnosticArtifact(
            context.run.id, executionStage.id, ArtifactKind::Report,
            "manifest-verification-primary.v1.json", "application/json",
            renderManifestProposalSetVerificationJson(
                *executed.primaryProposalSetVerification)));
    }
    const bool verificationPassed = executed.proposalSetVerification.has_value()
        && requiredAcceptancePassed(*executed.proposalSetVerification);
    const bool completed = executed.status == ManifestExecutionStatus::Completed
        && verificationPassed && verificationPersisted && verificationIdentityPersisted
        && provenance.success && bootstrapTransitionPersisted;
    if (!verificationPersistenceError.empty()) {
        summary += "\n## Verification persistence\n\nFailed: "
            + verificationPersistenceError + "\n";
    }
    if (!verificationIdentityPersistenceError.empty()) {
        summary += "\n## Verification identity persistence\n\nFailed: "
            + verificationIdentityPersistenceError + "\n";
    }
    if (!provenance.success) {
        summary += "\n## Workspace provenance\n\nFailed: "
            + provenance.errorCode + "\n";
    }
    if (!bootstrapTransitionPersistenceError.empty()) {
        summary += "\n## Bootstrap transition persistence\n\nFailed: "
            + bootstrapTransitionPersistenceError + "\n";
    }
    std::string terminalMessage = completed
        ? "Manifest execution completed" : executionFailureMessage(executed);
    if (!verificationPersistenceError.empty()) {
        terminalMessage = "Manifest execution failed: "
            + verificationPersistenceError;
    } else if (!verificationIdentityPersistenceError.empty()) {
        terminalMessage = "Manifest execution failed: "
            + verificationIdentityPersistenceError;
    } else if (!provenance.success) {
        terminalMessage = "Manifest execution failed: " + provenance.errorCode;
    } else if (!bootstrapTransitionPersistenceError.empty()) {
        terminalMessage = "Manifest execution failed: "
            + bootstrapTransitionPersistenceError;
    }

    eventSink.publish(stageEvent(
        completed ? RunEventType::StageCompleted : RunEventType::StageFailed,
        context.run.id, executionStage.id, 1.0,
        terminalMessage));

    std::optional<std::string> integrationAdvice;
    if (executed.executionErrorCode.empty() && needsIntegration(executed.status)) {
        ProviderChatRequest integrationRequest;
        integrationRequest.providerId = assignment.providerId;
        integrationRequest.modelId = assignment.modelId;
        integrationRequest.apiModelId = model.apiModelId;
        integrationRequest.systemPrompt =
            "You are the lead Architect reconciling a manifest-driven multi-agent run that did "
            "not complete cleanly. The embedded Manifest proposal-set verification section is "
            "authoritative. A worker task marked Completed means only that its worker reached a "
            "terminal outcome; it never proves build, lint, test, or acceptance success. Report "
            "verification statuses and diagnostics exactly as supplied, explain the concrete "
            "conflicts or incomplete acceptance criteria, and recommend the smallest safe next "
            "step. Never infer success or override a failed/not_verifiable Backend result.";
        integrationRequest.userContent = summary;
        integrationRequest.timeout = assignment.timeout.count() > 0
            ? assignment.timeout : kAgentToolTurnLoopDefaultTurnTimeout;
        integrationRequest.disableTotalTimeout = true;
        integrationRequest.maxOutputTokens = model.maxOutputTokens > 0
            ? static_cast<std::uint32_t>(std::min<std::uint64_t>(
                  model.maxOutputTokens, std::numeric_limits<std::uint32_t>::max()))
            : kAgentToolTurnLoopMaxOutputTokens;

        const auto attemptStartedAt = std::chrono::steady_clock::now();
        std::optional<ModelExecutionId> executionId;
        // Set when the integration call's durable accounting row could not be created
        // or started: the call is then skipped (see below), never made unaccounted.
        std::optional<std::string> integrationAccountingFailure;
        if (modelExecutionService_ != nullptr) {
            CreateModelExecutionRequest create;
            create.source = ModelExecutionSource::Run;
            create.sourceId = context.run.id;
            create.projectId = context.project.id;
            create.providerId = assignment.providerId;
            create.modelId = assignment.modelId;
            create.role = "integration";
            create.position = modelExecutionPositions.value().lastPosition + 1;
            create.requestHash = sha256Hex(
                integrationRequest.systemPrompt + "\n" + integrationRequest.userContent);
            create.contextHash = sha256Hex(integrationRequest.userContent);
            const auto created = modelExecutionService_->createExecution(create);
            if (!created.success()) {
                integrationAccountingFailure = created.error()->code;
            } else {
                executionId = created.value().id;
                const auto started = modelExecutionService_->startExecution(*executionId);
                if (!started.success()) {
                    FinishModelExecutionRequest failedStart;
                    failedStart.status = ModelExecutionStatus::Failed;
                    failedStart.errorCode = started.error()->code;
                    failedStart.errorMessage = started.error()->message;
                    static_cast<void>(
                        modelExecutionService_->finishExecution(*executionId, failedStart));
                    executionId.reset();
                    integrationAccountingFailure = started.error()->code;
                }
            }
        }

        if (integrationAccountingFailure.has_value()) {
            // Fail closed, exactly like a Manifest worker: a billable provider call
            // is never made without a durable accounting row for it. The advice is
            // optional and non-authoritative, so skipping it loses only a report;
            // the deterministic summary above already carries the verification truth.
            summary += "\n## Architect integration advice\n\nUnavailable: ["
                + *integrationAccountingFailure + "] (no provider call was made)\n";
        } else {
            const auto client = clientFactory_.clientFor(assignment.providerId, provider.kind);
            if (client.success()) {
                const auto response = client.value()->send(integrationRequest, stopToken);
                if (response.success()) {
                    accumulateResponseUsage(
                        totalUsage,
                        response.value().usage,
                        model.longContextPricing.thresholdInputTokens);
                    integrationAdvice = renderIntegrationAdvice(response.value().content);
                    if (executionId.has_value()) {
                        FinishModelExecutionRequest finish;
                        finish.status = ModelExecutionStatus::Completed;
                        finish.outputContent = response.value().content;
                        finish.outputPreview = response.value().content.substr(0, 1000);
                        finish.inputTokens = response.value().usage.inputTokens;
                        finish.outputTokens = response.value().usage.outputTokens;
                        finish.cacheCreationInputTokens =
                            response.value().usage.cacheCreationInputTokens;
                        finish.cacheReadInputTokens = response.value().usage.cacheReadInputTokens;
                        finish.cacheCreationDuration =
                            response.value().usage.cacheCreationDuration;
                        // The integration call is ONE provider response: it is classified
                        // against the lead model's long-context threshold by its own
                        // input_tokens (M2b).
                        const auto cost =
                            computeSingleResponseCostMicros(model, response.value().usage);
                        finish.costMicros = cost.costMicros;
                        finish.costPriceKnown = cost.priceKnown;
                        finish.latencyMs = static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - attemptStartedAt).count());
                        static_cast<void>(modelExecutionService_->finishExecution(*executionId, finish));
                    }
                } else if (executionId.has_value()) {
                    // A failed reconciliation call is a diagnostic gap, not a
                    // reason to change the already-decided terminal status: the
                    // deterministic summary above already reflects the truth.
                    summary += "\n## Architect integration\n\nUnavailable: "
                        + response.error()->message + "\n";
                    FinishModelExecutionRequest finish;
                    finish.status = ModelExecutionStatus::Failed;
                    finish.errorCode = response.error()->code;
                    finish.errorMessage = response.error()->message;
                    static_cast<void>(modelExecutionService_->finishExecution(*executionId, finish));
                }
            } else {
                summary += "\n## Architect integration advice\n\nUnavailable: ["
                    + client.error()->code + "]\n";
            }
        }
    }

    if (artifactService_ != nullptr) {
        if (integrationAdvice.has_value()) {
            const auto recorded = artifactService_->createArtifact(
                context.run.id, executionStage.id, ArtifactKind::Report,
                "manifest-integration-advice.md", "text/markdown",
                *integrationAdvice);
            if (recorded.success()) {
                summary += "\n## Architect integration advice\n\n"
                    "A separate non-authoritative recommendation artifact was recorded. "
                    "It cannot override the Backend verification above.\n";
            } else {
                summary += "\n## Architect integration advice\n\nUnavailable: ["
                    + recorded.error()->code + "]\n";
            }
        }
        static_cast<void>(artifactService_->createDiagnosticArtifact(
            context.run.id, executionStage.id, ArtifactKind::Report,
            "manifest-run-report.md", "text/markdown", summary));
    }

    return {completed, completed ? RunStatus::Completed : RunStatus::Failed,
        std::move(terminalMessage), totalUsage, summary};
}

} // namespace ai::application
