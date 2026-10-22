#include "ai/application/InMemoryManifestVerificationManualHandoffRepository.hpp"

#include <algorithm>

namespace ai::application {

common::Result<void> InMemoryManifestVerificationManualHandoffRepository::insertHandoff(
    ManifestVerificationManualHandoff handoff)
{
    const auto valid = validateManifestVerificationManualHandoff(handoff);
    if (!valid.success()) return valid;
    std::lock_guard lock(mutex_);
    if (handoffsByAnchorRun_.contains(handoff.verificationAnchorRunId)) {
        return common::Result<void>::failure({
            "manifest_verification_manual_handoff.already_exists",
            "A manual handoff already exists for this verification anchor Run"});
    }
    for (const auto& member : handoff.frozenMembers) {
        if (handoffIdByMemberProposalId_.contains(member.proposalId)) {
            return common::Result<void>::failure({
                "manifest_verification_manual_handoff.member_already_claimed",
                "A frozen member proposal is already claimed by a different manual handoff"});
        }
    }
    handoffAnchorRunById_.emplace(handoff.id, handoff.verificationAnchorRunId);
    for (const auto& member : handoff.frozenMembers) {
        handoffIdByMemberProposalId_.emplace(member.proposalId, handoff.id);
    }
    handoffsByAnchorRun_.emplace(handoff.verificationAnchorRunId, std::move(handoff));
    return common::Result<void>::ok();
}

common::Result<std::optional<ManifestVerificationManualHandoff>>
InMemoryManifestVerificationManualHandoffRepository::findHandoffByAnchorRun(
    const RunId& verificationAnchorRunId) const
{
    std::lock_guard lock(mutex_);
    const auto found = handoffsByAnchorRun_.find(verificationAnchorRunId);
    return common::Result<std::optional<ManifestVerificationManualHandoff>>::ok(
        found == handoffsByAnchorRun_.end() ? std::nullopt
            : std::optional<ManifestVerificationManualHandoff>(found->second));
}

common::Result<std::optional<ManifestVerificationManualHandoff>>
InMemoryManifestVerificationManualHandoffRepository::findHandoffById(
    const std::string& handoffId) const
{
    std::lock_guard lock(mutex_);
    const auto id = handoffAnchorRunById_.find(handoffId);
    if (id == handoffAnchorRunById_.end()) {
        return common::Result<std::optional<ManifestVerificationManualHandoff>>::ok(std::nullopt);
    }
    const auto found = handoffsByAnchorRun_.find(id->second);
    return common::Result<std::optional<ManifestVerificationManualHandoff>>::ok(
        found == handoffsByAnchorRun_.end() ? std::nullopt
            : std::optional<ManifestVerificationManualHandoff>(found->second));
}

common::Result<std::optional<ManifestVerificationManualHandoff>>
InMemoryManifestVerificationManualHandoffRepository::findHandoffByMemberProposalId(
    const ChangeSetId& proposalId) const
{
    std::lock_guard lock(mutex_);
    const auto id = handoffIdByMemberProposalId_.find(proposalId);
    if (id == handoffIdByMemberProposalId_.end()) {
        return common::Result<std::optional<ManifestVerificationManualHandoff>>::ok(std::nullopt);
    }
    const auto anchorRun = handoffAnchorRunById_.find(id->second);
    if (anchorRun == handoffAnchorRunById_.end()) {
        return common::Result<std::optional<ManifestVerificationManualHandoff>>::ok(std::nullopt);
    }
    const auto found = handoffsByAnchorRun_.find(anchorRun->second);
    return common::Result<std::optional<ManifestVerificationManualHandoff>>::ok(
        found == handoffsByAnchorRun_.end() ? std::nullopt
            : std::optional<ManifestVerificationManualHandoff>(found->second));
}

common::Result<std::vector<ManifestVerificationManualHandoff>>
InMemoryManifestVerificationManualHandoffRepository::listAllHandoffs() const
{
    std::lock_guard lock(mutex_);
    std::vector<ManifestVerificationManualHandoff> result;
    result.reserve(handoffsByAnchorRun_.size());
    for (const auto& [anchorRunId, handoff] : handoffsByAnchorRun_) result.push_back(handoff);
    return common::Result<std::vector<ManifestVerificationManualHandoff>>::ok(std::move(result));
}

common::Result<void> InMemoryManifestVerificationManualHandoffRepository::discardHandoff(
    const std::string& handoffId)
{
    std::lock_guard lock(mutex_);
    const auto idIt = handoffAnchorRunById_.find(handoffId);
    if (idIt == handoffAnchorRunById_.end()) {
        return common::Result<void>::failure({
            "manifest_verification_manual_handoff.not_found",
            "No manual handoff exists with this id"});
    }
    const RunId anchorRunId = idIt->second;
    const auto handoffIt = handoffsByAnchorRun_.find(anchorRunId);
    if (handoffIt != handoffsByAnchorRun_.end()) {
        // Release the GLOBAL UNIQUE(proposal_id) claim on every frozen
        // member - the entire point of this call, per the schema's own
        // real invariant.
        for (const auto& member : handoffIt->second.frozenMembers) {
            handoffIdByMemberProposalId_.erase(member.proposalId);
        }
        handoffsByAnchorRun_.erase(handoffIt);
    }
    handoffAnchorRunById_.erase(idIt);
    for (auto it = attemptOrder_.begin(); it != attemptOrder_.end();) {
        if (it->handoffId == handoffId) {
            attemptsByRunId_.erase(it->attemptRunId);
            attemptResultsByAttemptId_.erase(it->id);
            it = attemptOrder_.erase(it);
        } else {
            ++it;
        }
    }
    return common::Result<void>::ok();
}

common::Result<void> InMemoryManifestVerificationManualHandoffRepository::insertAttempt(
    ManifestVerificationManualHandoffAttempt attempt)
{
    const auto valid = validateManifestVerificationManualHandoffAttempt(attempt);
    if (!valid.success()) return valid;
    std::lock_guard lock(mutex_);
    if (attemptsByRunId_.contains(attempt.attemptRunId)) {
        return common::Result<void>::failure({
            "manifest_verification_manual_handoff.attempt_already_exists",
            "A manual-handoff attempt already exists for this attempt Run"});
    }
    const auto duplicateOrdinal = std::find_if(
        attemptOrder_.begin(), attemptOrder_.end(), [&](const auto& existing) {
            return existing.handoffId == attempt.handoffId
                && existing.attemptOrdinal == attempt.attemptOrdinal;
        });
    if (duplicateOrdinal != attemptOrder_.end()) {
        return common::Result<void>::failure({
            "manifest_verification_manual_handoff.attempt_already_exists",
            "This manual-handoff attempt ordinal is already reserved"});
    }
    attemptsByRunId_.emplace(attempt.attemptRunId, attempt);
    attemptOrder_.push_back(std::move(attempt));
    return common::Result<void>::ok();
}

common::Result<std::optional<ManifestVerificationManualHandoffAttempt>>
InMemoryManifestVerificationManualHandoffRepository::findAttemptByRunId(
    const RunId& attemptRunId) const
{
    std::lock_guard lock(mutex_);
    const auto found = attemptsByRunId_.find(attemptRunId);
    return common::Result<std::optional<ManifestVerificationManualHandoffAttempt>>::ok(
        found == attemptsByRunId_.end() ? std::nullopt
            : std::optional<ManifestVerificationManualHandoffAttempt>(found->second));
}

common::Result<std::optional<ManifestVerificationManualHandoffAttempt>>
InMemoryManifestVerificationManualHandoffRepository::findAttemptByFingerprint(
    const std::string& handoffId, const std::string& editedSnapshotFingerprint) const
{
    std::lock_guard lock(mutex_);
    std::optional<ManifestVerificationManualHandoffAttempt> newest;
    for (const auto& attempt : attemptOrder_) {
        if (attempt.handoffId == handoffId
                && attempt.editedSnapshotFingerprint == editedSnapshotFingerprint
                && (!newest.has_value()
                    || attempt.attemptOrdinal > newest->attemptOrdinal)) {
            newest = attempt;
        }
    }
    return common::Result<std::optional<ManifestVerificationManualHandoffAttempt>>::ok(
        std::move(newest));
}

common::Result<std::vector<ManifestVerificationManualHandoffAttempt>>
InMemoryManifestVerificationManualHandoffRepository::listAttemptsForHandoff(
    const std::string& handoffId) const
{
    std::lock_guard lock(mutex_);
    std::vector<ManifestVerificationManualHandoffAttempt> result;
    for (const auto& attempt : attemptOrder_) {
        if (attempt.handoffId == handoffId) result.push_back(attempt);
    }
    std::sort(result.begin(), result.end(),
        [](const ManifestVerificationManualHandoffAttempt& left,
            const ManifestVerificationManualHandoffAttempt& right) {
            return left.attemptOrdinal < right.attemptOrdinal;
        });
    return common::Result<std::vector<ManifestVerificationManualHandoffAttempt>>::ok(
        std::move(result));
}

common::Result<std::vector<ManifestVerificationManualHandoffAttempt>>
InMemoryManifestVerificationManualHandoffRepository::listAllAttempts() const
{
    std::lock_guard lock(mutex_);
    return common::Result<std::vector<ManifestVerificationManualHandoffAttempt>>::ok(
        attemptOrder_);
}

common::Result<void> InMemoryManifestVerificationManualHandoffRepository::insertAttemptResult(
    ManifestVerificationManualHandoffAttemptResult result)
{
    const auto valid = validateManifestVerificationManualHandoffAttemptResult(result);
    if (!valid.success()) return valid;
    std::lock_guard lock(mutex_);
    if (attemptResultsByAttemptId_.contains(result.attemptId)) {
        return common::Result<void>::failure({
            "manifest_verification_manual_handoff.attempt_result_already_exists",
            "A typed result already exists for this manual-handoff attempt"});
    }
    attemptResultsByAttemptId_.emplace(result.attemptId, std::move(result));
    return common::Result<void>::ok();
}

common::Result<std::optional<ManifestVerificationManualHandoffAttemptResult>>
InMemoryManifestVerificationManualHandoffRepository::findAttemptResultByAttemptId(
    const std::string& attemptId) const
{
    std::lock_guard lock(mutex_);
    const auto found = attemptResultsByAttemptId_.find(attemptId);
    return common::Result<std::optional<ManifestVerificationManualHandoffAttemptResult>>::ok(
        found == attemptResultsByAttemptId_.end() ? std::nullopt
            : std::optional<ManifestVerificationManualHandoffAttemptResult>(found->second));
}

} // namespace ai::application
