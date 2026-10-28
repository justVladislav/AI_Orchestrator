#pragma once

#include "ai/application/execution/LegacyConversionRecord.hpp"
#include "ai/application/execution/LegacyRosterAdapter.hpp"
#include "ai/application/RunManager.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace ai::application {

// AG-17.7C: the only fields a user may edit when converting a legacy roster Run into a Manifest
// Run. The roster, the workflow, the parent and the profile are NEVER inputs: the Backend builds
// them from the immutable source Run, which is what makes the confirmation stable.
struct ConvertedRerunOptions {
    std::string title;
    std::string task;
    std::optional<std::filesystem::path> workspacePath;
    std::uint32_t maxParallelism {4};
    PlanningCompletionPolicy planningCompletionPolicy {PlanningCompletionPolicy::RequireManifest};
    VerificationRepairPolicy verificationRepairPolicy {VerificationRepairPolicy::OneBoundedWave};
};

// What the conversion dialog shows, and what a conversion is confirmed against. Read-only: asking
// for a preview creates nothing and persists nothing.
struct LegacyConversionPreview {
    RunId sourceRunId;
    // The adapter's decision for the source Run's immutable snapshot (its roster, its dropped
    // settings with instruction LENGTHS only, its reason).
    LegacyRosterConversionPlan plan;
    // Bound to THIS source Run and this decision; empty unless the plan converts. The conversion
    // command recomputes it from the source Run and refuses a mismatch.
    std::string confirmationFingerprint;
    // A Retry of this Run converts it automatically (a lone workspace lead with nothing dropped).
    bool safeAutomaticRetry {false};
    // False for a lone workspace lead: the legacy runner cannot execute it, so Edit & rerun must
    // not offer the legacy variant.
    bool legacyRerunAvailable {true};
    // The source Run's own editable fields, as the dialog's defaults.
    ConvertedRerunOptions defaults;
};

// The stable message startup recovery writes on a Run it turns Interrupted and on every stage it
// settles under it (one wording, so the Run and its stages read as the one event they are).
inline constexpr const char* kInterruptedRunRecoveryMessage =
    "Recovered after an interrupted application session";

// AG-17.7C-3 (review): a converted Run's record TOGETHER with its source Run, as one read. The
// source is a historical fact the audit outlives (the record has no foreign key to it), so two
// different things must never be confused:
//   `source` empty            - the read SUCCEEDED and the source Run no longer exists: a fact;
//   the whole read fails      - the source could not be read at all: a fault, reported typed.
struct LegacyConversionView {
    LegacyConversionRecord record;
    std::optional<Run> source;
};

/** Public application facade for run commands and read models. */
class OrchestrationService {
public:
    OrchestrationService(
        IProjectRepository& projectRepository,
        IRunRepository& runRepository,
        RunManager& runManager);

    common::Result<RunId> startRun(const StartRunRequest& request);
    common::Result<void> cancelRun(const RunId& runId);
    // A Retry reuses the source Run's immutable snapshot. AG-17.7C: a legacy lone workspace lead
    // (the shape the legacy runner cannot execute, including every retired read-only Run) is
    // retried as a single-assignment Manifest Run - automatically, with a not_required_safe_retry
    // conversion record - but ONLY when the conversion drops nothing; otherwise it returns
    // legacy_roster.conversion_confirmation_required and no Run is created. Every other legacy
    // shape (a proposal roster, a text roster, ...) retries exactly as before.
    common::Result<RunId> retryRun(const RunId& runId);
    [[nodiscard]] common::Result<RerunDraft> prepareRerunDraft(
        const RunId& runId) const;
    // Edit & rerun. AG-17.7C: from a legacy roster Run this can no longer be switched to the
    // Manifest workflow (that is a conversion and must go through startConvertedRerun), and a
    // lone workspace lead can no longer be rerun on the legacy runner.
    common::Result<RunId> startEditedRerun(
        const RunId& sourceRunId,
        StartRunRequest request);
    // AG-17.7C: read-only preview of a legacy conversion (see LegacyConversionPreview).
    [[nodiscard]] common::Result<LegacyConversionPreview> previewLegacyConversion(
        const RunId& sourceRunId) const;
    // AG-17.7C: the explicit, user-confirmed conversion. The Backend rebuilds the plan from the
    // immutable source Run and requires `expectedConfirmationFingerprint` to be exactly what the
    // preview showed for THIS source Run (legacy_roster.confirmation_stale otherwise, and
    // legacy_roster.conversion_confirmation_required when it is empty). The new Run and its
    // explicit_user conversion record are created in one atomic aggregate insert.
    common::Result<RunId> startConvertedRerun(
        const RunId& sourceRunId,
        ConvertedRerunOptions options,
        const std::string& expectedConfirmationFingerprint);
    // The durable conversion record of a converted Run (nullopt for any other Run); a record that
    // fails its integrity or does not match its Run is a typed legacy_conversion.record_corrupt.
    [[nodiscard]] common::Result<std::optional<LegacyConversionRecord>> findLegacyConversion(
        const RunId& runId) const;
    // AG-17.7C-3 (review): the record and its source Run in one read (nullopt for a Run that was
    // not converted). A record that fails its integrity check is the typed
    // legacy_conversion.record_corrupt; a source Run that cannot be READ fails the whole view with
    // that read error - it is never reported as "the source no longer exists", which is only a
    // successful read that finds nothing.
    [[nodiscard]] common::Result<std::optional<LegacyConversionView>> findLegacyConversionView(
        const RunId& runId) const;
    // AG-17.7C-3: which Runs were created by a conversion, and how it was confirmed - the history
    // list's marker. Cheap and unvalidated by design; the record itself comes from
    // findLegacyConversion.
    [[nodiscard]] common::Result<std::vector<LegacyConversionIndexEntry>> listLegacyConversionIndex() const;
    [[nodiscard]] common::Result<std::vector<Run>> listRuns(
        const std::optional<ProjectId>& projectId = std::nullopt) const;
    // Single-row projection for RunController::handleEvent(): a RunEvent
    // always names an already-persisted Run, so refreshing just that row
    // never needs the full listRuns() scan.
    [[nodiscard]] common::Result<std::optional<Run>> findRun(const RunId& runId) const;
    [[nodiscard]] common::Result<std::vector<RunStage>> listStages(const RunId& runId) const;
    // Startup recovery of Runs a prior process left Queued/Running/Cancelling: each becomes
    // Interrupted, never resumed. AG-17.7D-1: its stage rows are settled FIRST, with the same
    // recovery instant and message (Running -> Failed, Pending -> Skipped, terminal stages
    // untouched), so a terminal Run never keeps a stage that claims to be live.
    common::Result<void> recoverInterruptedRuns();
    void setEventHandler(RunEventHandler handler);

private:
    IProjectRepository& projectRepository_;
    IRunRepository& runRepository_;
    RunManager& runManager_;
};

} // namespace ai::application
