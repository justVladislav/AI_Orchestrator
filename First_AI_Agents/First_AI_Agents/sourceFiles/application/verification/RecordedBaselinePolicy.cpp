#include "ai/application/verification/RecordedBaselinePolicy.hpp"

#include "ai/application/workspace/RecordedWorkspaceExclusions.hpp"
#include "ai/application/workspace/WorkspaceBaselineProvenance.hpp"

#include <vector>

namespace ai::application {

namespace {

constexpr const char* kBaselineArtifactName = "workspace-baseline.v1.json";

} // namespace

common::Result<WorkspaceReadPolicy> RecordedBaselinePolicy::forRun(
    const ArtifactService& artifacts,
    const RunId& ownerRunId,
    const std::optional<std::string>& expectedFingerprint,
    std::filesystem::path workspaceRoot)
{
    using Result = common::Result<WorkspaceReadPolicy>;

    // One notion of "a durable identity exists": a non-empty fingerprint. An empty string and std::nullopt both mean there is
    // none; a non-empty one is never optional.
    const bool hasDurableIdentity = expectedFingerprint.has_value() && !expectedFingerprint->empty();

    const auto listed = artifacts.listArtifacts(ownerRunId);
    if (!listed.success()) return Result::failure(*listed.error());

    const Artifact* baselineArtifact = nullptr;
    for (const auto& artifact : listed.value()) {
        if (artifact.name != kBaselineArtifactName) continue;
        if (baselineArtifact != nullptr) {
            return Result::failure({kRecordedBaselineAmbiguousCode,
                "More than one workspace baseline artifact exists for the Run"});
        }
        baselineArtifact = &artifact;
    }
    if (baselineArtifact == nullptr) {
        if (hasDurableIdentity) {
            return Result::failure({kRecordedBaselineMissingCode,
                "The Run's recorded workspace baseline is missing, so the exclusions it froze cannot be restored"});
        }
        return Result::ok(WorkspaceReadPolicy::secureDefaults(std::move(workspaceRoot)));
    }

    const auto content = artifacts.getArtifactContent(baselineArtifact->id);
    if (!content.success()) return Result::failure(*content.error());
    const auto baseline = WorkspaceBaselineProvenance::parseJson(content.value());
    if (!baseline.success()) return Result::failure(*baseline.error());
    if (hasDurableIdentity && baseline.value().fingerprint != *expectedFingerprint) {
        return Result::failure({kRecordedBaselineIdentityMismatchCode,
            "The Run's recorded workspace baseline does not match the baseline fingerprint it is compared against"});
    }
    return RecordedWorkspaceExclusions::policyFor(
        std::move(workspaceRoot), baseline.value().exclusions);
}

common::Result<void> RecordedBaselinePolicy::persist(
    ArtifactService& artifacts,
    const RunId& runId,
    const std::optional<StageId>& stageId,
    const WorkspaceBaselineSnapshot& snapshot)
{
    const auto json = WorkspaceBaselineProvenance::renderJson(snapshot);
    if (!json.success()) return common::Result<void>::failure(*json.error());
    const auto created = artifacts.createArtifact(
        runId, stageId, ArtifactKind::Report, kBaselineArtifactName, "application/json", json.value());
    if (!created.success()) {
        return common::Result<void>::failure({"workspace_baseline.persistence_failed",
            "The workspace baseline could not be stored for the Run: " + created.error()->code});
    }
    return common::Result<void>::ok();
}

} // namespace ai::application
