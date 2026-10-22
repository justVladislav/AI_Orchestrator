#include "ai/application/VerificationBootstrapRetryService.hpp"

#include "ai/application/IdGenerator.hpp"
#include "ai/application/VerificationEnvironmentDiscoveryService.hpp"

#include <chrono>

namespace ai::application {

StartRunRequest buildVerificationBootstrapRetryRequest(const Run& predecessor, const RunId& preselectedRunId)
{
    StartRunRequest request;
    request.projectId = predecessor.projectId;
    request.title = predecessor.title;
    request.task = predecessor.task;
    request.workflowId = predecessor.workflowId;
    request.executionProfileId = predecessor.executionProfileId;
    request.maxParallelism = predecessor.maxParallelism;
    request.workspacePath = predecessor.workspacePath;
    request.planningCompletionPolicy = predecessor.planningCompletionPolicy;
    // The user's paid-repair decision is immutable Run input: a linked retry must never turn a Disabled
    // verification repair back into the default one bounded paid wave (every other Retry route already
    // copies it from the Run).
    request.verificationRepairPolicy = predecessor.verificationRepairPolicy;
    request.interfaceContracts = predecessor.interfaceContracts;
    request.preselectedRunId = preselectedRunId;
    return request;
}

namespace {

common::Result<void> requirePresetFileForRetry(
    const Run& predecessor, IProjectRepository& projects)
{
    const auto project = projects.findById(predecessor.projectId);
    if (!project.success()) return common::Result<void>::failure(*project.error());
    if (!project.value().has_value()) {
        return common::Result<void>::failure({
            "verification_environment.retry_source_invalid",
            "The predecessor Run's Project could not be found"});
    }

    const std::filesystem::path workspaceRoot = predecessor.workspacePath.value_or(
        project.value()->rootPath);
    const auto present = VerificationEnvironmentDiscoveryService::
        hasPresetFileAtWorkspaceRoot(workspaceRoot);
    if (!present.success()) return common::Result<void>::failure(*present.error());
    if (!present.value()) {
        return common::Result<void>::failure({
            "verification_environment.setup_still_required",
            "No CMakePresets.json or CMakeUserPresets.json exists in the workspace. "
            "Create or restore a preset, then choose Retry after setup again."});
    }
    return common::Result<void>::ok();
}

} // namespace

VerificationBootstrapRetryService::VerificationBootstrapRetryService(
    IVerificationBootstrapRepository& bootstrapRepository,
    IRunRepository& runRepository,
    IProjectRepository& projectRepository,
    OrchestrationService& orchestration)
    : bootstrapRepository_(bootstrapRepository)
    , runRepository_(runRepository)
    , projectRepository_(projectRepository)
    , orchestration_(orchestration)
{
}

common::Result<RunId> VerificationBootstrapRetryService::createLinkedRetry(
    const RunId& predecessorRunId)
{
    // AG-18.3 hardening (blocker 4): a reservation-based, crash-safe
    // protocol. The retry link (naming the exact reserved newRunId) is
    // always durably persisted BEFORE the linked Run is created - never
    // after - so an interruption between those two steps is always
    // recoverable: a repeated call sees the existing reservation and either
    // finds the Run already created (idempotent success) or resumes
    // creating it with that exact same id, never a fresh one.
    const auto existingLink = bootstrapRepository_.findRetryLinkForPredecessor(predecessorRunId);
    if (!existingLink.success()) {
        return common::Result<RunId>::failure(*existingLink.error());
    }
    if (existingLink.value().has_value()) {
        const auto& link = *existingLink.value();
        const auto existingRun = runRepository_.findRun(link.newRunId);
        if (!existingRun.success()) {
            return common::Result<RunId>::failure(*existingRun.error());
        }
        if (existingRun.value().has_value()) {
            // AG-18.3 hardening (blocker 4): the reservation already
            // produced a Run at this exact id - idempotent success ONLY
            // when that Run's own stored identity still agrees with the
            // reservation (predecessor linkage, project). A stored Run
            // whose identity has diverged (e.g. a corrupted or
            // independently-colliding link row) must never be handed back
            // as if it were this retry's own Run.
            const Run& existing = *existingRun.value();
            if (!existing.parentRunId.has_value() || *existing.parentRunId != predecessorRunId
                || existing.projectId != link.projectId) {
                return common::Result<RunId>::failure(
                    {"verification_environment.retry_link_conflict",
                     "The retry reservation's Run identity does not match the recorded link"});
            }
            return common::Result<RunId>::ok(link.newRunId);
        }
        // Reserved but never created (interrupted between the two steps,
        // or a first attempt whose Run-creation call itself failed) -
        // resume with the exact same reserved id, never a new one.
        const auto source = runRepository_.findRun(predecessorRunId);
        if (!source.success()) {
            return common::Result<RunId>::failure(*source.error());
        }
        if (!source.value().has_value()) {
            return common::Result<RunId>::failure({"verification_environment.retry_source_invalid",
                "The predecessor Run could not be found"});
        }
        const auto presetReady = requirePresetFileForRetry(*source.value(), projectRepository_);
        if (!presetReady.success()) {
            return common::Result<RunId>::failure(*presetReady.error());
        }
        const auto resumed = orchestration_.startEditedRerun(
            predecessorRunId, buildVerificationBootstrapRetryRequest(*source.value(), link.newRunId));
        if (!resumed.success()) {
            return common::Result<RunId>::failure(*resumed.error());
        }
        return common::Result<RunId>::ok(resumed.value());
    }

    const auto decision = bootstrapRepository_.findDecisionForRun(predecessorRunId);
    if (!decision.success()) {
        return common::Result<RunId>::failure(*decision.error());
    }
    if (!decision.value().has_value()
        || decision.value()->disposition != VerificationBootstrapDisposition::ActionRequired) {
        return common::Result<RunId>::failure({"verification_environment.retry_source_invalid",
            "A linked retry can only be created from a Run with a durable ActionRequired "
            "disposition"});
    }

    const auto source = runRepository_.findRun(predecessorRunId);
    if (!source.success()) {
        return common::Result<RunId>::failure(*source.error());
    }
    if (!source.value().has_value()) {
        return common::Result<RunId>::failure({"verification_environment.retry_source_invalid",
            "The predecessor Run could not be found"});
    }
    const Run& predecessor = *source.value();

    // ActionRequired makes the command available, but it is not evidence
    // that the operator has actually completed setup since that immutable
    // decision was recorded. Check the live workspace at the command
    // boundary before persisting a reservation or creating a Run. This is a
    // deliberately cheap presence check: the new Run remains solely
    // responsible for full CMake discovery and baseline binding.
    const auto presetReady = requirePresetFileForRetry(predecessor, projectRepository_);
    if (!presetReady.success()) {
        return common::Result<RunId>::failure(*presetReady.error());
    }

    // Reserve first: persist the link naming a freshly-generated newRunId
    // BEFORE any Run is created. If two callers race here, the repository's
    // own predecessor-keyed uniqueness ensures only one reservation wins;
    // the other fails closed on this call and recovers on its next retry by
    // finding the winner's already-persisted reservation above.
    const RunId reservedRunId = generateUuid();
    VerificationBootstrapRetryLink link;
    link.predecessorRunId = predecessorRunId;
    link.newRunId = reservedRunId;
    link.projectId = predecessor.projectId;
    link.reason = "action_required_linked_retry";
    link.createdAt = std::chrono::system_clock::now();
    const auto recorded = bootstrapRepository_.recordRetryLink(link);
    if (!recorded.success()) {
        return common::Result<RunId>::failure(*recorded.error());
    }

    const auto started = orchestration_.startEditedRerun(
        predecessorRunId, buildVerificationBootstrapRetryRequest(predecessor, reservedRunId));
    if (!started.success()) {
        // The reservation survives - a later retry resumes it via the
        // existingLink branch above instead of orphaning a second
        // reservation.
        return common::Result<RunId>::failure(*started.error());
    }

    return common::Result<RunId>::ok(started.value());
}

} // namespace ai::application
