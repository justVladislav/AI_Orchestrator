#include "ai/application/ManifestVerificationManualHandoffTypes.hpp"

#include <algorithm>
#include <unordered_set>

namespace ai::application {
namespace {

bool bounded(const std::string& value, std::size_t limit = 4096)
{
    return !value.empty() && value.size() <= limit;
}

bool sha256Like(const std::string& value)
{
    return value.size() == 64
        && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
           });
}

bool safeRelativePath(const std::string& value)
{
    if (!bounded(value, 1024) || value.front() == '/' || value.back() == '/'
        || value.find('\\') != std::string::npos || value.find(':') != std::string::npos
        || value.find("//") != std::string::npos) return false;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find('/', start);
        const auto part = value.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

// Safe relative storage key: a single path component (no separators at
// all) so (manualHandoffRoot / storageKey) can never escape the root via a
// crafted multi-segment value - stricter than safeRelativePath, which still
// allows nested segments.
bool safeStorageKey(const std::string& value)
{
    return bounded(value, 128) && value.find('/') == std::string::npos
        && value.find('\\') == std::string::npos && value.find(':') == std::string::npos
        && value != "." && value != "..";
}

} // namespace

common::Result<void> validateManifestVerificationManualHandoff(
    const ManifestVerificationManualHandoff& handoff)
{
    if (handoff.schemaVersion != kManifestVerificationManualHandoffSchemaVersion
        || !bounded(handoff.id) || !bounded(handoff.originManifestRunId)
        || !bounded(handoff.verificationAnchorRunId)
        || !bounded(handoff.sourceProjectId) || !safeStorageKey(handoff.storageKey)
        || !bounded(handoff.workspaceIdentity) || !sha256Like(handoff.baselineFingerprint)
        || !sha256Like(handoff.sourceOverlayFingerprint)) {
        return common::Result<void>::failure({
            "manifest_verification_manual_handoff.invalid_identity",
            "Manual-handoff identity is invalid or outside bounded limits"});
    }
    if (handoff.frozenMembers.empty()
        || handoff.frozenMembers.size() > kManifestVerificationManualHandoffMaxFrozenMembers) {
        return common::Result<void>::failure({
            "manifest_verification_manual_handoff.invalid_identity",
            "Manual-handoff frozen member set is incomplete or outside bounded limits"});
    }
    std::unordered_set<std::string> proposalIds;
    std::unordered_set<std::string> lineageRoots;
    for (const auto& member : handoff.frozenMembers) {
        if (!bounded(member.proposalId) || !bounded(member.owningRunId)
            || !bounded(member.lineageRootProposalId) || !sha256Like(member.contentHash)
            || !proposalIds.insert(member.proposalId).second) {
            return common::Result<void>::failure({
                "manifest_verification_manual_handoff.invalid_identity",
                "Manual-handoff frozen member is malformed or duplicated"});
        }
        // Two frozen members sharing a lineage root would mean the exact set
        // contains two different versions (repair generations) of the same
        // original proposal - never a valid exact set.
        if (!lineageRoots.insert(member.lineageRootProposalId).second) {
            return common::Result<void>::failure({
                "manifest_verification_manual_handoff.duplicate_lineage_root",
                "Two frozen members share the same lineage root proposal"});
        }
    }
    if (handoff.allowedPaths.empty()
        || handoff.allowedPaths.size() > kManifestVerificationManualHandoffMaxAllowedPaths) {
        return common::Result<void>::failure({
            "manifest_verification_manual_handoff.invalid_allowed_paths",
            "A manual handoff must name at least one, bounded, editable path"});
    }
    std::unordered_set<std::string> seenPaths;
    for (const auto& allowed : handoff.allowedPaths) {
        if (!safeRelativePath(allowed.relativePath) || !bounded(allowed.ownerProposalId)
            || !bounded(allowed.ownerTaskId)
            || !proposalIds.contains(allowed.ownerProposalId)
            || !seenPaths.insert(allowed.relativePath).second) {
            return common::Result<void>::failure({
                "manifest_verification_manual_handoff.invalid_allowed_path",
                "An allowed manual-handoff path is unsafe, unbounded, or not owned by a "
                "member of the frozen proposal set"});
        }
    }
    if (handoff.originalSnapshot.schemaVersion != kWorkspaceBaselineSchemaVersion
        || !sha256Like(handoff.originalSnapshot.fingerprint)
        || handoff.originalSnapshot.files.size() > 20'000) {
        return common::Result<void>::failure({
            "manifest_verification_manual_handoff.invalid_snapshot",
            "The frozen manual-handoff candidate snapshot is invalid or outside bounded limits"});
    }
    return common::Result<void>::ok();
}

common::Result<void> validateManifestVerificationManualHandoffAttempt(
    const ManifestVerificationManualHandoffAttempt& attempt)
{
    if (attempt.schemaVersion != kManifestVerificationManualHandoffAttemptSchemaVersion
        || !bounded(attempt.id) || !bounded(attempt.handoffId)
        || !bounded(attempt.verificationAnchorRunId)
        || !bounded(attempt.attemptRunId)
        || attempt.attemptRunId == attempt.verificationAnchorRunId
        || !sha256Like(attempt.editedSnapshotFingerprint)) {
        return common::Result<void>::failure({
            "manifest_verification_manual_handoff.invalid_attempt",
            "Manual-handoff attempt identity is invalid or outside bounded limits"});
    }
    if (attempt.previousAttemptId.has_value() && !bounded(*attempt.previousAttemptId)) {
        return common::Result<void>::failure({
            "manifest_verification_manual_handoff.invalid_attempt",
            "Manual-handoff attempt's previous-attempt reference is malformed"});
    }
    if (attempt.attemptOrdinal == 0) {
        return common::Result<void>::failure({
            "manifest_verification_manual_handoff.invalid_attempt",
            "Manual-handoff attempt ordinal must start at 1"});
    }
    return common::Result<void>::ok();
}

common::Result<void> validateManifestVerificationManualHandoffAttemptResult(
    const ManifestVerificationManualHandoffAttemptResult& result)
{
    if (result.schemaVersion != kManifestVerificationManualHandoffAttemptResultSchemaVersion
        || !bounded(result.id) || !bounded(result.attemptId)
        || !bounded(result.attemptRunId)) {
        return common::Result<void>::failure({
            "manifest_verification_manual_handoff.invalid_attempt_result",
            "Manual-handoff attempt result identity is invalid or outside bounded limits"});
    }
    const bool isRejection = result.kind
        == ManifestVerificationManualHandoffAttemptResultKind::RejectedBeforeVerification;
    const bool bothRejectionFieldsPresent =
        result.rejectionCode.has_value() && result.rejectionMessage.has_value();
    const bool neitherRejectionFieldPresent =
        !result.rejectionCode.has_value() && !result.rejectionMessage.has_value();
    if (isRejection ? !bothRejectionFieldsPresent : !neitherRejectionFieldPresent) {
        return common::Result<void>::failure({
            "manifest_verification_manual_handoff.invalid_attempt_result",
            "rejectionCode/rejectionMessage must be present if and only if kind is "
            "RejectedBeforeVerification"});
    }
    if (isRejection
            && (!bounded(*result.rejectionCode, 256) || !bounded(*result.rejectionMessage))) {
        return common::Result<void>::failure({
            "manifest_verification_manual_handoff.invalid_attempt_result",
            "Manual-handoff attempt result rejection code/message is invalid or outside bounded "
            "limits"});
    }
    return common::Result<void>::ok();
}

} // namespace ai::application
