#pragma once

#include "ai/application/verification/VerificationEnvironmentSnapshotTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>

namespace ai::application {

// Persistence port for AG-18.1's Backend-authored VerificationEnvironmentSnapshot.
// Immutable per Run - the same "insert once, never mutated" contract as
// IRunWorkspaceScopeRepository::freeze/findForRun.
class IVerificationEnvironmentRepository {
public:
    virtual ~IVerificationEnvironmentRepository() = default;

    // A second call for the same runId fails with
    // "verification_environment.already_recorded" rather than silently
    // overwriting a possibly-more-complete original snapshot (both the
    // Sqlite and InMemory implementations enforce this identically).
    // VerificationEnvironmentDiscoveryService treats that specific code as
    // a benign idempotent no-op, the same precedent AG-14/AG-15 already
    // established for their own duplicate-insert codes.
    virtual common::Result<void> recordSnapshot(VerificationEnvironmentSnapshot snapshot) = 0;

    [[nodiscard]] virtual common::Result<std::optional<VerificationEnvironmentSnapshot>>
    findForRun(const RunId& runId) const = 0;
};

} // namespace ai::application
