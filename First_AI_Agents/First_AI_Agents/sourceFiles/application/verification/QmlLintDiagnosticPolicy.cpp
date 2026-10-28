#include "ai/application/verification/QmlLintDiagnosticPolicy.hpp"

#include "ai/application/workspace/FilesystemPathUtils.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>

namespace ai::application {
namespace {

// See QmlLintFindingSeverity::Advisory's own doc comment for why this list
// is small and evidence-driven rather than a general qmllint taxonomy.
constexpr std::string_view kAdvisoryTags[] = {"unqualified", "unused-imports"};

// Messages that mean the component cannot be instantiated (or cannot work), whatever tag qmllint
// files them under - see classifyQmlLintFindingSeverity(tag, text). qmllint reports the first one
// under [unqualified], next to the harmless "Unqualified access".
constexpr std::string_view kAlwaysHardMessages[] = {
    "no matching signal found for handler",
};

// Before/after findings can come from two independently reconstructed
// candidate overlays (e.g. a source Run's original candidate vs a repair
// child's successor) - their diagnostic fileKeys are not guaranteed to use
// the same "<candidate>/" wrapper, path separator, or case. Strip the
// wrapper (same prefixes normalizedFindingPath in
// ManifestVerificationAttributionService.cpp strips) and reuse the same
// generic-slash, case-folded key every other cross-Run path comparison in
// this codebase already uses, so two findings for the SAME real file always
// fingerprint identically regardless of which side produced them.
std::string canonicalQmlLintFileKey(const std::string& fileKey)
{
    std::string relative = fileKey;
    for (const std::string_view prefix :
        {std::string_view("<candidate>/"), std::string_view("<candidate>\\")}) {
        if (relative.starts_with(prefix)) {
            relative = relative.substr(prefix.size());
            break;
        }
    }
    return detail::normalizedPathKey(detail::pathFromUtf8(relative));
}

} // namespace

// Post-Stage-3 hardening: case-preserved, forward-slash, safety-validated
// candidate-relative display path for a New finding - the normalized key
// above is a lowercase lookup key only (see its own established precedent in
// ManifestVerificationManualHandoffReadModelService.cpp's own
// normalizedCandidateDiagnosticPath(), which this mirrors deliberately after
// that exact lowercase-leak bug was found and fixed there). Returns nullopt
// for a fileKey with no safe "<candidate>/" prefix, an empty remainder, or a
// traversal/drive-qualified shape - such a finding can never be classified
// as anything other than Unattributable. Exported (see this function's own
// declaration in QmlLintDiagnosticPolicy.hpp) so a later read-model
// projection of a durable finding never needs a second implementation.
std::optional<std::string> displayQmlLintFindingPath(const std::string& fileKey)
{
    std::string relative = fileKey;
    bool candidateRelative = false;
    for (const std::string_view prefix :
        {std::string_view("<candidate>/"), std::string_view("<candidate>\\")}) {
        if (relative.starts_with(prefix)) {
            relative = relative.substr(prefix.size());
            candidateRelative = true;
            break;
        }
    }
    if (!candidateRelative || relative.empty()) return std::nullopt;
    const auto rawPath = detail::pathFromUtf8(relative);
    if (rawPath.is_absolute() || rawPath.has_root_name() || rawPath.has_root_directory()
        || std::any_of(rawPath.begin(), rawPath.end(),
            [](const std::filesystem::path& component) { return component == ".."; })) {
        return std::nullopt;
    }
    const auto path = rawPath.lexically_normal();
    const auto key = detail::normalizedPathKey(path);
    if (key.empty() || key == "." || key.starts_with("../")
        || key.find("/../") != std::string::npos || key.find(':') != std::string::npos) {
        return std::nullopt;
    }
    return path.generic_string();
}

const char* toString(QmlLintFindingSeverity value) noexcept
{
    switch (value) {
    case QmlLintFindingSeverity::Hard: return "hard";
    case QmlLintFindingSeverity::Advisory: return "advisory";
    }
    return "hard";
}

QmlLintFindingSeverity classifyQmlLintFindingSeverity(std::string_view tag) noexcept
{
    for (const auto& advisory : kAdvisoryTags) {
        if (tag == advisory) return QmlLintFindingSeverity::Advisory;
    }
    return QmlLintFindingSeverity::Hard;
}

QmlLintFindingSeverity classifyQmlLintFindingSeverity(
    std::string_view tag, std::string_view text) noexcept
{
    for (const auto& message : kAlwaysHardMessages) {
        if (text.find(message) != std::string_view::npos) return QmlLintFindingSeverity::Hard;
    }
    return classifyQmlLintFindingSeverity(tag);
}

bool isSupportedQmlLintDiagnosticPolicyId(std::string_view policyId) noexcept
{
    return policyId == kQmlLintDiagnosticPolicyAdvisoryV1;
}

std::string fingerprintQmlLintFinding(const ManifestVerificationDiagnosticFinding& finding)
{
    return canonicalQmlLintFileKey(finding.fileKey) + '\0' + finding.tag + '\0' + finding.text;
}

QmlLintDiagnosticDelta computeQmlLintDiagnosticDelta(
    const std::vector<ManifestVerificationDiagnosticFinding>& before,
    const std::vector<ManifestVerificationDiagnosticFinding>& after)
{
    QmlLintDiagnosticDelta delta;
    std::map<std::string, std::vector<const ManifestVerificationDiagnosticFinding*>>
        beforeByFingerprint;
    for (const auto& finding : before)
        beforeByFingerprint[fingerprintQmlLintFinding(finding)].push_back(&finding);
    std::map<std::string, std::vector<const ManifestVerificationDiagnosticFinding*>>
        afterByFingerprint;
    for (const auto& finding : after)
        afterByFingerprint[fingerprintQmlLintFinding(finding)].push_back(&finding);

    std::set<std::string> allFingerprints;
    for (const auto& [fingerprint, findings] : beforeByFingerprint) allFingerprints.insert(fingerprint);
    for (const auto& [fingerprint, findings] : afterByFingerprint) allFingerprints.insert(fingerprint);

    for (const auto& fingerprint : allFingerprints) {
        const auto beforeIt = beforeByFingerprint.find(fingerprint);
        const auto afterIt = afterByFingerprint.find(fingerprint);
        const std::size_t beforeCount =
            beforeIt == beforeByFingerprint.end() ? 0 : beforeIt->second.size();
        const std::size_t afterCount =
            afterIt == afterByFingerprint.end() ? 0 : afterIt->second.size();
        const std::size_t matched = (std::min)(beforeCount, afterCount);
        const auto& representative = afterCount > 0
            ? *afterIt->second.front() : *beforeIt->second.front();
        // The fingerprint includes the message, so every instance of one fingerprint shares both
        // inputs of the classification.
        const auto severity = classifyQmlLintFindingSeverity(representative.tag, representative.text);

        for (std::size_t i = 0; i < matched; ++i) {
            delta.entries.push_back(
                {*afterIt->second[i], QmlLintFindingDeltaKind::Retained, severity});
            ++delta.retainedCount;
            if (severity == QmlLintFindingSeverity::Hard) ++delta.retainedHardCount;
            else ++delta.retainedAdvisoryCount;
        }
        for (std::size_t i = matched; i < afterCount; ++i) {
            delta.entries.push_back(
                {*afterIt->second[i], QmlLintFindingDeltaKind::New, severity});
            ++delta.newCount;
        }
        for (std::size_t i = matched; i < beforeCount; ++i) {
            delta.entries.push_back(
                {*beforeIt->second[i], QmlLintFindingDeltaKind::Eliminated, severity});
            ++delta.eliminatedCount;
        }
    }
    return delta;
}

const char* toString(ManifestContentOutcome value) noexcept
{
    switch (value) {
    case ManifestContentOutcome::Undifferentiated: return "undifferentiated";
    case ManifestContentOutcome::NeedsManualAction: return "needs_manual_action";
    }
    return "undifferentiated";
}

ManifestContentOutcome classifyManifestContentOutcome(
    const ManifestProposalSetVerification& verification)
{
    if (verification.proposalSafetyStatus != ExecutionVerificationStatus::Passed)
        return ManifestContentOutcome::Undifferentiated;

    bool sawFailedQmlLintCheck = false;
    for (const auto& check : verification.checks) {
        if (check.status == ExecutionVerificationStatus::Passed) continue;
        // Only a genuinely Failed QmlLint check is bounded/lint-only
        // evidence this policy can classify. A non-QmlLint check that did
        // not pass (Failed, NotVerifiable, or NotRun) disqualifies the
        // whole result, and so does a QmlLint check itself sitting in
        // NotVerifiable/NotRun (an infra-level uncertainty, never a real,
        // understood lint outcome) - even if it happens to carry structured
        // findings from some other source.
        if (check.kind != ExecutionVerificationKind::QmlLint
            || check.status != ExecutionVerificationStatus::Failed) {
            return ManifestContentOutcome::Undifferentiated;
        }
        sawFailedQmlLintCheck = true;
    }
    // Nothing actually failed here - this is not this policy's concern
    // (the caller's own terminal status already tells the truth).
    if (!sawFailedQmlLintCheck) return ManifestContentOutcome::Undifferentiated;

    bool sawAnyFinding = false;
    for (const auto& diagnostic : verification.diagnostics) {
        // Proven baseline debt is retained as operator-visible evidence on
        // the failed QML-lint check, but it is not part of the candidate's
        // repair burden. In particular, a hard-tagged historical finding
        // must not hide a separate soft candidate-introduced warning from
        // the bounded/manual repair classifier.
        if (diagnostic.code == "verification.qml_warning_baseline_debt"
            || diagnostic.code == "verification.qml_warning_outside_task_scope") {
            continue;
        }
        const bool belongsToFailedQmlLintCheck = std::any_of(
            verification.checks.begin(), verification.checks.end(),
            [&](const ManifestVerificationCheckResult& check) {
                return check.kind == ExecutionVerificationKind::QmlLint
                    && check.status != ExecutionVerificationStatus::Passed
                    && diagnostic.requirementId.has_value()
                    && *diagnostic.requirementId == check.profileId;
            });
        if (!belongsToFailedQmlLintCheck) continue;
        // A diagnostic with no structured findings (e.g. a prerequisite/
        // profile-argument infra failure, never real qmllint output) is not
        // evidence this policy can classify - fail closed.
        if (diagnostic.structuredFindings.empty()) return ManifestContentOutcome::Undifferentiated;
        for (const auto& finding : diagnostic.structuredFindings) {
            sawAnyFinding = true;
            if (classifyQmlLintFindingSeverity(finding.tag, finding.text)
                    == QmlLintFindingSeverity::Hard)
                return ManifestContentOutcome::Undifferentiated;
        }
    }
    return sawAnyFinding
        ? ManifestContentOutcome::NeedsManualAction
        : ManifestContentOutcome::Undifferentiated;
}

const char* toString(QmlLintFindingProvenance value) noexcept
{
    switch (value) {
    case QmlLintFindingProvenance::PreExistingVerificationDebt: return "pre_existing_verification_debt";
    case QmlLintFindingProvenance::ResolvedBaselineDebt: return "resolved_baseline_debt";
    case QmlLintFindingProvenance::CandidateIntroducedInScope: return "candidate_introduced_in_scope";
    case QmlLintFindingProvenance::CandidateIntroducedOutsideManifest:
        return "candidate_introduced_outside_manifest";
    case QmlLintFindingProvenance::Unattributable: return "unattributable";
    }
    return "unattributable";
}

QmlLintCriterionDelta resolveQmlLintCriterionDelta(
    const std::vector<ManifestVerificationDiagnosticFinding>& baselineFindings,
    bool baselineEvidenceAvailable,
    const std::vector<ManifestVerificationDiagnosticFinding>& candidateFindings,
    const std::unordered_set<std::string>& manifestOwnedNormalizedPaths,
    std::optional<std::string_view> frozenPolicyId)
{
    QmlLintCriterionDelta result;
    const bool advisoryPolicy = frozenPolicyId.has_value()
        && isSupportedQmlLintDiagnosticPolicyId(*frozenPolicyId);
    // computeQmlLintDiagnosticDelta() is the single shared multiset resolver:
    // a finding's identity deliberately excludes line/column (a shifted
    // warning caused by unrelated nearby edits is still the same finding),
    // and an extra duplicate instance of an otherwise-identical warning is
    // counted as a genuine New entry rather than silently collapsed - see
    // its own header doc comment.
    const auto delta = computeQmlLintDiagnosticDelta(baselineFindings, candidateFindings);
    for (const auto& entry : delta.entries) {
        if (entry.kind == QmlLintFindingDeltaKind::Eliminated) continue; // nothing to report
        QmlLintCriterionDeltaEntry item;
        item.finding = entry.finding;
        item.severity = advisoryPolicy
            ? entry.severity : QmlLintFindingSeverity::Hard;
        const auto path = displayQmlLintFindingPath(entry.finding.fileKey);
        item.relativePath = path.value_or(std::string());
        if (!baselineEvidenceAvailable || !path.has_value()) {
            // Even a byte-identical baseline/candidate pair is not proven
            // debt when its file identity cannot be bound to one safe,
            // candidate-relative path. Check this before Retained: the
            // multiset comparator accepts arbitrary fileKey strings as
            // opaque identity input, whereas this policy must fail closed.
            item.provenance = QmlLintFindingProvenance::Unattributable;
        } else if (entry.kind == QmlLintFindingDeltaKind::Retained) {
            item.provenance = QmlLintFindingProvenance::PreExistingVerificationDebt;
        } else {
            const auto key = detail::normalizedPathKey(detail::pathFromUtf8(*path));
            item.provenance = manifestOwnedNormalizedPaths.contains(key)
                ? QmlLintFindingProvenance::CandidateIntroducedInScope
                : QmlLintFindingProvenance::CandidateIntroducedOutsideManifest;
        }
        // QML-lint advisory-severity policy (2026-09-28): the sole carve-out
        // from "any non-debt entry fails the criterion" is a New, in-scope
        // finding whose severity classifies Advisory. Unattributable evidence
        // and any CandidateIntroducedOutsideManifest entry still fail closed
        // regardless of severity - only real, Manifest-owned attribution ever
        // qualifies for the carve-out.
        const bool safeAdvisoryInScope =
            item.provenance == QmlLintFindingProvenance::CandidateIntroducedInScope
            && item.severity == QmlLintFindingSeverity::Advisory;
        if (item.provenance != QmlLintFindingProvenance::PreExistingVerificationDebt
            && !safeAdvisoryInScope) {
            result.passed = false;
        }
        result.entries.push_back(std::move(item));
    }
    return result;
}

bool usesBaselineAwareQmlLintPolicy(
    const ManifestProposalSetVerification& verification) noexcept
{
    return std::any_of(verification.diagnostics.begin(), verification.diagnostics.end(),
        [](const ManifestVerificationDiagnostic& diagnostic) {
            return diagnostic.code == "verification.qml_warning_baseline_debt"
                || diagnostic.code == "verification.qml_warning_outside_manifest"
                || diagnostic.code == "verification.qml_baseline_unavailable"
                || diagnostic.code == "verification.qml_warning_advisory";
        });
}

bool isNonBlockingQmlWarningCode(const std::string& code) noexcept
{
    return code == kQmlBaselineDebtDiagnosticCode || code == kQmlAdvisoryDiagnosticCode;
}

const ManifestVerificationDiagnostic* primaryFailureDiagnostic(
    const ManifestProposalSetVerification& verification) noexcept
{
    if (verification.diagnostics.empty()) return nullptr;
    for (const auto& diagnostic : verification.diagnostics) {
        if (!isNonBlockingQmlWarningCode(diagnostic.code)) return &diagnostic;
    }
    return &verification.diagnostics.front();
}

namespace {

std::string lastPathComponent(const std::string& key)
{
    const auto separator = key.find_last_of("/\\");
    return separator == std::string::npos ? key : key.substr(separator + 1);
}

} // namespace

std::string renderQmlLintFindingLabel(const ManifestVerificationDiagnosticFinding& finding)
{
    const auto display = displayQmlLintFindingPath(finding.fileKey);
    std::string label = display.has_value() ? *display : lastPathComponent(finding.fileKey);
    if (finding.line > 0) {
        label += ':' + std::to_string(finding.line);
        if (finding.column > 0) label += ':' + std::to_string(finding.column);
    }
    if (!finding.text.empty()) label += ' ' + finding.text;
    for (char& ch : label) {
        if (ch == '\r' || ch == '\n') ch = ' ';
    }
    return label;
}

std::string describeDiagnosticFindings(
    const ManifestVerificationDiagnostic& diagnostic, std::size_t maxFindings)
{
    if (maxFindings == 0 || diagnostic.structuredFindings.empty()
        || isNonBlockingQmlWarningCode(diagnostic.code)) {
        return {};
    }
    const auto& first = diagnostic.structuredFindings.front();
    if (!first.text.empty() && diagnostic.message.find(first.text) != std::string::npos) {
        return {};
    }
    std::string text;
    const std::size_t shown = std::min(maxFindings, diagnostic.structuredFindings.size());
    for (std::size_t index = 0; index < shown; ++index) {
        if (index != 0) text += "; ";
        text += renderQmlLintFindingLabel(diagnostic.structuredFindings[index]);
    }
    if (diagnostic.structuredFindings.size() > shown) {
        text += " (+" + std::to_string(diagnostic.structuredFindings.size() - shown) + " more)";
    }
    return text;
}

std::string renderVerificationFailureReason(
    const ManifestVerificationDiagnostic& diagnostic, std::size_t maxFindings)
{
    std::string reason = "[" + diagnostic.code + "] " + diagnostic.message;
    const auto findings = describeDiagnosticFindings(diagnostic, maxFindings);
    if (!findings.empty()) reason += ": " + findings;
    return reason;
}

std::string renderVerificationDiagnosticLines(
    const std::vector<ManifestVerificationDiagnostic>& diagnostics,
    std::size_t maxFindingsPerDiagnostic)
{
    std::string out;
    for (const auto& diagnostic : diagnostics) {
        out += "- [" + diagnostic.code + "] " + diagnostic.message;
        if (diagnostic.requirementId.has_value()) out += " (" + *diagnostic.requirementId + ")";
        out += "\n";
        if (diagnostic.structuredFindings.empty()) continue;
        if (isNonBlockingQmlWarningCode(diagnostic.code)) {
            out += "  - " + std::to_string(diagnostic.structuredFindings.size())
                + " finding(s), not blocking\n";
            continue;
        }
        const auto& first = diagnostic.structuredFindings.front();
        if (!first.text.empty() && diagnostic.message.find(first.text) != std::string::npos) continue;
        const std::size_t shown =
            std::min(maxFindingsPerDiagnostic, diagnostic.structuredFindings.size());
        for (std::size_t index = 0; index < shown; ++index) {
            out += "  - " + renderQmlLintFindingLabel(diagnostic.structuredFindings[index]) + "\n";
        }
        if (diagnostic.structuredFindings.size() > shown) {
            out += "  - (+" + std::to_string(diagnostic.structuredFindings.size() - shown)
                + " more)\n";
        }
    }
    return out;
}

} // namespace ai::application
