#pragma once

#include "ai/application/catalog/ExecutionProfileTypes.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

// AG-17.7A: the compatibility adapter between the legacy fixed-stage roster runner
// (`execution-profile-v1`, ExecutionProfileWorkflowRunner) and the converged
// Manifest workflow (`execution-manifest-v1`).
//
// What it IS: a pure, provider-free decision function. From the IMMUTABLE source
// snapshot of a finished Run (its persisted workflow id and its frozen
// StageModelAssignment list - never a saved profile that may have changed since) it
// answers "can this roster be re-launched on the Manifest workflow, and with which
// roster?". The answer is a ConversionPlan: a roster and a target workflow id for a
// NEW Run request. That request then goes through the ordinary path - WorkflowRouter
// resolve, Planner, validate, bind, freeze.
//
// What it is NOT (the contract later slices must keep):
//   - it never builds an ExecutionManifestDraft or a Manifest and never freezes a
//     task: the Planner alone proposes tasks;
//   - it never rewrites a historical Run or its workflowId (history stays immutable;
//     the retired `read-only-analysis-v1` precedent in OrchestrationService is the
//     same pattern);
//   - it never starts, retries or schedules anything (no automatic paid Retry): the
//     caller decides, and only an explicit user action may launch the new Run;
//   - it reads no repository, so it cannot see provider/model catalog drift - the
//     router validates the plan against the current catalog when the new Run starts.
//
// Policy (legacy-roster-adapter.v1). Only two legacy shapes have a Manifest
// counterpart; the others deliberately stay on the legacy runner (user decision
// 2026-10-02, they are text pipelines the Manifest workflow cannot express):
//   lone workspace lead  -> Manifest single-assignment   (shape S2)
//   proposal roster      -> Manifest roster              (shape S4: one lead without
//                           workspace access + >= 1 worker, EVERY worker with
//                           workspace access)
//   lone text architect, text roster, mixed roster -> stay legacy (S1/S3)
// A conversion changes semantics on purpose (a roster is an available pool, one
// attempt per task, no provider fallback, the Planner declares dependencies), so
// every setting the converted Run will not act on is reported in `dropped` and must
// be shown to the user at the moment of conversion.
//
// The decision is auditable: `policyId` is versioned (a rule change needs a new id),
// `reasonCode` is a stable machine code and `sourceSnapshotFingerprint` binds the
// decision to the exact source snapshot it was made from.
inline constexpr const char* kLegacyRosterAdapterPolicyId = "legacy-roster-adapter.v1";

// The stable machine codes of a CONVERSION decision (the only two a durable conversion record
// may carry). The other reason codes describe decisions that create no Run.
inline constexpr const char* kLegacyRosterReasonConvertedLoneWorkspaceLead =
    "legacy_roster.converted_lone_workspace_lead";
inline constexpr const char* kLegacyRosterReasonConvertedProposalRoster =
    "legacy_roster.converted_proposal_roster";

// The retired single-agent runner whose rows are still retryable; its snapshot is
// first restored to the semantics of the runner it was written for (see
// migrateRetiredReadOnlyAssignments) and then classified like any other roster.
inline constexpr const char* kRetiredReadOnlyAnalysisWorkflowId = "read-only-analysis-v1";

enum class LegacyRosterShape {
    Unclassified,
    LoneWorkspaceLead,  // S2
    ProposalRoster,     // S4
    LoneTextArchitect,  // S1
    TextRoster,         // S3
    MixedRoster,        // workspace and text workers together
};

enum class LegacyRosterDisposition {
    ConvertToManifestSingle,
    ConvertToManifestRoster,
    StayLegacy,       // a legitimate legacy shape: nothing to convert, nothing wrong
    NotConvertible,   // the snapshot itself cannot be converted (see reasonCode)
};

[[nodiscard]] const char* toString(LegacyRosterShape shape) noexcept;
[[nodiscard]] const char* toString(LegacyRosterDisposition disposition) noexcept;

// A setting the legacy roster configured that the converted Run will not act on.
struct LegacyRosterDroppedSetting {
    std::string memberId;
    // "maxAttempts" | "fallback" | "dependencies" | "instructions"
    std::string setting;
    // A bounded, human-readable rendering of the legacy value. Instruction text is
    // never copied (only its length), so the record cannot leak or bloat.
    std::string legacyValue;
    std::string reason;
};

