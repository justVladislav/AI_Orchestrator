#pragma once

#include "ai/application/ExecutionManifestTypes.hpp"

#include <vector>

namespace ai::application {

// AG-18.3: the full manifest-level admission predicate a same-Run
// verification-environment bootstrap must satisfy before any requirement
// may legitimately carry verificationBootstrapPending=true into freeze().
// A per-requirement shape check alone (see
// validateExecutionVerificationDraftSelections/validateExecutionVerificationProfileArguments)
// is necessary but not sufficient - this is the deep check that actually
// earns the flag: exactly one declared bootstrap-owning task, exact
// supported repository-root preset filenames, correct ownedPaths/
// plannedCreatePaths role, no masquerading via excluded/reference/deferred/
// required-read paths, and explicit dependency edges from every task that
// needs the new environment to the bootstrap-owning task.
class ManifestBootstrapValidator final {
public:
    ManifestBootstrapValidator() = delete;

    [[nodiscard]] static std::vector<ExecutionManifestFinding> validate(
        const ExecutionManifest& manifest);
};

} // namespace ai::application
