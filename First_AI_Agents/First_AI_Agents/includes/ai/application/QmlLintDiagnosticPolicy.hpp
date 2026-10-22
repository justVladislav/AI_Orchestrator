#pragma once

#include "ai/application/ExecutionAcceptanceContract.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace ai::application {

// QML-P1 (ARCHITECTURE_GAPS.md's "bounded manual handoff after
// non-progressing lint repair"): Backend-owned, evidence-driven
// classification of a qmllint finding's severity - never provider-selected,
// never inferred from prose. A finding's tag
// (ManifestVerificationDiagnosticFinding::tag, e.g. "unqualified") is the
// sole classification input. The advisory-v1 frozen policy changes the live
// criterion result only for safely-attributed, Manifest-owned findings; a
// missing policy id retains the historical strict behavior.
enum class QmlLintFindingSeverity {
    // A hard semantic failure - an import/type/member/binding resolution
    // problem, or any tag this policy does not explicitly recognize as
    // Advisory. Fail-closed default: an unrecognized qmllint category is
    // never silently treated as cosmetic.
    Hard,
    // A stylistic/best-practice finding that does not indicate the QML is
    // broken. Currently "unqualified" (id/property access without an
    // explicit qualifier - the category two real incidents both showed a
    // repair worker could not reliably eliminate: it kept replacing one
    // unqualified reference with a different one, see the retained findings
    // on Run bb56e001-eac0-4b59-8d0b-d037c6bc1031, and the 2026-09-17 paid
    // continuation canary, source ffc15618-e9f1-4207-9379-ab54c17f23cc,
    // repair child bdbe0f7d-5c4c-4cf9-9dd5-1eb554abb4d4) and "unused-imports"
    // (an unused `import` statement - a qmllint Info-level style finding,
    // never a semantic risk). Grow this list only from a similarly real,
    // documented incident, or a tag qmllint itself reports at Info severity
    // with no semantic-resolution risk - never speculatively.
    //
    // QML-lint advisory-severity policy (2026-09-28, real Stage 4A Run
    // b4123f17-fb4f-4977-9a22-28e8d3d15e23): Advisory still does not mean
    // "ignored" or "Apply-eligible on its own" - an Advisory finding is
    // durable, visible, attributed, and never silently dropped. What changed
    // is narrower: resolveQmlLintCriterionDelta below now lets a qml_lint
    // criterion PASS when every remaining candidate-introduced finding is
    // Advisory (a NEW Hard finding, any CandidateIntroducedOutsideManifest
    // finding regardless of severity, and any Unattributable evidence all
    // still fail the criterion exactly as before). This never widens Manifest
    // ownership, never overrides AG-16 (which only ever reads the resulting
    // aggregate ExecutionVerificationStatus/orderedProposalIds - see
    // ManifestApplyVerificationGate.cpp), and never suppresses the diagnostic
    // itself - see verification.qml_warning_advisory in
    // ManifestProposalSetVerifier.cpp's qmlLintBaselineDeltaCheck().
    Advisory,
};

inline constexpr std::string_view kQmlLintDiagnosticPolicyAdvisoryV1 =
    "qml_lint_advisory_v1";

[[nodiscard]] bool isSupportedQmlLintDiagnosticPolicyId(
    std::string_view policyId) noexcept;

[[nodiscard]] const char* toString(QmlLintFindingSeverity value) noexcept;

// Tag-level default only. A production caller classifies a FINDING with the overload below: qmllint
// files two very different messages under the same "unqualified" tag, so the tag alone cannot tell a
// stylistic finding from one that breaks the component.
[[nodiscard]] QmlLintFindingSeverity classifyQmlLintFindingSeverity(
    std::string_view tag) noexcept;