struct LegacyRosterConversionPlan {
    std::string policyId;
    std::string sourceWorkflowId;
    // SHA-256 over the canonical form of (policy id, source workflow id, source
    // snapshot in position order). Independent of the order the snapshot was given in.
    std::string sourceSnapshotFingerprint;
    LegacyRosterShape shape {LegacyRosterShape::Unclassified};
    LegacyRosterDisposition disposition {LegacyRosterDisposition::NotConvertible};
    // Stable machine code of the decision, whatever the disposition
    // ("legacy_roster.converted_proposal_roster", "legacy_roster.ambiguous_..."...).
    std::string reasonCode;
    std::string reasonMessage;
    // Non-empty only for a conversion: the workflow id of the NEW Run request and the
    // roster to submit as its assignmentOverrides.
    std::string targetWorkflowId;
    std::vector<StageModelAssignment> roster;
    // AG-17.7C: SHA-256 over the canonical form of `roster` (see
    // LegacyRosterAdapter::rosterFingerprint); empty unless the plan converts. It is what a
    // durable conversion record certifies was actually frozen on the new Run.
    std::string targetRosterFingerprint;
    std::vector<LegacyRosterDroppedSetting> dropped;

    [[nodiscard]] bool converts() const noexcept
    {
        return disposition == LegacyRosterDisposition::ConvertToManifestSingle
            || disposition == LegacyRosterDisposition::ConvertToManifestRoster;
    }
};

class LegacyRosterAdapter final {
public:
    LegacyRosterAdapter() = delete;

    // The only entry point that decides. `sourceWorkflowId` and `sourceSnapshot` are
    // the persisted, immutable fields of the source Run (Run::workflowId and
    // Run::assignmentSnapshot). Accepted source workflows: `execution-profile-v1` and
    // the retired `read-only-analysis-v1`; any other workflow (Mock, Manifest, unknown)
    // is NotConvertible.
    [[nodiscard]] static LegacyRosterConversionPlan buildConversionPlan(
        std::string_view sourceWorkflowId,
        const std::vector<StageModelAssignment>& sourceSnapshot);

    // Restores a retired read-only Run's snapshot to the semantics of the runner it
    // was written for: a single workspace-tool agent that is the lead, one attempt, no
    // fallback. Persisted rows predate StageModelAssignment::workspaceAccess (schema
    // v23 reads them as false), so this is what makes them workspace-enabled again.
    [[nodiscard]] static std::vector<StageModelAssignment> migrateRetiredReadOnlyAssignments(
        std::vector<StageModelAssignment> assignments);

    // The canonical fingerprint bound into every plan (exposed for audit and tests).
    [[nodiscard]] static std::string sourceSnapshotFingerprint(
        std::string_view sourceWorkflowId,
        const std::vector<StageModelAssignment>& sourceSnapshot);

    // AG-17.7C: SHA-256 over the canonical form of a roster (members in position order, every
    // field a Run snapshot persists). Independent of the order the roster was handed over in.
    [[nodiscard]] static std::string rosterFingerprint(
        const std::vector<StageModelAssignment>& roster);

    // AG-17.7C: the fingerprint of what a user is asked to confirm (and of what a record
    // certifies). It binds the confirmation to ONE source Run and ONE decision, so it cannot be
    // carried over to another Run that merely has an identical snapshot:
    //   sourceRunId, sourceWorkflowId, policyId, reasonCode, sourceSnapshotFingerprint,
    //   targetWorkflowId, targetRosterFingerprint and the canonical `dropped` list.
    [[nodiscard]] static std::string confirmationFingerprint(
        std::string_view sourceRunId,
        std::string_view sourceWorkflowId,
        std::string_view policyId,
        std::string_view reasonCode,
        std::string_view sourceSnapshotFingerprint,
        std::string_view targetWorkflowId,
        std::string_view targetRosterFingerprint,
        const std::vector<LegacyRosterDroppedSetting>& dropped);
    // The same for a plan built from the source Run `sourceRunId`. Empty for a plan that does
    // not convert (there is nothing to confirm).
    [[nodiscard]] static std::string confirmationFingerprint(
        std::string_view sourceRunId, const LegacyRosterConversionPlan& plan);

    // AG-17.7C: the ONLY plans an automatic Retry may convert without asking the user: a lone
    // workspace lead that converts to a single-assignment Manifest Run with NOTHING dropped.
    // Every other plan - a proposal roster, a plan with any dropped setting, a text roster, a
    // plan that does not convert - is false, so it can never fall into the safe branch.
    [[nodiscard]] static bool isSafeAutomaticRetry(const LegacyRosterConversionPlan& plan) noexcept;
};

} // namespace ai::application
