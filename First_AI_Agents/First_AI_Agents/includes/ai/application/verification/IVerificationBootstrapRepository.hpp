#pragma once

#include "ai/application/verification/VerificationBootstrapTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <vector>

namespace ai::application {

// Persistence port for AG-18.3's bootstrap/action-required lifecycle.
// Decisions are immutable per Run (insert-once, like
// IVerificationEnvironmentRepository::recordSnapshot); transitions are
// append-only; retry links are at most one per predecessor Run. A
// repository outage must surface as a typed failure here, never be
// silently treated by a caller as "no action required" - see
// ManifestWorkflowRunner's own fail-closed handling of this repository.
class IVerificationBootstrapRepository {
public:
    virtual ~IVerificationBootstrapRepository() = default;

    // A second call for the same runId whose disposition/reasonCode/
    // baselineFingerprint/bootstrapSetupPaths match the already-recorded
    // decision is a benign idempotent success (mirrors
    // IVerificationEnvironmentRepository::recordSnapshot's
    // "already_recorded" precedent); a second call that disagrees fails
    // with "verification_environment.decision_conflict" since that would
    // mean two different decisions were made for one immutable Run record.
    virtual common::Result<void> recordDecision(VerificationBootstrapDecision decision) = 0;

    [[nodiscard]] virtual common::Result<std::optional<VerificationBootstrapDecision>>
    findDecisionForRun(const RunId& runId) const = 0;

    virtual common::Result<void> appendTransition(VerificationBootstrapTransition transition) = 0;

    [[nodiscard]] virtual common::Result<std::vector<VerificationBootstrapTransition>>
    listTransitionsForRun(const RunId& runId) const = 0;

    // Idempotent: a second call for the same predecessorRunId returns the
    // already-recorded link's newRunId unchanged rather than creating a
    // second one or failing.
    virtual common::Result<void> recordRetryLink(VerificationBootstrapRetryLink link) = 0;

    [[nodiscard]] virtual common::Result<std::optional<VerificationBootstrapRetryLink>>
    findRetryLinkForPredecessor(const RunId& predecessorRunId) const = 0;
};

} // namespace ai::application