// Finding-level classification (tag + message). A message that means the QML cannot be loaded or
// cannot work is Hard whatever tag qmllint reported it under:
//
// - `no matching signal found for handler "onXChanged"` - a handler for a signal the object does not
//   have. At run time that is `Cannot assign to non-existent property "onXChanged"`: the component
//   is not created at all. qmllint reports it under [unqualified], the same tag as the harmless
//   "Unqualified access", so before this overload it was Advisory, passed every Backend check and
//   broke the Dashboard after Apply (Stage 7B Run 1c37cc37-33cd-4c68-9b70-bea983b56a5f,
//   RecentAlertsPanel.qml:110 - an onMaxItemsChanged handler inside a nested ColumnLayout that has
//   no maxItems).
//
// Everything else keeps its tag-level classification. Same frozen policy id
// (kQmlLintDiagnosticPolicyAdvisoryV1): this corrects what the policy already meant ("Hard: any
// member/binding resolution problem"); it only ever turns an Advisory finding into a Hard one, and
// only for findings that break the component.
[[nodiscard]] QmlLintFindingSeverity classifyQmlLintFindingSeverity(
    std::string_view tag, std::string_view text) noexcept;

// QML-P2: one finding's participation in a before/after diagnostic
// comparison between two qmllint results for the same check (e.g. a source
// Run's original candidate vs a repair child's final successor).
enum class QmlLintFindingDeltaKind {
    Eliminated, // present before, absent after - real progress.
    // Absent before, present after. This kind alone is never itself the
    // pass/fail authority - see resolveQmlLintCriterionDelta below, the one
    // place that turns a New entry's (severity, in-Manifest-scope) pair into
    // a pass/fail decision. A New finding outside the frozen Manifest's
    // write scope, or one whose evidence cannot be attributed to a safe
    // path, still blocks regardless of severity.
    New,
    Retained,   // present in both - no progress either way.
};

struct QmlLintFindingDeltaEntry {
    ManifestVerificationDiagnosticFinding finding;
    QmlLintFindingDeltaKind kind {QmlLintFindingDeltaKind::Retained};
    QmlLintFindingSeverity severity {QmlLintFindingSeverity::Hard};
};

// The complete, deterministic classification of one qmllint check's
// findings across a before/after pair. Counts are the authoritative
// summary; `entries` is the full per-finding breakdown for a caller that
// needs it (e.g. a future read-model). Deciding what to DO with this data
// (suppress a retry, project NeedsManualAction, ...) is explicitly out of
// this package's scope - see ARCHITECTURE_GAPS.md's QML-P0/P3.
struct QmlLintDiagnosticDelta {
    std::vector<QmlLintFindingDeltaEntry> entries;
    std::size_t eliminatedCount {0};
    std::size_t newCount {0};
    std::size_t retainedCount {0};
    std::size_t retainedHardCount {0};
    std::size_t retainedAdvisoryCount {0};
};

// A stable identity for one finding, deliberately EXCLUDING line/column: a
// finding whose location merely shifted because an earlier, unrelated line
// was added or removed above it must still be recognized as the same
// finding, never miscounted as one eliminated plus one new. Line movement
// alone is not progress. `fileKey` is canonicalized first (the
// "<candidate>/" wrapper stripped, path separators and case normalized) so
// a before/after pair reconstructed from two independent candidate overlays
// - e.g. a source Run's original vs a repair child's successor - still
// fingerprints identically for the same real file even if the two sides
// produced their raw fileKey text slightly differently. Two distinct
// findings that happen to share the same (fileKey, tag, text) - e.g. two
// different unqualified references in the same file with an identical
// generic message - are intentionally indistinguishable by this
// fingerprint; computeQmlLintDiagnosticDelta below compares them as a
// multiset (by count per fingerprint), so a genuine reduction in how many
// such findings exist is still counted as progress even though which
// specific one was fixed cannot be named.
[[nodiscard]] std::string fingerprintQmlLintFinding(
    const ManifestVerificationDiagnosticFinding& finding);

