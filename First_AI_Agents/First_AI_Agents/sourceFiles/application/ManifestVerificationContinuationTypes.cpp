#include "ai/application/ManifestVerificationContinuationTypes.hpp"
#include "ai/application/Sha256.hpp"

#include <algorithm>
#include <sstream>

namespace ai::application {

const char* toString(ManifestVerificationContinuationStatus value) noexcept
{
    switch (value) {
    case ManifestVerificationContinuationStatus::Admitted: return "admitted";
    case ManifestVerificationContinuationStatus::Denied: return "denied";
    }
    return "denied";
}

std::optional<ManifestVerificationContinuationStatus>
manifestVerificationContinuationStatusFromString(std::string_view value) noexcept
{
    if (value == "admitted") return ManifestVerificationContinuationStatus::Admitted;
    if (value == "denied") return ManifestVerificationContinuationStatus::Denied;
    return std::nullopt;
}

const char* toString(ManifestVerificationFindingAttributionKind value) noexcept
{
    switch (value) {
    case ManifestVerificationFindingAttributionKind::SourceProposalOwner:
        return "source_proposal_owner";
    case ManifestVerificationFindingAttributionKind::IntegrationOwner:
        return "integration_owner";
    case ManifestVerificationFindingAttributionKind::PreExistingVerificationDebt:
        return "pre_existing_verification_debt";
    case ManifestVerificationFindingAttributionKind::IgnorableCascade:
        return "ignorable_cascade";
    }
    return "source_proposal_owner";
}

std::optional<ManifestVerificationFindingAttributionKind>
manifestVerificationFindingAttributionKindFromString(std::string_view value) noexcept
{
    if (value == "source_proposal_owner")
        return ManifestVerificationFindingAttributionKind::SourceProposalOwner;
    if (value == "integration_owner")
        return ManifestVerificationFindingAttributionKind::IntegrationOwner;
    if (value == "pre_existing_verification_debt")
        return ManifestVerificationFindingAttributionKind::PreExistingVerificationDebt;
    if (value == "ignorable_cascade")
        return ManifestVerificationFindingAttributionKind::IgnorableCascade;
    return std::nullopt;
}

const char* toString(ManifestVerificationAttributionStatus value) noexcept
{
    switch (value) {
    case ManifestVerificationAttributionStatus::Attributed: return "attributed";
    case ManifestVerificationAttributionStatus::Blocked: return "blocked";
    }
    return "blocked";
}

std::optional<ManifestVerificationAttributionStatus>
manifestVerificationAttributionStatusFromString(std::string_view value) noexcept
{
    if (value == "attributed") return ManifestVerificationAttributionStatus::Attributed;
    if (value == "blocked") return ManifestVerificationAttributionStatus::Blocked;
    return std::nullopt;
}

common::Result<void> validateManifestVerificationAttributionCheckpoint(
    const ManifestVerificationAttributionCheckpoint& checkpoint)
{
    const auto bounded = [](const std::string& value, std::size_t limit = 4096) {
        return !value.empty() && value.size() <= limit;
    };
    const auto safeRelative = [&](const std::string& path) {
        if (!bounded(path) || path.front() == '/' || path.back() == '/'
            || path.find('\\') != std::string::npos || path.find(':') != std::string::npos
            || path.find("//") != std::string::npos) return false;
        std::size_t start = 0;
        while (start <= path.size()) {
            const auto end = path.find('/', start);
            const auto part = path.substr(start, end == std::string::npos
                    ? std::string::npos : end - start);
            if (part.empty() || part == "." || part == "..") return false;
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return true;
    };
    if (checkpoint.schemaVersion != kManifestVerificationAttributionSchemaVersion
        || !bounded(checkpoint.id) || !bounded(checkpoint.sourceRunId)
        || !bounded(checkpoint.sourceProjectId) || !bounded(checkpoint.baselineFingerprint)
        || !bounded(checkpoint.sourceOverlayFingerprint) || checkpoint.findings.size() > 256) {
        return common::Result<void>::failure({
            "manifest_verification_attribution.invalid_identity",
            "Verification-attribution identity is invalid or outside bounded limits"});
    }
    if (checkpoint.status == ManifestVerificationAttributionStatus::Attributed) {
        if (!checkpoint.blockCode.empty() || !checkpoint.blockMessage.empty()
            || checkpoint.findings.empty()) {
            return common::Result<void>::failure({
                "manifest_verification_attribution.invalid_status",
                "An attributed checkpoint requires findings and no block reason"});
        }
    } else if (!bounded(checkpoint.blockCode) || !bounded(checkpoint.blockMessage)
        || !checkpoint.findings.empty()) {
        return common::Result<void>::failure({
            "manifest_verification_attribution.invalid_status",
            "A blocked checkpoint requires one bounded reason and no partial findings"});
    }
    for (const auto& finding : checkpoint.findings) {
        if (!bounded(finding.diagnosticCode) || !bounded(finding.requirementId)
            || finding.line < 0 || finding.column < 0 || finding.tag.size() > 4096) {
            return common::Result<void>::failure({
                "manifest_verification_attribution.invalid_finding",
                "A verification-attribution finding is invalid or outside bounded limits"});
        }
        const bool ownerKind = finding.kind
                == ManifestVerificationFindingAttributionKind::SourceProposalOwner
            || finding.kind == ManifestVerificationFindingAttributionKind::IntegrationOwner;
        if (ownerKind) {
            if (!safeRelative(finding.relativePath) || !finding.ownerTaskId.has_value()
                || !finding.ownerProposalId.has_value()
                || !bounded(*finding.ownerTaskId) || !bounded(*finding.ownerProposalId)) {
                return common::Result<void>::failure({
                    "manifest_verification_attribution.invalid_finding",
                    "An owner-routed finding is missing its safe path or owner identity"});
            }
        } else if (finding.kind
                == ManifestVerificationFindingAttributionKind::PreExistingVerificationDebt) {
            if (!safeRelative(finding.relativePath) || finding.ownerTaskId.has_value()
                || finding.ownerProposalId.has_value()) {
                return common::Result<void>::failure({
                    "manifest_verification_attribution.invalid_finding",
                    "Pre-existing verification debt must carry only a safe relative path"});
            }
        } else if (!finding.relativePath.empty() || finding.ownerTaskId.has_value()
            || finding.ownerProposalId.has_value()) {
            return common::Result<void>::failure({
                "manifest_verification_attribution.invalid_finding",
                "An ignorable cascade must not carry a path or owner"});
        }
    }
    return common::Result<void>::ok();
}

common::Result<void> validateManifestVerificationContinuationInput(
    const ManifestVerificationContinuationInput& input)
{
    const auto bounded = [](const std::string& value, std::size_t limit = 4096) {
        return !value.empty() && value.size() <= limit;
    };
    if (input.schemaVersion != kManifestVerificationContinuationInputSchemaVersion
        || !bounded(input.sourceRunId) || !bounded(input.childRunId)
        || input.sourceRunId == input.childRunId || !bounded(input.sourceProjectId)
        || !bounded(input.attributionId) || !bounded(input.baselineFingerprint)
        || !bounded(input.sourceWorkspaceIdentity)
        || !bounded(input.sourceOverlayFingerprint) || input.inputFingerprint.size() != 64
        || input.plannerRequests != 0 || input.orderedProposalIds.empty()
        || input.orderedProposalIds.size() > 64
        || input.orderedProposalIds.size() != input.orderedProposalContentHashes.size()) {
        return common::Result<void>::failure({
            "manifest_verification_continuation.invalid_input",
            "Continuation input identity is incomplete or outside bounded limits"});
    }
    std::vector<std::string> ids = input.orderedProposalIds;
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
        return common::Result<void>::failure({
            "manifest_verification_continuation.invalid_input",
            "Continuation input contains duplicate proposal identities"});
    }
    for (std::size_t i = 0; i < input.orderedProposalIds.size(); ++i) {
        if (!bounded(input.orderedProposalIds[i])
            || input.orderedProposalContentHashes[i].size() != 64
            || !std::all_of(input.orderedProposalContentHashes[i].begin(),
                input.orderedProposalContentHashes[i].end(), [](unsigned char value) {
                    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
                })) {
            return common::Result<void>::failure({
                "manifest_verification_continuation.invalid_input",
                "Continuation proposal identity is malformed"});
        }
    }
    if (input.inputFingerprint != computeManifestVerificationContinuationInputFingerprint(input)) {
        return common::Result<void>::failure({
            "manifest_verification_continuation.invalid_input_fingerprint",
            "Continuation input fingerprint does not match its immutable identity"});
    }
    return common::Result<void>::ok();
}

common::Result<void> validateManifestVerificationBaselineNormalization(
    const ManifestVerificationBaselineNormalization& normalization)
{
    const auto bounded = [](const std::string& value, std::size_t limit = 4096) {
        return !value.empty() && value.size() <= limit;
    };
    const auto safePrefix = [](const std::string& value) {
        return !value.empty() && value.size() <= 512 && value.front() != '/'
            && value.back() == '/' && value.find('\\') == std::string::npos
            && value.find(':') == std::string::npos && value.find("..") == std::string::npos;
    };
    const auto sha256 = [](const std::string& value) {
        return value.size() == 64
            && std::all_of(value.begin(), value.end(), [](const unsigned char ch) {
                   return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
               });
    };
    const auto safeRelativePath = [](const std::string& value) {
        return !value.empty() && value.size() <= 1024 && value.front() != '/'
            && value.back() != '/' && value.find('\\') == std::string::npos
            && value.find(':') == std::string::npos && value.find("//") == std::string::npos
            && !value.starts_with("../") && value.find("/../") == std::string::npos
            && !value.starts_with("./") && value.find("/./") == std::string::npos;
    };
    if (normalization.schemaVersion != kManifestVerificationBaselineNormalizationSchemaVersion
        || !bounded(normalization.sourceRunId)
        || normalization.id != "verification-baseline-normalization:" + normalization.sourceRunId
        || normalization.policyId != kLegacyBaselineNormalizationPolicyId
        || !sha256(normalization.durableBaselineFingerprint)
        || !sha256(normalization.normalizedBaselineFingerprint)
        || normalization.durableBaselineFingerprint
            == normalization.normalizedBaselineFingerprint
        || normalization.excludedPrefixes
            != kApprovedLegacyBaselineNormalizationPrefixes
        || normalization.normalizedPaths.empty() || normalization.normalizedPaths.size() > 4096) {
        return common::Result<void>::failure({
            "manifest_verification_baseline_normalization.invalid_identity",
            "Baseline-normalization identity is invalid or outside bounded limits"});
    }
    for (const auto& prefix : normalization.excludedPrefixes) {
        if (!safePrefix(prefix)) {
            return common::Result<void>::failure({
                "manifest_verification_baseline_normalization.invalid_prefix",
                "A baseline-normalization excluded prefix is unsafe or not directory-shaped"});
        }
    }
    std::string previousPath;
    for (const auto& path : normalization.normalizedPaths) {
        if (!safeRelativePath(path) || (!previousPath.empty() && previousPath >= path)
            || std::none_of(normalization.excludedPrefixes.begin(),
                normalization.excludedPrefixes.end(), [&](const std::string& prefix) {
                    return path.starts_with(prefix);
                })) {
            return common::Result<void>::failure({
                "manifest_verification_baseline_normalization.invalid_path",
                "A normalized path does not fall under any of its own excluded prefixes"});
        }
        previousPath = path;
    }
    return common::Result<void>::ok();
}

std::string computeManifestVerificationContinuationInputFingerprint(
    const ManifestVerificationContinuationInput& input)
{
    std::ostringstream material;
    material << input.schemaVersion << '\n' << input.sourceRunId << '\n'
             << input.childRunId << '\n' << input.sourceProjectId << '\n'
             << input.attributionId << '\n' << input.baselineFingerprint << '\n'
             << input.sourceWorkspaceIdentity << '\n'
             << input.sourceOverlayFingerprint << '\n';
    for (std::size_t i = 0; i < input.orderedProposalIds.size(); ++i)
        material << input.orderedProposalIds[i] << '\0'
                 << input.orderedProposalContentHashes[i] << '\n';
    material << "planner_requests=" << input.plannerRequests << '\n';
    return sha256Hex(material.str());
}

} // namespace ai::application
