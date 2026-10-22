#include "ai/application/RunPathClaimRegistry.hpp"

namespace ai::application {

RunPathClaimRegistry::ClaimOutcome RunPathClaimRegistry::tryClaim(
    const RunId& runId,
    const ChangeSetId& changeSetId,
    const std::vector<PathClaim>& claims)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& runClaims = claimsByRun_[runId];

    // All-or-nothing: first check every claim before registering any of
    // them, so a partial collision never leaves this changeSetId owning a
    // subset of its requested paths.
    for (const auto& claim : claims) {
        const auto found = runClaims.find(claim.relativePathGeneric);
        if (found != runClaims.end() && found->second.changeSetId != changeSetId) {
            return {false, found->second.changeId, found->second.changeSetId};
        }
    }

    for (const auto& claim : claims) {
        runClaims[claim.relativePathGeneric] = Claim {changeSetId, claim.changeId};
    }
    return {true, {}, {}};
}

void RunPathClaimRegistry::release(const RunId& runId, const ChangeSetId& changeSetId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto runFound = claimsByRun_.find(runId);
    if (runFound == claimsByRun_.end()) return;

    auto& runClaims = runFound->second;
    for (auto it = runClaims.begin(); it != runClaims.end();) {
        if (it->second.changeSetId == changeSetId) {
            it = runClaims.erase(it);
        } else {
            ++it;
        }
    }
}

RunPathClaimRegistry::ClaimOutcome RunPathClaimRegistry::transferClaim(
    const RunId& runId,
    const ChangeSetId& fromChangeSetId,
    const ChangeSetId& toChangeSetId,
    const std::vector<PathClaim>& claims)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& runClaims = claimsByRun_[runId];

    // Snapshot fromChangeSetId's current claims (path -> Claim) so a failed
    // transfer can restore them exactly, then remove them from the live map
    // under this SAME lock acquisition - no other thread can ever observe a
    // state where fromChangeSetId's paths are free but toChangeSetId has not
    // yet claimed them (or vice versa).
    std::vector<std::pair<std::string, Claim>> saved;
    for (const auto& [path, claim] : runClaims) {
        if (claim.changeSetId == fromChangeSetId) {
            saved.emplace_back(path, claim);
        }
    }
    for (const auto& [path, unusedClaim] : saved) {
        static_cast<void>(unusedClaim);
        runClaims.erase(path);
    }

    // Manifest verification reliability hardening follow-up: the exact
    // snapshot returned to the caller either way (see
    // ClaimOutcome::previousClaims's own doc comment) - built once, from
    // `saved` alone, so it always reflects exactly what fromChangeSetId
    // actually held before this call, never a separately reconstructed
    // approximation. Empty when fromChangeSetId held nothing.
    std::vector<PathClaim> previousClaims;
    previousClaims.reserve(saved.size());
    for (const auto& [path, savedClaim] : saved) {
        previousClaims.push_back({path, savedClaim.changeId});
    }

    // All-or-nothing, exactly like tryClaim: check every requested path
    // before claiming any of them. A path already held by toChangeSetId
    // itself (re-transferring the same set) is a no-op success for that
    // path, matching tryClaim's own idempotent-reclaim contract.
    for (const auto& claim : claims) {
        const auto found = runClaims.find(claim.relativePathGeneric);
        if (found != runClaims.end() && found->second.changeSetId != toChangeSetId) {
            // Conflict with a third changeset - restore fromChangeSetId's
            // original claims exactly and fail closed.
            for (const auto& [path, savedClaim] : saved) {
                runClaims[path] = savedClaim;
            }
            return {false, found->second.changeId, found->second.changeSetId, previousClaims};
        }
    }

    for (const auto& claim : claims) {
        runClaims[claim.relativePathGeneric] = Claim {toChangeSetId, claim.changeId};
    }
    return {true, {}, {}, previousClaims};
}

void RunPathClaimRegistry::releaseRun(const RunId& runId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    claimsByRun_.erase(runId);
}

} // namespace ai::application