// Case-preserved, forward-slash, safety-validated candidate-relative display
// path for a finding's raw fileKey (e.g. "<candidate>/qml/views/Alerts.qml"
// -> "qml/views/Alerts.qml"). Returns nullopt for a fileKey with no safe
// "<candidate>/" prefix, an empty remainder, or a traversal/drive-qualified
// shape. The one shared implementation for "is this finding's file identity
// safe to show/attribute a path for at all" - reused by
// resolveQmlLintCriterionDelta below and by any later read-model projection
// of a durable ManifestVerificationDiagnosticFinding, so a fileKey is never
// re-parsed by a second, independent implementation.
[[nodiscard]] std::optional<std::string> displayQmlLintFindingPath(
    const std::string& fileKey);

[[nodiscard]] QmlLintDiagnosticDelta computeQmlLintDiagnosticDelta(
    const std::vector<ManifestVerificationDiagnosticFinding>& before,
    const std::vector<ManifestVerificationDiagnosticFinding>& after);

// QML-P0: the truthful, typed disposition of one terminal combined
// verification result, layered ON TOP of (never replacing or overriding)
// the owning Run's own terminal RunStatus. A verification that failed only
// because bounded, content-repairable qmllint findings remain - proposal
// safety and every OTHER required check passed - is NeedsManualAction, not
// an undifferentiated Failed: real, safety-clean work exists and is worth a
// human's review, not silence or another paid attempt. This NEVER
// authorizes AG-16 Apply and NEVER changes RunStatus, terminal truth, or
// what AG-16 itself evaluates - it is purely an additional, honest
// projection for a read-model/UI layer to surface alongside the real
// status.
enum class ManifestContentOutcome {
    // Either nothing failed, or a failure this policy does not recognize as
    // bounded/lint-only: a non-QmlLint required check failed or produced no
    // evidence, proposal safety failed, or at least one remaining QmlLint
    // finding classifies as Hard. Fail-closed default.
    Undifferentiated,
    // Proposal safety passed, every required non-QmlLint check passed, and
    // every remaining QmlLint finding classifies as Advisory (see
    // classifyQmlLintFindingSeverity). A genuine, actionable manual-review
    // candidate.
    NeedsManualAction,
};

[[nodiscard]] const char* toString(ManifestContentOutcome value) noexcept;

[[nodiscard]] ManifestContentOutcome classifyManifestContentOutcome(
    const ManifestProposalSetVerification& verification);

// Post-Stage-3 QML-lint hardening: the single baseline/candidate delta
// resolver every baseline-aware qml_lint criterion uses to decide PASS/FAIL,
// instead of treating any warning as candidate-introduced. Built on top
// of computeQmlLintDiagnosticDelta() above - the one already-multiset-aware
// before/after finding comparison in this codebase - never a second,
// independent baseline/candidate comparison. Every classification stays
// deliberately narrow: only Eliminated/Retained findings can ever be safe:
// - PreExistingVerificationDebt: Retained (present in both baseline and
//   candidate, unchanged identity).
// - ResolvedBaselineDebt: Eliminated (present in baseline, absent from
//   candidate) - informational only, never itself a finding to report.
// - CandidateIntroducedInScope: a New finding whose safe candidate-relative
//   path is owned (ownedPaths/plannedCreatePaths) by some task in the frozen
//   Manifest.
// - CandidateIntroducedOutsideManifest: a New finding with a safe
//   candidate-relative path that no task in the frozen Manifest owns -
//   fail-closed; the exact relative path is always preserved for the
//   operator, and this can never widen Manifest ownership or allowedPaths on
//   its own.
// - Unattributable: baseline evidence itself was not available (the baseline
//   command did not genuinely start/finish), so no finding can be proven
//   pre-existing - every candidate finding fails closed rather than being
//   guessed as debt.
enum class QmlLintFindingProvenance {
    PreExistingVerificationDebt,
    ResolvedBaselineDebt,
    CandidateIntroducedInScope,
    CandidateIntroducedOutsideManifest,
    Unattributable,
};

