#include "ai/application/InMemoryVerificationBootstrapRepository.hpp"

#include <algorithm>

namespace ai::application {

namespace {

bool decisionsAgree(
    const VerificationBootstrapDecision& a, const VerificationBootstrapDecision& b)
{
    return a.disposition == b.disposition && a.reasonCode == b.reasonCode
        && a.baselineFingerprint == b.baselineFingerprint
        && a.bootstrapSetupPaths == b.bootstrapSetupPaths;
}

bool candidateIdentitiesAgree(
    const std::optional<VerificationBootstrapCandidateIdentity>& a,
    const std::optional<VerificationBootstrapCandidateIdentity>& b)
{
    if (a.has_value() != b.has_value()) return false;
    if (!a.has_value()) return true;
    return a->overlayFingerprint == b->overlayFingerprint
        && a->candidateProfileId == b->candidateProfileId
        && a->candidateBuildProfileId == b->candidateBuildProfileId
        && a->candidateTargetId == b->candidateTargetId;
}

bool transitionsAgree(
    const VerificationBootstrapTransition& a, const VerificationBootstrapTransition& b)
{
    return a.runId == b.runId && a.schemaVersion == b.schemaVersion
        && a.toDisposition == b.toDisposition && a.reasonCode == b.reasonCode
        && candidateIdentitiesAgree(a.candidateIdentity, b.candidateIdentity);
}

// "Complete" means enough to actually identify a real candidate build - the
// target id is legitimately empty for a cmake_build-only bootstrap (see
// VerificationBootstrapCandidateIdentity's own doc comment), so it is not
// part of completeness.
bool candidateIdentityComplete(const VerificationBootstrapCandidateIdentity& identity)
{
    return !identity.overlayFingerprint.empty() && !identity.candidateProfileId.empty()
        && !identity.candidateBuildProfileId.empty();
}

} // namespace

common::Result<void> InMemoryVerificationBootstrapRepository::recordDecision(
    VerificationBootstrapDecision decision)
{
    std::lock_guard lock(mutex_);
    const auto found = decisions_.find(decision.runId);
    if (found != decisions_.end()) {
        if (decisionsAgree(found->second, decision)) return common::Result<void>::ok();
        return common::Result<void>::failure({"verification_environment.decision_conflict",
            "A different verification bootstrap decision already exists for this Run"});
    }
    decisions_[decision.runId] = std::move(decision);
    return common::Result<void>::ok();
}

common::Result<std::optional<VerificationBootstrapDecision>>
InMemoryVerificationBootstrapRepository::findDecisionForRun(const RunId& runId) const
{
    std::lock_guard lock(mutex_);
    const auto it = decisions_.find(runId);
    if (it == decisions_.end()) {
        return common::Result<std::optional<VerificationBootstrapDecision>>::ok(std::nullopt);
    }
    return common::Result<std::optional<VerificationBootstrapDecision>>::ok(it->second);
}

common::Result<void> InMemoryVerificationBootstrapRepository::appendTransition(
    VerificationBootstrapTransition transition)
{
    std::lock_guard lock(mutex_);
    // AG-18.3 hardening (blocker 1): the owning decision must exist and be
    // BootstrapPending - a transition can never be appended for a Run with
    // no decision, or one already resolved to ActionRequired/Verified/
    // Failed (append-only means exactly one resolving transition ever
    // wins).
    const auto decisionIt = decisions_.find(transition.runId);
    if (decisionIt == decisions_.end()
        || decisionIt->second.disposition != VerificationBootstrapDisposition::BootstrapPending) {
        return common::Result<void>::failure({"verification_environment.decision_conflict",
            "A verification bootstrap transition requires an owning BootstrapPending decision"});
    }
    if (transition.toDisposition != VerificationBootstrapDisposition::BootstrapVerified
        && transition.toDisposition != VerificationBootstrapDisposition::BootstrapFailed) {
        return common::Result<void>::failure({"verification_environment.decision_conflict",
            "Only BootstrapVerified or BootstrapFailed may be appended as a transition"});
    }
    if (transition.toDisposition == VerificationBootstrapDisposition::BootstrapVerified
        && (!transition.candidateIdentity.has_value()
            || !candidateIdentityComplete(*transition.candidateIdentity))) {
        return common::Result<void>::failure({"verification_environment.decision_conflict",
            "BootstrapVerified requires a complete candidate identity"});
    }
    if (transition.toDisposition == VerificationBootstrapDisposition::BootstrapFailed
        && transition.candidateIdentity.has_value()) {
        return common::Result<void>::failure({"verification_environment.decision_conflict",
            "BootstrapFailed must not carry a candidate identity"});
    }

    auto& list = transitions_[transition.runId];
    const auto existing = std::find_if(list.begin(), list.end(),
        [&](const VerificationBootstrapTransition& stored) { return stored.id == transition.id; });
    if (existing != list.end()) {
        if (transitionsAgree(*existing, transition)) return common::Result<void>::ok();
        return common::Result<void>::failure({"verification_environment.decision_conflict",
            "A different verification bootstrap transition already exists with this identity"});
    }
    // Append-only past the first resolving transition: once BootstrapVerified
    // or BootstrapFailed has been recorded for this Run under any identity,
    // a differently-identified transition (which would only ever arise from
    // a genuine logic error, since the identity is deterministic per Run)
    // must not silently coexist with it.
    const bool alreadyResolved = std::any_of(list.begin(), list.end(),
        [](const VerificationBootstrapTransition& stored) {
            return stored.toDisposition == VerificationBootstrapDisposition::BootstrapVerified
                || stored.toDisposition == VerificationBootstrapDisposition::BootstrapFailed;
        });
    if (alreadyResolved) {
        return common::Result<void>::failure({"verification_environment.decision_conflict",
            "This Run's bootstrap lifecycle already has a resolving transition"});
    }
    list.push_back(std::move(transition));
    return common::Result<void>::ok();
}

common::Result<std::vector<VerificationBootstrapTransition>>
InMemoryVerificationBootstrapRepository::listTransitionsForRun(const RunId& runId) const
{
    std::lock_guard lock(mutex_);
    const auto it = transitions_.find(runId);
    if (it == transitions_.end()) {
        return common::Result<std::vector<VerificationBootstrapTransition>>::ok({});
    }
    return common::Result<std::vector<VerificationBootstrapTransition>>::ok(it->second);
}

common::Result<void> InMemoryVerificationBootstrapRepository::recordRetryLink(
    VerificationBootstrapRetryLink link)
{
    std::lock_guard lock(mutex_);
    const auto found = retryLinksByPredecessor_.find(link.predecessorRunId);
    if (found != retryLinksByPredecessor_.end()) {
        // AG-18.3 hardening (blocker 4): the primary key already existing
        // must never itself mean success - only a byte-identical repeat
        // request is idempotent; a conflicting one (a different newRunId or
        // projectId for the same predecessor) fails closed instead of
        // silently keeping whichever link happened to be recorded first.
        if (found->second.newRunId == link.newRunId
            && found->second.projectId == link.projectId) {
            return common::Result<void>::ok();
        }
        return common::Result<void>::failure({"verification_environment.decision_conflict",
            "A different verification bootstrap retry link already exists for this predecessor"});
    }
    retryLinksByPredecessor_[link.predecessorRunId] = std::move(link);
    return common::Result<void>::ok();
}

common::Result<std::optional<VerificationBootstrapRetryLink>>
InMemoryVerificationBootstrapRepository::findRetryLinkForPredecessor(
    const RunId& predecessorRunId) const
{
    std::lock_guard lock(mutex_);
    const auto it = retryLinksByPredecessor_.find(predecessorRunId);
    if (it == retryLinksByPredecessor_.end()) {
        return common::Result<std::optional<VerificationBootstrapRetryLink>>::ok(std::nullopt);
    }
    return common::Result<std::optional<VerificationBootstrapRetryLink>>::ok(it->second);
}

} // namespace ai::application
