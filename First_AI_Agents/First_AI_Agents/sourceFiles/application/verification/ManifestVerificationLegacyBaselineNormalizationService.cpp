#include "ai/application/verification/ManifestVerificationLegacyBaselineNormalizationService.hpp"

#include "ai/application/verification/RecordedBaselinePolicy.hpp"
#include "ai/application/workspace/FilesystemPathUtils.hpp"

#include <algorithm>
#include <chrono>
#include <set>

namespace ai::application {
namespace {

bool withinApprovedPrefixes(const std::string& normalizedPath)
{
    return std::any_of(kApprovedLegacyBaselineNormalizationPrefixes.begin(),
        kApprovedLegacyBaselineNormalizationPrefixes.end(),
        [&](const std::string& prefix) { return normalizedPath.starts_with(prefix); });
}

std::string normalizedKey(const std::string& utf8RelativePath)
{
    return detail::normalizedPathKey(detail::pathFromUtf8(utf8RelativePath));
}

std::string normalizedKey(const std::filesystem::path& relativePath)
{
    return detail::normalizedPathKey(relativePath);
}

bool proofMatchesRequest(const ManifestVerificationBaselineNormalization& proof,
    const RunId& sourceRunId, const std::string& durableBaselineFingerprint,
    const WorkspaceBaselineSnapshot& currentBaseline)
{
    return proof.sourceRunId == sourceRunId
        && proof.id == "verification-baseline-normalization:" + sourceRunId
        && proof.policyId == kLegacyBaselineNormalizationPolicyId
        && proof.excludedPrefixes == kApprovedLegacyBaselineNormalizationPrefixes
        && proof.durableBaselineFingerprint == durableBaselineFingerprint
        && proof.normalizedBaselineFingerprint == currentBaseline.fingerprint;
}

} // namespace

ManifestVerificationLegacyBaselineNormalizationService::
ManifestVerificationLegacyBaselineNormalizationService(
    IManifestVerificationBaselineNormalizationRepository& normalizations,
    ArtifactService& artifacts,
    ChangeProposalService& proposals)
    : normalizations_(normalizations), artifacts_(artifacts), proposals_(proposals)
{
}

common::Result<std::optional<ManifestVerificationBaselineNormalization>>
ManifestVerificationLegacyBaselineNormalizationService::normalizeOrEvaluate(
    const RunId& sourceRunId,
    const std::string& durableBaselineFingerprint,
    const WorkspaceBaselineSnapshot& currentBaseline,
    const std::vector<ChangeSetId>& sourceProposalIds)
{
    using ReturnT = common::Result<std::optional<ManifestVerificationBaselineNormalization>>;

    // A stored proof intentionally does not copy proposal bodies. Recheck
    // the exact caller-supplied source set on every invocation, including
    // the idempotent/restart path, so a proof can never be reused after the
    // durable proposal set or its reviewability has changed.
    if (sourceProposalIds.empty()) return ReturnT::ok(std::nullopt);
    std::set<ChangeSetId> seenProposalIds;
    for (const auto& proposalId : sourceProposalIds) {
        if (!seenProposalIds.insert(proposalId).second) return ReturnT::ok(std::nullopt);
        const auto proposal = proposals_.findChangeSet(proposalId);
        if (!proposal.success()) return ReturnT::failure(*proposal.error());
        if (proposal.value().runId != sourceRunId
            || proposal.value().state != ChangeSetState::ReadyForReview) {
            return ReturnT::ok(std::nullopt);
        }
        const auto changes = proposals_.listChanges(proposalId);
        if (!changes.success()) return ReturnT::failure(*changes.error());
        for (const auto& change : changes.value()) {
            if (withinApprovedPrefixes(normalizedKey(change.relativePath))
                || (change.sourcePath.has_value()
                    && withinApprovedPrefixes(normalizedKey(*change.sourcePath)))) {
                return ReturnT::ok(std::nullopt);
            }
        }
    }

    const auto existing = normalizations_.findBySourceRun(sourceRunId);
    if (!existing.success()) return ReturnT::failure(*existing.error());
    if (existing.value().has_value()) {
        // Idempotent, but never stale: a durable proof from an earlier
        // attempt is only reused if it still matches this attempt's fresh
        // capture exactly - never trusted against a workspace that has
        // drifted again (in an approved or unapproved way) since the proof
        // was made.
        if (!proofMatchesRequest(*existing.value(), sourceRunId,
                durableBaselineFingerprint, currentBaseline)) return ReturnT::ok(std::nullopt);
        return ReturnT::ok(*existing.value());
    }

    const auto artifacts = artifacts_.listArtifacts(sourceRunId);
    if (!artifacts.success()) return ReturnT::failure(*artifacts.error());
    const Artifact* baselineArtifact = nullptr;
    for (const auto& artifact : artifacts.value()) {
        if (artifact.name != "workspace-baseline.v1.json") continue;
        if (baselineArtifact != nullptr) {
            return ReturnT::failure({
                "verification_continuation.baseline_normalization_ambiguous_artifact",
                "More than one durable workspace baseline artifact exists for the source Run"});
        }
        baselineArtifact = &artifact;
    }
    if (baselineArtifact == nullptr) return ReturnT::ok(std::nullopt);
    const auto content = artifacts_.getArtifactContent(baselineArtifact->id);
    if (!content.success()) return ReturnT::failure(*content.error());
    const auto durableBaseline = WorkspaceBaselineProvenance::parseJson(content.value());
    if (!durableBaseline.success()) return ReturnT::ok(std::nullopt);
    // The artifact is only evidence for the exact durable identity already
    // bound into the source Work Ledger. A valid-looking baseline artifact
    // for another generation must never mint (and permanently persist) a
    // proof for this Run.
    if (durableBaseline.value().fingerprint != durableBaselineFingerprint)
        return ReturnT::ok(std::nullopt);

    const auto diff = WorkspaceBaselineProvenance::compare(durableBaseline.value(), currentBaseline);
    if (!diff.success()) return ReturnT::ok(std::nullopt);

    std::vector<std::string> normalizedPaths;
    for (const auto* group : {&diff.value().addedPaths, &diff.value().modifiedPaths,
             &diff.value().deletedPaths}) {
        for (const auto& path : *group) {
            if (!withinApprovedPrefixes(normalizedKey(path))) return ReturnT::ok(std::nullopt);
            normalizedPaths.push_back(path);
        }
    }
    if (normalizedPaths.empty()) return ReturnT::ok(std::nullopt);

    const auto pruned = WorkspaceBaselineProvenance::pruneExcludedPrefixes(
        durableBaseline.value(), kApprovedLegacyBaselineNormalizationPrefixes);
    if (!pruned.success()) return ReturnT::failure(*pruned.error());
    if (pruned.value().fingerprint != currentBaseline.fingerprint) return ReturnT::ok(std::nullopt);

    ManifestVerificationBaselineNormalization proof;
    proof.id = "verification-baseline-normalization:" + sourceRunId;
    proof.sourceRunId = sourceRunId;
    proof.policyId = std::string(kLegacyBaselineNormalizationPolicyId);
    proof.durableBaselineFingerprint = durableBaseline.value().fingerprint;
    proof.normalizedBaselineFingerprint = currentBaseline.fingerprint;
    proof.excludedPrefixes = kApprovedLegacyBaselineNormalizationPrefixes;
    std::sort(normalizedPaths.begin(), normalizedPaths.end());
    normalizedPaths.erase(std::unique(normalizedPaths.begin(), normalizedPaths.end()),
        normalizedPaths.end());
    proof.normalizedPaths = std::move(normalizedPaths);
    proof.createdAt = std::chrono::system_clock::now();

    const auto inserted = normalizations_.insert(proof);
    if (!inserted.success()) {
        if (inserted.error()->code == "manifest_verification_baseline_normalization.already_exists") {
            const auto raced = normalizations_.findBySourceRun(sourceRunId);
            if (raced.success() && raced.value().has_value()
                && proofMatchesRequest(*raced.value(), sourceRunId,
                    durableBaselineFingerprint, currentBaseline))
                return ReturnT::ok(*raced.value());
            return ReturnT::ok(std::nullopt);
        }
        return ReturnT::failure(*inserted.error());
    }
    return ReturnT::ok(std::move(proof));
}

common::Result<WorkspaceReadPolicy>
ManifestVerificationLegacyBaselineNormalizationService::policyForRecordedBaseline(
    const RunId& ownerRunId,
    const std::string& expectedFingerprint,
    std::filesystem::path workspaceRoot) const
{
    return RecordedBaselinePolicy::forRun(
        artifacts_, ownerRunId, expectedFingerprint, std::move(workspaceRoot));
}

common::Result<void>
ManifestVerificationLegacyBaselineNormalizationService::persistRecordedBaseline(
    const RunId& runId,
    const std::optional<StageId>& stageId,
    const WorkspaceBaselineSnapshot& snapshot) const
{
    return RecordedBaselinePolicy::persist(artifacts_, runId, stageId, snapshot);
}

} // namespace ai::application