[[nodiscard]] const char* toString(QmlLintFindingProvenance value) noexcept;

struct QmlLintCriterionDeltaEntry {
    ManifestVerificationDiagnosticFinding finding;
    // Empty for a baseline-only (ResolvedBaselineDebt) entry - normalized,
    // candidate-relative, safety-validated; never an absolute path.
    std::string relativePath;
    QmlLintFindingProvenance provenance {QmlLintFindingProvenance::Unattributable};
    // Propagated verbatim from computeQmlLintDiagnosticDelta's own
    // classifyQmlLintFindingSeverity(finding.tag) call - never recomputed
    // independently, so there is exactly one place a finding's severity is
    // ever decided. Meaningful only for CandidateIntroducedInScope (see
    // QmlLintCriterionDelta::passed below); a Retained/debt entry's severity
    // is still populated but never changes its own already-safe provenance.
    QmlLintFindingSeverity severity {QmlLintFindingSeverity::Hard};
};

// The complete classification of one qml_lint check's candidate findings
// against its baseline counterpart. `passed` is the sole authority this
// resolver ever offers for whether the criterion itself may pass: false
// whenever any CandidateIntroducedOutsideManifest or Unattributable entry
// exists, or any CandidateIntroducedInScope entry whose severity is Hard.
// A CandidateIntroducedInScope entry whose severity is Advisory does NOT by
// itself force `passed` false - this is the one, sole place QML-lint
// advisory-severity policy (2026-09-28) changes pass/fail. `entries` never
// includes a ResolvedBaselineDebt line item (nothing to report once a
// finding is gone); it is tracked only via QmlLintDiagnosticDelta::
// eliminatedCount internally.
struct QmlLintCriterionDelta {
    std::vector<QmlLintCriterionDeltaEntry> entries;
    bool passed {true};
};

// baselineFindings/candidateFindings must already be the exact structured
// findings from the SAME qml_lint check identity (one requirement/command)
// on each side - never mixed across different requirements. When
// baselineEvidenceAvailable is false (the baseline command itself did not
// genuinely start and finish - a tool/timeout/cancellation failure, never a
// content judgment), every candidate finding is classified Unattributable
// regardless of whether it happens to textually match a baseline entry, so a
// broken baseline probe can never silently launder a real regression as old
// debt. manifestOwnedNormalizedPaths is the caller-resolved set of every
// path any task in the frozen Manifest owns (ownedPaths + plannedCreatePaths,
// detail::normalizedPathKey-normalized) - this function never inspects a
// Manifest/task type directly, keeping it usable from contexts that only
// have the resolved ownership set.
[[nodiscard]] QmlLintCriterionDelta resolveQmlLintCriterionDelta(
    const std::vector<ManifestVerificationDiagnosticFinding>& baselineFindings,
    bool baselineEvidenceAvailable,
    const std::vector<ManifestVerificationDiagnosticFinding>& candidateFindings,
    const std::unordered_set<std::string>& manifestOwnedNormalizedPaths,
    std::optional<std::string_view> frozenPolicyId =
        kQmlLintDiagnosticPolicyAdvisoryV1);

// Durable compatibility marker. Old immutable ledger rows predate the
// baseline-aware policy and must reconstruct under their historical
// semantics during continuation/materialize identity checks. A current
// failed result differs from that historical behavior only when it carries
// one of the new stable diagnostic codes below; clean-baseline/new-warning
// failures are semantically identical and need no marker.
//
// verification.qml_warning_advisory is included for the same reconstruction
// reason, not a new one: a Failed mixed Hard+Advisory result can legitimately
// carry ONLY verification.qml_warning (hard) plus
// verification.qml_warning_advisory, with zero baseline-debt or
// outside-Manifest diagnostics - and that row still used the baseline-aware
// resolver (advisory provenance only ever comes from
// resolveQmlLintCriterionDelta). Without this marker such a row would be
// misclassified as pre-baseline-aware and reconstructed via the plain,
// non-delta QmlLint path on a later continuation/manual-handoff repair.
[[nodiscard]] bool usesBaselineAwareQmlLintPolicy(
    const ManifestProposalSetVerification& verification) noexcept;

