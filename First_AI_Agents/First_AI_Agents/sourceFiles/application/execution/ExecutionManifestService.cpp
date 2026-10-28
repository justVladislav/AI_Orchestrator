#include "ai/application/execution/ExecutionManifestService.hpp"

#include <chrono>
#include <utility>

namespace ai::application {

ExecutionManifestService::ExecutionManifestService(
    IExecutionManifestRepository& manifests,
    IRunRepository& runs)
    : manifests_(manifests)
    , runs_(runs)
{
}

std::vector<ExecutionManifestFinding> ExecutionManifestService::validate(
    ExecutionManifest candidate,
    const ExecutionManifestPolicy& trustedPolicy) const
{
    candidate.policy = trustedPolicy;
    return ExecutionManifestValidator::validate(candidate, nullptr);
}

common::Result<ExecutionManifest> ExecutionManifestService::freeze(
    ExecutionManifest candidate,
    const ExecutionManifestPolicy& trustedPolicy)
{
    // Never trust a policy envelope emitted by a planner/provider. Only the
    // separately supplied Backend/user policy becomes part of the snapshot.
    candidate.policy = trustedPolicy;
    // AG-18.2: the freeze boundary itself - not merely the Planner's own
    // call path - must reject a manifest whose verification selections were
    // never resolved by ManifestVerificationProfileBinder. Passing true
    // here means any caller of freeze() (now or in the future) can never
    // persist a bare draft with a selected profile/target id but no actual
    // Backend-resolved verificationArguments.
    const auto findings =
        ExecutionManifestValidator::validate(candidate, nullptr, true);
    if (!findings.empty()) {
        return common::Result<ExecutionManifest>::failure(
            {{findings.front().code}, {findings.front().message}});
    }

    const auto run = runs_.findRun(candidate.runId);
    if (!run.success()) {
        return common::Result<ExecutionManifest>::failure(*run.error());
    }
    if (!run.value().has_value()) {
        return common::Result<ExecutionManifest>::failure(
            {{"manifest.run_not_found"}, {"Cannot freeze a manifest for an unknown Run"}});
    }

    candidate.createdAt = std::chrono::system_clock::now();
    return manifests_.insert(std::move(candidate));
}

common::Result<std::optional<ExecutionManifest>> ExecutionManifestService::findForRun(
    const RunId& runId) const
{
    return manifests_.findForRun(runId);
}

} // namespace ai::application
