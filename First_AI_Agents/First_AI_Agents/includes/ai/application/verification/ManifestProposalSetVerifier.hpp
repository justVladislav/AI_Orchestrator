#pragma once

#include "ai/application/execution/ExecutionAcceptanceContract.hpp"
#include "ai/application/execution/ExecutionManifestTypes.hpp"
#include "ai/application/verification/ManifestProposalOverlay.hpp"

#include <chrono>
#include <optional>
#include <stop_token>

namespace ai::application {

struct ManifestVerificationCommandRequest {
    std::string commandIdentity;
    std::string executable;
    std::vector<std::string> arguments;
    std::filesystem::path workingDirectory;
    std::chrono::milliseconds timeout {120'000};
    std::size_t maxOutputBytes {256 * 1024};
};

struct ManifestVerificationCommandOutcome {
    bool started {false};
    bool cancelled {false};
    bool timedOut {false};
    bool outputTruncated {false};
    std::optional<int> exitCode;
    std::string boundedOutput;
    std::uint64_t durationMilliseconds {0};
    std::string errorCode;
};

class IManifestVerificationCommandRunner {
public:
    virtual ~IManifestVerificationCommandRunner() = default;
    [[nodiscard]] virtual ManifestVerificationCommandOutcome run(
        const ManifestVerificationCommandRequest& request,
        std::stop_token stopToken) = 0;
};

// Executes only the exact executable/argument vector selected by the Backend
// verifier. It never receives a provider-authored shell string.
class SystemManifestVerificationCommandRunner final
    : public IManifestVerificationCommandRunner {
public:
    [[nodiscard]] ManifestVerificationCommandOutcome run(
        const ManifestVerificationCommandRequest& request,
        std::stop_token stopToken) override;
};

struct ManifestProposalSetVerifierRequest {
    ManifestProposalOverlayDescriptor overlay;
    std::vector<ExecutionManifestTask> tasks;
    // Optional source-only baseline overlay (empty orderedProposalIds) for
    // the same captured workspace baseline as overlay. When present, every
    // qml_lint warning-bearing candidate result is compared with an
    // independently executed baseline result through the one shared
    // resolveQmlLintCriterionDelta() policy. This applies equally to tasks
    // with and without a static QML scope: unchanged debt is non-blocking,
    // while genuinely new findings remain blocking and are classified
    // against the complete frozen Manifest ownership set. The verifier
    // prepares the baseline's required CMake prerequisite lazily before the
    // baseline lint command; unavailable evidence stays fail-closed.
    // Absence preserves the legacy single-candidate behavior for isolated
    // compatibility callers, but production workflow callers must provide
    // the baseline overlay.
    const ManifestProposalOverlayDescriptor* baselineOverlay {nullptr};
    // prepare()/materialize() already execute a complete verification of
    // baselineOverlay for attribution. Supplying a non-owning pointer to
    // that immutable value avoids running the same baseline command twice
    // and avoids copying a large verification aggregate onto Windows stack
    // frames. The verifier checks its full
    // workspace/fingerprint/empty-proposal identity before consuming it.
    // Callers without precomputed evidence leave this empty and use the lazy
    // independent baseline command path above.
    const ManifestProposalSetVerification* baselineVerification {nullptr};
};

class ManifestProposalSetVerifier final {
public:
    [[nodiscard]] static common::Result<ManifestProposalSetVerification> verify(
        const ManifestProposalSetVerifierRequest& request,
        ChangeProposalService& proposals,
        IManifestVerificationCommandRunner& commandRunner,
        std::stop_token stopToken);
};

} // namespace ai::application