// The two diagnostic codes that never block a check: proven unchanged baseline debt and an
// advisory-severity in-scope warning. The single source of this pair for the policy, the manual-handoff
// read model and the repairability classification - do not spell the strings anywhere else.
inline constexpr std::string_view kQmlBaselineDebtDiagnosticCode =
    "verification.qml_warning_baseline_debt";
inline constexpr std::string_view kQmlAdvisoryDiagnosticCode = "verification.qml_warning_advisory";
[[nodiscard]] bool isNonBlockingQmlWarningCode(const std::string& code) noexcept;

// The diagnostic that best explains why a verification did not pass, for the
// one-line terminal reason / ledger summary. Diagnostics are stored in emission
// order, and the baseline-aware lint check always emits the harmless
// verification.qml_warning_baseline_debt (and the non-blocking
// verification.qml_warning_advisory) BEFORE any blocking finding, so
// diagnostics.front() named proven pre-existing debt as the reason a Run failed
// while the real blocker (e.g. verification.qml_warning_outside_manifest) sat
// further down the list. This returns the first diagnostic that is not one of
// those two non-blocking codes, falling back to the first diagnostic when every
// entry is non-blocking, and nullptr when there are none. Presentation only -
// never an input to pass/fail decisions.
[[nodiscard]] const ManifestVerificationDiagnostic* primaryFailureDiagnostic(
    const ManifestProposalSetVerification& verification) noexcept;

// User-facing text for a diagnostic's structured findings (2026-10-07, Stage 8B Run 969e35fa...: the
// Run failed on `no matching signal found for handler "onAction"`, yet its terminal message, the
// verification report and the Architect-advice input only said "QML lint emitted one or more
// warnings"; file, line and text lived solely in the identity JSON). Presentation only - never an input
// to a pass/fail decision. Everything is built from the already sanitized, per-file bounded finding
// records, never from raw command output.

// "qml/views/X.qml:192:17 no matching signal found for handler "onAction" [unqualified]". The path is
// the candidate-relative display path; a key without a safe candidate prefix is reduced to its file name
// so an absolute path can never reach a message or a report. One line (CR/LF become spaces).
[[nodiscard]] std::string renderQmlLintFindingLabel(
    const ManifestVerificationDiagnosticFinding& finding);

// The diagnostic's own findings on one line: up to `maxFindings` labels joined by "; " plus " (+N more)".
// Empty for a diagnostic that carries no findings, for the non-blocking baseline-debt / advisory codes
// (naming debt as a failure reason would mislead) and when the flat `message` already contains the first
// finding's text (a diagnostic whose message IS the excerpt is not repeated).
[[nodiscard]] std::string describeDiagnosticFindings(
    const ManifestVerificationDiagnostic& diagnostic, std::size_t maxFindings);

// "[code] message" of a failing diagnostic, followed by ": " and its describeDiagnosticFindings() when
// there are any - the reason text of a Run's terminal message.
[[nodiscard]] std::string renderVerificationFailureReason(
    const ManifestVerificationDiagnostic& diagnostic, std::size_t maxFindings);

// The "- [code] message (requirement)" list of the verification report's Diagnostics section. A
// diagnostic with structured findings gets nested lines: a blocking one lists up to
// `maxFindingsPerDiagnostic` labels (+ "(+N more)"), a baseline-debt / advisory one only a count so the
// report stays focused on what blocks. A diagnostic without findings renders exactly as before.
[[nodiscard]] std::string renderVerificationDiagnosticLines(
    const std::vector<ManifestVerificationDiagnostic>& diagnostics,
    std::size_t maxFindingsPerDiagnostic);

} // namespace ai::application
