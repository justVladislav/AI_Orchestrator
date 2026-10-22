#pragma once

#include "ai/application/IManifestVerificationBaselineNormalizationRepository.hpp"
#include "ai/application/IManifestVerificationContinuationRepository.hpp"
#include "ai/application/IManifestWorkLedgerRepository.hpp"
#include "ai/application/IRunRepository.hpp"
#include "ai/application/ManifestVerificationContinuationService.hpp"
#include "ai/application/QmlLintDiagnosticPolicy.hpp"

namespace ai::application {

struct ManifestVerificationContinuationCriterionView {
    std::string requirementId;
    bool required {false};
    std::string sourceStatus;
    std::string finalStatus;
};

struct ManifestVerificationContinuationFindingView {
    std::string diagnosticCode;
    std::string requirementId;
    std::string relativePath;
    std::string attributionKind;
    std::string ownerTaskId;
    std::string ownerProposalId;
};

// QML-P0: one remaining structured finding from the CHILD's own final
// combined verification, projected only when contentOutcome is
// NeedsManualAction - never a substitute for the attribution findings
// above, which describe the SOURCE's pre-repair diagnostics.
struct ManifestVerificationContinuationRemainingFindingView {
    std::string diagnosticCode;
    std::string requirementId;
    std::string relativePath;
    int line {0};
    int column {0};
    std::string tag;
    // Already leak-safe/bounded - the same sanitized excerpt text every
    // other structured-finding consumer in this codebase uses (see
    // ManifestVerificationDiagnosticFinding's own doc comment), never a raw
    // command line.
    std::string message;
    std::string severity; // "hard" | "advisory"
};

// Backend-owned P5.5 projection. QML receives decisions, identities and safe
// normalized evidence; it never reconstructs eligibility or proposal lineage.
struct ManifestVerificationContinuationView {
    RunId selectedRunId;
    RunId sourceRunId;
    RunId childRunId;
    std::string selectedRole; // source | child
    std::string sourceStatus;
    std::string childStatus;
    bool hasDurableCheckpoint {false};
    std::string checkpointStatus;
    bool hasAttribution {false};
    std::string attributionStatus;
    bool plannerSkipped {false};
    bool canContinue {false};
    // Separate paid-boundary decision. canContinue answers whether a source
    // Run may be prepared; this flag answers whether an already Prepared
    // child has an owner-only repair scope with no prerequisite debt.
    bool canStartRepair {false};
    std::string blockCode;
    std::string blockMessage;
    std::vector<ManifestVerificationContinuationCriterionView> criteria;
    std::vector<ManifestVerificationContinuationFindingView> findings;
    // QML-P0: the child's own final combined verification, classified by
    // ManifestContentOutcome - "undifferentiated" (default; the child's own
    // RunStatus already tells the whole truth) or "needs_manual_action" (a
    // safety-clean, build-clean, Advisory-lint-only result worth a human's
    // review). NEVER authorizes Apply and never overrides childStatus -
    // purely an additional, honest projection. Empty unless the selected
    // child has its own terminal final verification to classify.
    std::string contentOutcome;
    std::vector<ManifestVerificationContinuationRemainingFindingView> remainingFindings;
};

class ManifestVerificationContinuationReadModelService final {
public:
    ManifestVerificationContinuationReadModelService(
        IRunRepository& runs,
        IManifestWorkLedgerRepository& workLedger,
        IManifestVerificationContinuationRepository& continuations,
        IManifestVerificationBaselineNormalizationRepository& baselineNormalizations,
        ManifestVerificationContinuationService& admission);

    [[nodiscard]] common::Result<std::optional<ManifestVerificationContinuationView>>
    viewForRun(const RunId& selectedRunId) const;

private:
    IRunRepository& runs_;
    IManifestWorkLedgerRepository& workLedger_;
    IManifestVerificationContinuationRepository& continuations_;
    IManifestVerificationBaselineNormalizationRepository& baselineNormalizations_;
    ManifestVerificationContinuationService& admission_;
};

} // namespace ai::application
