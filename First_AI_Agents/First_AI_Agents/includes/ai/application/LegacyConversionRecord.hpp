#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/application/LegacyRosterAdapter.hpp"
#include "ai/common/Result.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

// AG-17.7C: the durable audit of a legacy roster -> Manifest conversion.
//
// A record is written ONLY together with the new Run it describes (one atomic aggregate
// insert; see IRunRepository::insertRunWithStagesAndLegacyConversion), is immutable, and says:
// which immutable source Run and workflow it came from, which adapter policy decided, why,
// what exact roster was frozen, what was NOT honoured (`dropped`), and whether a user
// confirmed it. It never holds instruction text or any other free-form user text: an
// `instructions` entry carries only its length.

// Bounds. A record that exceeds them is refused (never silently truncated).
inline constexpr std::size_t kLegacyConversionMaxDroppedEntries = 256;   // 64 members x 4 settings
inline constexpr std::size_t kLegacyConversionMaxLegacyValueBytes = 256;
inline constexpr std::size_t kLegacyConversionMaxReasonBytes = 512;
inline constexpr std::size_t kLegacyConversionMaxDroppedJsonBytes = 64 * 1024;

// Errors of the record itself.
//   record_invalid - a record that must not be written;
//   record_corrupt - a stored record that fails its own integrity or its Run's - fail closed,
//                    never shown half trusted;
//   unsupported    - a repository that cannot store the audit refuses to create the Run.
inline constexpr const char* kLegacyConversionRecordInvalidCode = "legacy_conversion.record_invalid";
inline constexpr const char* kLegacyConversionRecordCorruptCode = "legacy_conversion.record_corrupt";
inline constexpr const char* kLegacyConversionUnsupportedCode = "run.legacy_conversion_unsupported";

// Typed outcomes of the conversion commands (see OrchestrationService).
inline constexpr const char* kLegacyConversionConfirmationRequiredCode =
    "legacy_roster.conversion_confirmation_required";
inline constexpr const char* kLegacyConversionConfirmationStaleCode = "legacy_roster.confirmation_stale";
inline constexpr const char* kLegacyConversionNotConvertibleCode = "legacy_roster.not_convertible";
inline constexpr const char* kLegacyConversionStaysLegacyCode = "legacy_roster.stays_legacy";

enum class LegacyConversionConfirmationMode {
    // The user saw the summary and confirmed it through the explicit conversion command.
    ExplicitUser,
    // An automatic Retry of a safe lone workspace lead (nothing dropped): no confirmation was
    // asked for and none must ever be claimed.
    NotRequiredSafeRetry,
};
[[nodiscard]] const char* toString(LegacyConversionConfirmationMode mode) noexcept;
[[nodiscard]] std::optional<LegacyConversionConfirmationMode>
legacyConversionConfirmationModeFromString(std::string_view value) noexcept;

// What a conversion command knows before the new Run exists.
struct LegacyConversionDraft {
    RunId sourceRunId;
    std::string sourceWorkflowId;
    std::string policyId;
    std::string reasonCode;
    std::string sourceSnapshotFingerprint;
    std::string targetRosterFingerprint;
    std::string confirmationFingerprint;
    std::vector<LegacyRosterDroppedSetting> dropped;
    LegacyConversionConfirmationMode mode {LegacyConversionConfirmationMode::ExplicitUser};
};

struct LegacyConversionRecord {
    RunId runId;                  // the NEW Run
    RunId sourceRunId;            // the immutable legacy Run (== the new Run's parentRunId)
    std::string sourceWorkflowId;
    std::string policyId;
    std::string reasonCode;
    std::string sourceSnapshotFingerprint;
    std::string targetRosterFingerprint;
    std::string confirmationFingerprint;
    std::vector<LegacyRosterDroppedSetting> dropped;
    LegacyConversionConfirmationMode confirmationMode {LegacyConversionConfirmationMode::ExplicitUser};
    // Set exactly when confirmationMode is ExplicitUser, from the Backend clock.
    std::optional<std::chrono::system_clock::time_point> confirmedAt;
    std::chrono::system_clock::time_point createdAt;
};

// AG-17.7C-3: one line of the conversion INDEX - which Runs were created by a conversion and in
// which confirmation mode - for the history list's marker. It is deliberately NOT the record:
// reading it validates nothing beyond the mode vocabulary, so a row whose mode is not recognised
// is reported as such (`mode` empty) instead of hidden. The record itself, with its full
// integrity check, still comes only from IRunRepository::findLegacyConversion.
struct LegacyConversionIndexEntry {
    RunId runId;
    std::optional<LegacyConversionConfirmationMode> mode;
};

// The draft of a conversion plan built from the source Run `sourceRunId`. Fails when the plan
// does not convert (there is nothing to record) or when the safe-retry mode is requested for a
// plan that is not a safe automatic Retry.
[[nodiscard]] common::Result<LegacyConversionDraft> makeLegacyConversionDraft(
    const RunId& sourceRunId,
    const LegacyRosterConversionPlan& plan,
    LegacyConversionConfirmationMode mode);

// The record of `draft` for the new Run `runId`. `confirmedAt` is honoured only for
// ExplicitUser (it is dropped for the safe-retry mode, which never claims a confirmation).
[[nodiscard]] LegacyConversionRecord makeLegacyConversionRecord(
    const LegacyConversionDraft& draft,
    const RunId& runId,
    std::chrono::system_clock::time_point createdAt,
    std::optional<std::chrono::system_clock::time_point> confirmedAt);

// The instant the Backend stamps on an explicit confirmation: its own clock, but never earlier
// than the Run being confirmed (the system clock may step backwards, e.g. NTP; the record's
// integrity check requires `confirmedAt >= createdAt`).
[[nodiscard]] std::chrono::system_clock::time_point legacyConversionConfirmationTime(
    std::chrono::system_clock::time_point now,
    std::chrono::system_clock::time_point runCreatedAt) noexcept;

// Integrity of a record on its own: bounded non-empty ids, closed vocabularies, 64-hex
// fingerprints, every `dropped` entry well formed (an instructions entry is a length and
// nothing else), the confirmation mode consistent with confirmedAt and with `dropped`, and a
// confirmation fingerprint that matches the record's own fields. nullopt = consistent.
[[nodiscard]] std::optional<std::string> legacyConversionRecordProblem(
    const LegacyConversionRecord& record);

// The record against the Run it describes: the record names this Run, its parent is the
// source Run, the Run is a Manifest Run and its frozen assignment snapshot is exactly the
// roster the record certifies. Includes legacyConversionRecordProblem. nullopt = consistent.
[[nodiscard]] std::optional<std::string> legacyConversionAggregateProblem(
    const Run& run, const LegacyConversionRecord& record);

// Strict serialization of `dropped`. Writing fails (record_invalid) on an entry outside the
// bounds; reading fails (record_corrupt) on anything that is not exactly an array of objects
// with the four string fields, within the bounds.
[[nodiscard]] common::Result<std::string> legacyConversionDroppedToJson(
    const std::vector<LegacyRosterDroppedSetting>& dropped);
[[nodiscard]] common::Result<std::vector<LegacyRosterDroppedSetting>> legacyConversionDroppedFromJson(
    std::string_view json);

} // namespace ai::application
