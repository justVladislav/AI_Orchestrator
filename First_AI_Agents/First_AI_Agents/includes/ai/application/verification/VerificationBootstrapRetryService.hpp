#pragma once

#include "ai/application/projects/IProjectRepository.hpp"
#include "ai/application/verification/IVerificationBootstrapRepository.hpp"
#include "ai/application/OrchestrationService.hpp"
#include "ai/common/Result.hpp"

namespace ai::application {

// The request a linked retry starts: the predecessor's own immutable inputs (workflow, profile, parallelism,
// workspace, planning policy, PC-2 interface contracts) with the reserved id. Exposed so the inheritance
// is unit-testable without a full manifest-profile fixture.
[[nodiscard]] StartRunRequest buildVerificationBootstrapRetryRequest(
    const Run& predecessor, const RunId& preselectedRunId);

// AG-18.3: the Backend-level operation that creates a new Run linked to a
// predecessor Run's durable ActionRequired disposition, for later UI use
// (AG-18.4). Never resumes or mutates the predecessor Run; the new Run
// goes through the exact same normal new-Run startup path as any other Run
// (fresh baseline capture, fresh AG-18.1 discovery). Immediately before it
// reserves/creates that Run, this service performs the cheap provider-free
// command-boundary check that a well-known preset file now exists. Full
// validity remains the fresh Run's own baseline-bound discovery contract.
class VerificationBootstrapRetryService final {
public:
    VerificationBootstrapRetryService(
        IVerificationBootstrapRepository& bootstrapRepository,
        IRunRepository& runRepository,
        IProjectRepository& projectRepository,
        OrchestrationService& orchestration);

    // Fails with "verification_environment.retry_source_invalid" unless
    // predecessorRunId names a Run with a durably-recorded ActionRequired
    // decision. A second call for the same predecessor is idempotent - it
    // returns the already-created linked Run's id rather than creating a
    // second one.
    [[nodiscard]] common::Result<RunId> createLinkedRetry(const RunId& predecessorRunId);

private:
    IVerificationBootstrapRepository& bootstrapRepository_;
    IRunRepository& runRepository_;
    IProjectRepository& projectRepository_;
    OrchestrationService& orchestration_;
};

} // namespace ai::application
